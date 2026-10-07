// Smooth servo movement for the bucket and AUX servos.
//
// Commands (sliders, the bucket joystick) only set a target angle. servoLoop() moves each servo
// towards its target with exponential smoothing plus a speed limit, so a slider dragged in
// small steps gives one smooth movement instead of a series of jumps, and a big jump is eased.
//
// The servo PWM uses 16-bit resolution instead of ESP32Servo's default 10 bits. At 50 Hz, 10
// bits is one step per 19.5 us, about 2 degrees: every small slider move made the servo jump
// by a whole step. 16 bits is one step per 0.3 us, about 0.03 degrees.
//
// The angles are kept here as floats and never read back from the servo (Servo::read()
// converts back from the pulse width and is rounded).

// Time constant of the smoothing: after this long the servo has covered about 63% of the way to
// a new target. Larger = smoother but slower to follow.
const float servoSmoothingMs = 80;
// Speed limit, degrees per second
const float servoMaxSpeed = 300;
const int servoTimerWidth = 16; // bits
// Pulse widths for 0 and 180 degrees, ESP32Servo's defaults
const int servoMinUs = 544;
const int servoMaxUs = 2400;
// How often a moving servo's position is logged
const unsigned long servoLogIntervalMs = 100;
const char *servoNames[2] = {"bucket", "aux   "}; // same width, so the log lines line up

Servo *servos[2] = {&bucketServo, &auxServo};
const int servoPins[2] = {bucketServoPin, auxServoPin};
// Allowed angle range per servo; the bucket's is set in the app (Drive.ino, setServoLimits())
float servoLimitMin[2] = {servoMinAngle, servoMinAngle};
float servoLimitMax[2] = {servoMaxAngle, servoMaxAngle};
float servoTarget[2];
float servoPos[2];
int servoWrittenUs[2] = {-1, -1};
bool servoMoving[2] = {false, false};
unsigned long lastServoLog[2] = {0, 0};
unsigned long lastServoUpdate = 0;

int servoAngleToUs(float angle)
{
  return (int)(servoMinUs + angle * (servoMaxUs - servoMinUs) / 180.0 + 0.5);
}

void writeServo(int i)
{
  int us = servoAngleToUs(servoPos[i]);
  if (us != servoWrittenUs[i])
  {
    servoWrittenUs[i] = us;
    servos[i]->writeMicroseconds(us);
  }
}

// Attaches the servos and moves them straight (without smoothing) to the start angles, kept
// inside each servo's allowed range (loadSettings() has set the bucket's range already)
void setUpServos(float bucketAngle, float auxAngle)
{
  float start[2] = {bucketAngle, auxAngle};
  for (int i = 0; i < 2; i++)
  {
    servos[i]->attach(servoPins[i], servoMinUs, servoMaxUs);
  }
  // Only after attach(), which resets the width to the default; this re-attaches with 16 bits.
  // Both servos share one LEDC timer, so both are switched before anything is written.
  for (int i = 0; i < 2; i++)
  {
    servos[i]->setTimerWidth(servoTimerWidth);
  }
  for (int i = 0; i < 2; i++)
  {
    servoPos[i] = servoTarget[i] = constrain(start[i], servoLimitMin[i], servoLimitMax[i]);
    writeServo(i);
  }
  lastServoUpdate = millis();
}

// Every command goes through here, so no control (joystick, Classic slider) can move a servo
// outside its allowed range
void setServoTarget(int i, float angle)
{
  servoTarget[i] = constrain(angle, servoLimitMin[i], servoLimitMax[i]);
}

// Changes a servo's allowed range. A servo outside the new range glides back into it.
void setServoLimits(int i, float minAngle, float maxAngle)
{
  servoLimitMin[i] = constrain(minAngle, (float)servoMinAngle, (float)servoMaxAngle);
  servoLimitMax[i] = constrain(maxAngle, servoLimitMin[i], (float)servoMaxAngle);
  setServoTarget(i, servoTarget[i]);
}

float getServoMin(int i)
{
  return servoLimitMin[i];
}
float getServoMax(int i)
{
  return servoLimitMax[i];
}

float getServoTarget(int i)
{
  return servoTarget[i];
}

// Angle and pulse width sent to the servo right now (on the way to the target while smoothing)
float getServoAngle(int i)
{
  return servoPos[i];
}
int getServoPulseUs(int i)
{
  return servoWrittenUs[i];
}

void servoLoop()
{
  unsigned long now = millis();
  float dt = (now - lastServoUpdate) / 1000.0;
  if (dt <= 0)
  {
    return;
  }
  lastServoUpdate = now;
  float follow = min(1.0f, dt * 1000 / servoSmoothingMs);
  float maxStep = servoMaxSpeed * dt;
  for (int i = 0; i < 2; i++)
  {
    float diff = servoTarget[i] - servoPos[i];
    if (fabs(diff) < 0.05)
    {
      servoPos[i] = servoTarget[i];
    }
    else
    {
      servoPos[i] += constrain(diff * follow, -maxStep, maxStep);
    }
    writeServo(i);
    logServoPosition(i, now);
  }
}

// Logs the angle the servo is set to right now (the pulse sent to it, converted to degrees):
// every servoLogIntervalMs while it moves, and once when it has reached its target. A hobby
// servo can't report its real position; it follows this signal unless something blocks it.
void logServoPosition(int i, unsigned long now)
{
  bool moving = servoPos[i] != servoTarget[i];
  if (moving && (!servoMoving[i] || now - lastServoLog[i] >= servoLogIntervalMs))
  {
    lastServoLog[i] = now;
    LOGD("Servo %s moving  | position %05.1f deg %04d us | target %05.1f deg | range %03.0f..%03.0f deg",
         servoNames[i], servoPos[i], servoWrittenUs[i], servoTarget[i], servoLimitMin[i], servoLimitMax[i]);
  }
  else if (!moving && servoMoving[i])
  {
    LOGD("Servo %s reached | position %05.1f deg %04d us | target %05.1f deg | range %03.0f..%03.0f deg",
         servoNames[i], servoPos[i], servoWrittenUs[i], servoTarget[i], servoLimitMin[i], servoLimitMax[i]);
  }
  servoMoving[i] = moving;
}
