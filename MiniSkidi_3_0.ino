/* 
  Some tidbits to check

  -Install the esp32 boards manager into the arduino IDE"
  Programming Electronics Academy has a good tutorial: https://youtu.be/py91SMg_TeY?si=m1OWPBPlK-QHJ2Xx"
  -Select "ESP32 Dev Module" under tools>Board>ESP32 Arduino before uploading sketch.
  -The following include statements with comments "by -----" are libraries that can be installed
  directly inside the arduino IDE under Sketch>Include Library>Manage Libraries
*/
#include <Arduino.h>

#include <ESP32Servo.h> // by Kevin Harrington
#include <iostream>
#include <sstream>
#include <vector>

// The rest is part of the esp32 core
#include <WiFi.h>
#include <WiFiUdp.h> // captive portal DNS (CaptivePortalDns.ino)
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_log.h>
#include <esp_https_server.h> // ESP-IDF HTTP(S) server with WebSocket support
#include <lwip/sockets.h>

// Used by Certificate.ino
#include <mbedtls/asn1.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

// logging
// Log level printed to the serial console (115200 baud), from most to least output:
//   ARDUHAL_LOG_LEVEL_VERBOSE, ARDUHAL_LOG_LEVEL_DEBUG, ARDUHAL_LOG_LEVEL_INFO,
//   ARDUHAL_LOG_LEVEL_WARN, ARDUHAL_LOG_LEVEL_ERROR, ARDUHAL_LOG_LEVEL_NONE
// This controls the sketch's own messages. Logs from the esp32 core and libraries (WiFi, UART,
// ESP32Servo, ...) are compiled in only up to "Tools > Core Debug Level" (or DebugLevel in
// sketch.yaml), so set that to the same level. LOG_LEVEL NONE mutes them too.
#define LOG_LEVEL ARDUHAL_LOG_LEVEL_DEBUG

#if CORE_DEBUG_LEVEL < LOG_LEVEL
#warning "Core Debug Level is lower than LOG_LEVEL: core and library logs will be missing. Raise Tools > Core Debug Level."
#elif CORE_DEBUG_LEVEL > LOG_LEVEL && LOG_LEVEL > ARDUHAL_LOG_LEVEL_NONE
#warning "Core Debug Level is higher than LOG_LEVEL: core and library logs will be more detailed than LOG_LEVEL."
#endif

