"""webcam.py — a webcam on this PC as a frame source for the panel: the OBSBOT if it's there, else the next one in CAMERAS.

One capture is kept open while anyone is looking (frames arrive at 30 fps in a thread); the newest frame is
JPEG-encoded on demand and shared by every viewer. The camera is released after IDLE_SECS with no viewer, so
Video Control can record with it in between. If something else holds the camera, open() fails and the panel
falls back to the board's own camera. Each open tries CAMERAS in order, so whichever is plugged in (and free) is used.
"""
from __future__ import annotations

import io
import threading
import time

CAMERAS = [("OBSBOT Tiny 2 StreamCamera", "1280x720"),     # (DirectShow name, size), first that opens wins
           ("USB2.0 PC CAMERA", "640x480")]               # the plain bench webcam: 640x480 is its biggest size
IDLE_SECS = 20

_lock = threading.Lock()
_cap = None          # the open container
_frame = None        # newest PIL image
_jpeg = (0.0, b"")   # (frame time, encoded)
_last_view = 0.0
_err = ""
_device = ""         # the camera that's open now (or was last)


def _run(c, s):
    global _frame, _cap, _err
    try:
        for f in c.decode(s):
            if _cap is not c: break
            _frame = f.to_image()
            if time.time() - _last_view > IDLE_SECS: break      # nobody looking: let the camera go
    except Exception as e:
        _err = f"{type(e).__name__}: {e}"[:120]
    finally:
        with _lock:
            if _cap is c:
                _cap = None; _frame = None
        try: c.close()
        except Exception: pass


def _open() -> str:
    """Start the capture if it isn't running. Returns '' or an error."""
    global _cap, _err, _device
    with _lock:
        if _cap is not None: return ""
        import av
        c, errs = None, []
        for name, size in CAMERAS:
            try:
                c = av.open("video=" + name, format="dshow", options={"video_size": size, "framerate": "30"})
                break
            except Exception as e:                          # unplugged and busy both read as I/O error on DirectShow
                errs.append(f"{type(e).__name__}: {e}"[:80])
        if c is None:
            _err = ("no webcam free: unplugged, or busy (Video Control recording? OBSBOT Center?)"
                    if all("I/O error" in e for e in errs) else "; ".join(errs)[:160])
            return _err
        _cap, _err, _device = c, "", name
        threading.Thread(target=_run, args=(c, c.streams.video[0]), daemon=True).start()
        return ""


def jpeg(quality: int = 80, max_age: float = 0.1) -> tuple[bytes, str]:
    """The newest frame as JPEG, or (b'', why)."""
    global _jpeg, _last_view
    _last_view = time.time()
    err = _open()
    if err: return b"", err
    t0 = time.time()
    while _frame is None and time.time() - t0 < 3: time.sleep(0.05)     # first frame after opening
    if _frame is None: return b"", _err or "no frame yet"
    t, data = _jpeg
    if data and time.time() - t < max_age: return data, ""
    buf = io.BytesIO(); _frame.save(buf, "JPEG", quality=quality)
    _jpeg = (time.time(), buf.getvalue())
    return _jpeg[1], ""


def status() -> dict:
    return {"open": _cap is not None, "error": _err, "device": _device, "cameras": [n for n, _ in CAMERAS]}
