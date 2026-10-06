/*
  Some tidbits to check

  -Install the esp32 boards manager into the arduino IDE"
  Programming Electronics Academy has a good tutorial: https://youtu.be/py91SMg_TeY?si=m1OWPBPlK-QHJ2Xx"
  -Select "ESP32 Dev Module" under tools>Board>ESP32 Arduino before uploading sketch.
  -The following include statements with comments "by -----" are libraries that can be installed
  directly inside the arduino IDE under Sketch>Include Library>Manage Libraries

  Control is over Bluetooth Low Energy only: the phone runs the web app in web/ (Chrome on
  Android, Web Bluetooth) and talks to the BLE GATT server in Ble.ino.
*/
#include <Arduino.h>

#include <ESP32Servo.h> // by Kevin Harrington
#include <NimBLEDevice.h> // NimBLE-Arduino by h2zero
#include <string>
#include <vector>

// The rest is part of the esp32 core
#include <Preferences.h>
#include <esp_log.h>

// logging
// Log level printed to the serial console (115200 baud), from most to least output:
//   ARDUHAL_LOG_LEVEL_VERBOSE, ARDUHAL_LOG_LEVEL_DEBUG, ARDUHAL_LOG_LEVEL_INFO,
//   ARDUHAL_LOG_LEVEL_WARN, ARDUHAL_LOG_LEVEL_ERROR, ARDUHAL_LOG_LEVEL_NONE
// This controls the sketch's own messages. Logs from the esp32 core and libraries (UART,
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
// Printed at every LOG_LEVEL, even NONE: for rare events the user must see
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

// Servo index for setServoTarget() (Servos.ino)
#define BUCKET_SERVO 0
#define AUX_SERVO 1

// global constants

// Bluetooth name "<bleNamePrefix>-XXXXXX", XXXXXX = end of the ESP32 MAC address, so several
// MiniSkidis don't clash. This is the name the phone shows in the Connect list.
const char* bleNamePrefix = "MiniSkidi";
// 6-digit passkey entered on the phone when pairing for the first time. Change it before use!
// After changing it, phones that paired before must "Forget" the MiniSkidi in Android's
// Bluetooth settings, and the MiniSkidi's own bonds are cleared from the app's Settings tab.
const uint32_t blePasskey = 123456;

// Servo angle limits, the same as the Classic tab sliders
const int servoMinAngle = 10;
const int servoMaxAngle = 180;

// global variables

String macSuffix;  // last 3 bytes of the factory MAC address, e.g. "A1B2C3"
String deviceName; // "<bleNamePrefix>-A1B2C3": the Bluetooth device name

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

// All motor and servo commands run on the loop() task: BLE writes are queued by Ble.ino and
// handled in loop(), so no locking is needed.
void rotateMotor(int motorNumber, int motorDirection)
{
  if (motorDirection == FORWARD)
  {
    setMotorSpeed(motorNumber, 255);
  }
  else if (motorDirection == BACKWARD)
  {
    setMotorSpeed(motorNumber, -255);
  }
  else
  {
    if (removeArmMomentum)
    {
      setMotorSpeed(ARM_MOTOR, 255);
      delay(10);
      setMotorSpeed(motorNumber, 0);
      delay(5);
      setMotorSpeed(ARM_MOTOR, 255);
      delay(10);
      removeArmMomentum = false;
    }
    setMotorSpeed(motorNumber, 0);
  }
}

// Runs a MoveCar command as is, without the horizontalScreen rotation of the Classic tab
void driveCar(int inputValue)
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
}

void moveCar(int inputValue)
{
  LOGD("Got value as %d", inputValue);
  if (horizontalScreen)
  {
    // The page is used sideways: the screen's up is the machine's left, and so on
    switch (inputValue)
    {
      case UP: inputValue = LEFT; break;
      case DOWN: inputValue = RIGHT; break;
      case LEFT: inputValue = DOWN; break;
      case RIGHT: inputValue = UP; break;
    }
  }
  driveCar(inputValue);
}

// The servos don't jump to a new angle: Servos.ino moves them there smoothly
void bucketTilt(float bucketServoValue)
{
  setServoTarget(BUCKET_SERVO, bucketServoValue);
}
void auxControl(float auxServoValue)
{
  setServoTarget(AUX_SERVO, auxServoValue);
}
void setLight(bool on)
{
  digitalWrite(lightPin1, on ? HIGH : LOW);
  digitalWrite(lightPin2, LOW);
  light = on;
  LOGI("Lights %s", on ? "on" : "off");
}
void lightControl()
{
  setLight(!light);
}

