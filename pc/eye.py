"""eye.py — Johnny's eyes, running on the shop PC.

The board's ribbon camera only takes pictures (GET /cam.jpg). This finds the slot colors in them and posts the
blobs back (POST /api/blobs), so the board's pick code sees them exactly as it would from the Hiwonder vision
module: one box per slot, centre x,y and size w,h on a 240x240 frame.

    python eye.py                    # board from johnny.json
    python eye.py --board 192.168.1.27
    python eye.py --once             # one frame: print what it sees, save eye_view.jpg, exit

The latest annotated frame is saved to eye_view.jpg every few seconds, for checking aim and colors.
"""
import argparse, io, json, os, sys, time
from collections import deque

import numpy as np
import requests
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
FRAME = 240                       # the board's pick code works on a 240x240 frame (core.h CAM_FRAME)
WORK = (160, 120)                 # detect at this size: plenty for blocks, fast in numpy

# OpenCV-style HSV (H 0-180), the same ranges as the Hiwonder module (cam/xarm_color/color_detection.cpp).
# lo > hi on H means the range wraps through 0 (red).
COLORS = {
    "red":    (151, 15, 70, 255, 90, 255),
    "orange": (8, 22, 90, 255, 100, 255),
    "yellow": (23, 34, 70, 255, 90, 255),
    "green":  (45, 75, 70, 255, 90, 255),
    "blue":   (97, 117, 70, 255, 90, 255),
    "purple": (125, 150, 60, 255, 70, 255),
}
MIN_AREA = 64 / (240 * 240)       # the module's 64-pixel floor, as a fraction of the picture


def load_board():
    p = os.path.join(HERE, "johnny.json")
    try: return json.load(open(p)).get("board", "johnny.local")
    except (OSError, ValueError): return "johnny.local"


def hsv(img):
    """PIL HSV (all 0-255) -> H scaled to OpenCV's 0-180."""
    a = np.asarray(img.convert("HSV"), dtype=np.int16)
    return a[..., 0] * 180 // 255, a[..., 1], a[..., 2]


def largest_blob(mask):
    """Bounding box (x0, y0, x1, y1) and pixel count of the biggest 4-connected region, or None."""
    h, w = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    best, best_n = None, 0
    ys, xs = np.nonzero(mask)
    for y, x in zip(ys, xs):
        if seen[y, x]: continue
        seen[y, x] = True
        q, n = deque([(y, x)]), 0
        x0 = x1 = x; y0 = y1 = y
        while q:
            cy, cx = q.popleft(); n += 1
            x0, x1, y0, y1 = min(x0, cx), max(x1, cx), min(y0, cy), max(y1, cy)
            for ny, nx in ((cy + 1, cx), (cy - 1, cx), (cy, cx + 1), (cy, cx - 1)):
                if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True; q.append((ny, nx))
        if n > best_n: best, best_n = (x0, y0, x1, y1), n
    return best, best_n


def find(img, names):
    """One [x, y, w, h] on the 240x240 frame (or None) per slot name."""
    small = img.resize(WORK)
    H, S, V = hsv(small)
    out = []
    for name in names:
        c = COLORS.get(name.lower().strip())
        if not c: out.append(None); continue
        h0, h1, s0, s1, v0, v1 = c
        hm = (H >= h0) & (H <= h1) if h0 <= h1 else (H >= h0) | (H <= h1)
        mask = hm & (S >= s0) & (S <= s1) & (V >= v0) & (V <= v1)
        box, n = largest_blob(mask)
        if not box or n < MIN_AREA * WORK[0] * WORK[1]: out.append(None); continue
        x0, y0, x1, y1 = box
        sx, sy = FRAME / WORK[0], FRAME / WORK[1]
        out.append([round((x0 + x1 + 1) / 2 * sx), round((y0 + y1 + 1) / 2 * sy),
                    max(1, round((x1 - x0 + 1) * sx)), max(1, round((y1 - y0 + 1) * sy))])
    return out


def annotate(img, names, blobs, path):
    im = img.copy(); d = ImageDraw.Draw(im)
    sx, sy = im.width / FRAME, im.height / FRAME
    for name, b in zip(names, blobs):
        if not b: continue
        x, y, w, h = b
        d.rectangle([(x - w / 2) * sx, (y - h / 2) * sy, (x + w / 2) * sx, (y + h / 2) * sy], outline="white", width=2)
        d.text(((x - w / 2) * sx + 3, (y - h / 2) * sy + 2), name, fill="white")
    im.save(path, quality=85)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board")
    ap.add_argument("--once", action="store_true")
    a = ap.parse_args()
    base = "http://" + (a.board or load_board())
    s = requests.Session()
    names, names_at, last_say, view_at, fails = [], 0, None, 0, 0
    print("eyes: looking through", base + "/cam.jpg")
    while True:
        try:
            if time.time() - names_at > 5:                    # the slot colors can change in Workshop -> Settings
                st = s.get(base + "/api/state", timeout=5).json()
                names = [sl["name"] for sl in st.get("cam", {}).get("slots", [])]
                names_at = time.time()
                if not st.get("cam", {}).get("enabled"):
                    print("eyes: the camera is switched off on the board (Workshop -> Settings -> Camera)"); time.sleep(10); continue
            r = s.get(base + "/cam.jpg", timeout=5)
            if r.status_code == 503:                            # nobody at the bench: the board rests the camera
                if last_say != "resting": print("eyes: camera resting (nobody here)"); last_say = "resting"
                time.sleep(2); continue
            if r.status_code != 200 or not r.content.startswith(b"\xff\xd8"):
                raise RuntimeError(f"no picture ({r.status_code}): {r.text[:80]}")
            img = Image.open(io.BytesIO(r.content)).convert("RGB")
            blobs = find(img, names)
            s.post(base + "/api/blobs", json={"blobs": blobs}, timeout=5)
            fails = 0
            say = ", ".join(f"{n} at {b[0]},{b[1]}" if b else f"no {n}" for n, b in zip(names, blobs))
            if say != last_say and (a.once or not last_say or time.time() - view_at > 1): print("eyes:", say); last_say = say
            if a.once or time.time() - view_at > 3:
                annotate(img, names, blobs, os.path.join(HERE, "eye_view.jpg")); view_at = time.time()
            if a.once:
                print(f"frame {img.width}x{img.height}, saved eye_view.jpg"); return
            time.sleep(0.15)
        except Exception as e:                                  # board rebooting, WiFi blip: keep trying
            fails += 1
            if fails in (1, 10) or fails % 100 == 0: print("eyes:", type(e).__name__, e)
            if a.once: sys.exit(1)
            time.sleep(1 if fails < 10 else 3)


if __name__ == "__main__":
    main()
