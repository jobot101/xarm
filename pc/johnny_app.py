"""johnny_app.py — Shop MotherShip's desktop app: see what it sees, tune the bell and the mics, send him commands.

    python johnny_app.py                     # board from johnny.json (johnny.local)
    python johnny_app.py --board 192.168.1.27

Talks to the board's web API only (/api/state, /cam.jpg, /api/bell, /api/voice, /api/cmd), so it runs alongside
johnny.py (the brain) and eye.py (the color finder) without getting in their way. Needs: requests, pillow.
"""
import argparse, io, json, os, socket, statistics, threading, time
import tkinter as tk
from tkinter import ttk, messagebox

import requests
from PIL import Image, ImageDraw, ImageTk

HERE = os.path.dirname(os.path.abspath(__file__))
FRAME = 240                                   # blob coordinates are on a 240x240 frame (board core.h)
VIEW = (400, 300)
SWEEP = [10, 15, 20, 30, 40, 55, 70, 90, 120]  # bell auto-tune: pulse lengths tried, ms
COMMANDS = ["ring the bell", "hold this for me", "grab it", "let go", "put it down",
            "give it to me", "go home", "wave hello", "take a bow", "dance robot"]

# colors: oak, brass and ink, like the board's web app
BG, PANEL, INK, DIM, BRASS, RED, BLUE, OK = "#211a14", "#2d241c", "#f1e6d2", "#a8977e", "#c9a14a", "#d0573f", "#5b8fd6", "#7bb36a"


def load_board():
    try: return json.load(open(os.path.join(HERE, "johnny.json"))).get("board", "shop-mothership.local")
    except (OSError, ValueError): return "shop-mothership.local"


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


def resolve(host):
    """The board's address. Windows often can't look up "johnny.local" (mDNS), so the last address that worked is
    kept in board_ip.txt and used when the name doesn't resolve."""
    cache = os.path.join(HERE, "board_ip.txt")
    try:
        ip = socket.gethostbyname(host)
        if not host.replace(".", "").isdigit():
            with open(cache, "w") as f: f.write(ip)
        return ip
    except OSError:
        try: return open(cache).read().strip() or host
        except OSError: return host


class Board:
    def __init__(self, host):
        self.host = host
        self.base = "http://" + resolve(host)
        self.s = requests.Session()

    def state(self):
        try: return self.s.get(self.base + "/api/state", timeout=6).json()
        except requests.ConnectionError:
            self.base = "http://" + resolve(self.host)      # the board may have a new address: look again
            raise
    def jpg(self): return self.s.get(self.base + "/cam.jpg", timeout=4)
    def post(self, path, body, timeout=8): return self.s.post(self.base + path, json=body, timeout=timeout).json()
    def cmd(self, text): return self.post("/api/cmd", {"text": text})
    def bell(self, **kw): return self.post("/api/bell", kw, timeout=15)
    def voice(self, **kw): return self.post("/api/voice", kw)