// Splits "a,b,c" into at most maxParts fields, returns the number found
int splitCommand(const std::string &message, std::string parts[], int maxParts)
{
  int count = 0;
  size_t start = 0;
  while (count < maxParts)
  {
    size_t comma = message.find(',', start);
    parts[count++] = message.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (comma == std::string::npos)
    {
      break;
    }
    start = comma + 1;
  }
  return count;
}

// One text command from the phone (cmd characteristic), "key,value[,value...]":
//   Classic tab:  MoveCar,n  Bucket,angle  AUX,angle  Light,0  Switch,0
//   Joystick tab: Joy,turn,drive,tilt,lift (each -100..100)  Attach,0..100
//   Settings tab: Set,name,value
//   Ping (keep-alive for the watchdog, Drive.ino)
void handleCarInput(const std::string &message)
{
  std::string parts[5];
  int count = splitCommand(message, parts, 5);
  const std::string &key = parts[0];
  int valueInt = count > 1 ? atoi(parts[1].c_str()) : 0;
  if (key != "Ping" && key != "Joy")
  {
    LOGD("Key [%s] Value[%s]", key.c_str(), count > 1 ? parts[1].c_str() : "");
  }
  if (key == "Ping")
  {
    return;
  }
  else if (key == "MoveCar")
  {
    joystickReset(); // the Classic buttons take over from the sticks
    moveCar(valueInt);
  }
  else if (key == "Joy" && count == 5)
  {
    joystickInput(valueInt, atoi(parts[2].c_str()), atoi(parts[3].c_str()), atoi(parts[4].c_str()));
  }
  else if (key == "AUX")
  {
    auxControl(valueInt);
  }
  else if (key == "Attach")
  {
    auxControl(servoMinAngle + constrain(valueInt, 0, 100) * (servoMaxAngle - servoMinAngle) / 100.0);
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
  else if (key == "Set" && count == 3)
  {
    changeSetting(parts[1], atoi(parts[2].c_str()));
  }
  else
  {
    LOGW("Unknown command [%s]", message.c_str());
  }
}

void setUpPinModes()
{
  // Let ESP32Servo's PWM channel allocator use all four LEDC timers. The servos (50 Hz) and
  // the motor PWM (Drive.ino, motorPwmFreq) then get separate timers and never share a channel.
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  setUpServos(140, 150); // start angles: bucket, AUX
  setUpMotorPwm();
  moveCar(STOP);

  pinMode(lightPin1, OUTPUT);
  pinMode(lightPin2, OUTPUT);
}

// Last 3 bytes of the factory MAC address as hex, e.g. "A1B2C3"
String getMacSuffix()
{
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(suffix);
}

void setup(void)
{
  Serial.begin(115200);
  // Route core and library logs to the serial console, or mute them for LOG_LEVEL NONE
  Serial.setDebugOutput(LOG_LEVEL > ARDUHAL_LOG_LEVEL_NONE);
  // ESP-IDF components (Bluetooth controller, NVS, ...)
  esp_log_level_set("*", (esp_log_level_t)LOG_LEVEL);
  setUpPinModes();
  loadSettings();

  macSuffix = getMacSuffix();
  deviceName = String(bleNamePrefix) + "-" + macSuffix;
  bleSetup();
  LOGI("Heap after start: free %u, largest block %u bytes", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void loop()
{
  bleLoop();      // runs queued phone commands, sends the status notification
  driveLoop();    // bucket tilt rate, signal-lost watchdog
  servoLoop();    // moves the servos smoothly towards their target angles

  static unsigned long lastHeapLog = 0;
  if (millis() - lastHeapLog >= 5000)
  {
    lastHeapLog = millis();
    LOGV("Heap: free %u, largest block %u, lowest free since boot %u bytes",
         ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
  }

  // Without a delay loop() spins at 100% CPU on core 1 and never blocks: the idle task on
  // core 1 never runs, so it can't free the memory of deleted tasks and the CPU never idles,
  // which wastes battery and heats the chip. delay() blocks the task and lets everything else
  // run.
  // Why 2 ms: delay() sleeps in whole FreeRTOS ticks (1 ms here). delay(1) waits only until
  // the next tick boundary, which can be almost no time at all; delay(2) always sleeps at
  // least one full tick. Phone commands wait in the queue for at most these 2 ms, far less
  // than the BLE connection interval.
  delay(2);
}