// Same format as the core logs: [millis][level] message
#define LOG_AT(level, letter, fmt, ...) \
  do { if (LOG_LEVEL >= (level)) Serial.printf("[%6lu][" letter "][MiniSkidi] " fmt "\n", millis(), ##__VA_ARGS__); } while (0)
#define LOGE(fmt, ...) LOG_AT(ARDUHAL_LOG_LEVEL_ERROR, "E", fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_AT(ARDUHAL_LOG_LEVEL_WARN, "W", fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) LOG_AT(ARDUHAL_LOG_LEVEL_INFO, "I", fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) LOG_AT(ARDUHAL_LOG_LEVEL_DEBUG, "D", fmt, ##__VA_ARGS__)
#define LOGV(fmt, ...) LOG_AT(ARDUHAL_LOG_LEVEL_VERBOSE, "V", fmt, ##__VA_ARGS__)
// Printed at every LOG_LEVEL, even NONE: for rare events the user must see, e.g. the long
// pause while the TLS certificate is generated on first boot
#define LOGA(letter, fmt, ...) LOG_AT(ARDUHAL_LOG_LEVEL_NONE, letter, fmt, ##__VA_ARGS__)


// defines
#define bucketServoPin  23
#define auxServoPin 22
#define lightPin1 18
#define lightPin2 5
#define UP 1
#define DOWN 2
#define LEFT 3
#define RIGHT 4
#define ARMUP 5
#define ARMDOWN 6
#define STOP 0

#define RIGHT_MOTOR 1
#define LEFT_MOTOR 0
#define ARM_MOTOR 2

#define FORWARD 1
#define BACKWARD -1

// global constants

extern const char* htmlHomePage PROGMEM;
// Access point mode (default): the ESP32 creates its own Wi-Fi network named
// "<apSsidPrefix>-XXXXXX" and is reachable as https://192.168.4.1 or https://miniskidi.local.
// XXXXXX is the end of the ESP32 MAC address, so several MiniSkidis don't clash.
const char* apSsidPrefix = "ProfBoots MiniSkidi OG";
const char* apPassphrase = "deadbeef"; // at least 8 characters
const int channel = 3; // wifi channel
const char* hostnamePrefix = "miniskidi";
const char* apDefaultIP = "192.168.4.1"; // IP of the ESP32 in access point mode

// Client mode: set staSsid to join an existing Wi-Fi network instead. The hostname is then
// "miniskidi-xxxxxx" (e.g. https://miniskidi-a1b2c3.local) and the IP comes from the router.
// If the connection fails within staConnectTimeoutMs, the ESP32 falls back to access point mode.
const char* staSsid = ""; // empty = access point mode
const char* staPassphrase = "";
const unsigned long staConnectTimeoutMs = 15000;

// global variables

String macSuffix;      // last 3 bytes of the factory MAC address, e.g. "A1B2C3"
String deviceName;     // "<apSsidPrefix>-A1B2C3": access point SSID and mDNS instance name
String deviceHostname; // "miniskidi" in access point mode, "miniskidi-a1b2c3" in client mode
bool apMode = true;
extern String tlsCertificatePem; // defined in Certificate.ino
extern String tlsPrivateKeyPem;

Servo bucketServo;
Servo auxServo;

bool horizontalScreen;//When screen orientation is locked vertically this rotates the D-Pad controls so that forward would now be left.
bool removeArmMomentum = false;
bool light = false;

struct MOTOR_PINS
{
  int pinIN1;
  int pinIN2;
};

std::vector<MOTOR_PINS> motorPins =
{
  {25, 26},  //RIGHT_MOTOR Pins (IN1, IN2)
  {33, 32},  //LEFT_MOTOR  Pins
  {21, 19}, //ARM_MOTOR pins
};

// Web servers (ESP-IDF esp_http_server): the control page and WebSocket are served over HTTPS
// on port 443; plain HTTP on port 80 only redirects to HTTPS and answers captive portal checks.
// All handlers of one server run on that server's own task, one at a time.
httpd_handle_t httpsServer = NULL;
httpd_handle_t httpServer = NULL;
const int maxWebSocketClients = 4;
int webSocketFds[maxWebSocketClients] = {-1, -1, -1, -1}; // sockets of connected /CarInput clients


void rotateMotor(int motorNumber, int motorDirection)
{
  if (motorDirection == FORWARD)
  {
    digitalWrite(motorPins[motorNumber].pinIN1, HIGH);
    digitalWrite(motorPins[motorNumber].pinIN2, LOW);
  }
  else if (motorDirection == BACKWARD)
  {
    digitalWrite(motorPins[motorNumber].pinIN1, LOW);
    digitalWrite(motorPins[motorNumber].pinIN2, HIGH);
  }
  else
  {
    if (removeArmMomentum)
    {
      digitalWrite(motorPins[ARM_MOTOR].pinIN1, HIGH);
      digitalWrite(motorPins[ARM_MOTOR].pinIN2, LOW);
      delay(10);
      digitalWrite(motorPins[motorNumber].pinIN1, LOW);
      digitalWrite(motorPins[motorNumber].pinIN2, LOW);
      delay(5);
      digitalWrite(motorPins[ARM_MOTOR].pinIN1, HIGH);
      digitalWrite(motorPins[ARM_MOTOR].pinIN2, LOW);
      delay(10);
      removeArmMomentum = false;
    }
    digitalWrite(motorPins[motorNumber].pinIN1, LOW);
    digitalWrite(motorPins[motorNumber].pinIN2, LOW);
  }
}

void moveCar(int inputValue)
{
  LOGD("Got value as %d", inputValue);
  if (!(horizontalScreen))
  {
    switch (inputValue)
    {

      case UP:
        rotateMotor(RIGHT_MOTOR, FORWARD);
        rotateMotor(LEFT_MOTOR, FORWARD);
        break;

      case DOWN:
        rotateMotor(RIGHT_MOTOR, BACKWARD);
        rotateMotor(LEFT_MOTOR, BACKWARD);
        break;

      case LEFT:
        rotateMotor(RIGHT_MOTOR, BACKWARD);
        rotateMotor(LEFT_MOTOR, FORWARD);
        break;

      case RIGHT:
        rotateMotor(RIGHT_MOTOR, FORWARD);
        rotateMotor(LEFT_MOTOR, BACKWARD);
        break;

      case STOP:
        rotateMotor(ARM_MOTOR, STOP);
        rotateMotor(RIGHT_MOTOR, STOP);
        rotateMotor(LEFT_MOTOR, STOP);
        break;

      case ARMUP:
        rotateMotor(ARM_MOTOR, FORWARD);
        break;

      case ARMDOWN:
        rotateMotor(ARM_MOTOR, BACKWARD);
        removeArmMomentum = true;
        break;

      default:
        rotateMotor(ARM_MOTOR, STOP);
        rotateMotor(RIGHT_MOTOR, STOP);
        rotateMotor(LEFT_MOTOR, STOP);
        break;
    }
  } else {
    switch (inputValue)
    {
      case UP:
        rotateMotor(RIGHT_MOTOR, BACKWARD);
        rotateMotor(LEFT_MOTOR, FORWARD);
        break;

      case DOWN:
        rotateMotor(RIGHT_MOTOR, FORWARD);
        rotateMotor(LEFT_MOTOR, BACKWARD);
        break;

      case LEFT:
        rotateMotor(RIGHT_MOTOR, BACKWARD);
        rotateMotor(LEFT_MOTOR, BACKWARD);
        break;

      case RIGHT:
        rotateMotor(RIGHT_MOTOR, FORWARD);
        rotateMotor(LEFT_MOTOR, FORWARD);
        break;

      case STOP:
        rotateMotor(ARM_MOTOR, STOP);
        rotateMotor(RIGHT_MOTOR, STOP);
        rotateMotor(LEFT_MOTOR, STOP);
        break;

      case ARMUP:
        rotateMotor(ARM_MOTOR, FORWARD);
        break;

      case ARMDOWN:
        rotateMotor(ARM_MOTOR, BACKWARD);
        removeArmMomentum = true;
        break;

      default:
        rotateMotor(ARM_MOTOR, STOP);
        rotateMotor(RIGHT_MOTOR, STOP);
        rotateMotor(LEFT_MOTOR, STOP);
        break;
    }
  }
}

void bucketTilt(int bucketServoValue)
{
  bucketServo.write(bucketServoValue);
}
void auxControl(int auxServoValue)
{
  auxServo.write(auxServoValue);
}
void lightControl()
{
  if (!light)
  {
    digitalWrite(lightPin1, HIGH);
    digitalWrite(lightPin2, LOW);
    light = true;
    LOGI("Lights on");
  }
  else
  {
    digitalWrite(lightPin1, LOW);
    digitalWrite(lightPin2, LOW);
    light = false;
    LOGI("Lights off");
  }
}

// Client IP of a request. The server listens on IPv6 and IPv4, so IPv4 clients show up as
// IPv4-mapped IPv6 addresses (::ffff:a.b.c.d)
String clientIP(httpd_req_t *req)
{
  struct sockaddr_in6 addr;
  socklen_t len = sizeof(addr);
  if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0)
  {
    return "?";
  }
  if (addr.sin6_family == AF_INET)
  {
    return IPAddress(((struct sockaddr_in *)&addr)->sin_addr.s_addr).toString();
  }
  return IPAddress(addr.sin6_addr.un.u32_addr[3]).toString();
}

String requestHeader(httpd_req_t *req, const char *name)
{
  char value[64];
  if (httpd_req_get_hdr_value_str(req, name, value, sizeof(value)) != ESP_OK)
  {
    return "";
  }
  return String(value);
}

void logRequest(httpd_req_t *req, const char *scheme, const String &note)
{
  LOGI("HTTP %s %s://%s%s from %s%s", http_method_str((enum http_method)req->method), scheme,
       requestHeader(req, "Host").c_str(), req->uri, clientIP(req).c_str(), note.c_str());
}

esp_err_t handleRoot(httpd_req_t *req)
{
  logRequest(req, "https", "");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, htmlHomePage, HTTPD_RESP_USE_STRLEN);
}