class App:
    def __init__(self, root, board):
        self.root, self.b = root, board
        self.st, self.img, self.cam_err, self.busy = {}, None, "", False
        root.title("Shop MotherShip")
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
        eyes = ttk.LabelFrame(left, text="What MotherShip sees", padding=8); eyes.pack(fill="x")
        # a pixel-sized placeholder: a Label with no image measures width/height in text characters
        self.tkimg = ImageTk.PhotoImage(Image.new("RGB", VIEW, "black"))
        self.cam = tk.Label(eyes, image=self.tkimg, bg="black", bd=0); self.cam.pack()
        self.cam_note = ttk.Label(eyes, text="", style="Dim.TLabel"); self.cam_note.pack(anchor="w", pady=(6, 0))

        # ---- controls: tabs
        s.configure("TNotebook", background=BG, borderwidth=0)
        s.configure("TNotebook.Tab", background=PANEL, foreground=DIM, padding=(10, 4))
        s.map("TNotebook.Tab", background=[("selected", "#3a2f24")], foreground=[("selected", BRASS)])
        s.configure("TFrame", background=BG)
        nb = ttk.Notebook(left); nb.pack(fill="both", expand=True, pady=(10, 0))
        tabs = {}
        for name in ("Quick", "Drive", "Joints", "Teach", "Tricks", "All commands"):
            tabs[name] = ttk.Frame(nb, padding=8); nb.add(tabs[name], text=name)
        self.build_quick(tabs["Quick"]); self.build_drive(tabs["Drive"]); self.build_joints(tabs["Joints"])
        self.build_teach(tabs["Teach"]); self.build_tricks(tabs["Tricks"]); self.build_all(tabs["All commands"])
        row = ttk.Frame(left); row.pack(fill="x", pady=(6, 0))
        ttk.Label(row, text="Say to MotherShip").pack(side="left", padx=(0, 6))
        self.say = ttk.Entry(row); self.say.pack(side="left", fill="x", expand=True)
        self.say.bind("<Return>", lambda e: self.send(self.say.get()))
        ttk.Button(row, text="Send", command=lambda: self.send(self.say.get())).pack(side="left", padx=(6, 0))
        self.reply = ttk.Label(left, text="", style="Dim.TLabel"); self.reply.pack(anchor="w", pady=(4, 0))

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
        ttk.Checkbutton(mic, text="Use both mics (restarts MotherShip)", variable=self.dual, command=self.set_dual).pack(anchor="w", pady=(6, 0))
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
                    self.cam_err = ("camera resting: nobody at the bench (radar)" if r.status_code == 503 else
                                    "no camera picture: this firmware has no ribbon camera yet" if r.status_code in (200, 404, 405)
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
            bt = st.get("boot", {})
            up = bt.get("uptime_s", 0)
            boot = f"    up {up // 60} min" if up >= 120 else f"    up {up} s" if bt else ""
            if bt and up < 600: boot += f" (last reset: {bt.get('reset')})"
            self.status.config(foreground=RED if bt.get("reset", "").startswith(("brownout", "crash", "watchdog", "power glitch")) and up < 600 else INK,
                               text=f"{st.get('state', '?')} - {st.get('detail', '')}    "
                               f"brain {'connected' if br.get('pc') else 'not connected'}    "
                               f"arm {batt:.1f} V    fw {st.get('fw', {}).get('version', '?')}{boot}")
            if not self.synced and v:
                self.gain.set(v.get("gain", 24)); self.gain_lbl.config(text=f"{v.get('gain', 24)} dB")
                self.dual.set(bool(v.get("dual"))); self.synced = True
            self.draw_radar(st.get("radar"))
            self.sync_arm(st)
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
        if r.get("kind") == "serial" and r.get("format") in (None, "unknown", "finding baud"):
            self.rdot.create_oval(3, 3, 23, 23, fill=BRASS, width=0)
            self.rtxt.config(text="radar: " + (r.get("format") or "talking serial"), foreground=BRASS)
            self.rsub.config(text=f"{r.get('baud') or '?'} baud  {(r.get('raw') or '')[:24]}"); return
        here, t = r.get("present"), int(r.get("since_s", 0))
        span = f"{t // 60} min" if t >= 120 else f"{t} s"
        self.rdot.create_oval(3, 3, 23, 23, fill=OK if here else PANEL, outline=OK if here else DIM, width=2)
        self.rtxt.config(text=("someone's here" if here else "nobody") + f"  ({span})", foreground=INK if here else DIM)
        d, ang = r.get("distance_cm", -1), r.get("angle_deg")
        where = (f"{d / 100:.1f} m" if here and d is not None and d >= 0 else "") +                 (f", {abs(ang)}° {'right' if ang > 0 else 'left'}" if here and ang not in (None, 0) else ", ahead" if here and ang == 0 else "")
        self.rsub.config(text=(where + "  " if where else "") +
                         (f"{r.get('format')} {r.get('baud')} baud" if r.get("kind") == "serial" else f"GPIO{r.get('gpio')} level {int(bool(r.get('level')))}"))

    def draw_cam(self):
        cam = self.st.get("cam", {})
        if self.img is None:
            self.cam_note.config(text=self.cam_err or "waiting for a picture...")
            return
        im = self.img.resize(VIEW)
        self.tkimg = ImageTk.PhotoImage(im)
        self.cam.config(image=self.tkimg)
        note = self.cam_err or f"camera {cam.get('ribbon', '')}: live view"
        self.cam_note.config(text=note)

    # ---------------------------------------------------------------- commands
    # ---------------------------------------------------------------- control tabs
    def act(self, path, body, what, confirm=None):
        """POST to the board and show its answer under the tabs."""
        if confirm and not messagebox.askyesno("Shop MotherShip", confirm): return
        self.reply.config(text=f"{what} ...")
        self.bg(lambda: self.b.post(path, body), lambda r: self.reply.config(
            text=f"{what}: " + (str(r) if isinstance(r, Exception) else r.get("msg", "ok" if r.get("ok") else "failed"))))

    def grid_buttons(self, parent, items, cols, start_row=0):
        for i, (label, fn) in enumerate(items):
            ttk.Button(parent, text=label, command=fn).grid(row=start_row + i // cols, column=i % cols, sticky="ew", padx=2, pady=2)
        for c in range(cols): parent.columnconfigure(c, weight=1)

    def build_quick(self, f):
        self.grid_buttons(f, [(c, lambda c=c: self.send(c)) for c in COMMANDS], 5)

    DRIVE = [("base", "Base", "◀ left", "right ▶"), ("lift", "Lift", "▼ down", "up ▲"), ("reach", "Reach", "◀ in", "out ▶"),
             ("tilt", "Wrist tilt", "▼", "▲"), ("twist", "Wrist twist", "↺", "↻"), ("grip", "Gripper", "open", "close")]

    def build_drive(self, f):
        self.driving, self.drive_busy = None, False
        ttk.Label(f, text="Hold a button to move, let go to stop.", style="Dim.TLabel").grid(row=0, column=0, columnspan=6, sticky="w")
        for i, (axis, name, neg, pos) in enumerate(self.DRIVE):
            r, c = 1 + i // 2, (i % 2) * 3
            ttk.Label(f, text=name).grid(row=r, column=c, sticky="e", padx=(8, 4), pady=2)
            for k, (txt, sign) in enumerate(((neg, -1), (pos, 1))):
                b = ttk.Button(f, text=txt, width=9); b.grid(row=r, column=c + 1 + k, sticky="ew", padx=2, pady=2)
                b.bind("<ButtonPress-1>", lambda e, a=axis, sg=sign: self.drive_start(a, sg))
                b.bind("<ButtonRelease-1>", lambda e: self.drive_stop())
        r = ttk.Frame(f); r.grid(row=5, column=0, columnspan=6, sticky="ew", pady=(8, 0))
        ttk.Label(r, text="Speed").pack(side="left")
        self.speed = tk.IntVar(value=10)
        ttk.Scale(r, from_=3, to=30, variable=self.speed).pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(r, text="Limp (pose by hand)", command=lambda: self.act("/api/torque", {"on": False}, "limp")).pack(side="left", padx=2)
        ttk.Button(r, text="Hold", command=lambda: self.act("/api/torque", {"on": True}, "hold")).pack(side="left", padx=2)
        ttk.Button(r, text="Go home", command=lambda: self.send("go home")).pack(side="left", padx=2)

    def drive_start(self, axis, sign):
        self.driving = (axis, sign); self.drive_tick()

    def drive_stop(self): self.driving = None

    def drive_tick(self):
        if not self.driving: return
        if not self.drive_busy:                        # one request in flight at a time: a slow board never piles up moves
            axis, sign = self.driving
            body = {axis: sign, "step": int(self.speed.get()), "ms": 120}
            self.drive_busy = True
            def done(r):
                self.drive_busy = False
                if isinstance(r, dict) and not r.get("ok"): self.reply.config(text="drive: " + r.get("msg", "refused"))
            self.bg(lambda: self.b.post("/api/drive", body, timeout=2), done)
        self.root.after(100, self.drive_tick)

    JOINTS = ["Gripper", "Wrist twist", "Wrist", "Elbow", "Shoulder", "Base"]

    def build_joints(self, f):
        self.jvars, self.jlbls, self.jscales, self.jdrag, self.jsent = [], [], [], None, 0
        for i, name in enumerate(self.JOINTS):
            ttk.Label(f, text=name, width=11).grid(row=i, column=0, sticky="w", pady=1)
            v = tk.IntVar(value=500)
            sc = ttk.Scale(f, from_=0, to=1000, variable=v, command=lambda val, i=i: self.joint_move(i, val))
            sc.grid(row=i, column=1, sticky="ew", padx=6)
            sc.bind("<ButtonPress-1>", lambda e, i=i: setattr(self, "jdrag", i))
            sc.bind("<ButtonRelease-1>", lambda e, i=i: self.joint_release(i))
            lb = ttk.Label(f, text="500", width=5); lb.grid(row=i, column=2, sticky="e")
            self.jvars.append(v); self.jlbls.append(lb); self.jscales.append(sc)
        f.columnconfigure(1, weight=1)
        ttk.Label(f, text="Drag a slider to move one joint. The arm follows smoothly.", style="Dim.TLabel").grid(row=6, column=0, columnspan=3, sticky="w", pady=(4, 0))

    def joint_move(self, i, val):
        p = int(float(val)); self.jlbls[i].config(text=str(p))
        if self.jdrag != i or time.time() - self.jsent < 0.1: return
        self.jsent = time.time()
        self.bg(lambda: self.b.post("/api/jog", {"id": i + 1, "pos": p}, timeout=2))   # servo bus ID = joint + 1

    def joint_release(self, i):
        p = int(self.jvars[i].get())
        self.bg(lambda: self.b.post("/api/jog", {"id": i + 1, "pos": p}, timeout=2), lambda r: setattr(self, "jdrag", None))

    TEACH = [("Home", "home", "where he rests"), ("Drop", "drop", "where \"put it down\" sets things"),
             ("Hand-off", "give", "where \"give it to me\" hands you things"), ("Helper", "helper", "the \"hold this for me\" spot"),
             ("Bell", "bell", "over the bell")]

    def build_teach(self, f):
        ttk.Label(f, text="Pose the arm (Drive, Joints or Limp), then save where it is as:", style="Dim.TLabel").grid(row=0, column=0, columnspan=5, sticky="w")
        self.grid_buttons(f, [(lbl, lambda w=w, lbl=lbl, why=why: self.act("/api/teach", {"what": w}, f"taught {lbl}",
                                confirm=f"Save the arm's pose right now as {lbl} ({why})?")) for lbl, w, why in self.TEACH], 5, 1)


    def build_tricks(self, f):
        top = ttk.Frame(f); top.pack(fill="x")
        ttk.Button(top, text="● Record", command=lambda: self.act("/api/moves", {"action": "record"}, "recording: move the arm")).pack(side="left", padx=2)
        ttk.Label(top, text="name").pack(side="left", padx=(10, 4))
        self.trick_name = ttk.Entry(top, width=14); self.trick_name.pack(side="left")
        ttk.Button(top, text="Save", command=lambda: self.act("/api/moves", {"action": "save", "name": self.trick_name.get().strip() or "trick"}, "saved")).pack(side="left", padx=2)
        ttk.Button(top, text="Cancel", command=lambda: self.act("/api/moves", {"action": "cancel"}, "cancelled")).pack(side="left", padx=2)
        self.rec_lbl = ttk.Label(f, text="Record, move the arm (Limp it and pose by hand, or Drive), then Save with a name. "
                                 "Then \"MotherShip, do the <name>\" plays it.", style="Dim.TLabel", wraplength=520)
        self.rec_lbl.pack(anchor="w", pady=(6, 4))
        self.trick_box = ttk.Frame(f); self.trick_box.pack(fill="x")
        self.tricks_seen = None

    def build_all(self, f):
        top = ttk.Frame(f); top.pack(fill="x")
        ttk.Label(top, text="Everything MotherShip can do (tap to run):", style="Dim.TLabel").pack(side="left")
        ttk.Button(top, text="Refresh", command=self.load_phrases).pack(side="right")
        cv = tk.Canvas(f, bg=BG, highlightthickness=0, height=150); sb = ttk.Scrollbar(f, orient="vertical", command=cv.yview)
        self.phr = ttk.Frame(cv); self.phr.bind("<Configure>", lambda e: cv.configure(scrollregion=cv.bbox("all")))
        cv.create_window((0, 0), window=self.phr, anchor="nw"); cv.configure(yscrollcommand=sb.set)
        cv.pack(side="left", fill="both", expand=True, pady=(4, 0)); sb.pack(side="right", fill="y", pady=(4, 0))
        cv.bind_all("<MouseWheel>", lambda e: cv.yview_scroll(-e.delta // 120, "units") if str(e.widget).startswith(str(cv)) else None)
        self.root.after(1500, self.load_phrases)

    def load_phrases(self):
        def done(r):
            if isinstance(r, Exception): self.reply.config(text=f"commands: {r}"); return
            board = r if isinstance(r, list) else r.get("phrases", [])
            # the board's own voice list on Johnny is just "stop"; the real vocabulary is the brain's action menu
            # (johnny.py COMMANDS, all plain router text) plus the camera colors and recorded tricks
            try:
                import johnny; brain = list(johnny.COMMANDS)
            except Exception: brain = list(COMMANDS)
            colors = []
            tricks = [f"do the {t[1]}" for t in self.tricks_seen or []]
            ph, seen = [], set()
            for p in brain + colors + tricks + board:
                if p not in seen: seen.add(p); ph.append(p)
            for w in self.phr.winfo_children(): w.destroy()
            self.grid_buttons(self.phr, [(p, lambda p=p: self.send(p)) for p in ph], 4)
            self.reply.config(text=f"{len(ph)} commands")
        self.bg(lambda: self.b.s.get(self.b.base + "/api/phrases", timeout=6).json(), done)

    def sync_arm(self, st):
        arm, mv = st.get("arm", {}), st.get("moves", {})
        pos, lim = arm.get("pos") or [], arm.get("lim") or []
        for i, p in enumerate(pos[:len(self.jvars)]):
            if i < len(lim): self.jscales[i].config(from_=lim[i][0], to=lim[i][1])
            if self.jdrag != i:
                self.jvars[i].set(p); self.jlbls[i].config(text=str(p))
        if mv.get("recording"): self.rec_lbl.config(text=f"● recording: {mv.get('frames', 0)} frames so far. Save or Cancel when done.")
        tl = [(t.get("i", k), t.get("name", "")) if isinstance(t, dict) else (k, t) for k, t in enumerate(mv.get("list") or [])]
        if tl != self.tricks_seen:
            self.tricks_seen = tl
            for w in self.trick_box.winfo_children(): w.destroy()
            if not tl: ttk.Label(self.trick_box, text="No tricks recorded yet.", style="Dim.TLabel").grid(row=0, column=0, sticky="w")
            for row, (slot, name) in enumerate(tl):          # slot = the board's move number (gaps after a delete)
                ttk.Label(self.trick_box, text=name).grid(row=row, column=0, sticky="w", padx=(0, 10))
                ttk.Button(self.trick_box, text="Play", command=lambda i=slot, n=name: self.act("/api/moves", {"action": "play", "i": i}, f"play {n}")).grid(row=row, column=1, padx=2)
                ttk.Button(self.trick_box, text="Delete", command=lambda i=slot, n=name: self.act("/api/moves", {"action": "delete", "i": i}, f"deleted {n}",
                                                                                                   confirm=f"Delete the trick \"{n}\"?")).grid(row=row, column=2, padx=2)

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
        if not messagebox.askyesno("Mics", f"{'Use both mics' if on else 'Use one mic'}? MotherShip restarts (about 15 s)."):
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
