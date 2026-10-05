# MiniSkidi 3.0

Firmware for the MiniSkidi skid steer loader, running on an ESP32-WROOM-32D ("ESP32 Dev Module").

## Dependencies

All dependency versions are pinned in [sketch.yaml](sketch.yaml).

## Option A: arduino-cli (recommended)

`arduino-cli` reads `sketch.yaml` and installs the exact versions listed there. They go into its own cache, separate from `~/Arduino/libraries`, so other sketches are not affected.

1. Install `arduino-cli`:

   ```bash
   curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh
   ```

2. Build the sketch. The first build downloads the board core and the libraries:

   ```bash
   arduino-cli compile --profile miniskidi
   ```

3. Upload to the board. Replace `/dev/ttyUSB0` with your port; `arduino-cli board list` shows it:

   ```bash
   arduino-cli upload --profile miniskidi -p /dev/ttyUSB0
   ```

4. Optional: open the serial monitor:

   ```bash
   arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200
   ```

## Option B: Arduino IDE

The Arduino IDE does not install the versions pinned in `sketch.yaml`, so install them by hand:

1. **File > Preferences > Additional boards manager URLs**: add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Tools > Board > Boards Manager**: search for `esp32` by Espressif Systems and install version **2.0.17**.
3. **Sketch > Include Library > Manage Libraries**: install these versions:
   - `ESP32Servo` by Kevin Harrington: **3.2.1**
   - `ESPAsyncWebSrv` by dvarrel: **1.2.9**
   - `AsyncTCP` by dvarrel: **1.1.4**
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

## Connecting

`XXXXXX` below is the end of the ESP32 MAC address. The serial monitor
prints the exact network name and hostname at startup.

### Access point mode (default)

The MiniSkidi creates its own Wi-Fi network.

1. Connect your phone to the Wi-Fi network `ProfBoots MiniSkidi OG-XXXXXX`
   (password: `deadbeef`).
2. Tap the **"Sign in to Wi-Fi network"** notification. The control page
   opens over Wi-Fi even when mobile data is on.
3. If the notification does not appear, turn off mobile data and open
   `http://192.168.4.1` or `http://miniskidi.local` in a browser.

### Client mode

The MiniSkidi joins an existing Wi-Fi network, for example your home router.

1. In `MiniSkidi_3_0.ino`, set `staSsid` and `staPassphrase` to the network's
   name and password, then upload.
2. Open `http://miniskidi-xxxxxx.local` (lowercase) from a device on the same
   network, or use the IP address printed in the serial monitor.

If the MiniSkidi cannot connect within 15 seconds, it falls back to access
point mode.