// Device details shown in the page footer, e.g.
// {"mode":"ap","hostname":"miniskidi.local","ip":"192.168.4.1","mac":"A0:B1:C2:A1:B2:C4"}
esp_err_t handleInfo(httpd_req_t *req)
{
  logRequest(req, "https", "");
  // IP and MAC of the interface in use: the access point MAC is the factory MAC + 1
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String mac = apMode ? WiFi.softAPmacAddress() : WiFi.macAddress();
  char json[160];
  snprintf(json, sizeof(json), "{\"mode\":\"%s\",\"hostname\":\"%s.local\",\"ip\":\"%s\",\"mac\":\"%s\"}",
           apMode ? "ap" : "client", deviceHostname.c_str(), ip.c_str(), mac.c_str());
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handleNotFound(httpd_req_t *req, httpd_err_code_t error)
{
  logRequest(req, "https", " (not found)");
  return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File Not Found");
}

// Plain HTTP (port 80): everything is redirected to HTTPS.
// Access point mode: this is also the captive portal. Connectivity checks (e.g. Android
// /generate_204) and any other host are sent to the control page, so the phone shows
// "Sign in to Wi-Fi network". Client mode: same host and path, over HTTPS.
esp_err_t handleHttpRedirect(httpd_req_t *req)
{
  String host = requestHeader(req, "Host");
  if (host.length() == 0)
  {
    host = WiFi.localIP().toString();
  }
  String location = apMode ? String("https://") + apDefaultIP + "/" : "https://" + host + req->uri;
  logRequest(req, "http", " -> " + location);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location.c_str());
  return httpd_resp_send(req, NULL, 0);
}

