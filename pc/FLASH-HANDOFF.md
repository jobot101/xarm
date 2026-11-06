# Task for the local model: flash Johnny onto the arm board

The Waveshare ESP32-S3 arm board is plugged into this PC over USB-C. Put the **Johnny** firmware on it.
Everything is scripted. Don't write new code. Run the steps below in a terminal in this folder and report what happened.

## Steps

1. Check the build is out: open https://jobot101.github.io/xarm/fw/johnny/manifest.json.
   It must load and show `"name": "xarm johnny"`. If it 404s, the cloud build isn't published yet: stop and say so.
2. Install the flasher tool (once):
   ```
   python -m pip install --upgrade esptool
   ```
3. Flash, keeping the board's settings (WiFi, taught poses):
   ```
   python flash_johnny.py
   ```
4. Success looks like `Hash of data verified.` for every file, then `Done: xarm johnny <version> is on the board.`

## If something goes wrong

| What you see | Do this |
|---|---|
| `No ESP32-S3 found on USB` | Check the USB-C cable carries data (try another). Unplug and replug the board, then run step 3 again. |
| `Failed to connect` / `No serial data received` | Hold the board's **BOOT** button, tap **RESET**, release BOOT, then run step 3 again. |
| `could not open port ... Access is denied` | Something else has the port (Arduino IDE serial monitor, a browser flasher tab). Close it, then run step 3 again. |
| Board boots but acts strange | `python flash_johnny.py --erase`. This wipes WiFi and poses, so they get set again at http://johnny.local or on WiFi "Johnny". |

To put Muffin back, run `python flash_johnny.py --muffin`.

## After it's flashed

- The LED ring breathes **dim gold**: Johnny is running and waiting for the PC brain.
- If the board had WiFi saved, it shows up at http://johnny.local and http://johnny. Otherwise join the WiFi network **Johnny** (password `ragamuffin`) and open http://192.168.4.1 to set the shop WiFi (ClarkShop).
- From now on it updates itself over WiFi from `fw/johnny/` whenever a new build is pushed.
