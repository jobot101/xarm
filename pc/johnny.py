"""johnny.py — Johnny's brain, running on the shop PC.

The arm board (Johnny build) is ears, mouth and hands; this is everything else:
  mic audio from the board -> voice detector -> Whisper (local, faster-whisper)
  -> "Johnny ..." -> the local model in LM Studio picks what to say + which arm commands to run
  -> ElevenLabs speaks it in Johnny's voice (Windows' own voice if that's down) -> back to the board's speaker.

    python johnny.py                 # board at johnny.local
    python johnny.py --board 192.168.1.50
    python johnny.py --text          # no board: type to Johnny, see what he'd say and do

Settings live in johnny.json next to this file (made on first run). Keys can also come from the environment:
ELEVENLABS_API_KEY. "stop" is handled on the board itself, so it works even with this program down.
"""
import argparse, asyncio, io, json, os, re, sys, threading, time, wave
from collections import deque

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CFG_PATH = os.path.join(HERE, "johnny.json")
DEFAULTS = {
    "board": "johnny.local",
    "lmstudio_url": "http://localhost:1234/v1",
    "model": "",                          # blank = whatever LM Studio has loaded
    "whisper_model": "auto",              # auto: large-v3-turbo on a GPU, small.en on CPU
    "elevenlabs_api_key": "",
    "elevenlabs_voice_id": "",            # Johnny's voice (the same one the Oracle phone uses)
    "elevenlabs_model": "eleven_flash_v2_5",
    "volume": 80,
    "follow_up_s": 10,                    # after he answers, no need to say "Johnny" for this long
}
RATE = 16000
WAKE = re.compile(r"\b(hey |ok |okay )?(jo(h)?n+(y|ie|ey|i)?|johnny)\b[,.!?]?", re.I)

# Plain router text the board understands (firmware/main/router.c). The model may only pick from these.
COMMANDS = {
    "hold this for me": "third hand: reach out to the helper spot with the gripper open, ready for a part",
    "grab it":          "close the gripper on what's in it",
    "let go":           "open the gripper and let go of what it holds",
    "give it to me":    "carry what it's holding to the hand-off spot and let go there",
    "put it down":      "set what it's holding down at the drop spot",
    "go home":          "go back to the resting pose",
    "a little left":    "nudge the whole arm left", "a little right": "nudge the whole arm right",
    "a little up":      "nudge up", "a little down": "nudge down",
    "turn left":        "twist the wrist left", "turn right": "twist the wrist right",
    "tilt up":          "tilt the wrist up", "tilt down": "tilt the wrist down",
    "open your hand":   "open the gripper", "close your hand": "close the gripper",
    "wave hello":       "wave", "take a bow": "bow", "dance robot": "dance",
    "ring the bell":    "ring the bell",
    "stop":             "stop everything right now",
}

PERSONA = """You are Johnny, the shop helper at Ragamuffin Workshop in McCoysville, Pennsylvania: a small six-joint
robot arm on the bench, with a deep, unhurried, plainspoken voice. You're the man in black from the old records
come back as a workshop hand: dry humor, a little gravel, a lot of patience, respect for honest work. You call
people "friend" now and then, never overdo it. You never say your last name, and you never say "Cash" at all.

You talk OUT LOUD through a small speaker in a working shop, and you are a man of very few words. Most of the
time say nothing at all: leave "say" empty and just do the job. When a word helps, use two to five, like
"Got it." or "Can't lift that." A question gets one short sentence, never more. No lists, no emoji, no stage
directions, no markdown, no small talk, no asking how they are. Numbers in words when spoken naturally.

You are a real arm. What you can do is ONLY the actions listed. {sight} You're small: light parts only, about the weight of a phone. Never hold anything sharp, hot or
spinning near someone's hand; say so plainly. When someone asks you to hold something, use "hold this for me",
then "grab it" once they say it's in your hand. "a little ..." nudges are for fine positioning.

If they're just talking, talk back and do nothing. If they ask for something you can't do, say so in a line.
Answer workshop questions (CNC, wood, brass, electronics, guitars) briefly and truly; if you don't know, say so."""


def load_cfg():
    cfg = dict(DEFAULTS)
    if os.path.exists(CFG_PATH):
        cfg.update(json.load(open(CFG_PATH)))
    else:
        json.dump(DEFAULTS, open(CFG_PATH, "w"), indent=2)
        print(f"made {CFG_PATH}: put Johnny's ElevenLabs voice id + key in it")
    cfg["elevenlabs_api_key"] = cfg["elevenlabs_api_key"] or os.environ.get("ELEVENLABS_API_KEY", "")
    return cfg