void handleCarInput(const std::string &message)
{
  std::istringstream ss(message);
  std::string key, value;
  std::getline(ss, key, ',');
  std::getline(ss, value, ',');
  LOGD("Key [%s] Value[%s]", key.c_str(), value.c_str());
  int valueInt = atoi(value.c_str());
  if (key == "MoveCar")
  {
    moveCar(valueInt);
  }
  else if (key == "AUX")
  {
    auxControl(valueInt);
  }
  else if (key == "Bucket")
  {
    bucketTilt(valueInt);
  }
  else if (key == "Light")
  {
    lightControl();
  }
  else if (key == "Switch")
  {
    if (!(horizontalScreen))
    {
      horizontalScreen = true;
    }
    else {
      horizontalScreen = false;
    }
  }
}

void rememberWebSocket(int fd)
{
  for (int i = 0; i < maxWebSocketClients; i++)
  {
    if (webSocketFds[i] == -1)
    {
      webSocketFds[i] = fd;
      return;
    }
  }
}

bool forgetWebSocket(int fd)
{
  for (int i = 0; i < maxWebSocketClients; i++)
  {
    if (webSocketFds[i] == fd)
    {
      webSocketFds[i] = -1;
      return true;
    }
  }
  return false;
}

// /CarInput WebSocket: called once for the handshake, then for every received data frame
// (ping/pong/close frames are answered by the server itself)
esp_err_t handleCarInputWebSocket(httpd_req_t *req)
{
  int fd = httpd_req_to_sockfd(req);
  if (req->method == HTTP_GET)
  {
    rememberWebSocket(fd);
    LOGI("WebSocket client connected from %s (socket %d)", clientIP(req).c_str(), fd);
    LOGD("Free heap: %u bytes", ESP.getFreeHeap());
    return ESP_OK;
  }

  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  // With max_len 0 only the frame header is read, to get the length
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK)
  {
    LOGE("WebSocket receive failed (socket %d): %s", fd, esp_err_to_name(err));
    return err;
  }
  char buf[129];
  if (frame.len >= sizeof(buf))
  {
    LOGW("WebSocket message too long (%u bytes), closing socket %d", frame.len, fd);
    return ESP_FAIL;
  }
  frame.payload = (uint8_t *)buf;
  // The payload must always be read, even for frames that are ignored below
  err = httpd_ws_recv_frame(req, &frame, frame.len);
  if (err != ESP_OK)
  {
    LOGE("WebSocket receive failed (socket %d): %s", fd, esp_err_to_name(err));
    return err;
  }
  if (frame.type != HTTPD_WS_TYPE_TEXT || !frame.final)
  {
    LOGW("Ignoring WebSocket frame type %d (final %d) on socket %d", frame.type, frame.final, fd);
    return ESP_OK;
  }
  handleCarInput(std::string(buf, frame.len));
  return ESP_OK;
}

// Called by the HTTPS server for every closed connection, before it closes the socket itself.
// Stopping the motors here is a safety feature: the machine must not keep driving when the
// phone disconnects (tab closed, out of Wi-Fi range, ...).
void onHttpsSocketClose(httpd_handle_t hd, int sockfd)
{
  if (forgetWebSocket(sockfd))
  {
    LOGI("WebSocket client disconnected (socket %d), stopping motors", sockfd);
    moveCar(STOP);
    LOGD("Free heap: %u bytes", ESP.getFreeHeap());
  }
}

