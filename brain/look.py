"""look.py — check with the PC webcam (the OBSBOT, else the bench webcam) that MotherShip really did what the board says it did.

    python look.py snap shot.jpg                          # one frame
    python look.py cmd "go home" --secs 5                 # film a router command
    python look.py bell --ms 60                           # film a solenoid strike (POST /api/bell)
    python look.py watch --secs 20                        # film whatever happens (idle breathing, naps)

Each run saves <name>_sheet.jpg (frames in a grid, the motion box drawn on) and prints a motion score per frame
against the first one: how much of the picture changed and where. A move that the board reports "ok" but that
shows ~0% change didn't happen. Uses PyAV (already installed with faster-whisper), no OpenCV.
"""
import argparse, json, os, sys, threading, time

import av
import numpy as np
import requests
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
CAMERAS = [("OBSBOT Tiny 2 StreamCamera", "1280x720", 0.5),  # (DirectShow name, size, % change that counts as MOVED);
           ("USB2.0 PC CAMERA", "640x480", 1.8)]            # first that opens wins. Same order as mothership/webcam.py.
                                                            # Bench webcam: a still scene peaks ~1.2%, a small wrist tilt ~2.3%.


def board():
    try: b = json.load(open(os.path.join(HERE, "brain.json"))).get("board", "shop-mothership.local")
    except (OSError, ValueError): b = "shop-mothership.local"
    return "http://" + b


class Cam:
    """Keeps the webcam open and the newest frame at hand (frames arrive at 30 fps in a thread)."""
    def __init__(self):
        errs = []
        for name, size, moved in CAMERAS:
            try:
                self.c = av.open("video=" + name, format="dshow", options={"video_size": size, "framerate": "30"})
                self.name, self.moved = name, moved
                break
            except Exception as e:
                errs.append(f"{name}: {e}")
        else:
            raise OSError("[Errno 5] I/O error: no webcam opened (unplugged or busy): " + "; ".join(errs))
        print("camera:", self.name, file=sys.stderr)
        self.s = self.c.streams.video[0]
        self.frame, self.t, self.on = None, 0.0, True
        threading.Thread(target=self._run, daemon=True).start()
        t0 = time.time()
        while self.frame is None and time.time() - t0 < 5: time.sleep(0.02)
        self.settle()

    def settle(self, most=6.0):
        """Wait for auto-exposure: until two frames 0.3 s apart barely differ (the bench webcam takes ~2-3 s)."""
        t0, prev = time.time(), None
        while time.time() - t0 < most:
            time.sleep(0.3)
            im = self.grab()
            if prev is not None and motion(prev, im)[0] < 0.3: return
            prev = im

    def _run(self):
        for f in self.c.decode(self.s):
            if not self.on: break
            self.frame, self.t = f.to_image(), time.time()

    def grab(self): return self.frame.copy()

    def close(self):
        self.on = False
        time.sleep(0.1)
        self.c.close()


def small(im): return np.asarray(im.convert("L").resize((160, 90)), dtype=np.int16)


def motion(a, b):
    """% of the picture that changed between two frames, and the box (in full-size pixels) around the change."""
    d = np.abs(small(a) - small(b)) > 25
    pct = 100.0 * d.mean()
    if not d.any(): return pct, None
    ys, xs = np.nonzero(d)
    sx, sy = a.width / 160, a.height / 90
    return pct, (int(xs.min() * sx), int(ys.min() * sy), int((xs.max() + 1) * sx), int((ys.max() + 1) * sy))


def sheet(frames, path, cols=4):
    w = 320
    h = w * frames[0][1].height // frames[0][1].width            # 180 for the OBSBOT, 240 for the 4:3 bench webcam
    rows = (len(frames) + cols - 1) // cols
    out = Image.new("RGB", (cols * w, rows * h), "black")
    first = frames[0][1]
    for i, (t, im, label) in enumerate(frames):
        pct, box = motion(first, im)
        tile = im.resize((w, h)); d = ImageDraw.Draw(tile)
        if box and i:
            k = w / im.width
            d.rectangle([box[0] * k, box[1] * k, box[2] * k, box[3] * k], outline="yellow", width=2)
        d.rectangle([0, 0, w, 16], fill="black")
        d.text((4, 2), f"{t:+.1f}s  {pct:4.1f}% moved  {label}", fill="white")
        out.paste(tile, ((i % cols) * w, (i // cols) * h))
    out.save(path, quality=85)


def film(cam, act, secs, fps, label):
    frames = [(0.0, cam.grab(), "before")]
    t0 = time.time()
    result = act() if act else None
    while time.time() - t0 < secs:
        time.sleep(1 / fps)
        frames.append((time.time() - t0, cam.grab(), ""))
    return frames, result


def report(frames, result, name, moved=0.5):
    path = os.path.join(os.getcwd(), name + "_sheet.jpg")
    sheet(frames, path)
    first = frames[0][1]
    scores = [motion(first, im) for _, im, _ in frames[1:]]
    peak = max(scores, key=lambda s: s[0]) if scores else (0, None)
    print("board said:", json.dumps(result) if result is not None else "-")
    print("motion vs before:", " ".join(f"{p:.1f}" for p, _ in scores))
    print(f"peak {peak[0]:.1f}% of the picture changed" + (f", around x {peak[1][0]}-{peak[1][2]}, y {peak[1][1]}-{peak[1][3]}" if peak[1] else ""))
    print("verdict:", "MOVED" if peak[0] >= moved else "NO VISIBLE CHANGE")
    print("sheet:", path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["snap", "cmd", "bell", "watch"])
    ap.add_argument("arg", nargs="?", default="")
    ap.add_argument("--secs", type=float, default=4)
    ap.add_argument("--fps", type=float, default=3)
    ap.add_argument("--ms", type=int, default=60)
    ap.add_argument("--name", default="look")
    a = ap.parse_args()
    cam = Cam()
    try:
        if a.what == "snap":
            p = a.arg or "snap.jpg"; cam.grab().save(p, quality=90); print("saved", p); return
        base = board()
        if a.what == "cmd":
            act = lambda: requests.post(base + "/api/cmd", json={"text": a.arg}, timeout=10).json()
        elif a.what == "bell":
            act = lambda: {k: v for k, v in requests.post(base + "/api/bell", json={"ms": a.ms, "listen": False}, timeout=10).json().items()}
        else:
            act = None
        frames, result = film(cam, act, a.secs, a.fps, a.what)
        report(frames, result, a.name, cam.moved)
    finally:
        cam.close()


if __name__ == "__main__":
    main()