# ------------------------------------------------------------------ brain: LM Studio (OpenAI-compatible)
class Brain:
    def __init__(self, cfg):
        import requests
        self.rq, self.url, self.model = requests, cfg["lmstudio_url"].rstrip("/"), cfg["model"]
        self.history = deque(maxlen=12)
        self.moves = []
        self.slots = []                   # colors the camera can pick, from the board's hello (empty = no eyes)

    def _model(self):
        if not self.model:
            ms = self.rq.get(self.url + "/models", timeout=5).json()["data"]
            self.model = next((m["id"] for m in ms if "embed" not in m["id"]), ms[0]["id"])
            print("brain:", self.model)
        return self.model

    def think(self, heard, arm_state):
        cmds = dict(COMMANDS)
        for s in self.slots:
            cmds[f"pick up the {s}"] = f"find the {s} thing on the mat with the camera, grab it and lift it"
        sight = (f"You have a camera over the mat: you can find and pick up {' and '.join(self.slots)} things, nothing else."
                 if self.slots else "You have no camera: you can't see, so don't claim to.")
        for m in self.moves: cmds[f"do the {m}"] = f"play the recorded move '{m}'"
        menu = "\n".join(f'- "{k}": {v}' for k, v in cmds.items())
        sys_p = (PERSONA.replace("{sight}", sight) + f"\n\nActions (run in order, at most 3):\n{menu}\n\nThe arm right now: {arm_state}.\n"
                 'Reply as JSON: {"say": "<what you say out loud>", "actions": ["<action>", ...]}')
        schema = {"type": "object", "additionalProperties": False, "required": ["say", "actions"], "properties": {
            "say": {"type": "string"},
            "actions": {"type": "array", "maxItems": 3, "items": {"type": "string", "enum": list(cmds)}}}}
        msgs = [{"role": "system", "content": sys_p}, *self.history, {"role": "user", "content": heard}]
        r = self.rq.post(self.url + "/chat/completions", timeout=60, json={
            "model": self._model(), "messages": msgs, "temperature": 0.7, "max_tokens": 200,
            "response_format": {"type": "json_schema", "json_schema": {"name": "reply", "strict": True, "schema": schema}}})
        r.raise_for_status()
        txt = r.json()["choices"][0]["message"]["content"]
        try:
            out = json.loads(re.sub(r"^```(json)?|```$", "", txt.strip()))
        except ValueError:
            out = {"say": txt.strip()[:300], "actions": []}
        out["actions"] = [a for a in out.get("actions", []) if a in cmds][:3]
        out["say"] = re.sub(r"\bCash\b", "", out.get("say", "")).strip()
        self.history.append({"role": "user", "content": heard})
        self.history.append({"role": "assistant", "content": json.dumps(out)})
        return out


# ------------------------------------------------------------------ voice: ElevenLabs, Windows voice as backup
class Voice:
    def __init__(self, cfg):
        import requests
        self.rq, self.cfg = requests, cfg
        self.cloud_ok = bool(cfg["elevenlabs_api_key"] and cfg["elevenlabs_voice_id"])
        if not self.cloud_ok: print("voice: no ElevenLabs key/voice id in johnny.json, using the Windows voice")

    def stream(self, text):
        """yields 16 kHz mono int16 PCM chunks as they arrive"""
        if self.cloud_ok:
            try:
                c = self.cfg
                r = self.rq.post(f"https://api.elevenlabs.io/v1/text-to-speech/{c['elevenlabs_voice_id']}/stream",
                                 params={"output_format": "pcm_16000", "optimize_streaming_latency": 3},
                                 headers={"xi-api-key": c["elevenlabs_api_key"]}, stream=True, timeout=20,
                                 json={"text": text, "model_id": c["elevenlabs_model"],
                                       "voice_settings": {"stability": 0.5, "similarity_boost": 0.85}})
                r.raise_for_status()
                left = b""
                for chunk in r.iter_content(4096):
                    chunk = left + chunk
                    cut = len(chunk) & ~1
                    left = chunk[cut:]
                    if cut: yield chunk[:cut]
                return
            except Exception as e:
                print("voice: ElevenLabs failed, Windows voice instead:", e)
        yield self.local(text)

    def local(self, text):
        import pyttsx3, tempfile
        f = os.path.join(tempfile.gettempdir(), "johnny_say.wav")
        eng = pyttsx3.init()
        for v in eng.getProperty("voices"):
            if "david" in v.name.lower(): eng.setProperty("voice", v.id)   # the deepest stock Windows voice
        eng.setProperty("rate", 165)
        eng.save_to_file(text, f)
        eng.runAndWait()
        with wave.open(f) as w:
            sr, ch, x = w.getframerate(), w.getnchannels(), np.frombuffer(w.readframes(w.getnframes()), np.int16)
        if ch > 1: x = x.reshape(-1, ch).mean(1)
        n = int(len(x) * RATE / sr)
        y = np.interp(np.linspace(0, len(x) - 1, n), np.arange(len(x)), x.astype(np.float32))
        return y.astype(np.int16).tobytes()