// Backup for onHttpsSocketClose, queued from loop() to run on the HTTPS server task (so it
// never runs at the same time as the handlers). Stops the motors if a WebSocket client is gone.
void checkWebSocketClients(void *arg)
{
  for (int i = 0; i < maxWebSocketClients; i++)
  {
    if (webSocketFds[i] != -1 && httpd_ws_get_fd_info(httpsServer, webSocketFds[i]) != HTTPD_WS_CLIENT_WEBSOCKET)
    {
      LOGW("WebSocket client (socket %d) is gone, stopping motors", webSocketFds[i]);
      webSocketFds[i] = -1;
      moveCar(STOP);
    }
  }
}

void setUpPinModes()
{

  for (int i = 0; i < motorPins.size(); i++)
  {
    pinMode(motorPins[i].pinIN1, OUTPUT);
    pinMode(motorPins[i].pinIN2, OUTPUT);
  }
  moveCar(STOP);
  bucketServo.attach(bucketServoPin);
  auxServo.attach(auxServoPin);
  auxControl(150);
  bucketTilt(140);

  pinMode(lightPin1, OUTPUT);
  pinMode(lightPin2, OUTPUT);
}

// Last 3 bytes of the factory (station) MAC address as hex, e.g. "A1B2C3". The access point
// MAC is the factory MAC + 1, so the AP's BSSID may end in a different last digit.
String getMacSuffix()
{
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(suffix);
}

