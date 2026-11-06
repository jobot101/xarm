"""flash_mothership.py — put the MotherShip firmware on the Waveshare ESP32-S3 arm board over USB.

    python flash_mothership.py              # keep settings (WiFi, poses, limits): no erase
    python flash_mothership.py --erase      # wipe the chip first (fresh board)
    python flash_mothership.py --muffin     # put Muffin back instead
    python flash_mothership.py --port COM7  # if it can't find the board by itself

Downloads the latest build from GitHub Pages (manifest.json lists every file and its offset),
finds the board by its USB ID (Espressif 303A), and writes it with esptool. Needs: pip install esptool
"""
import argparse, json, os, sys, tempfile, urllib.request

BASE = "https://jobot101.github.io/xarm/fw/"


def get(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def find_port():
    from serial.tools import list_ports
    ports = [p for p in list_ports.comports() if p.vid == 0x303A]
    if not ports:
        sys.exit("No ESP32-S3 found on USB. Plug the board in (data cable), or pass --port COMx.\n"
                 "Ports seen: " + ", ".join(f"{p.device} ({p.description})" for p in list_ports.comports()))
    if len(ports) > 1:
        print("More than one Espressif board, using", ports[0].device)
    return ports[0].device


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--erase", action="store_true", help="erase the whole chip first (loses WiFi and taught poses)")
    ap.add_argument("--muffin", action="store_true", help="flash Muffin instead of MotherShip")
    a = ap.parse_args()

    base = BASE if a.muffin else BASE + "mothership/"
    man = json.loads(get(base + "manifest.json?t=" + str(os.getpid())))
    parts = man["builds"][0]["parts"]
    print(f"{man['name']} {man['version']}: {len(parts)} files")

    d = tempfile.mkdtemp(prefix="mothership-")
    args = []
    for p in parts:
        f = os.path.join(d, p["path"])
        print(f"  download {p['path']:22} -> 0x{p['offset']:06x}")
        open(f, "wb").write(get(base + p["path"] + "?v=" + man["version"]))
        args += [hex(p["offset"]), f]

    port = a.port or find_port()
    import esptool
    common = ["--chip", "esp32s3", "--port", port, "--baud", "921600"]
    if a.erase:
        print("erasing", port)
        esptool.main(common + ["erase_flash"])
    print("flashing", port)
    esptool.main(common + ["write_flash", "--flash_mode", "keep", "--flash_size", "keep"] + args)
    print(f"\nDone: {man['name']} {man['version']} is on the board. It restarts by itself.")
    if not a.muffin:
        print("Ring breathes dim gold = MotherShip is up and waiting for the PC brain (http://shop-mothership.local).")


if __name__ == "__main__":
    main()
