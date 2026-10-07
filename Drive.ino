// Motor speed (PWM), joystick driving, the signal-lost watchdog and the settings.
//
// The tracks and the arm are DC motors on H-bridge inputs (IN1, IN2). PWM on one input with
// the other one LOW sets the speed: IN1 = duty, IN2 = 0 forward; IN1 = 0, IN2 = duty backward;
// both 0 stops (coast), the same as the old digitalWrite LOW/LOW.

// PWM frequency and resolution for the motor pins
const int motorPwmFreq = 1000; // Hz
const int motorPwmBits = 8;    // duty 0..255; 255 is fully on
// Default start power (duty 0..255): the lowest duty a motor gets once the stick leaves the dead
// zone. Below about 90 the motors only hum. Adjustable in the app's Settings tab, separately
// for the drive motors and the boom.
const int defaultMinDuty = 90;
const int maxMinDuty = 200;

// Bucket tilt speed at 100% "Bucket top speed" and full right-stick deflection, degrees per second
const float bucketMaxRate = 180.0;

// Signal-lost watchdog: the phone sends something (at least "Ping") every 150 ms. If nothing
// arrives for this long while anything moves, everything is stopped.
const unsigned long watchdogTimeoutMs = 500;

// Settings from the app's Settings tab, stored in flash (NVS) so they survive a reboot and are
// the same for every phone. The app applies the joystick ones itself; the MiniSkidi only keeps
// them. The start powers are used here, in setMotorSpeed().
Preferences settingsPrefs;
bool settingSwapSticks = false;   // left stick drives the arm/bucket, right stick drives
bool settingSwapTiltLift = false; // right stick X = boom lift, Y = bucket tilt
bool settingInvTilt = false;
bool settingInvLift = false;
bool settingArmZones = true;      // arm stick snaps to 8 directions (up, up-right, right, ...)
int settingDeadZone = 15;         // percent of the stick travel, per axis
const int maxDeadZone = 40;
int settingDriveMinDuty = defaultMinDuty; // start power of the track motors
int settingArmMinDuty = defaultMinDuty;   // start power of the boom motor
// Top speeds, percent: how much of the range above the start power a full stick deflection uses
// (100 = full power). They keep e.g. the boom from moving too fast for fine work.
const int defaultDriveMax = 80, defaultTurnMax = 80, defaultBoomMax = 20;
int settingDriveMax = defaultDriveMax; // forward / backward
int settingTurnMax = defaultTurnMax;   // turning (track speed difference)
int settingBoomMax = defaultBoomMax;
// Bucket tilt speed range, percent of bucketMaxRate: at the smallest and at full deflection
const int defaultTiltMin = 0, defaultTiltMax = 20;
int settingTiltMin = defaultTiltMin;
int settingTiltMax = defaultTiltMax;
// Bucket angle range, degrees: the bucket servo never goes outside it, from any control. Use it
// to keep the bucket from pushing against the frame or the boom.
int settingBucketMin = servoMinAngle;
int settingBucketMax = servoMaxAngle;
const int minBucketRange = 10; // degrees between min and max at least
// Servo angles at power-on (the bucket's is kept inside the bucket angle range)
const int defaultBucketStart = 140, defaultAuxStart = 150;
int settingBucketStart = defaultBucketStart;
int settingAuxStart = defaultAuxStart;

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

// speed -255 (full backward) .. 0 (stop) .. 255 (full forward). A non-zero speed is scaled into
// start power..255, so the smallest stick movement already gives enough power to move.
void setMotorSpeed(int motorNumber, int speed)
{
  speed = constrain(speed, -255, 255);
  motorSpeed[motorNumber] = speed;
  int duty = abs(motorDuty(motorNumber, speed));
  motorPwm[motorNumber][0].write(speed > 0 ? duty : 0);
  motorPwm[motorNumber][1].write(speed < 0 ? duty : 0);
}

// PWM duty setMotorSpeed() uses for a speed, with the sign of the direction: -255..255
int motorDuty(int motorNumber, int speed)
{
  int minDuty = motorNumber == ARM_MOTOR ? settingArmMinDuty : settingDriveMinDuty;
  int duty = speed == 0 ? 0 : map(abs(constrain(speed, -255, 255)), 1, 255, minDuty, 255);
  return speed < 0 ? -duty : duty;
}

