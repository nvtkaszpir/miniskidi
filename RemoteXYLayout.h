// RemoteXY GUI layout for the RemoteXY phone app (RemoteXYControl.ino uses it).
//
// The layout is a binary description that only the remotexy.com editor generates. Until it is
// pasted here, REMOTEXY_LAYOUT_PLACEHOLDER keeps RemoteXY switched off (the web UI still works).
//
// How to create it (README.md, "RemoteXY app" has the details):
//   1. In the editor (https://remotexy.com/en/editor/) pick any WiFi connection and the ESP32
//      board. The connection settings don't matter: the sketch keeps its own Wi-Fi setup and
//      only takes the layout.
//   2. Add these elements and set each one's "Variable name" exactly as below. The order and
//      position don't matter, the code uses the names:
//        Joystick  joystick       -> joystick_x, joystick_y  (drive)
//        Button    arm_up                                    (arm up while held)
//        Button    arm_down                                  (arm down while held)
//        Switch    light                                     (lights)
//        Slider    bucket         (0..100)                   (bucket tilt)
//        Slider    aux            (0..100)                   (AUX servo)
//   3. Click "Get source code" and copy the part between "RemoteXY configurate" and
//      "END RemoteXY include": the RemoteXY_CONF_PROGMEM array and the RemoteXY struct.
//   4. Replace everything below the "#pragma once" line with it. Delete the
//      REMOTEXY_LAYOUT_PLACEHOLDER define.
#pragma once
#include <Arduino.h>

#define REMOTEXY_LAYOUT_PLACEHOLDER

// Placeholder with the variables the code expects, so the sketch compiles without a layout.
// The editor generates the same struct (field order may differ) plus RemoteXY_CONF_PROGMEM.
#pragma pack(push, 1)
struct {

    // input variables
  int8_t joystick_x; // from -100 to 100
  int8_t joystick_y; // from -100 to 100
  uint8_t arm_up; // =1 if button pressed, else =0
  uint8_t arm_down; // =1 if button pressed, else =0
  uint8_t light; // =1 if switch ON and =0 if OFF
  int8_t bucket; // from 0 to 100
  int8_t aux; // from 0 to 100

    // other variable
  uint8_t connect_flag;  // =1 if wire connected, else =0

} RemoteXY;
#pragma pack(pop)