# ------------------------------------------------------------------ ears: voice detector + Whisper
class Ears:
    FRAME = 480                           # 30 ms

    def __init__(self, cfg):
        import webrtcvad
        from faster_whisper import WhisperModel
        self.vad = webrtcvad.Vad(2)
        m = cfg["whisper_model"]
        try:
            import ctranslate2
            gpu = ctranslate2.get_cuda_device_count() > 0
        except Exception:
            gpu = False
        if m == "auto": m = "large-v3-turbo" if gpu else "small.en"
        self.model = None
        if gpu and cfg.get("whisper_device", "auto") != "cpu":
            # a GPU can be there without CUDA's cuBLAS/cuDNN installed: that only shows when it first transcribes,
            # so try one second of silence now and fall back to the CPU (same model, no new download)
            try:
                self.model = WhisperModel(m, device="cuda", compute_type="float16")
                list(self.model.transcribe(np.zeros(RATE, np.float32), language="en")[0])
                print(f"ears: whisper {m} on GPU")
            except Exception as e:
                print(f"ears: GPU not usable ({str(e)[:70]}), using the CPU")
                self.model = None
        if self.model is None:
            print(f"ears: whisper {m} on CPU (first run downloads it)")
            self.model = WhisperModel(m, device="cpu", compute_type="int8")
        self.buf, self.ring, self.seg, self.quiet, self.on = b"", deque(maxlen=10), [], 0, False

    def feed(self, pcm):
        """mic bytes in; returns a finished utterance (int16 bytes) or None"""
        self.buf += pcm
        done = None
        while len(self.buf) >= self.FRAME * 2:
            f, self.buf = self.buf[:self.FRAME * 2], self.buf[self.FRAME * 2:]
            sp = self.vad.is_speech(f, RATE)
            if not self.on:
                self.ring.append((f, sp))
                if sum(s for _, s in self.ring) >= 6:              # 180 ms of voice in the last 300: start
                    self.on, self.seg, self.quiet = True, [x for x, _ in self.ring], 0
            else:
                self.seg.append(f)
                self.quiet = 0 if sp else self.quiet + 1
                if self.quiet >= 22 or len(self.seg) > 400:        # 660 ms pause, or 12 s
                    self.on = False
                    self.ring.clear()
                    if len(self.seg) > 15: done = b"".join(self.seg)
        return done

    def reset(self):
        self.buf, self.on = b"", False
        self.ring.clear()

    def hear(self, pcm):
        x = np.frombuffer(pcm, np.int16).astype(np.float32) / 32768
        segs, _ = self.model.transcribe(x, language="en", beam_size=1, vad_filter=False, condition_on_previous_text=False,
                                        initial_prompt="Johnny, hold this for me. Grab it. Let go. A little left. Put it down. Pick up the red.")
        return " ".join(s.text for s in segs if s.no_speech_prob < 0.6).strip()


