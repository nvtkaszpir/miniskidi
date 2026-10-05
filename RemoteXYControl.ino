// Control with the RemoteXY phone app (https://remotexy.com/en/download/), alongside the web UI.
//
// The app connects over the same Wi-Fi as the web page (access point or client mode) to TCP
// port remoteXYPort. The GUI layout is in RemoteXYLayout.h. Both the app and the web page can
// be connected at once; each sends a command only when one of its controls changes, so they
// don't fight over the motors.
//
// Everything here runs in loop(). The web handlers run on the HTTPS server task; rotateMotor()
// is locked, so commands from both never interleave.

#include "RemoteXYLayout.h"

// Shares the sketch's Wi-Fi with RemoteXY. The library's own CRemoteXYNet_WiFi and
// CRemoteXYNet_WiFiPoint call WiFi.mode() and connect or create a network themselves, which would
// undo the access point / client mode setup in setup().
class CRemoteXYNet_SharedWiFi : public CRemoteXYNet
{
  public:
  uint8_t configured() override
  {
    return apMode || WiFi.status() == WL_CONNECTED;
  }
  CRemoteXYServer *createServer(uint16_t port) override
  {
    return new CRemoteXYServer_WiFi(this, port);
  }
  CRemoteXYClient *newClient() override
  {
    return new CRemoteXYClient_WiFi();
  }
};

bool remoteXYStarted = false;

// Slider (0..100) <-> servo angle, same range as the web page sliders (10..180)
int sliderToAngle(int slider)
{
  return map(constrain(slider, 0, 100), 0, 100, 10, 180);
}
int angleToSlider(int angle)
{
  return map(constrain(angle, 10, 180), 10, 180, 0, 100);
}

// Joystick -> UP/DOWN/LEFT/RIGHT/STOP. The motors are only on or off, so the stronger axis
// wins; inside the dead zone the tracks stop.
int joystickDirection(int x, int y)
{
  const int deadZone = 30; // of 100
  if (abs(x) < deadZone && abs(y) < deadZone)
  {
    return STOP;
  }
  if (abs(y) >= abs(x))
  {
    return y > 0 ? UP : DOWN;
  }
  return x > 0 ? RIGHT : LEFT;
}

void stopTracks()
{
  rotateMotor(RIGHT_MOTOR, STOP);
  rotateMotor(LEFT_MOTOR, STOP);
}

void remoteXYSetup()
{
#ifdef REMOTEXY_LAYOUT_PLACEHOLDER
  LOGW("RemoteXY app control is off: paste the layout from remotexy.com into RemoteXYLayout.h");
#else
  RemoteXYGui *gui = RemoteXYEngine.addGui(RemoteXY_CONF_PROGMEM, &RemoteXY, remoteXYPassword);
  gui->addConnectionServer(new CRemoteXYNet_SharedWiFi(), remoteXYPort);
  // addGui() zeroes all variables: start the sliders at the current servo positions
  RemoteXY.bucket = angleToSlider(bucketServo.read());
  RemoteXY.aux = angleToSlider(auxServo.read());
  RemoteXY.light = light;
  remoteXYStarted = true;
  LOGI("RemoteXY app server started on port %d", remoteXYPort);
#endif
}

void remoteXYLoop()
{
  if (!remoteXYStarted)
  {
    return;
  }
  RemoteXYEngine.handler();

  static bool connected = false;
  static int lastDirection = STOP;
  static bool armUp = false, armDown = false;
  static int lastBucket, lastAux, lastBucketAngle, lastAuxAngle;
  static bool lastLight;

  if (!RemoteXY.connect_flag)
  {
    // Safety: like a closed web page, a lost app stops the machine. The library notices a closed
    // connection at once, a phone out of Wi-Fi range only after 8 s without data.
    if (connected)
    {
      LOGI("RemoteXY app disconnected, stopping motors");
      connected = false;
      moveCar(STOP);
    }
    return;
  }

  if (!connected)
  {
    LOGI("RemoteXY app connected");
    connected = true;
    // The app's current control positions become the baseline, so connecting doesn't stop or
    // move anything the web page is driving
    lastDirection = joystickDirection(RemoteXY.joystick_x, RemoteXY.joystick_y);
    armUp = RemoteXY.arm_up;
    armDown = RemoteXY.arm_down;
    lastBucket = RemoteXY.bucket;
    lastAux = RemoteXY.aux;
    lastLight = RemoteXY.light;
    lastBucketAngle = bucketServo.read();
    lastAuxAngle = auxServo.read();
  }

  // Changes made on the web page go to the app, so its controls show the real state
  if (bucketServo.read() != lastBucketAngle)
  {
    lastBucketAngle = bucketServo.read();
    RemoteXY.bucket = lastBucket = angleToSlider(lastBucketAngle);
  }
  if (auxServo.read() != lastAuxAngle)
  {
    lastAuxAngle = auxServo.read();
    RemoteXY.aux = lastAux = angleToSlider(lastAuxAngle);
  }
  if (light != lastLight)
  {
    RemoteXY.light = lastLight = light;
  }

  // App -> machine
  int direction = joystickDirection(RemoteXY.joystick_x, RemoteXY.joystick_y);
  if (direction != lastDirection)
  {
    LOGD("RemoteXY drive %d (x %d, y %d)", direction, RemoteXY.joystick_x, RemoteXY.joystick_y);
    lastDirection = direction;
    if (direction == STOP)
    {
      stopTracks(); // not moveCar(STOP): that would also stop the arm
    }
    else
    {
      driveCar(direction);
    }
  }

  if (RemoteXY.arm_up != armUp || RemoteXY.arm_down != armDown)
  {
    bool wasDown = armDown;
    armUp = RemoteXY.arm_up;
    armDown = RemoteXY.arm_down;
    LOGD("RemoteXY arm up %d, down %d", armUp, armDown);
    if (armUp && !armDown)
    {
      rotateMotor(ARM_MOTOR, FORWARD);
    }
    else if (armDown && !armUp)
    {
      rotateMotor(ARM_MOTOR, BACKWARD);
    }
    else
    {
      // Set only now, not when lowering starts: a track stop in between would use it up
      removeArmMomentum = wasDown;
      rotateMotor(ARM_MOTOR, STOP);
    }
  }

  if (RemoteXY.bucket != lastBucket)
  {
    lastBucket = RemoteXY.bucket;
    bucketTilt(sliderToAngle(lastBucket));
    lastBucketAngle = bucketServo.read();
  }
  if (RemoteXY.aux != lastAux)
  {
    lastAux = RemoteXY.aux;
    auxControl(sliderToAngle(lastAux));
    lastAuxAngle = auxServo.read();
  }
  if (RemoteXY.light != lastLight)
  {
    lastLight = RemoteXY.light;
    setLight(lastLight);
  }
}
