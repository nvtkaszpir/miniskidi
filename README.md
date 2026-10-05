# MiniSkidi 3.0

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
   uv sync
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
2. **Tools > Board > Boards Manager**: search for `esp32` by Espressif Systems and install version **2.0.17**.
3. **Sketch > Include Library > Manage Libraries**: install this version:
   - `ESP32Servo` by Kevin Harrington: **3.2.1**

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

## Connecting

`XXXXXX` below is the end of the ESP32 MAC address. The serial monitor
prints the exact network name and hostname at startup.

### Access point mode (default)

The MiniSkidi creates its own Wi-Fi network.

1. Connect your phone to the Wi-Fi network `ProfBoots MiniSkidi OG-XXXXXX`
   (password: `deadbeef`).
2. Open the sign-in page the device offers: Android shows a **"Sign in to
   Wi-Fi network"** notification, iPhones/iPads/Macs open a sign-in window,
   Windows shows "Action needed", and Firefox and Linux desktops show a
   login banner. The control page opens over Wi-Fi even when mobile data is
   on. The list of supported checks is in `CaptivePortalDns.ino`.
3. If the notification does not appear, or the sign-in page only shows a
   certificate error, tap ⋮ > **Use this network as is** (or turn off mobile
   data), then open `https://192.168.4.1` or `https://miniskidi.local` in
   Chrome.

### Client mode

The MiniSkidi joins an existing Wi-Fi network, for example your home router.

1. In `MiniSkidi_3_0.ino`, set `staSsid` and `staPassphrase` to the network's
   name and password, then upload.
2. Open `https://miniskidi-xxxxxx.local` (lowercase) from a device on the
   same network, or use the IP address printed in the serial monitor.

If the MiniSkidi cannot connect within 15 seconds, it falls back to access
point mode.

### HTTPS and the certificate warning

The control page is served over HTTPS. Plain `http://` addresses redirect to
`https://`.

The ESP32 creates its own self-signed certificate on first boot and keeps it
in flash, so each MiniSkidi has its own private key. Browsers do not know
who issued it, so the first visit shows a warning such as "Your connection
is not private":

- Chrome: **Advanced > Proceed to 192.168.4.1 (unsafe)**
- Firefox: **Advanced > Accept the Risk and Continue**
- Safari: **Show Details > visit this website**

The browser remembers this. The warning comes back when the certificate
changes:

- the client-mode IP address changes (it is part of the certificate),
- the certificate expires (825 days after the firmware build date; uploading
  newer firmware renews it before then),
- `regenerateCertificate` is set to `true` in `Certificate.ino`.

#### Certificate generation time

On the first boot after uploading (and whenever the certificate has to be
replaced), the ESP32 generates a new key and certificate after joining or
starting Wi-Fi. This usually takes **less than a second, at most about 5
seconds**. During that time the Wi-Fi network is already visible, but the
web page does not load yet. This is normal; the device is not stuck.
Do not power it off.

The serial monitor (115200 baud) always shows these lines, even with
`LOG_LEVEL` set to `NONE` (example, times and names differ):

```text
Generating a new TLS certificate because no stored certificate. This ...
Certificate names/IPs: DNS:miniskidi.local,DNS:miniskidi-xxxxxx.local,IP:192.168.4.1
TLS certificate generated in 850 ms, valid 20261005000000 - 20290107000000
```

The certificate is stored in flash, so later boots reuse it without delay
(`Using stored TLS certificate ...` at `INFO` level).
