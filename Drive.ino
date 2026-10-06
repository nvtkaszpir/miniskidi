// Motor speed (PWM), joystick driving, the signal-lost watchdog and the settings.
//
// The tracks and the arm are DC motors on H-bridge inputs (IN1, IN2). PWM on one input with
// the other one LOW sets the speed: IN1 = duty, IN2 = 0 forward; IN1 = 0, IN2 = duty backward;
// both 0 stops (coast), the same as the old digitalWrite LOW/LOW.

// PWM frequency and resolution for the motor pins
const int motorPwmFreq = 1000; // Hz
const int motorPwmBits = 8;    // duty 0..255; 255 is fully on
// Lowest duty a motor still turns at. Small joystick movements map to this, not to 0..90, where
// the motors only hum. Raise it if the machine doesn't move at small stick deflections.
const int motorMinDuty = 90;

// Bucket tilt speed at full right-stick deflection, degrees per second
const float bucketMaxRate = 90.0;

// Signal-lost watchdog: the phone sends something (at least "Ping") every 150 ms. If nothing
// arrives for this long while anything moves, everything is stopped.
const unsigned long watchdogTimeoutMs = 500;

ESP32PWM motorPwm[3][2]; // [motor][IN1, IN2]
int motorSpeed[3] = {0, 0, 0}; // last speed per motor, -255..255

void setUpMotorPwm()
{
  for (int i = 0; i < (int)motorPins.size(); i++)
  {
    motorPwm[i][0].attachPin(motorPins[i].pinIN1, motorPwmFreq, motorPwmBits);
    motorPwm[i][1].attachPin(motorPins[i].pinIN2, motorPwmFreq, motorPwmBits);
    motorPwm[i][0].write(0);
    motorPwm[i][1].write(0);
  }
}

// speed -255 (full backward) .. 0 (stop) .. 255 (full forward)
void setMotorSpeed(int motorNumber, int speed)
{
  speed = constrain(speed, -255, 255);
  motorSpeed[motorNumber] = speed;
  int duty = speed == 0 ? 0 : map(abs(speed), 1, 255, motorMinDuty, 255);
  motorPwm[motorNumber][0].write(speed > 0 ? duty : 0);
  motorPwm[motorNumber][1].write(speed < 0 ? duty : 0);
}

// Joystick tab state, -100..100 each. Dead zone, swaps and inversions are applied by the app.
int joyTurn = 0, joyDrive = 0, joyTilt = 0, joyLift = 0;
float bucketAngle = 0;          // rate-controlled bucket angle, kept as float for slow rates
int bucketAngleWritten = 0;     // last whole angle sent to the servo by the rate control
unsigned long lastBucketUpdate = 0;
unsigned long lastCommandMillis = 0; // last command of any kind from the phone

int percentToSpeed(int percent)
{
  return constrain(percent, -100, 100) * 255 / 100;
}

void joystickInput(int turn, int drive, int tilt, int lift)
{
  turn = constrain(turn, -100, 100);
  drive = constrain(drive, -100, 100);
  tilt = constrain(tilt, -100, 100);
  lift = constrain(lift, -100, 100);

  if (turn != joyTurn || drive != joyDrive)
  {
    // Tank mixing, same directions as the Classic D-pad: turning left (turn < 0) runs the left
    // track forward and the right track backward, like the LEFT case in driveCar()
    int left = constrain(drive - turn, -100, 100);
    int right = constrain(drive + turn, -100, 100);
    setMotorSpeed(LEFT_MOTOR, percentToSpeed(left));
    setMotorSpeed(RIGHT_MOTOR, percentToSpeed(right));
    LOGD("Joystick tracks: turn %d drive %d -> left %d right %d", turn, drive, left, right);
  }

  if (lift != joyLift)
  {
    if (lift == 0)
    {
      // Arm stops after lowering: brake pulse against the arm's momentum, as in the Classic tab
      removeArmMomentum = joyLift < 0;
      rotateMotor(ARM_MOTOR, STOP);
    }
    else
    {
      setMotorSpeed(ARM_MOTOR, percentToSpeed(lift)); // lift > 0 = arm up = ARMUP direction
    }
    LOGD("Joystick lift %d", lift);
  }

  if (tilt != 0 && joyTilt == 0)
  {
    // Start of a tilt movement: continue from where the bucket is (the Classic slider may
    // have moved it)
    bucketAngle = bucketAngleWritten = bucketServo.read();
    lastBucketUpdate = millis();
  }

  joyTurn = turn;
  joyDrive = drive;
  joyTilt = tilt;
  joyLift = lift;
}

