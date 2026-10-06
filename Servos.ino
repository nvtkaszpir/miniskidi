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

Servo *servos[2] = {&bucketServo, &auxServo};
const int servoPins[2] = {bucketServoPin, auxServoPin};
float servoTarget[2];
float servoPos[2];
int servoWrittenUs[2] = {-1, -1};
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

// Attaches the servos and moves them straight (without smoothing) to the start angles
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
    servoPos[i] = servoTarget[i] = constrain(start[i], (float)servoMinAngle, (float)servoMaxAngle);
    writeServo(i);
  }
  lastServoUpdate = millis();
}

void setServoTarget(int i, float angle)
{
  servoTarget[i] = constrain(angle, (float)servoMinAngle, (float)servoMaxAngle);
}

float getServoTarget(int i)
{
  return servoTarget[i];
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
  }
}
