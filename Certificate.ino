// Self-signed TLS certificate for the HTTPS server.
//
// The ESP32 generates its own private key and certificate on first boot and stores them in
// flash (NVS), so every MiniSkidi has its own key and no key is in the source code or firmware.
// Browsers show a warning for self-signed certificates: accept it once per browser
// ("Advanced > Proceed"). The warning comes back when the certificate changes, which happens
// when the names/IPs below change (e.g. a new client-mode IP from the router), when it expires,
// or when regenerateCertificate is set.
//
// The certificate meets current browser requirements (Chrome, Firefox, Safari/iOS):
// EC P-256 key, ECDSA-SHA256 signature, SubjectAltName with all names/IPs, basicConstraints
// CA:FALSE, keyUsage digitalSignature, extendedKeyUsage serverAuth, at most 825 days validity.
// The includes it needs are at the top of MiniSkidi_3_0.ino.

// Set to true and upload to create a new key and certificate on every boot; set back to false
// afterwards, otherwise browsers show the warning again after each restart.
const bool regenerateCertificate = false;
// 825 days: Safari/iOS/macOS reject server certificates valid for longer. Chrome and Firefox
// would accept longer-lived self-signed certificates (e.g. 3652 for 10 years).
const int certificateValidityDays = 825;

String tlsCertificatePem; // server certificate, PEM, used by the HTTPS server
String tlsPrivateKeyPem;  // private key, PEM, used by the HTTPS server

// Days since 1970-01-01 for a calendar date, and back (algorithms by Howard Hinnant)
long daysFromCivil(int y, int m, int d)
{
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const long yoe = y - era * 400;
  const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civilFromDays(long z, int &y, int &m, int &d)
{
  z += 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const long doe = z - era * 146097;
  const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const long mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = yoe + era * 400 + (m <= 2);
}

// The ESP32 has no clock in access point mode, so the certificate validity starts at the date
// the firmware was compiled (__DATE__, e.g. "Oct  5 2026")
long buildDateDays()
{
  const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char month[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
  int m = (strstr(months, month) - months) / 3 + 1;
  return daysFromCivil(atoi(__DATE__ + 7), m, atoi(__DATE__ + 4));
}

// X.509 time as used by mbedtls, e.g. "20261005000000"
String x509Time(long days)
{
  int y, m, d;
  civilFromDays(days, y, m, d);
  char buf[15];
  snprintf(buf, sizeof(buf), "%04d%02d%02d000000", y, m, d);
  return String(buf);
}

// Names and IPs the certificate is valid for. It covers both Wi-Fi modes, e.g.
// "DNS:miniskidi.local,DNS:miniskidi-a1b2c3.local,IP:192.168.4.1,IP:192.168.1.50"
String certificateSubjectAltNames()
{
  String clientHostname = String(hostnamePrefix) + "-" + macSuffix;
  clientHostname.toLowerCase();
  String sans = "DNS:" + String(hostnamePrefix) + ".local,DNS:" + clientHostname + ".local,IP:" + apDefaultIP;
  if (!apMode)
  {
    sans += ",IP:" + WiFi.localIP().toString();
  }
  return sans;
}

// DER-encode the SubjectAltName extension value from the list above
// (mbedtls 2.28 has no helper for it). Returns the encoded length, or 0 on error.
size_t encodeSubjectAltNames(const String &sans, unsigned char *der, size_t size)
{
  unsigned char body[250];
  size_t n = 0;
  int start = 0;
  while (start < (int)sans.length())
  {
    int end = sans.indexOf(',', start);
    if (end < 0)
    {
      end = sans.length();
    }
    String entry = sans.substring(start, end);
    start = end + 1;
    if (entry.startsWith("DNS:"))
    {
      String name = entry.substring(4);
      if (name.length() > 127 || n + 2 + name.length() > sizeof(body))
      {
        return 0;
      }
      body[n++] = 0x82; // [2] dNSName
      body[n++] = name.length();
      memcpy(body + n, name.c_str(), name.length());
      n += name.length();
    }
    else if (entry.startsWith("IP:"))
    {
      IPAddress ip;
      if (!ip.fromString(entry.substring(3)) || n + 6 > sizeof(body))
      {
        return 0;
      }
      body[n++] = 0x87; // [7] iPAddress
      body[n++] = 4;
      for (int i = 0; i < 4; i++)
      {
        body[n++] = ip[i];
      }
    }
  }
  // SEQUENCE header: short length form below 128 bytes, else 0x81 + one length byte
  size_t header = n < 128 ? 2 : 3;
  if (header + n > size)
  {
    return 0;
  }
  der[0] = 0x30;
  if (n < 128)
  {
    der[1] = n;
  }
  else
  {
    der[1] = 0x81;
    der[2] = n;
  }
  memcpy(der + header, body, n);
  return header + n;
}

// Create a new EC P-256 key and a self-signed certificate for the given names/IPs
bool generateCertificate(const String &sans, const String &notBefore, const String &notAfter)
{
  // extendedKeyUsage = SEQUENCE { id-kp-serverAuth (1.3.6.1.5.5.7.3.1) }
  static const unsigned char extKeyUsage[] = {0x30, 0x0A, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};
  const char* personalization = "miniskidi-tls";
  String commonName = String(hostnamePrefix) + "-" + macSuffix;
  commonName.toLowerCase();
  String subject = "CN=" + commonName + ",O=MiniSkidi";
  unsigned char san[260];
  size_t sanLen = encodeSubjectAltNames(sans, san, sizeof(san));
  std::vector<unsigned char> certPem(1500);
  std::vector<unsigned char> keyPem(400);

  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_pk_context key;
  mbedtls_x509write_cert crt;
  mbedtls_mpi serial;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&crt);
  mbedtls_mpi_init(&serial);

  const char* step = "SubjectAltName encoding";
  int ret = sanLen > 0 ? 0 : -1;
  // Random numbers come from the ESP32 hardware RNG through mbedtls_entropy_func
  if (ret == 0) { step = "RNG seed"; ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)personalization, strlen(personalization)); }
  if (ret == 0) { step = "key setup"; ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)); }
  if (ret == 0) { step = "key generation"; ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg); }
  if (ret == 0) { step = "serial number"; ret = mbedtls_mpi_fill_random(&serial, 16, mbedtls_ctr_drbg_random, &drbg); }
  if (ret == 0)
  {
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key); // self-signed: issuer = subject
  }
  if (ret == 0) { step = "subject"; ret = mbedtls_x509write_crt_set_subject_name(&crt, subject.c_str()); }
  if (ret == 0) { step = "issuer"; ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject.c_str()); }
  if (ret == 0) { step = "serial"; ret = mbedtls_x509write_crt_set_serial(&crt, &serial); }
  if (ret == 0) { step = "validity"; ret = mbedtls_x509write_crt_set_validity(&crt, notBefore.c_str(), notAfter.c_str()); }
  if (ret == 0) { step = "basicConstraints"; ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1); }
  if (ret == 0) { step = "keyUsage"; ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE); }
  if (ret == 0) { step = "subjectAltName"; ret = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME, MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, san, sanLen); }
  if (ret == 0) { step = "extendedKeyUsage"; ret = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_EXTENDED_KEY_USAGE, MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE), 0, extKeyUsage, sizeof(extKeyUsage)); }
  if (ret == 0) { step = "certificate signing"; ret = mbedtls_x509write_crt_pem(&crt, certPem.data(), certPem.size(), mbedtls_ctr_drbg_random, &drbg); }
  if (ret == 0) { step = "key export"; ret = mbedtls_pk_write_key_pem(&key, keyPem.data(), keyPem.size()); }

  if (ret == 0)
  {
    tlsCertificatePem = String((const char *)certPem.data());
    tlsPrivateKeyPem = String((const char *)keyPem.data());
  }
  else
  {
    char err[100];
    mbedtls_strerror(ret, err, sizeof(err));
    LOGA("E", "TLS certificate generation failed at %s: -0x%04x %s", step, -ret, err);
  }

  mbedtls_mpi_free(&serial);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  return ret == 0;
}