// Forget the joystick state without moving anything (e.g. when the Classic buttons are used)
void joystickReset()
{
  joyTurn = joyDrive = joyTilt = joyLift = 0;
}

// Called for every command from the phone, feeds the watchdog
void noteCommandReceived()
{
  lastCommandMillis = millis();
}

bool anythingMoving()
{
  return motorSpeed[0] != 0 || motorSpeed[1] != 0 || motorSpeed[2] != 0 || joyTilt != 0;
}

// Stops all motors and the bucket tilt movement. Safety: called when the phone disconnects and
// by the watchdog when no command arrives in time.
void stopEverything(const char *reason)
{
  if (anythingMoving())
  {
    LOGW("Stopping all motors: %s", reason);
  }
  joystickReset();
  moveCar(STOP);
}

void driveLoop()
{
  // Bucket tilt: the right stick sets a speed, the angle follows. Releasing the stick (tilt 0)
  // leaves the bucket where it is.
  if (joyTilt != 0)
  {
    unsigned long now = millis();
    float dt = (now - lastBucketUpdate) / 1000.0;
    lastBucketUpdate = now;
    bucketAngle = constrain(bucketAngle + joyTilt / 100.0 * bucketMaxRate * dt,
                            (float)servoMinAngle, (float)servoMaxAngle);
    // Compared with the last written angle, not bucketServo.read(): read() converts back from
    // the pulse width and may differ by a degree, which would rewrite the servo every loop
    int angle = (int)(bucketAngle + 0.5);
    if (angle != bucketAngleWritten)
    {
      bucketAngleWritten = angle;
      bucketTilt(angle);
    }
  }

  if (anythingMoving() && millis() - lastCommandMillis > watchdogTimeoutMs)
  {
    stopEverything("no command from the phone for too long (signal lost?)");
  }
}

// Settings from the app's Settings tab, stored in flash (NVS) so they survive a reboot and are
// the same for every phone. The app applies them to the joysticks; the MiniSkidi only keeps them.
Preferences settingsPrefs;
bool settingSwapSticks = false;   // left stick drives the arm/bucket, right stick drives
bool settingSwapTiltLift = false; // right stick X = boom lift, Y = bucket tilt
bool settingInvTilt = false;
bool settingInvLift = false;
int settingDeadZone = 15;         // percent of the stick radius
const int maxDeadZone = 40;

void loadSettings()
{
  settingsPrefs.begin("ui", true);
  settingSwapSticks = settingsPrefs.getBool("swapSticks", false);
  settingSwapTiltLift = settingsPrefs.getBool("swapTiltLift", false);
  settingInvTilt = settingsPrefs.getBool("invTilt", false);
  settingInvLift = settingsPrefs.getBool("invLift", false);
  settingDeadZone = constrain(settingsPrefs.getInt("deadZone", 15), 0, maxDeadZone);
  settingsPrefs.end();
  LOGI("Settings: %s", settingsJson().c_str());
}

void changeSetting(const std::string &name, int value)
{
  if (name == "forgetBonds")
  {
    LOGW("Deleting all paired phones (bonds)");
    NimBLEDevice::deleteAllBonds();
    return;
  }
  settingsPrefs.begin("ui", false);
  if (name == "swapSticks")
  {
    settingSwapSticks = value != 0;
    settingsPrefs.putBool("swapSticks", settingSwapSticks);
  }
  else if (name == "swapTiltLift")
  {
    settingSwapTiltLift = value != 0;
    settingsPrefs.putBool("swapTiltLift", settingSwapTiltLift);
  }
  else if (name == "invTilt")
  {
    settingInvTilt = value != 0;
    settingsPrefs.putBool("invTilt", settingInvTilt);
  }
  else if (name == "invLift")
  {
    settingInvLift = value != 0;
    settingsPrefs.putBool("invLift", settingInvLift);
  }
  else if (name == "deadZone")
  {
    settingDeadZone = constrain(value, 0, maxDeadZone);
    settingsPrefs.putInt("deadZone", settingDeadZone);
  }
  else
  {
    LOGW("Unknown setting [%s]", name.c_str());
  }
  settingsPrefs.end();
  LOGI("Settings: %s", settingsJson().c_str());
}

String settingsJson()
{
  char json[120];
  snprintf(json, sizeof(json),
           "{\"swapSticks\":%d,\"swapTiltLift\":%d,\"invTilt\":%d,\"invLift\":%d,\"deadZone\":%d}",
           settingSwapSticks, settingSwapTiltLift, settingInvTilt, settingInvLift, settingDeadZone);
  return String(json);
}
