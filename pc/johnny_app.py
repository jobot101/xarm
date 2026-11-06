"""johnny_app.py — Johnny's desktop app: see what he sees, tune the bell and the mics, send him commands.

    python johnny_app.py                     # board from johnny.json (johnny.local)
    python johnny_app.py --board 192.168.1.27

Talks to the board's web API only (/api/state, /cam.jpg, /api/bell, /api/voice, /api/cmd), so it runs alongside
johnny.py (the brain) and eye.py (the color finder) without getting in their way. Needs: requests, pillow.
"""
import argparse, io, json, os, statistics, threading, time
import tkinter as tk
from tkinter import ttk, messagebox

import requests
from PIL import Image, ImageDraw, ImageTk

HERE = os.path.dirname(os.path.abspath(__file__))
FRAME = 240                                   # blob coordinates are on a 240x240 frame (board core.h)
VIEW = (480, 360)
SWEEP = [10, 15, 20, 30, 40, 55, 70, 90, 120]  # bell auto-tune: pulse lengths tried, ms
COMMANDS = ["ring the bell", "pick up the red", "pick up the blue", "hold this for me", "grab it", "let go",
            "put it down", "give it to me", "go home", "wave hello"]

# colors: oak, brass and ink, like the board's web app
BG, PANEL, INK, DIM, BRASS, RED, BLUE, OK = "#211a14", "#2d241c", "#f1e6d2", "#a8977e", "#c9a14a", "#d0573f", "#5b8fd6", "#7bb36a"


def load_board():
    try: return json.load(open(os.path.join(HERE, "johnny.json"))).get("board", "johnny.local")
    except (OSError, ValueError): return "johnny.local"


def ring_stats(db, pre):
    """Room floor, peak, how far above the room it rang, and how long it rang (ms) from one strike's levels."""
    if not db or pre < 1 or len(db) <= pre: return None
    floor = statistics.median(db[:pre])
    tail = db[pre:]
    pk = max(range(len(tail)), key=lambda i: tail[i])
    peak = tail[pk]
    stop = max(floor + 6, peak - 40)
    ring = next((i for i in range(pk, len(tail)) if tail[i] < stop), len(tail)) - pk
    return {"floor": floor, "peak": peak, "rise": peak - floor, "ring_ms": ring * 16, "clip": peak > -1.0}


class Board:
    def __init__(self, host):
        self.base = "http://" + host
        self.s = requests.Session()

    def state(self): return self.s.get(self.base + "/api/state", timeout=6).json()
    def jpg(self): return self.s.get(self.base + "/cam.jpg", timeout=4)
    def post(self, path, body, timeout=8): return self.s.post(self.base + path, json=body, timeout=timeout).json()
    def cmd(self, text): return self.post("/api/cmd", {"text": text})
    def bell(self, **kw): return self.post("/api/bell", kw, timeout=15)
    def voice(self, **kw): return self.post("/api/voice", kw)


