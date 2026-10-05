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
#include <ESPAsyncWebSrv.h> // by dvarrel
#include <iostream>
#include <sstream>

#if defined(ESP32)
#include <AsyncTCP.h> // by dvarrel
#include <WiFi.h>
#include <DNSServer.h> // part of the esp32 core
#include <ESPmDNS.h> // part of the esp32 core
#elif defined(ESP8266)
#include <ESPAsyncTCP.h> // by dvarrel
#endif
#include <esp_log.h>

// logging
// Log level printed to the serial console (115200 baud), from most to least output:
//   ARDUHAL_LOG_LEVEL_VERBOSE, ARDUHAL_LOG_LEVEL_DEBUG, ARDUHAL_LOG_LEVEL_INFO,
//   ARDUHAL_LOG_LEVEL_WARN, ARDUHAL_LOG_LEVEL_ERROR, ARDUHAL_LOG_LEVEL_NONE
// This controls the sketch's own messages. Logs from the esp32 core and libraries (WiFi, UART,
// ESP32Servo, ...) are compiled in only up to "Tools > Core Debug Level" (or DebugLevel in
// sketch.yaml), so set that to the same level. LOG_LEVEL NONE mutes them too.
#define LOG_LEVEL ARDUHAL_LOG_LEVEL_INFO

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
// "<apSsidPrefix>-XXXXXX" and is reachable as http://192.168.4.1 or http://miniskidi.local.
// XXXXXX is the end of the ESP32 MAC address, so several MiniSkidis don't clash.
const char* apSsidPrefix = "ProfBoots MiniSkidi OG";
const char* apPassphrase = "deadbeef"; // at least 8 characters
const int channel = 3; // wifi channel
const char* hostnamePrefix = "miniskidi";

// Client mode: set staSsid to join an existing Wi-Fi network instead. The hostname is then
// "miniskidi-xxxxxx" (e.g. http://miniskidi-a1b2c3.local) and the IP comes from the router.
// If the connection fails within staConnectTimeoutMs, the ESP32 falls back to access point mode.
const char* staSsid = ""; // empty = access point mode
const char* staPassphrase = "";
const unsigned long staConnectTimeoutMs = 15000;

// global variables

String macSuffix;      // last 3 bytes of the factory MAC address, e.g. "A1B2C3"
String deviceName;     // "<apSsidPrefix>-A1B2C3": access point SSID and mDNS instance name
String deviceHostname; // "miniskidi" in access point mode, "miniskidi-a1b2c3" in client mode
bool apMode = true;

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

AsyncWebServer server(80);
AsyncWebSocket wsCarInput("/CarInput");
DNSServer dnsServer; // resolves every hostname to the AP IP, so phones detect a captive portal


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

void handleRoot(AsyncWebServerRequest *request)
{
  LOGI("HTTP %s http://%s%s from %s", request->methodToString(), request->host().c_str(),
       request->url().c_str(), request->client()->remoteIP().toString().c_str());
  request->send_P(200, "text/html", htmlHomePage);
}

// Device details shown in the page footer, e.g.
// {"mode":"ap","hostname":"miniskidi.local","ip":"192.168.4.1","mac":"A0:B1:C2:A1:B2:C4"}
void handleInfo(AsyncWebServerRequest *request)
{
  LOGI("HTTP %s http://%s%s from %s", request->methodToString(), request->host().c_str(),
       request->url().c_str(), request->client()->remoteIP().toString().c_str());
  // IP and MAC of the interface in use: the access point MAC is the factory MAC + 1
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String mac = apMode ? WiFi.softAPmacAddress() : WiFi.macAddress();
  char json[160];
  snprintf(json, sizeof(json), "{\"mode\":\"%s\",\"hostname\":\"%s.local\",\"ip\":\"%s\",\"mac\":\"%s\"}",
           apMode ? "ap" : "client", deviceHostname.c_str(), ip.c_str(), mac.c_str());
  request->send(200, "application/json", json);
}

void handleNotFound(AsyncWebServerRequest *request)
{
  LOGI("HTTP %s http://%s%s from %s (no handler)", request->methodToString(),
       request->host().c_str(), request->url().c_str(), request->client()->remoteIP().toString().c_str());
  // Captive portal (access point mode only): redirect connectivity checks (e.g. Android
  // /generate_204) and any foreign host to the control page, so the phone opens it in its
  // sign-in browser, which always uses Wi-Fi.
  if (apMode && (request->host() != WiFi.softAPIP().toString() || request->url() != "/"))
  {
    request->redirect("http://" + WiFi.softAPIP().toString() + "/");
    return;
  }
  request->send(404, "text/plain", "File Not Found");
}

void onCarInputWebSocketEvent(AsyncWebSocket *server,
                              AsyncWebSocketClient *client,
                              AwsEventType type,
                              void *arg,
                              uint8_t *data,
                              size_t len)
{
  switch (type)
  {
    case WS_EVT_CONNECT:
      LOGI("WebSocket client #%u connected from %s", client->id(), client->remoteIP().toString().c_str());
      break;
    case WS_EVT_DISCONNECT:
      LOGI("WebSocket client #%u disconnected", client->id());
      moveCar(STOP);
      break;
    case WS_EVT_DATA:
      AwsFrameInfo *info;
      info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
      {
        std::string myData = "";
        myData.assign((char *)data, len);
        std::istringstream ss(myData);
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
      break;
    case WS_EVT_PONG:
      LOGV("WebSocket client #%u pong", client->id());
      break;
    case WS_EVT_ERROR:
      LOGE("WebSocket client #%u error", client->id());
      break;
    default:
      break;
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
  dnsServer.start(53, "*", IP);
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
    MDNS.addService("http", "tcp", 80);
    LOGI("mDNS responder started: %s.local", deviceHostname.c_str());
  }
  else
  {
    LOGE("mDNS responder failed to start");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/info", HTTP_GET, handleInfo);
  server.onNotFound(handleNotFound);

  wsCarInput.onEvent(onCarInputWebSocketEvent);
  server.addHandler(&wsCarInput);

  server.begin();
  LOGI("HTTP server started");

}

void loop()
{
  if (apMode)
  {
    dnsServer.processNextRequest();
  }
  wsCarInput.cleanupClients();
  // Without a delay loop() spins at 100% CPU on core 1 and never blocks. The WebSocket/HTTP
  // handling is not affected (the async_tcp task has a higher priority and preempts loop()),
  // but the idle task on core 1 never runs: it can't free the memory of deleted tasks and the
  // CPU never idles, which wastes battery and heats the chip. delay() blocks the task and
  // lets everything else run.
  // Why 2 ms: delay() sleeps in whole FreeRTOS ticks (1 ms here). delay(1) waits only until
  // the next tick boundary, which can be almost no time at all; delay(2) always sleeps at
  // least one full tick. Motor commands arrive via the async_tcp task, not loop(), so this
  // adds no control lag; it only delays the captive portal DNS replies by up to 2 ms.
  delay(2);
}
