// Captive portal DNS server (access point mode only).
//
// Operating systems and some browsers check a fixed URL after joining a Wi-Fi network. If the
// answer isn't the expected one, they show a "Sign in to network" page. This DNS server
// answers those check hostnames with the ESP32's IP, and the HTTP server on port 80 then
// redirects the check to the control page (handleHttpRedirect).
//
// All other names get "no such domain". Answering every name would send every app's
// background HTTPS traffic to the ESP32, and each of those TLS connections takes ~35 KB of
// heap until the app gives up, which exhausts memory (mbedtls_ssl_setup returned -0x7F00).

// Connectivity check hostnames (the URL each one checks in the comment)
const char* captivePortalHosts[] = {
  // Android, ChromeOS, Chrome: http://connectivitycheck.gstatic.com/generate_204
  "connectivitycheck.gstatic.com",
  "connectivitycheck.android.com",
  "clients3.google.com",
  // Android variants: Xiaomi (MIUI), Huawei
  "connect.rom.miui.com",
  "connectivitycheck.platform.hicloud.com",
  // iPhone, iPad, macOS: http://captive.apple.com/hotspot-detect.html
  "captive.apple.com",
  // Windows: http://www.msftconnecttest.com/connecttest.txt (older: www.msftncsi.com/ncsi.txt)
  "www.msftconnecttest.com",
  "ipv6.msftconnecttest.com",
  "www.msftncsi.com",
  // Firefox on any OS: http://detectportal.firefox.com/canonical.html
  "detectportal.firefox.com",
  // Linux NetworkManager: Ubuntu, GNOME, KDE, Fedora
  "connectivity-check.ubuntu.com",
  "nmcheck.gnome.org",
  "networkcheck.kde.org",
  "fedoraproject.org",
};

const uint16_t dnsPort = 53;
const uint32_t dnsAnswerTtl = 10; // seconds: short, so devices don't keep the ESP32 IP after leaving
WiFiUDP captiveDnsUdp;
IPAddress captiveDnsIP;
bool captiveDnsRunning = false;

void captiveDnsStart(IPAddress ip)
{
  captiveDnsIP = ip;
  captiveDnsRunning = captiveDnsUdp.begin(dnsPort);
  if (captiveDnsRunning)
  {
    LOGI("Captive portal DNS started, answering %u connectivity check hostnames",
         sizeof(captivePortalHosts) / sizeof(captivePortalHosts[0]));
  }
  else
  {
    LOGE("Captive portal DNS failed to start on port %u", dnsPort);
  }
}

// Names answered with the ESP32 IP: the check hostnames, plus its own name as a fallback for
// devices that resolve .local through regular DNS instead of mDNS
bool isCaptivePortalHost(const String &name)
{
  if (name == String(hostnamePrefix) || name == String(hostnamePrefix) + ".local")
  {
    return true;
  }
  for (const char* host : captivePortalHosts)
  {
    if (name == host)
    {
      return true;
    }
  }
  return false;
}

// Handle at most one DNS query per call (from loop())
void captiveDnsLoop()
{
  if (!captiveDnsRunning || captiveDnsUdp.parsePacket() == 0)
  {
    return;
  }
  uint8_t packet[512];
  int len = captiveDnsUdp.read(packet, sizeof(packet));
  // Header: ID(2) flags(2) QDCOUNT(2) ANCOUNT(2) NSCOUNT(2) ARCOUNT(2). Only standard queries
  // (QR=0, opcode 0) with exactly one question are answered.
  if (len < 12 || (packet[2] & 0x80) || (packet[2] & 0x78) || packet[4] != 0 || packet[5] != 1)
  {
    return;
  }

  // Question: name as length-prefixed labels, then QTYPE(2) QCLASS(2)
  String name;
  int pos = 12;
  while (pos < len && packet[pos] != 0)
  {
    int labelLen = packet[pos];
    if (labelLen > 63 || pos + 1 + labelLen >= len)
    {
      return; // compressed or malformed name
    }
    if (name.length() > 0)
    {
      name += '.';
    }
    for (int i = 0; i < labelLen; i++)
    {
      name += (char)tolower(packet[pos + 1 + i]);
    }
    pos += 1 + labelLen;
  }
  if (pos + 5 > len)
  {
    return;
  }
  uint16_t qtype = (packet[pos + 1] << 8) | packet[pos + 2];
  uint16_t qclass = (packet[pos + 3] << 8) | packet[pos + 4];
  int questionEnd = pos + 5;

  bool known = isCaptivePortalHost(name);
  bool answerA = known && qtype == 1 && qclass == 1; // A record, class IN

  // Reply: same ID and question, QR=1, AA=1, RD copied. Known names asked for other record
  // types (e.g. AAAA) get an empty NOERROR answer so the device falls back to IPv4;
  // unknown names get NXDOMAIN (rcode 3).
  uint8_t reply[512 + 16];
  memcpy(reply, packet, questionEnd);
  reply[2] = 0x84 | (packet[2] & 0x01);
  reply[3] = known ? 0x00 : 0x03;
  reply[6] = 0;
  reply[7] = answerA ? 1 : 0; // ANCOUNT
  reply[8] = reply[9] = reply[10] = reply[11] = 0; // NSCOUNT, ARCOUNT (EDNS record dropped)
  int replyLen = questionEnd;
  if (answerA)
  {
    const uint8_t answer[] = {
      0xC0, 0x0C,                 // name: pointer to the question name
      0x00, 0x01, 0x00, 0x01,     // type A, class IN
      (uint8_t)(dnsAnswerTtl >> 24), (uint8_t)(dnsAnswerTtl >> 16),
      (uint8_t)(dnsAnswerTtl >> 8), (uint8_t)dnsAnswerTtl,
      0x00, 0x04,                 // data length
      captiveDnsIP[0], captiveDnsIP[1], captiveDnsIP[2], captiveDnsIP[3],
    };
    memcpy(reply + replyLen, answer, sizeof(answer));
    replyLen += sizeof(answer);
  }

  captiveDnsUdp.beginPacket(captiveDnsUdp.remoteIP(), captiveDnsUdp.remotePort());
  captiveDnsUdp.write(reply, replyLen);
  captiveDnsUdp.endPacket();

  if (known)
  {
    LOGD("DNS %s type %u from %s -> %s", name.c_str(), qtype, captiveDnsUdp.remoteIP().toString().c_str(),
         answerA ? captiveDnsIP.toString().c_str() : "no IPv4 answer");
  }
  else
  {
    LOGV("DNS %s type %u from %s -> no such domain", name.c_str(), qtype, captiveDnsUdp.remoteIP().toString().c_str());
  }
}
