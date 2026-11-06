# Task for the local model: flash MotherShip onto the arm board

The Waveshare ESP32-S3 arm board is on **COM4**, plugged into this PC over USB-C. Put the **MotherShip** firmware on it.
It's all scripted. Don't write new code, and don't use the `flash_s3` tool: it flashes Muffin's files.
Run the steps below in a terminal in this folder (`Documents\Ragamuffin Workshop\brain`) and report what happened.

## Steps

1. **Check the build is published.** Open https://jobot101.github.io/xarm/fw/mothership/manifest.json.
   It must show `"name": "xarm mothership"`. If it gives a 404, the cloud build isn't finished: wait 2 minutes and try again
   (give up after 20 minutes and say so). **Don't flash before this loads.**
2. **Install the flasher** (once). `flash_mothership.py` talks to COM4 directly, so the shop tools' port registration (`ports.py`) isn't needed:
   ```
   python -m pip install --upgrade esptool
   ```
3. **Flash, keeping the board's settings** (WiFi, taught poses):
   ```
   python flash_mothership.py --port COM4
   ```
4. **Success** looks like `Hash of data verified.` after each file, then `Done: xarm mothership <version> is on the board.`

COM4 is running **Muffin** right now (camera on). This replaces Muffin's firmware with MotherShip's and keeps WiFi, poses and the
camera setting. `--muffin` puts Muffin back. The exact pre-MotherShip state is backed up at `C:\Users\mark\esp32s3_backup\full_flash_16MB.bin`.

## If something goes wrong

| What you see | Do this |
|---|---|
| `could not open port 'COM4'` / `Access is denied` | Another program has the port (Arduino IDE serial monitor, a browser flasher tab, the ragamuffin MCP). Close it, then run step 3 again. |
| `Failed to connect` / `No serial data received` | Hold the board's **BOOT** button, tap **RESET**, release BOOT, then run step 3 again. |
| `could not open port 'COM4'` / `FileNotFoundError` | The port number changed after a reset. Run `python flash_mothership.py` with no `--port` (it finds the board by its USB ID). |
| Board boots but acts strange | `python flash_mothership.py --port COM4 --erase`. This wipes WiFi and poses. |

Don't try anything else: no `idf.py`, no editing the firmware, no other offsets. To put Muffin back, run `python flash_mothership.py --port COM4 --muffin`.

## After it's flashed (tell Joseph)

- The LED ring breathes **dim gold**: MotherShip is running and waiting for the PC brain.
- With WiFi saved, it's at http://shop-mothership.local. Otherwise join WiFi **MotherShip** (password `ragamuffin`), open
  http://192.168.4.1 and set ClarkShop.
- The PC brain is `run-brain.bat` in this folder. It needs LM Studio's server on (port 1234) with a model loaded.
  Before the first run, put MotherShip's ElevenLabs voice id and key in `brain.json`.
