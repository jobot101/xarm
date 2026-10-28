# xarm-voice

Tell a Hiwonder xArm 1S to pick something up by voice. You can also drive it from a phone.

```
 "pick up the red"
        │
  ┌─────▼──────────────────────┐  UART 9600   ┌──────────────────┐
  │ Waveshare ESP32-S3-AUDIO   │─────────────▶│ xArm 1S servo    │──▶ 6 bus servos
  │  mics → ESP-SR MultiNet    │              │ controller       │
  │  router → pick sequence    │  I2C 0x52    ├──────────────────┤
  │  web server + React app    │◀─────────────│ Hiwonder S3 cam  │  color blobs x,y,w,h
  └─────────────┬──────────────┘              └──────────────────┘
                │ WiFi  http://xarm.local
              phone / laptop
```

## Wiring

Use the 2×9 header on the audio board.

| Audio board header | Goes to | Notes |
|---|---|---|
| IO5 (TX) | xArm controller serial **RX** | Use the 4-pin serial port (the Bluetooth module port) |
| IO6 (RX) | xArm controller serial **TX** | Check this is 3.3 V at idle. If it reads 5 V, add a 1k/2k divider |
| GND | controller GND | Required |
| 5V | vision module 5V | |
| GND | vision module GND | |
| SDA (GPIO11) | vision module SDA | Shares the onboard bus (codecs 0x18/0x40, TCA9555 0x20, RTC 0x51) |
| SCL (GPIO10) | vision module SCL | |

- **Power:** the arm runs on its 7.5 V brick. The audio board runs on USB-C (5 V), and the cam is fed from the board's 5V pin. Never put 7.5 V near the board.
- **Pins to leave alone:** IO19/IO20 are the native USB lines. IO3/4/5/7/8/9 are the LCD pins and are free when no screen is fitted.
- **Camera mount:** fix it overhead, looking straight down at the mat, roughly 350–450 mm (14–18 in) up. It needs to see the arm's whole reach. It must not move after calibration, so use a rigid printed bracket.

## Flash it

1. **Vision module:** flash Hiwonder's `ColorDetection.ino` (from their ESP32-S3 docs, "Communicate with Other Controllers"). First set its two color thresholds to your two target colors. Slot 1 and slot 2 become "color 1" and "color 2" here.
2. **Web app** (prebuilt copy already in `firmware/main/www/`):
   ```
   cd web && npm i && npm run build
   ```
3. **Firmware** (ESP-IDF ≥ 5.5):
   ```
   cd firmware
   idf.py set-target esp32s3
   idf.py menuconfig        # xArm Voice → WiFi SSID/pass
   idf.py build flash monitor
   ```
   If there's no WiFi or it fails, the board starts its own network `xarm-voice` (password `ragamuffin`). Open http://192.168.4.1.

### From an Android phone

GitHub Actions builds every push. The files are on the **latest** release: https://github.com/jobot101/xarm/releases/tag/latest

- **First flash:** download the five `.bin` files and `OFFSETS.txt` from that release. Plug the board into the phone with a USB-C OTG cable, open https://espressif.github.io/esptool-js/ in Chrome, add each file at its offset, and press Program. If it won't connect, hold BOOT, tap RESET, and release BOOT.
- **Every flash after that:** open http://xarm.local, go to **Firmware**, pick `xarm-voice.bin`, and upload. It goes over WiFi with no cable. If a bad build won't start, the board rolls back on its own.

## First run

1. Open **http://xarm.local**. All three status lights (arm, cam, mic) should be green.
2. **Jog:** check each joint's ID and direction. Find the gripper's open and closed values and enter them under Settings.
3. **Teach:** press "Limp", pose the arm by hand, then save **Home** (safe carry pose) and **Drop** (where things go).
4. **Calibrate:** do this for 5–9 spots across the mat (corners and center):
   - Place a target with the arm clear of the view, then press **Mark target**.
   - Pose the open gripper around the target, then press **Save point**.
5. **Lift offset:** this is added to the grasp pose to make the approach pose. From a grasp pose, press "Save + test lift". If the arm dives instead of rising, flip the sign.
6. **Settings:** name the colors. The names become voice words immediately.

## Use it

Say **"pick up the red"**, "grab the blue", "drop it", "go home", "open hand", "close hand", or **"stop"**. There's no wake word, and "stop" works at any time. The web app sends the same commands and has a big STOP button.

## API

The React app uses this API. You can also script it with curl.

| | | |
|---|---|---|
| `GET /api/state` | Everything: state, servo positions, blobs, calibration points, mic, net | Polled at 4 Hz |
| `POST /api/cmd` | `{"text":"pick up the red"}` | Same vocabulary as voice |
| `POST /api/jog` | `{"id":6,"pos":420,"ms":150}` or `{"pos":[6 values],"ms":800}` | |
| `POST /api/torque` | `{"on":false}` | Limp for hand-teaching |
| `POST /api/teach` | `{"what":"home"\|"drop"\|"calib_mark"\|"calib_add"\|"calib_undo"\|"calib_clear"\|"test_lift"}` | |
| `GET/POST /api/config` | Slot names, grip open/close, move/grip ms, lift[6] | Saved to NVS |
| `POST /api/ota` | Raw `xarm-voice.bin` body | Writes the other app slot, reboots after 500 ms. `/api/state` `fw` shows the running version |

## Layout

```
firmware/main/
  core.c     protocol frames, parser, cam decode, pose interpolation (pure C, host-tested)
  arm.c      UART link to the xArm controller
  cam.c      I2C poll of the vision module
  pick.c     motion sequences (pick / drop / home), stop preempts
  router.c   text command → action. Voice and web both go through here
  voice.c    ES7210 mics → ESP-SR MultiNet
  web.c      HTTP + JSON API + embedded app
  ota.c      firmware upload over WiFi (POST /api/ota), running version
  net.c      WiFi STA / fallback AP, mDNS
  store.c    persistent config
firmware/test/   host unit tests for core.c
web/             React app (Vite), built to one gzipped file inside the firmware
```