// Joystick tab state, -100..100 each. Dead zone, swaps and inversions are applied by the app.
int joyTurn = 0, joyDrive = 0, joyTilt = 0, joyLift = 0;
float bucketAngle = 0;          // rate-controlled bucket target angle
unsigned long lastBucketUpdate = 0;
unsigned long lastCommandMillis = 0; // last command of any kind from the phone

// -100..100 percent -> setMotorSpeed() speed -255..255. Not rounded to 0 for tiny values, so a
// stick just outside the dead zone still starts the motor (at its start power).
int percentToSpeed(float percent)
{
  percent = constrain(percent, -100.0f, 100.0f);
  int speed = (int)(percent * 2.55f + (percent >= 0 ? 0.5f : -0.5f));
  if (speed == 0 && percent != 0)
  {
    speed = percent > 0 ? 1 : -1;
  }
  return speed;
}

void joystickInput(int turn, int drive, int tilt, int lift)
{
  turn = constrain(turn, -100, 100);
  drive = constrain(drive, -100, 100);
  tilt = constrain(tilt, -100, 100);
  lift = constrain(lift, -100, 100);

  if (turn != joyTurn || drive != joyDrive)
  {
    // Top speeds first, then tank mixing, same directions as the Classic D-pad: turning left
    // (turn < 0) runs the left track forward and the right track backward, like the LEFT case
    // in driveCar()
    float d = drive * settingDriveMax / 100.0;
    float t = turn * settingTurnMax / 100.0;
    // Limit hit: a stick at full deflection (the output is at its top speed), or the mixed
    // drive + turn more than a track can do
    bool limitHit = abs(drive) >= 100 || abs(turn) >= 100 || fabs(d - t) > 100 || fabs(d + t) > 100;
    float left = constrain(d - t, -100.0f, 100.0f);
    float right = constrain(d + t, -100.0f, 100.0f);
    setMotorSpeed(LEFT_MOTOR, percentToSpeed(left));
    setMotorSpeed(RIGHT_MOTOR, percentToSpeed(right));
    // stick input | limits (top speed %, start power duty) | result: % and PWM duty per track
    LOGD("Joystick tracks: turn %+04d drive %+04d | limits turn %03d%% drive %03d%% start %03d"
         " | left %+04d%% duty %+04d, right %+04d%% duty %+04d | limit_hit: %c",
         turn, drive, settingTurnMax, settingDriveMax, settingDriveMinDuty,
         (int)round(left), motorDuty(LEFT_MOTOR, motorSpeed[LEFT_MOTOR]),
         (int)round(right), motorDuty(RIGHT_MOTOR, motorSpeed[RIGHT_MOTOR]), limitHit ? 'y' : 'n');
  }

  if (lift != joyLift)
  {
    // lift > 0 = boom up (stick up, ISO layout). On the machines tested the arm motor's FORWARD
    // direction (the Classic tab's ARMUP) lowers the boom, so the joystick drives it the other
    // way. "Invert boom lift" in the app flips it for a machine wired the other way round.
    // No brake pulse when the arm stops (the Classic tab keeps it): with speed control it only
    // got in the way of small, precise arm movements.
    removeArmMomentum = false;
    float boom = lift * settingBoomMax / 100.0;
    setMotorSpeed(ARM_MOTOR, -percentToSpeed(boom));
    // stick input | limits | result: % and PWM duty (sign = motor direction, reversed for the boom)
    // Limit hit: stick at full deflection, the boom runs at its top speed
    LOGD("Joystick lift  %+04d | limits boom %03d%% start %03d | boom %+04d%% duty %+04d | limit_hit: %c",
         lift, settingBoomMax, settingArmMinDuty, (int)round(boom),
         motorDuty(ARM_MOTOR, motorSpeed[ARM_MOTOR]), abs(lift) >= 100 ? 'y' : 'n');
  }

  if (tilt != 0 && joyTilt == 0)
  {
    // Start of a tilt movement: continue from the bucket's current target (the Classic slider
    // may have moved it)
    bucketAngle = getServoTarget(BUCKET_SERVO);
    lastBucketUpdate = millis();
  }
  if (tilt != joyTilt)
  {
    // stick input | limits (min..top speed %) | result: tilt speed of the bucket
    // stick input | limits | target angle, and the signal the servo gets right now (angle and
    // pulse width; Servos.ino glides it to the target). Limit hit: stick at full deflection,
    // the bucket tilts at its top speed. While it moves, servoLoop() logs the position.
    LOGD("Joystick tilt  %+04d | limits bucket %03d%%..%03d%% | target %05.1f deg"
         " | servo %05.1f deg %04d us | limit_hit: %c",
         tilt, settingTiltMin, settingTiltMax, getServoTarget(BUCKET_SERVO),
         getServoAngle(BUCKET_SERVO), getServoPulseUs(BUCKET_SERVO), abs(tilt) >= 100 ? 'y' : 'n');
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

// Bucket tilt speed in degrees per second for a stick value -100..100: from "Bucket min speed"
// at the smallest deflection to "Bucket top speed" at full deflection
float tiltRate(int tilt)
{
  if (tilt == 0)
  {
    return 0;
  }
  float tiltMin = min(settingTiltMin, settingTiltMax);
  float percent = tiltMin + (settingTiltMax - tiltMin) * abs(tilt) / 100.0;
  return (tilt > 0 ? 1 : -1) * percent / 100.0 * bucketMaxRate;
}

void driveLoop()
{
  // Bucket tilt: the right stick sets a speed, the target angle follows (Servos.ino then moves
  // the servo smoothly). Releasing the stick (tilt 0) leaves the bucket where it is.
  if (joyTilt != 0)
  {
    unsigned long now = millis();
    float dt = (now - lastBucketUpdate) / 1000.0;
    lastBucketUpdate = now;
    bucketAngle = constrain(bucketAngle + tiltRate(joyTilt) * dt, getServoMin(BUCKET_SERVO),
                            getServoMax(BUCKET_SERVO));
    bucketTilt(bucketAngle);
  }

  if (anythingMoving() && millis() - lastCommandMillis > watchdogTimeoutMs)
  {
    stopEverything("no command from the phone for too long (signal lost?)");
  }
}

void loadSettings()
{
  settingsPrefs.begin("ui", false);
  // The boom direction was flipped in the firmware: the old "invLift" tick was a workaround
  // for the old direction and would now flip it back, so it's dropped once. The setting is
  // stored as "invBoom" from now on.
  if (settingsPrefs.isKey("invLift"))
  {
    LOGI("Removing the old boom invert setting (the default boom direction changed)");
    settingsPrefs.remove("invLift");
  }
  settingSwapSticks = settingsPrefs.getBool("swapSticks", false);
  settingSwapTiltLift = settingsPrefs.getBool("swapTiltLift", false);
  settingInvTilt = settingsPrefs.getBool("invTilt", false);
  settingInvLift = settingsPrefs.getBool("invBoom", false);
  settingArmZones = settingsPrefs.getBool("armZones", true);
  settingDeadZone = constrain(settingsPrefs.getInt("deadZone", 15), 0, maxDeadZone);
  settingDriveMinDuty = constrain(settingsPrefs.getInt("driveMinDuty", defaultMinDuty), 0, maxMinDuty);
  settingArmMinDuty = constrain(settingsPrefs.getInt("armMinDuty", defaultMinDuty), 0, maxMinDuty);
  settingDriveMax = constrain(settingsPrefs.getInt("driveMax", defaultDriveMax), 0, 100);
  settingTurnMax = constrain(settingsPrefs.getInt("turnMax", defaultTurnMax), 0, 100);
  settingBoomMax = constrain(settingsPrefs.getInt("boomMax", defaultBoomMax), 0, 100);
  settingTiltMin = constrain(settingsPrefs.getInt("tiltMin", defaultTiltMin), 0, 100);
  settingTiltMax = constrain(settingsPrefs.getInt("tiltMax", defaultTiltMax), 0, 100);
  settingBucketMin = constrain(settingsPrefs.getInt("bucketMinAng", servoMinAngle), servoMinAngle,
                               servoMaxAngle - minBucketRange);
  settingBucketMax = constrain(settingsPrefs.getInt("bucketMaxAng", servoMaxAngle),
                               settingBucketMin + minBucketRange, servoMaxAngle);
  settingBucketStart = constrain(settingsPrefs.getInt("bucketStart", defaultBucketStart), servoMinAngle, servoMaxAngle);
  settingAuxStart = constrain(settingsPrefs.getInt("auxStart", defaultAuxStart), servoMinAngle, servoMaxAngle);
  settingsPrefs.end();
  setServoLimits(BUCKET_SERVO, settingBucketMin, settingBucketMax);
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
    settingsPrefs.putBool("invBoom", settingInvLift);
  }
  else if (name == "armZones")
  {
    settingArmZones = value != 0;
    settingsPrefs.putBool("armZones", settingArmZones);
  }
  else if (name == "deadZone")
  {
    settingDeadZone = constrain(value, 0, maxDeadZone);
    settingsPrefs.putInt("deadZone", settingDeadZone);
  }
  else if (name == "driveMinDuty")
  {
    settingDriveMinDuty = constrain(value, 0, maxMinDuty);
    settingsPrefs.putInt("driveMinDuty", settingDriveMinDuty);
  }
  else if (name == "armMinDuty")
  {
    settingArmMinDuty = constrain(value, 0, maxMinDuty);
    settingsPrefs.putInt("armMinDuty", settingArmMinDuty);
  }
  else if (name == "driveMax" || name == "turnMax" || name == "boomMax" || name == "tiltMin" ||
           name == "tiltMax")
  {
    value = constrain(value, 0, 100);
    int *setting = name == "driveMax" ? &settingDriveMax
                 : name == "turnMax"  ? &settingTurnMax
                 : name == "boomMax"  ? &settingBoomMax
                 : name == "tiltMin"  ? &settingTiltMin
                                      : &settingTiltMax;
    *setting = value;
    settingsPrefs.putInt(name.c_str(), value);
  }
  else if (name == "bucketMin")
  {
    settingBucketMin = constrain(value, servoMinAngle, settingBucketMax - minBucketRange);
    settingsPrefs.putInt("bucketMinAng", settingBucketMin);
    setServoLimits(BUCKET_SERVO, settingBucketMin, settingBucketMax);
  }
  else if (name == "bucketMax")
  {
    settingBucketMax = constrain(value, settingBucketMin + minBucketRange, servoMaxAngle);
    settingsPrefs.putInt("bucketMaxAng", settingBucketMax);
    setServoLimits(BUCKET_SERVO, settingBucketMin, settingBucketMax);
  }
  else if (name == "bucketStart" || name == "auxStart")
  {
    // Used at the next power-on; doesn't move the servo now
    value = constrain(value, servoMinAngle, servoMaxAngle);
    *(name == "bucketStart" ? &settingBucketStart : &settingAuxStart) = value;
    settingsPrefs.putInt(name.c_str(), value);
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
  char json[384];
  snprintf(json, sizeof(json),
           "{\"swapSticks\":%d,\"swapTiltLift\":%d,\"invTilt\":%d,\"invLift\":%d,\"armZones\":%d,"
           "\"deadZone\":%d,\"driveMinDuty\":%d,\"armMinDuty\":%d,\"driveMax\":%d,\"turnMax\":%d,"
           "\"boomMax\":%d,\"tiltMin\":%d,\"tiltMax\":%d,\"bucketMin\":%d,\"bucketMax\":%d,"
           "\"bucketStart\":%d,\"auxStart\":%d}",
           settingSwapSticks, settingSwapTiltLift, settingInvTilt, settingInvLift, settingArmZones,
           settingDeadZone, settingDriveMinDuty, settingArmMinDuty, settingDriveMax, settingTurnMax,
           settingBoomMax, settingTiltMin, settingTiltMax, settingBucketMin, settingBucketMax,
           settingBucketStart, settingAuxStart);
  return String(json);
}

// Servo angle at power-on, from the settings
int servoStartAngle(int servo)
{
  return servo == BUCKET_SERVO ? settingBucketStart : settingAuxStart;
}