# ------------------------------------------------------------------ the link to the board
class Johnny:
    def __init__(self, cfg, board):
        self.cfg, self.board = cfg, board
        self.brain, self.voice, self.ears = Brain(cfg), Voice(cfg), Ears(cfg)
        self.state = {"state": "idle", "holding": False, "busy": False}
        self.speaking_until = 0.0
        self.window_until = 0.0
        self.replies = {}
        self.next_id = 1
        self.loop = None
        self.busy = False                # between hearing something and finishing the answer

    async def send(self, **kw):
        await self.ws.send(json.dumps(kw))

    async def cmd(self, text):
        i = self.next_id; self.next_id += 1
        fut = self.loop.create_future(); self.replies[i] = fut
        await self.send(cmd=text, id=i)
        try:
            r = await asyncio.wait_for(fut, 5)
        except asyncio.TimeoutError:
            return {"ok": False, "msg": "no answer"}
        print(f"   arm: {text} -> {r.get('msg')}")
        return r

    async def wait_arm(self, timeout=25):
        t0 = time.time()
        await asyncio.sleep(0.4)
        while time.time() - t0 < timeout and self.state.get("busy"):
            await asyncio.sleep(0.2)

    async def say(self, text):
        if not text: return
        q = asyncio.Queue()
        def pump():                                   # the HTTP stream runs in a thread
            try:
                for c in self.voice.stream(text): self.loop.call_soon_threadsafe(q.put_nowait, c)
            finally:
                self.loop.call_soon_threadsafe(q.put_nowait, None)
        threading.Thread(target=pump, daemon=True).start()
        t0, sent = time.time(), 0
        while (c := await q.get()) is not None:
            for k in range(0, len(c), 4096):           # stay at most ~2 s ahead of the speaker
                part = c[k:k + 4096]
                ahead = sent / (RATE * 2) - (time.time() - t0)
                if ahead > 2: await asyncio.sleep(ahead - 2)
                await self.ws.send(part)
                sent += len(part)
                self.speaking_until = t0 + sent / (RATE * 2) + 0.6
        print(f"   said {sent / (RATE * 2):.1f} s")

    def arm_words(self):
        s = self.state
        return f"{s.get('state')}{', holding something' if s.get('holding') else ''}{', moving' if s.get('busy') else ''}"

    async def handle(self, text):
        now = time.time()
        woke = WAKE.search(text)
        if not woke and now > self.window_until:
            print(f"(not for me) {text}")
            return
        heard = WAKE.sub("", text, count=1).strip(" ,.!?") if woke else text
        print(f"heard: {text}")
        await self.send(led="think", attn=True)
        if not heard:
            heard = "(said your name and nothing else)"
        try:
            out = await self.loop.run_in_executor(None, self.brain.think, heard, self.arm_words())
        except Exception as e:
            print("brain error:", e)
            out = {"say": "Brain's down. Check LM Studio.", "actions": []}
        print(f"   johnny: {out['say']}  {out['actions']}")
        await self.send(led="idle")
        talk = asyncio.create_task(self.say(out["say"]))
        for a in out["actions"]:
            r = await self.cmd(a)
            if r.get("ok"): await self.wait_arm()
        await talk
        self.window_until = self.speaking_until + self.cfg["follow_up_s"]
        await self.send(led="listen", listening=True)

    async def run(self):
        import websockets
        self.loop = asyncio.get_running_loop()
        url = f"ws://{self.board}/ws"
        while True:
            try:
                print("connecting to", url)
                async with websockets.connect(url, max_size=None, ping_interval=None, open_timeout=8) as ws:
                    self.ws = ws
                    await self.serve()
            except (OSError, asyncio.TimeoutError, Exception) as e:
                print("board link:", type(e).__name__, e, "- retrying in 3 s")
                await asyncio.sleep(3)

    async def serve(self):
        work = asyncio.Queue()

        async def keepalive():
            while True:
                await asyncio.sleep(2)
                await self.send(ping=True)
                if not self.busy and time.time() > self.window_until: await self.send(led="idle", listening=False)

        async def worker():
            while True:
                pcm = await work.get()
                self.busy = True
                try:
                    text = await self.loop.run_in_executor(None, self.ears.hear, pcm)
                    if text: await self.handle(text)
                except Exception as e:
                    print("error:", e)
                finally:
                    self.busy = False

        tasks = [asyncio.create_task(keepalive()), asyncio.create_task(worker())]
        try:
            async for m in self.ws:
                if isinstance(m, bytes):
                    if self.busy or time.time() < self.speaking_until:
                        self.ears.reset(); continue          # half duplex: never hear himself
                    utt = self.ears.feed(m)
                    if utt: work.put_nowait(utt)
                    continue
                e = json.loads(m)
                ev = e.get("ev")
                if ev == "hello":
                    self.brain.moves = e.get("moves", [])
                    self.brain.slots = e.get("slots", []) if e.get("camera") else []
                    print(f"board: {e.get('name')} {e.get('version')} - moves {self.brain.moves} - eyes {self.brain.slots or 'off'}")
                    await self.send(volume=self.cfg["volume"])
                elif ev == "state":
                    self.state = e
                elif ev == "cmd" and e.get("id") in self.replies:
                    f = self.replies.pop(e["id"])
                    if not f.done(): f.set_result(e)
                elif ev == "stop":
                    print("board heard STOP")
                    self.speaking_until = 0
        finally:
            for t in tasks: t.cancel()


def text_mode(cfg):
    b = Brain(cfg)
    print("type to Johnny (ctrl-c to quit)")
    while True:
        try: t = input("> ")
        except (EOFError, KeyboardInterrupt): return
        out = b.think(t, "idle at home")
        print(f"  {out['say']}\n  actions: {out['actions']}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--board")
    ap.add_argument("--text", action="store_true", help="no board: type to the brain")
    a = ap.parse_args()
    cfg = load_cfg()
    if a.text: text_mode(cfg); sys.exit()
    asyncio.run(Johnny(cfg, a.board or cfg["board"]).run())