// Load the stored certificate, or generate and store a new one when needed.
// Call after Wi-Fi is up (the client-mode IP is part of the certificate).
bool ensureCertificate()
{
  String sans = certificateSubjectAltNames();
  long buildDays = buildDateDays();

  Preferences prefs;
  prefs.begin("tls", false);
  tlsCertificatePem = prefs.getString("cert", "");
  tlsPrivateKeyPem = prefs.getString("key", "");
  String storedSans = prefs.getString("sans", "");
  String storedNotAfter = prefs.getString("notAfter", "");
  int storedValidityDays = prefs.getInt("days", 0);

  const char* reason = NULL;
  if (regenerateCertificate)
  {
    reason = "regenerateCertificate is set";
  }
  else if (tlsCertificatePem.length() == 0 || tlsPrivateKeyPem.length() == 0)
  {
    reason = "no stored certificate";
  }
  else if (storedSans != sans)
  {
    reason = "names/IPs changed";
  }
  else if (storedValidityDays != certificateValidityDays)
  {
    reason = "certificateValidityDays changed";
  }
  else if (storedNotAfter < x509Time(buildDays + 30)) // same format, so text comparison works
  {
    reason = "it expires within 30 days of the firmware build date";
  }

  if (reason == NULL)
  {
    LOGI("Using stored TLS certificate for %s, valid until %s", sans.c_str(), storedNotAfter.c_str());
    prefs.end();
    return true;
  }

  String notBefore = x509Time(buildDays);
  String notAfter = x509Time(buildDays + certificateValidityDays);
  // Always logged, whatever LOG_LEVEL is: the web page is unreachable until this is done, and
  // without a message a first boot can look like a hung device
  LOGA("I", "Generating a new TLS certificate because %s. This usually takes less than a second, "
            "at most about 5 seconds; the web page becomes available when it is done.", reason);
  LOGA("I", "Certificate names/IPs: %s", sans.c_str());
  unsigned long start = millis();
  bool ok = generateCertificate(sans, notBefore, notAfter);
  if (ok)
  {
    LOGA("I", "TLS certificate generated in %lu ms, valid %s - %s", millis() - start, notBefore.c_str(), notAfter.c_str());
    prefs.putString("cert", tlsCertificatePem);
    prefs.putString("key", tlsPrivateKeyPem);
    prefs.putString("sans", sans);
    prefs.putString("notAfter", notAfter);
    prefs.putInt("days", certificateValidityDays);
  }
  prefs.end();
  return ok;
}
