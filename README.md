# MiniSkidi 3.0

All the code is written by Claude Opus 5.5 with a human in the loop.

Firmware for the MiniSkidi skid steer loader, running on an ESP32-WROOM-32D
("ESP32 Dev Module").

## Dependencies

All arduino libraries and dependency versions are pinned in [sketch.yaml](sketch.yaml).

## Option A: arduino-cli (recommended)

`arduino-cli` reads `sketch.yaml` and installs the exact versions listed there.
They go into its own cache, separate from `~/Arduino/libraries`,
so other sketches are not affected.

1. Install `arduino-cli`:

   ```bash
   curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh
   ```

2. Install the Python packages the ESP32 build tools need, listed in
   [pyproject.toml](pyproject.toml). On Linux the ESP32 core runs
   `esptool.py` with the first `python` on `PATH`, and it needs `pyserial`
   (otherwise the build fails with `No module named 'serial'`). With
   [uv](https://docs.astral.sh/uv/):

   ```bash
   uv sync --frozen
   ```

   This creates `.venv` in the sketch folder. `uv run` puts it first on
   `PATH` for the commands below.

3. Build the sketch. The first build downloads the board core and the
   libraries:

   ```bash
   uv run arduino-cli compile --profile miniskidi
   ```

4. Upload to the board. Replace `/dev/ttyUSB0` with your port;
   `arduino-cli board list` shows it:

   ```bash
   uv run arduino-cli upload --profile miniskidi -p /dev/ttyUSB0
   ```

5. Optional: open the serial monitor:

   ```bash
   arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200
   ```

## Option B: Arduino IDE

The Arduino IDE does not install the versions pinned in `sketch.yaml`, so install them by hand:

1. **File > Preferences > Additional boards manager URLs**: add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Tools > Board > Boards Manager**: search for `esp32` by Espressif Systems
   and install version **2.0.17**.
3. **Sketch > Include Library > Manage Libraries**: install these versions:
   - `ESP32Servo` by Kevin Harrington: **3.2.1**
   - `NimBLE-Arduino` by h2zero: **2.5.1**

   On Linux the IDE also runs `esptool.py` with the first `python` on
   `PATH`, so that Python needs `pyserial`
   (`python -m pip install pyserial`).
4. **Tools > Board > esp32**: select **ESP32 Dev Module**.
5. **Tools > Port**: select the port the ESP32 is connected to, for example
   `/dev/ttyUSB0` on Linux or `COM3` on Windows. If no port appears, check
   the USB cable (some cables are charge-only) and the USB-to-serial driver
   (CP210x or CH340).
6. Click **Upload**.
7. **Tools > Serial Monitor**: set the baud rate dropdown in the bottom-right
   corner to **115200**, the speed the sketch uses (`Serial.begin(115200)`).
   Other values show garbled text.

## Updating sketch.yaml

To regenerate the profile from the versions a build actually used:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 --dump-profile
```

Copy the printed profile into `sketch.yaml`.

## How it works

The MiniSkidi is controlled over **Bluetooth Low Energy (BLE)** only, from a
web app running in **Chrome on Android**. The ESP32 has no Wi-Fi in this
setup.

- The ESP32 runs a BLE server ([Ble.ino](Ble.ino), using
  [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino), Apache-2.0).
- The phone app is a plain web page in [web/](web/). It uses the browser's
  [Web Bluetooth](https://developer.mozilla.org/en-US/docs/Web/API/Web_Bluetooth_API)
  API, so there is nothing to install from an app store. It is free and
  open source, with no account or cloud service.
- Web Bluetooth only works on `https://` pages, so the app is hosted on
  GitHub Pages (free), right . After the first visit it also works offline, and it
  can be installed to the home screen like an app.

iPhones are not supported: Safari and the other iOS browsers have no Web
Bluetooth.

## Publishing the app (one time)

1. Create a **public** repository on GitHub and push this project to it
   (branch `main` or `master`).
2. In the repository, open **Settings > Pages** and set **Source** to
   **GitHub Actions**.
3. The workflow in [.github/workflows/pages.yml](.github/workflows/pages.yml)
   publishes the `web/` folder on every push that changes it. The address
   is shown in the workflow run and on the Pages settings page, for example
   `https://<your-user>.github.io/<repository>/`.

   In this repo - visit [https://nvtkaszpir.github.io/miniskidi/](https://nvtkaszpir.github.io/miniskidi/)
  for the currently rendered github pages.


After changing files in `web/`, raise `CACHE` in [web/sw.js](web/sw.js)
(for example `miniskidi-v2`), so installed apps load the new version.

## Connecting

1. Change the passkey before the first upload: `blePasskey` in
   [MiniSkidi_3_0.ino](MiniSkidi_3_0.ino) (6 digits, default `123456`).
2. Power on the MiniSkidi. The serial monitor shows its Bluetooth name,
   `MiniSkidi-XXXXXX`, where `XXXXXX` is the end of the ESP32 MAC address.
3. On the phone, turn on Bluetooth (on Android 11 and older, also Location),
   open the app address in **Chrome**, and tap **Connect**.
4. Pick `MiniSkidi-XXXXXX` in the list.
5. The first time, Android asks to pair: enter the passkey. The phone stays
   paired after that and connects without the passkey.
6. Optional: Chrome menu ⋮ > **Add to Home screen** / **Install app**. The
   installed app opens fullscreen and works without internet.

Only one phone can be connected at a time. After a connection is lost, the
app reconnects by itself while the page is open. After the page is
reloaded, tap **Connect** again.

### Changing the passkey or removing phones

The MiniSkidi remembers up to 3 paired phones. To make every phone pair
again (for example after changing `blePasskey`):

1. In the app, **Settings > Forget paired phones** (or erase the flash with
   `esptool.py erase_flash`).
2. On each phone, open Android **Settings > Bluetooth**, tap the MiniSkidi,
   and choose **Forget**.

## Using the app

The top bar is the same on every tab:

- **Signal strength**: how well the MiniSkidi receives the phone, in dBm.
  - green: -67 dBm or better
  - amber: -68 to -75 dBm
  - red with **weak**: below -75 dBm, close to losing the connection
  - red **No signal**: no status from the MiniSkidi for 1 second
- **Connect / Disconnect**
- the tabs: **Classic**, **Joystick**, **Settings**

### Classic tab

The original MiniSkidi page: arrow buttons for driving and the arm, light
button, and Bucket and AUX sliders. The keyboard shortcuts still work with
a keyboard attached: arrows drive, W/S arm, Q/A bucket, E/D AUX. The motors
run at full speed while a button is held.

### Joystick tab

Landscape layout, the same as the former RemoteXY screen:

| Control                    | Left / right            | Up / down          |
|----------------------------|-------------------------|--------------------|
| Left joystick              | turn left / right       | forward / backward |
| Right joystick             | bucket tilt             | boom lift up / down |
| Vertical slider ("Attach") | attachment (AUX) servo, bottom 0 to top 100 | |
| ☼ button                   | lights on / off         |                    |

- Both joysticks can be used at the same time with two thumbs.
- Speed follows how far a stick is pushed. Pushing the drive stick
  diagonally makes curves; sideways only turns on the spot.
- Lifting a thumb puts that stick back in the center and stops what it
  controlled.
- The bucket tilt sets a speed, not a position: the bucket keeps tilting
  while the stick is pushed and stays where it is when the stick is let go.
  It moves between 10° and 180°, the same range as the Classic slider.
- Small movements near the center (the dead zone, 15% by default) are
  ignored, so a resting thumb doesn't move the machine.
- The tab switches to fullscreen landscape where Android allows it. If the
  phone stays in portrait (for example with rotation lock on), the screen
  is turned sideways; hold the phone in landscape.

### Settings tab

- **Swap joysticks**: driving on the right stick, bucket and boom on the left
- **Swap bucket tilt and boom lift**: boom on horizontal, bucket on vertical
- **Invert bucket tilt**, **Invert boom lift**
- **Dead zone**: 0 to 40%
- **Device**: name, Bluetooth address, firmware build date, uptime, chip,
  free memory, connection details, number of paired phones
- **Forget paired phones**

Settings are saved on the MiniSkidi, so they survive a restart and are the
same for every phone.

## Safety

The motors stop when the connection to the phone is lost:

- **Watchdog**: the app sends a message at least every 150 ms. If nothing
  arrives for **500 ms** while anything moves, the MiniSkidi stops all
  motors and the bucket.
- **Bluetooth link loss**: the MiniSkidi asks the phone for a 400 ms
  supervision timeout; after that long without contact the connection is
  dropped and the motors stop. Phones may choose a longer timeout, so the
  watchdog above is the real limit.
- **App in the background**: switching apps, locking the screen or closing
  the page stops everything.
- **Disconnect** stops everything before the connection closes.

The serial monitor logs why the motors were stopped.

## Tuning

Constants at the top of [Drive.ino](Drive.ino):

- `motorMinDuty` (default 90 of 255): the lowest power a motor gets once
  a stick leaves the dead zone. Raise it if the machine only hums and
  doesn't move at small stick movements; lower it for finer slow driving.
- `motorPwmFreq` (default 1000 Hz): PWM frequency of the motor outputs.
- `bucketMaxRate` (default 90°/s): bucket tilt speed at full deflection.
- `watchdogTimeoutMs` (default 500 ms): see [Safety](#safety).

## Testing the app without GitHub

Web Bluetooth also works on `http://localhost`:

```bash
python3 -m http.server -d web 8000
```

- On a computer with Bluetooth: open `http://localhost:8000` in Chrome.
- On an Android phone connected by USB, with USB debugging on: in desktop
  Chrome open `chrome://inspect/#devices`, click **Port forwarding**, add
  `8000` → `localhost:8000`, then open `http://localhost:8000` in Chrome on
  the phone.

## Bluetooth details

For other apps, such as a generic BLE tool like nRF Connect:

- Service `d0280000-59cd-41da-9d1e-e7992ddd2cb1`
- `d0280001-…` **cmd** (write): text commands, for example `MoveCar,1`,
  `Joy,<turn>,<drive>,<tilt>,<lift>` (-100..100), `Attach,<0..100>`,
  `Light,0`, `Set,<name>,<value>`, `Ping`. See `handleCarInput()` in
  [MiniSkidi_3_0.ino](MiniSkidi_3_0.ino).
- `d0280002-…` **status** (read, notify every 250 ms):
  `{"rssi":-58,"light":1,"bucket":120,"aux":150}`
- `d0280003-…` **info** (read): device info and settings as JSON

All three need an encrypted, paired connection (passkey).