// Join the staSsid network as a client. Returns false if not connected within the timeout.
bool startStation()
{
  deviceHostname = String(hostnamePrefix) + "-" + macSuffix;
  deviceHostname.toLowerCase();
  // The core applies the hostname when WiFi.mode() starts the station, so set it first
  WiFi.setHostname(deviceHostname.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(staSsid, staPassphrase);
  LOGI("Connecting to Wi-Fi network \"%s\" as %s", staSsid, deviceHostname.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < staConnectTimeoutMs)
  {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    LOGW("Could not connect to \"%s\" (status %d), falling back to access point mode",
         staSsid, WiFi.status());
    WiFi.disconnect(true);
    return false;
  }
  // After this, the core reconnects automatically if the connection drops
  LOGI("Connected to \"%s\", IP address: %s", staSsid, WiFi.localIP().toString().c_str());
  return true;
}

// Create the "<apSsidPrefix>-XXXXXX" network with a captive portal
void startAccessPoint()
{
  deviceHostname = hostnamePrefix;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(deviceName.c_str(), apPassphrase, channel);
  WiFi.softAPsetHostname(deviceHostname.c_str());
  IPAddress IP = WiFi.softAPIP();
  LOGI("Access point \"%s\" started, IP address: %s", deviceName.c_str(), IP.toString().c_str());
  captiveDnsStart(IP);
}

// Start HTTPS on port 443 (page, /info, /CarInput WebSocket) and HTTP on port 80 (redirects)
bool startWebServers()
{
  httpd_ssl_config_t https = HTTPD_SSL_CONFIG_DEFAULT();
  // In this ESP-IDF version "cacert" is the server's own certificate; lengths include the NUL
  https.cacert_pem = (const uint8_t *)tlsCertificatePem.c_str();
  https.cacert_len = tlsCertificatePem.length() + 1;
  https.prvtkey_pem = (const uint8_t *)tlsPrivateKeyPem.c_str();
  https.prvtkey_len = tlsPrivateKeyPem.length() + 1;
  // Each TLS connection needs about 35-40 KB of heap (fixed 16 KB buffers per direction), so
  // the number of connections is capped. When all are in use, the least recently used one is
  // closed to make room for a new one. 3 is enough for one phone: the page and /info share a
  // keep-alive connection, the WebSocket has its own, and one is spare.
  https.httpd.max_open_sockets = 3;
  https.httpd.lru_purge_enable = true;
  https.httpd.close_fn = onHttpsSocketClose;
  esp_err_t err = httpd_ssl_start(&httpsServer, &https);
  if (err != ESP_OK)
  {
    LOGE("HTTPS server failed to start: %s", esp_err_to_name(err));
    return false;
  }

  httpd_uri_t root = {};
  root.uri = "/";
  root.method = HTTP_GET;
  root.handler = handleRoot;
  httpd_register_uri_handler(httpsServer, &root);

  httpd_uri_t info = {};
  info.uri = "/info";
  info.method = HTTP_GET;
  info.handler = handleInfo;
  httpd_register_uri_handler(httpsServer, &info);

  httpd_uri_t carInput = {};
  carInput.uri = "/CarInput";
  carInput.method = HTTP_GET;
  carInput.handler = handleCarInputWebSocket;
  carInput.is_websocket = true;
  httpd_register_uri_handler(httpsServer, &carInput);

  httpd_register_err_handler(httpsServer, HTTPD_404_NOT_FOUND, handleNotFound);
  LOGI("HTTPS server started on port %d", https.port_secure);

  httpd_config_t http = HTTPD_DEFAULT_CONFIG();
  http.server_port = 80;
  http.ctrl_port = 32769; // the HTTPS server uses the default 32768
  http.max_open_sockets = 3;
  http.lru_purge_enable = true;
  http.uri_match_fn = httpd_uri_match_wildcard;
  err = httpd_start(&httpServer, &http);
  if (err != ESP_OK)
  {
    LOGE("HTTP redirect server failed to start: %s", esp_err_to_name(err));
    return true; // HTTPS still works
  }
  httpd_uri_t redirect = {};
  redirect.uri = "/*";
  redirect.method = HTTP_GET;
  redirect.handler = handleHttpRedirect;
  httpd_register_uri_handler(httpServer, &redirect);
  LOGI("HTTP server started on port 80 (redirects to HTTPS)");
  LOGI("Heap after start: free %u, largest block %u bytes", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

void setup(void)
{
  Serial.begin(115200);
  // Route core and library logs to the serial console, or mute them for LOG_LEVEL NONE
  Serial.setDebugOutput(LOG_LEVEL > ARDUHAL_LOG_LEVEL_NONE);
  // ESP-IDF components (WiFi driver, TCP/IP, ...); the precompiled IDF only goes up to ERROR
  esp_log_level_set("*", (esp_log_level_t)LOG_LEVEL);
  setUpPinModes();

  macSuffix = getMacSuffix();
  deviceName = String(apSsidPrefix) + "-" + macSuffix;
  apMode = !(strlen(staSsid) > 0 && startStation());
  if (apMode)
  {
    startAccessPoint();
  }

  // Ping by IP (ICMP echo) is answered by the network stack without extra code.
  // mDNS adds a name: "ping miniskidi.local" (miniskidi-xxxxxx.local in client mode), and
  // advertises the web server so network discovery apps (Bonjour/Zeroconf browsers,
  // avahi-browse) list it.
  if (MDNS.begin(deviceHostname.c_str()))
  {
    MDNS.setInstanceName(deviceName);
    MDNS.addService("https", "tcp", 443);
    MDNS.addService("http", "tcp", 80);
    LOGI("mDNS responder started: %s.local", deviceHostname.c_str());
  }
  else
  {
    LOGE("mDNS responder failed to start");
  }

  // After Wi-Fi: the certificate includes the client-mode IP. If generating a new one fails,
  // a previously stored certificate is still used.
  if (!ensureCertificate() && tlsCertificatePem.length() == 0)
  {
    LOGE("No TLS certificate, web server not started");
    return;
  }
  startWebServers();
}

void loop()
{
  if (apMode)
  {
    captiveDnsLoop();
  }

  static unsigned long lastWebSocketCheck = 0;
  if (httpsServer != NULL && millis() - lastWebSocketCheck >= 200)
  {
    lastWebSocketCheck = millis();
    httpd_queue_work(httpsServer, checkWebSocketClients, NULL);
  }

  // Heap status: each new TLS connection needs two contiguous ~16.7 KB buffers, so if
  // "largest block" drops below ~17 KB or "free" below ~40 KB, new HTTPS connections fail
  static unsigned long lastHeapLog = 0;
  if (millis() - lastHeapLog >= 5000)
  {
    lastHeapLog = millis();
    LOGD("Heap: free %u, largest block %u, lowest free since boot %u bytes",
         ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
  }

  // Without a delay loop() spins at 100% CPU on core 1 and never blocks. The web servers are
  // not affected (their tasks have a higher priority, tskIDLE_PRIORITY+5, and preempt loop()),
  // but the idle task on core 1 never runs: it can't free the memory of deleted tasks and the
  // CPU never idles, which wastes battery and heats the chip. delay() blocks the task and
  // lets everything else run.
  // Why 2 ms: delay() sleeps in whole FreeRTOS ticks (1 ms here). delay(1) waits only until
  // the next tick boundary, which can be almost no time at all; delay(2) always sleeps at
  // least one full tick. Motor commands arrive via the HTTPS server task, not loop(), so this
  // adds no control lag; it only delays the captive portal DNS replies by up to 2 ms.
  delay(2);
}