class App:
    def __init__(self, root, board):
        self.root, self.b = root, board
        self.st, self.img, self.cam_err, self.busy = {}, None, "", False
        root.title("Johnny")
        root.configure(bg=BG)
        root.minsize(980, 640)
        s = ttk.Style(); s.theme_use("clam")
        s.configure(".", background=BG, foreground=INK, fieldbackground=PANEL, font=("Segoe UI", 10))
        s.configure("TLabelframe", background=BG, bordercolor=BRASS)
        s.configure("TLabelframe.Label", background=BG, foreground=BRASS, font=("Segoe UI Semibold", 11))
        s.configure("TButton", background=PANEL, foreground=INK, bordercolor=BRASS, padding=6)
        s.map("TButton", background=[("active", "#3a2f24"), ("disabled", BG)], foreground=[("disabled", DIM)])
        s.configure("Stop.TButton", background="#7a2a1e", foreground="white", font=("Segoe UI Semibold", 12), padding=10)
        s.map("Stop.TButton", background=[("active", "#9a3526")])
        s.configure("TCheckbutton", background=BG, foreground=INK)
        s.configure("Dim.TLabel", foreground=DIM)
        s.configure("Big.TLabel", font=("Segoe UI Semibold", 13))

        top = ttk.Frame(root, padding=(12, 10)); top.pack(fill="x")
        self.status = ttk.Label(top, text="connecting to " + board.base, style="Big.TLabel"); self.status.pack(side="left")
        ttk.Button(top, text="STOP", style="Stop.TButton", command=lambda: self.send("stop")).pack(side="right")

        body = ttk.Frame(root, padding=(12, 0, 12, 12)); body.pack(fill="both", expand=True)
        body.columnconfigure(0, weight=3); body.columnconfigure(1, weight=2); body.rowconfigure(0, weight=1)
        left = ttk.Frame(body); left.grid(row=0, column=0, sticky="nsew", padx=(0, 10))
        right = ttk.Frame(body); right.grid(row=0, column=1, sticky="nsew")

        # ---- eyes
        eyes = ttk.LabelFrame(left, text="What Johnny sees", padding=8); eyes.pack(fill="x")
        # a pixel-sized placeholder: a Label with no image measures width/height in text characters
        self.tkimg = ImageTk.PhotoImage(Image.new("RGB", VIEW, "black"))
        self.cam = tk.Label(eyes, image=self.tkimg, bg="black", bd=0); self.cam.pack()
        self.cam_note = ttk.Label(eyes, text="", style="Dim.TLabel"); self.cam_note.pack(anchor="w", pady=(6, 0))

        # ---- commands
        cmds = ttk.LabelFrame(left, text="Tell Johnny", padding=8); cmds.pack(fill="x", pady=(10, 0))
        for i, c in enumerate(COMMANDS):
            ttk.Button(cmds, text=c, command=lambda c=c: self.send(c)).grid(row=i // 5, column=i % 5, sticky="ew", padx=2, pady=2)
        for i in range(5): cmds.columnconfigure(i, weight=1)
        row = ttk.Frame(cmds); row.grid(row=2, column=0, columnspan=5, sticky="ew", pady=(6, 0))
        self.say = ttk.Entry(row); self.say.pack(side="left", fill="x", expand=True)
        self.say.bind("<Return>", lambda e: self.send(self.say.get()))
        ttk.Button(row, text="Send", command=lambda: self.send(self.say.get())).pack(side="left", padx=(6, 0))
        self.reply = ttk.Label(cmds, text="", style="Dim.TLabel"); self.reply.grid(row=3, column=0, columnspan=5, sticky="w", pady=(4, 0))

        # ---- radar
        rad = ttk.LabelFrame(right, text="Radar", padding=8); rad.pack(fill="x", pady=(0, 10))
        self.rdot = tk.Canvas(rad, width=26, height=26, bg=BG, highlightthickness=0); self.rdot.pack(side="left")
        self.rtxt = ttk.Label(rad, text="no radar in this firmware yet", style="Big.TLabel"); self.rtxt.pack(side="left", padx=8)
        self.rsub = ttk.Label(rad, text="", style="Dim.TLabel"); self.rsub.pack(side="right")

        # ---- bell
        bell = ttk.LabelFrame(right, text="Bell", padding=8); bell.pack(fill="x")
        r = ttk.Frame(bell); r.pack(fill="x")
        ttk.Label(r, text="Pulse").pack(side="left")
        self.ms = tk.IntVar(value=40)
        ttk.Scale(r, from_=5, to=150, variable=self.ms, command=lambda v: self.ms_lbl.config(text=f"{int(float(v))} ms")).pack(side="left", fill="x", expand=True, padx=6)
        self.ms_lbl = ttk.Label(r, text="40 ms", width=7); self.ms_lbl.pack(side="left")
        r = ttk.Frame(bell); r.pack(fill="x", pady=(6, 0))
        self.bell_btns = [ttk.Button(r, text="Strike", command=self.strike),
                          ttk.Button(r, text="Auto-tune", command=self.autotune),
                          ttk.Button(r, text="Save pulse", command=self.save_pulse)]
        for w in self.bell_btns: w.pack(side="left", padx=(0, 6))
        self.plot = tk.Canvas(bell, width=360, height=150, bg=PANEL, highlightthickness=0); self.plot.pack(fill="x", pady=(8, 0))
        self.bell_note = ttk.Label(bell, text="Strike to hear it. Both mics are plotted.", style="Dim.TLabel", wraplength=360)
        self.bell_note.pack(anchor="w", pady=(4, 0))

        # ---- mics
        mic = ttk.LabelFrame(right, text="Mics", padding=8); mic.pack(fill="x", pady=(10, 0))
        self.meter = tk.Canvas(mic, width=360, height=18, bg=PANEL, highlightthickness=0); self.meter.pack(fill="x")
        r = ttk.Frame(mic); r.pack(fill="x", pady=(6, 0))
        ttk.Label(r, text="Gain").pack(side="left")
        self.gain = tk.IntVar(value=24)
        ttk.Scale(r, from_=0, to=37, variable=self.gain, command=lambda v: self.gain_lbl.config(text=f"{int(float(v))} dB")).pack(side="left", fill="x", expand=True, padx=6)
        self.gain_lbl = ttk.Label(r, text="24 dB", width=6); self.gain_lbl.pack(side="left")
        r = ttk.Frame(mic); r.pack(fill="x", pady=(6, 0))
        ttk.Button(r, text="Set gain", command=self.set_gain).pack(side="left")
        ttk.Button(r, text="Level to my voice (talk 5 s)", command=self.level_voice).pack(side="left", padx=6)
        self.dual = tk.BooleanVar()
        ttk.Checkbutton(mic, text="Use both mics (restarts Johnny)", variable=self.dual, command=self.set_dual).pack(anchor="w", pady=(6, 0))
        self.mic_note = ttk.Label(mic, text="", style="Dim.TLabel", wraplength=360); self.mic_note.pack(anchor="w", pady=(4, 0))

        self.synced = False
        threading.Thread(target=self.poll_state, daemon=True).start()
        threading.Thread(target=self.poll_cam, daemon=True).start()
        self.tick()

    # ---------------------------------------------------------------- background
    def poll_state(self):
        while True:
            try: self.st = self.b.state(); self.st["_at"] = time.time()
            except Exception as e:
                if time.time() - self.st.get("_at", 0) > 5: self.st = {"_err": str(e)}   # one slow answer isn't "offline"
            time.sleep(1.0)                   # the board's internal RAM is tight: don't hammer its web server

    def poll_cam(self):
        while True:
            try:
                r = self.b.jpg()
                if r.status_code == 200 and r.content[:2] == b"\xff\xd8":
                    self.img, self.cam_err = Image.open(io.BytesIO(r.content)).convert("RGB"), ""
                else:
                    self.cam_err = ("no camera picture: this firmware has no ribbon camera yet" if r.status_code in (200, 404, 405)
                                    else f"camera: {r.status_code} {r.text[:60]}")
            except Exception as e: self.cam_err = "camera: " + type(e).__name__
            time.sleep(0.25 if not self.cam_err else 3)

    def bg(self, fn, done=None):
        def run():
            try: res = fn()
            except Exception as e: res = e
            if done: self.root.after(0, done, res)
        threading.Thread(target=run, daemon=True).start()

    # ---------------------------------------------------------------- UI refresh
    def tick(self):
        st = self.st
        if "_err" in st:
            self.status.config(text=f"can't reach {self.b.base}  ({st['_err'][:60]})", foreground=RED)
        elif st:
            br, v = st.get("brain", {}), st.get("voice", {})
            batt = st.get("arm", {}).get("battery_mv", 0) / 1000
            self.status.config(foreground=INK, text=f"{st.get('state', '?')} - {st.get('detail', '')}    "
                               f"brain {'connected' if br.get('pc') else 'not connected'}    "
                               f"arm {batt:.1f} V    fw {st.get('fw', {}).get('version', '?')}")
            if not self.synced and v:
                self.gain.set(v.get("gain", 24)); self.gain_lbl.config(text=f"{v.get('gain', 24)} dB")
                self.dual.set(bool(v.get("dual"))); self.synced = True
            self.draw_radar(st.get("radar"))
            lvl = v.get("level", 0)
            self.meter.delete("all")
            w = self.meter.winfo_width() or 360
            self.meter.create_rectangle(0, 0, w * lvl / 100, 18, fill=OK if lvl < 85 else RED, width=0)
            self.meter.create_text(6, 9, anchor="w", fill=INK, text=f"{lvl * 0.6 - 60:.0f} dBFS  {v.get('status', '')}")
        self.draw_cam()
        self.root.after(200, self.tick)

    def draw_radar(self, r):
        self.rdot.delete("all")
        if not r or r.get("kind") == "none":
            self.rdot.create_oval(3, 3, 23, 23, outline=DIM, width=2)
            self.rtxt.config(text="no radar" if r else "no radar in this firmware yet", foreground=DIM)
            self.rsub.config(text=f"GPIO{r['gpio']}" if r and r.get("gpio", -1) >= 0 else ""); return
        if r.get("kind") == "serial":
            self.rdot.create_oval(3, 3, 23, 23, fill=BRASS, width=0)
            self.rtxt.config(text="radar is talking serial", foreground=BRASS)
            self.rsub.config(text=f"{r.get('edges_s', 0)} edges/s on GPIO{r.get('gpio')}"); return
        here, t = r.get("present"), int(r.get("since_s", 0))
        span = f"{t // 60} min" if t >= 120 else f"{t} s"
        self.rdot.create_oval(3, 3, 23, 23, fill=OK if here else PANEL, outline=OK if here else DIM, width=2)
        self.rtxt.config(text=("someone's here" if here else "nobody") + f"  ({span})", foreground=INK if here else DIM)
        self.rsub.config(text=f"GPIO{r.get('gpio')}  level {int(bool(r.get('level')))}  {r.get('edges_s', 0)} edges/s")

    def draw_cam(self):
        cam = self.st.get("cam", {})
        if self.img is None:
            self.cam_note.config(text=self.cam_err or "waiting for a picture...")
            return
        im = self.img.resize(VIEW)
        d = ImageDraw.Draw(im)
        sx, sy = VIEW[0] / FRAME, VIEW[1] / FRAME
        seen = []
        for i, sl in enumerate(cam.get("slots", [])):
            if not sl.get("seen"): continue
            x, y, w, h = sl["x"], sl["y"], sl["w"], sl["h"]
            col = RED if i == 0 else BLUE
            d.rectangle([(x - w / 2) * sx, (y - h / 2) * sy, (x + w / 2) * sx, (y + h / 2) * sy], outline=col, width=3)
            d.text(((x - w / 2) * sx + 4, (y - h / 2) * sy + 3), sl["name"], fill="white")
            seen.append(sl["name"])
        self.tkimg = ImageTk.PhotoImage(im)
        self.cam.config(image=self.tkimg)
        if self.cam_err: note = self.cam_err
        elif not cam.get("ok"): note = f"camera {cam.get('ribbon', '')}: no color reports - is eye.py running?"
        else: note = f"camera {cam.get('ribbon', '')}: sees " + (", ".join(seen) if seen else "nothing it knows")
        self.cam_note.config(text=note)

    # ---------------------------------------------------------------- commands
    def send(self, text):
        text = text.strip()
        if not text: return
        self.reply.config(text=f"> {text} ...")
        self.bg(lambda: self.b.cmd(text), lambda r: self.reply.config(
            text=f"> {text}: " + (str(r) if isinstance(r, Exception) else r.get("msg", ""))))

    # ---------------------------------------------------------------- bell
    def lock(self, on):
        self.busy = on
        for w in self.bell_btns: w.config(state="disabled" if on else "normal")

    def strike(self):
        if self.busy: return
        self.lock(True)
        ms = self.ms.get()
        def done(r):
            self.lock(False)
            if isinstance(r, Exception) or not r.get("ok"):
                self.bell_note.config(text="no bell: " + (str(r) if isinstance(r, Exception) else "this firmware has no solenoid set up")); return
            self.show_ring(r)
        self.bg(lambda: self.b.bell(ms=ms), done)

    def show_ring(self, r, title=None):
        pre, m1, m2 = r.get("pre", 0), r.get("mic1", []), r.get("mic2", [])
        c = self.plot; c.delete("all")
        W, H = c.winfo_width() or 360, 150
        lo, hi = -80, 0
        y = lambda db: H - (max(lo, min(hi, db)) - lo) / (hi - lo) * H
        for g in (-60, -40, -20):
            c.create_line(0, y(g), W, y(g), fill="#3d3226"); c.create_text(W - 4, y(g) - 7, anchor="e", fill=DIM, text=f"{g}")
        if pre and m1: c.create_line(pre / len(m1) * W, 0, pre / len(m1) * W, H, fill=BRASS, dash=(3, 3))
        for arr, col in ((m1, RED), (m2, BLUE)):
            if len(arr) > 1 and max(arr) > -98:
                c.create_line(*[v for i, db in enumerate(arr) for v in (i / (len(arr) - 1) * W, y(db))], fill=col, width=2)
        a, b = ring_stats(m1, pre), ring_stats(m2, pre) if m2 and max(m2) > -98 else None
        txt = title or f"{r.get('ms')} ms pulse"
        if a: txt += f":  mic1 rang {a['rise']:.0f} dB over the room for {a['ring_ms']} ms" + ("  (clipping)" if a["clip"] else "")
        if b: txt += f";  mic2 {b['rise']:.0f} dB, {b['ring_ms']} ms (mic2 {b['peak'] - a['peak']:+.1f} dB vs mic1)" if a else ""
        if not a: txt += ": the mic didn't hear it"
        self.bell_note.config(text=txt)
        return a

    def autotune(self):
        if self.busy: return
        if not messagebox.askyesno("Auto-tune the bell", f"Strike the bell {2 * len(SWEEP)} times at different pulse lengths "
                                   "and keep the one that rings loudest and longest?\n\nKeep the shop quiet while it runs (about a minute)."):
            return
        self.lock(True)
        def run():
            res = []
            for ms in SWEEP:
                scores = []
                for _ in range(2):
                    r = self.b.bell(ms=ms)
                    if not r.get("ok"): raise RuntimeError("this firmware has no solenoid set up")
                    a = ring_stats(r.get("mic1", []), r.get("pre", 0))
                    self.root.after(0, self.show_ring, r, f"trying {ms} ms")
                    # loud and long; a clipped peak can't show a difference in loudness, so ring time decides
                    if a: scores.append(max(0, a["rise"]) * (a["ring_ms"] / 1000 + 0.05))
                    time.sleep(0.4)
                res.append((statistics.mean(scores) if scores else 0, ms))
            return res
        def done(res):
            self.lock(False)
            if isinstance(res, Exception): self.bell_note.config(text=f"auto-tune stopped: {res}"); return
            best = max(res)[1]
            self.ms.set(best); self.ms_lbl.config(text=f"{best} ms")
            table = "  ".join(f"{ms}:{s:.0f}" for s, ms in res)
            self.bell_note.config(text=f"Best ring at {best} ms (score per pulse  {table}). Press Save pulse to keep it.")
        self.bg(run, done)

    def save_pulse(self):
        if self.busy: return
        ms = self.ms.get()
        self.lock(True)
        self.bg(lambda: self.b.bell(ms=ms, save=True, listen=False),
                lambda r: (self.lock(False), self.bell_note.config(
                    text=f"saved: \"ring the bell\" now uses {ms} ms" if not isinstance(r, Exception) and r.get("ok") else f"not saved: {r}")))

    # ---------------------------------------------------------------- mics
    def set_gain(self):
        g = self.gain.get()
        self.bg(lambda: self.b.voice(gain=g), lambda r: self.mic_note.config(text=f"gain {g} dB: " + (str(r) if isinstance(r, Exception) else r.get("msg", "set"))))

    def level_voice(self):
        self.mic_note.config(text="Talk normally from where you'll stand, for 5 seconds...")
        def run():
            peak, t0 = 0, time.time()
            while time.time() - t0 < 5:
                peak = max(peak, self.st.get("voice", {}).get("level", 0)); time.sleep(0.1)
            return peak
        def done(peak):
            if isinstance(peak, Exception): self.mic_note.config(text=str(peak)); return
            db = peak * 0.6 - 60
            g = self.gain.get()
            want = max(0, min(37, round(g + (-9 - db))))     # voice peaks around -9 dBFS: loud enough, room before clipping
            if peak < 5: self.mic_note.config(text="Didn't hear anything. Check the mic or talk louder."); return
            self.gain.set(want); self.gain_lbl.config(text=f"{want} dB")
            self.mic_note.config(text=f"Your voice peaked at {db:.0f} dBFS with {g} dB gain. Suggest {want} dB (aim -9 dBFS). Press Set gain.")
        self.bg(run, done)

    def set_dual(self):
        on = self.dual.get()
        if not messagebox.askyesno("Mics", f"{'Use both mics' if on else 'Use one mic'}? Johnny restarts (about 15 s)."):
            self.dual.set(not on); return
        self.bg(lambda: self.b.voice(dual=int(on)), lambda r: self.mic_note.config(text=str(r) if isinstance(r, Exception) else r.get("msg", "")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board")
    a = ap.parse_args()
    root = tk.Tk()
    try: root.iconbitmap(default="")
    except tk.TclError: pass
    App(root, Board(a.board or load_board()))
    root.mainloop()


if __name__ == "__main__":
    main()
