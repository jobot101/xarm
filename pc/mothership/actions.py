"""actions.py — play Hiwonder xArm action files (.rob) on Shop MotherShip, safely.

File format (Hiwonder PC software): 16-byte header  b"SOBARM", frame count (uint8), zero padding;
then per frame 7 x uint16 little-endian: move time in ms, servo 1..6 positions (0-1000).
Servo order = MotherShip's: 1 gripper, 2 wrist twist, 3 wrist, 4 elbow, 5 shoulder, 6 base.

Safety (same rules as mcp_arm.py): positions clamped to the joint limits the board reports; a floor on travel
time (MIN_MS_PER_100 per 100 units of the biggest change); played at `speed` (default 0.5 = half speed), and at
half that again with someone within NEAR_CM; the first frame is approached slowly from wherever the arm is;
nothing starts while the arm is busy; stop() (or the board's STOP) ends it between frames.
"""
from __future__ import annotations

import glob
import json
import os
import struct
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "actions")
MIN_MS_PER_100 = 250
NEAR_CM = 50
_stop = threading.Event()
_lock = threading.Lock()


def load(path: str) -> list[tuple[int, list[int]]]:
    b = open(path, "rb").read()
    if len(b) < 16 or b[:6] != b"SOBARM": raise ValueError(f"{os.path.basename(path)} isn't a Hiwonder action file")
    n = b[6]
    if len(b) < 16 + n * 14: raise ValueError(f"{os.path.basename(path)} is cut short")
    out = []
    for i in range(n):
        v = struct.unpack_from("<7H", b, 16 + i * 14)
        out.append((v[0], list(v[1:])))
    return out


def library() -> dict[str, str]:
    """name -> file. 'NO.6 Place Left 90°.rob' is listed as 'Place Left 90°'."""
    out = {}
    for f in sorted(glob.glob(os.path.join(LIB, "*.rob"))):
        name = os.path.splitext(os.path.basename(f))[0]
        if name.upper().startswith("NO.") and " " in name: name = name.split(" ", 1)[1]
        out[name] = f
    return out


def _norm(s: str) -> str: return "".join(ch for ch in s.lower() if ch.isalnum())


def find(name: str) -> str | None:
    """Exact match first (ignoring case, spaces and the degree sign), else the SHORTEST name containing the words:
    "place left 45" -> "Place Left 45°", not "Grip and Place Left 45°"."""
    lib, key = library(), _norm(name)
    if not key: return None
    for n, f in lib.items():
        if _norm(n) == key: return f
    hits = [(len(n), f) for n, f in lib.items() if key in _norm(n)]
    return min(hits)[1] if hits else None


def playing() -> bool: return _lock.locked()


def stop(): _stop.set()


def play(board: str, name: str, speed: float = 0.5, log=print) -> str:
    """Play an action by (part of) its name. speed 0.1-1.0 (1.0 = as recorded)."""
    f = find(name)
    if not f: return f"no action called {name!r} (have: {', '.join(library()) or 'none'})"
    if not _lock.acquire(blocking=False): return "another action is already playing"
    try:
        _stop.clear()
        speed = max(0.1, min(1.0, float(speed)))
        frames = load(f)

        def call(path, body=None):
            data = json.dumps(body).encode() if body is not None else None
            req = urllib.request.Request(f"http://{board}{path}", data=data, method="POST" if data else "GET",
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=10) as r: return json.loads(r.read() or b"{}")

        st = call("/api/state")
        if st.get("busy"): return "REFUSED: the arm is busy with a job"
        if not st["arm"].get("torque"): return "REFUSED: the arm is limp (press Hold first)"
        lim, now = st["arm"].get("lim") or [[0, 1000]] * 6, st["arm"]["pos"]
        clamp = lambda p: [max(lo, min(hi, int(v))) for v, (lo, hi) in zip(p, lim)]
        clamped = 0
        for k, (ms, pos) in enumerate(frames):
            if _stop.is_set(): return f"stopped after {k} of {len(frames)} frames"
            st = call("/api/state")
            if k and st.get("busy"): return f"stopped: the arm started a job ({st.get('state')})"
            r = st.get("radar") or {}
            near = bool(r.get("present")) and 0 <= (r.get("distance_cm") if r.get("distance_cm") is not None else -1) < NEAR_CM
            goal = clamp(pos)
            clamped += goal != pos
            biggest = max(abs(g - c) for g, c in zip(goal, now))
            t = int(ms / speed)
            t = max(t, int(biggest * MIN_MS_PER_100 / 100), 200)
            if k == 0: t = max(t, 1500, biggest * 6)      # first frame: come in slowly from wherever it is
            if near: t *= 2
            res = call("/api/jog", {"pos": goal, "ms": t})
            if not res.get("ok"): return f"board refused frame {k + 1}: {res.get('msg')}"
            log(f"  frame {k + 1}/{len(frames)}: {goal} over {t} ms" + ("  (someone close: half speed)" if near else ""))
            deadline = time.time() + t / 1000 + 0.05
            while time.time() < deadline:
                if _stop.is_set(): call("/api/cmd", {"text": "stop"}); return f"stopped during frame {k + 1}"
                time.sleep(0.05)
            now = goal
        return f"played {os.path.basename(f)}: {len(frames)} frames at {int(speed * 100)}% speed" + \
               (f", {clamped} frame(s) kept inside the joint limits" if clamped else "")
    finally:
        _lock.release()


if __name__ == "__main__":
    import sys
    for n, f in library().items():
        fr = load(f)
        print(f"{n:28} {len(fr):2} frames  {sum(t for t, _ in fr) / 1000:4.1f} s   first {fr[0][1]}")
    if len(sys.argv) > 2 and sys.argv[1] == "play":
        print(play(sys.argv[2], sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else 0.5))
