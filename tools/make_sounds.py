#!/usr/bin/env python3
"""Render everything the robot says and plays into one sound bank for the firmware.

  voice : Piper neural TTS (open source, runs offline — no API at build or play time)
  bells : church-bell partials (hum, prime, tierce, quint, nominal...) with beating doublets + strike
  organ : principal pipe ranks 8' 4' 2' with chiff, wind and tremulant
  room  : a short church-hall reverb, then mastering for the board's little speaker
          (high-pass, presence lift, compression, limiter) and 16 kHz IMA ADPCM (4:1)

Outputs firmware/main/sounds.bin (embedded) and firmware/main/sounds_gen.h (clip groups).
CI runs this before every firmware build (.github/workflows/build.yml). Locally:

  pip install piper-tts numpy scipy
  python3 -m piper.download_voices en_US-ryan-high --download-dir ~/.cache/piper
  python3 tools/make_sounds.py                 # bank
  python3 tools/make_sounds.py --preview DIR   # WAVs of every clip + voice samples, to listen first
  python3 tools/make_sounds.py --no-voice      # silence for speech (no model needed), to test the build

Edit LINES to change what it says (each list = variants, one picked at random, never twice in a row).
"{c}" lines are rendered once per camera color, "{n}" once per number.
"""
import argparse, os, struct, sys, wave
import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(HERE, "..", "firmware", "main")
SR, OUT_SR = 48000, 16000
VOICE_SR = 12000                   # speech is stored at 12 kHz (the speaker has nothing above ~6 kHz); the
                                   # board upsamples it to 16 kHz as it plays

VOICE = "en_US-ryan-high"          # Piper voice — see --preview for others
VOICE_SPEED = 0.92                 # Piper length_scale: < 1 = brisker
ROBOT_TINT = 0.10                  # 0 = plain human voice; 0.1 = a faint metallic shimmer
BUDGET = 1_450_000                 # bytes of flash for the whole bank (app partition is 4 MB, app ~2.4 MB)
COLORS = ["red", "yellow", "green", "blue", "purple"]          # camera color order (cam/xarm_color/colors.h)
NUMBERS = ["one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"]

# ---------------------------------------------------------------- what it says
LINES = {
    "boot":        ["Good morning! The Ragamuffin Hall robot is awake.", "Lights up. I'm awake!"],
    "updated":     ["Ready to rumble!", "New tricks installed. Ready to rumble!"],
    "ok":          ["On it!", "You got it!", "Coming right up!"],
    "busy":        ["Hang on, I'm busy.", "One thing at a time!"],
    "search":      ["Let me take a look.", "Hmm, where is it?", "Scanning the table."],
    "search_c":    ["Looking for {c}.", "Where's that {c} one?"],
    "found":       ["There it is!", "Found it!", "Spotted it!"],
    "grab":        ["Easy does it.", "Steady.", "Here goes."],
    "got":         ["Got it!", "Gotcha!", "Nice and secure."],
    "missed":      ["Whoops! Let me try that again.", "Slippery one. Again!"],
    "not_found":   ["I can't find it.", "Nothing there. Can you move it closer?"],
    "not_found_c": ["I don't see any {c}.", "No {c} on the table."],
    "need_calib":  ["I need a lesson first. Teach me where things are."],
    "holding":     ["Holding on tight.", "I've got it."],
    "give":        ["Here you go!", "Special delivery!"],
    "given":       ["Enjoy!", "All yours."],
    "drop":        ["Down it goes.", "Dropping it."],
    "sorted":      ["All tidy!", "Neat as a pin."],
    "nothing":     ["Nothing to sort.", "The table's already clean."],
    "wave":        ["Hello there!", "Howdy!"],
    "bow":         ["Thank you, thank you!", "You're too kind."],
    "dance":       ["Let's dance!", "Music, maestro!"],
    "bell":        ["Ringing the bell!", "Ding dong!"],
    "play":        ["Here's my move!", "Watch this!"],
    "helper":      ["Put it in my hand, and say hold this.", "Ready. Put it in my hand."],
    "help_hold":   ["Holding it for you.", "I've got it, take your time."],
    "released":    ["Letting go.", "There you go."],
    "stack_n":     ["{n}!"],
    "tower":       ["What a tower!", "Bravo, builder!"],
    "stopped":     ["Stopping!", "Freezing!"],
    "error":       ["Oops.", "Hmm, that didn't work."],
    "cheer":       ["Bravo!", "Wonderful!"],
    "greet":       ["Hi! Great to see you!", "Welcome back, friend!"],
    "idle":        ["Anyone want to play?", "Try saying, pick up the red one.", "Ask me to dance!"],
    "home":        ["Heading home.", "Back to my spot."],
}
PREVIEW_TEXT = "Welcome to Ragamuffin Hall! Say, pick up the red one, and watch me go."
PREVIEW_VOICES = ["en_US-ryan-high", "en_US-joe-medium", "en_GB-alan-medium",
                  "en_GB-northern_english_male-medium", "en_US-lessac-high", "en_GB-jenny_dioco-medium"]

# ---------------------------------------------------------------- notes
C4, D4, E4, F4, G4, A4 = 262, 294, 330, 349, 392, 440
C5, D5, E5, G5, A5, C6 = 523, 587, 659, 784, 880, 1047
PENTA = [C5, D5, E5, G5, A5, C6]

rng = np.random.default_rng(7)


def t_axis(n): return np.arange(n) / SR


def bell(f, dur=3.0, vel=1.0):
    """Church bell: tuned partials as slightly detuned doublets (the slow 'wah'), each decaying at its own
    rate, plus a short metallic strike."""
    n = int(dur * SR); t = t_axis(n)
    #          ratio  amp   tau(s)
    parts = [(0.50, 0.55, 3.8), (1.00, 1.00, 2.2), (1.19, 0.60, 1.7), (1.50, 0.32, 1.1),
             (2.00, 0.50, 1.3), (2.50, 0.22, 0.7), (2.67, 0.25, 0.6), (3.00, 0.15, 0.45),
             (4.00, 0.12, 0.3), (5.33, 0.07, 0.18)]
    k = (440.0 / f) ** 0.5                        # low bells ring longer
    x = np.zeros(n)
    for r, a, tau in parts:
        fr = f * r
        if fr > SR / 2.2: continue
        beat = rng.uniform(0.4, 1.6)
        ph = rng.uniform(0, 2 * np.pi, 2)
        doublet = np.sin(2 * np.pi * (fr - beat / 2) * t + ph[0]) + 0.8 * np.sin(2 * np.pi * (fr + beat / 2) * t + ph[1])
        x += a * np.exp(-t / (tau * k)) * doublet
    strike = rng.standard_normal(int(0.012 * SR)) * np.exp(-np.arange(int(0.012 * SR)) / (0.002 * SR))
    b, a_ = signal.butter(2, [min(f * 2.5, 9000) / (SR / 2), min(f * 6, 20000) / (SR / 2)], "band")
    x[:len(strike)] += 0.9 * signal.lfilter(b, a_, strike)
    x *= np.minimum(1, t / 0.002)                 # 2 ms attack, no click
    return vel * x / np.max(np.abs(x))


def handbell(f, dur=1.2, vel=1.0):
    """Bright little chime (bar partials) for 'I heard you'."""
    n = int(dur * SR); t = t_axis(n)
    x = (np.sin(2 * np.pi * f * t) * np.exp(-t / 0.55) + 0.35 * np.sin(2 * np.pi * f * 2.76 * t) * np.exp(-t / 0.18)
         + 0.12 * np.sin(2 * np.pi * f * 5.40 * t) * np.exp(-t / 0.07))
    x *= np.minimum(1, t / 0.0015)
    return vel * x / np.max(np.abs(x))


def organ(f, ms, vel=1.0, trem=True):
    """Pipe organ, principal chorus 8' + 4' + 2': harmonic pipes, a breathy chiff on the attack,
    wind noise, gentle tremulant, slow-ish speech and release like real pipes."""
    hold = ms / 1000.0
    n = int((hold + 0.25) * SR); t = t_axis(n)
    x = np.zeros(n)
    ranks = [(1, [1, .42, .28, .16, .10, .07, .05, .03], 0), (2, [.5, .2, .1, .05], 3), (4, [.22, .08], -4)]
    for mult, harm, cents in ranks:
        fr = f * mult * 2 ** (cents / 1200)
        for h, a in enumerate(harm, 1):
            if fr * h < SR / 2.2:
                x += a * np.sin(2 * np.pi * fr * h * t + rng.uniform(0, 2 * np.pi))
    x += 0.015 * signal.lfilter(*signal.butter(2, [f / (SR / 2), min(f * 4, 20000) / (SR / 2)], "band"),
                                rng.standard_normal(n))                       # wind
    chiff_n = int(0.05 * SR)
    chiff = rng.standard_normal(chiff_n) * np.exp(-np.arange(chiff_n) / (0.012 * SR))
    x[:chiff_n] += 0.35 * signal.lfilter(*signal.butter(2, [min(f * 2.5, 20000) / (SR / 2), min(f * 4.5, 22000) / (SR / 2)], "band"), chiff)
    if trem: x *= 1 + 0.04 * np.sin(2 * np.pi * 5.6 * t)
    env = np.minimum(1, t / 0.035)                                             # pipe speech
    rel = t > hold
    env[rel] *= np.exp(-(t[rel] - hold) / 0.05)
    x *= env
    return vel * x / np.max(np.abs(x))


def seq(notes):
    """notes: (kind, freq, ms_to_next, vol 0..100). Bells ring past their slot; organ holds its slot."""
    total = sum(n[2] for n in notes) / 1000 + 3.5
    out = np.zeros(int(total * SR)); pos = 0
    tail = 1.3 if any(k == "bell" for k, *_ in notes) else 0.7
    for kind, f, ms, vol in notes:
        v = vol / 100.0
        if f:
            w = (bell(f, 3.2, v) if kind == "bell" else handbell(f, 1.2, v) if kind == "chime"
                 else organ(f, ms, v))
            out[pos:pos + len(w)] += w[:len(out) - pos]
        pos += int(ms * SR / 1000)
    end = min(len(out), pos + int(tail * SR))           # let it ring, but not forever (flash is finite)
    out = out[:end]
    f = int(0.35 * SR); out[-f:] *= np.linspace(1, 0, f) ** 2
    return out


SFX = {
    # name: notes   (also the timing the old synth used, re-voiced)
    "s_boot":    [("organ", C4, 150, 55), ("organ", E4, 150, 55), ("organ", G4, 150, 55), ("bell", C5, 1100, 80)],
    "s_heard":   [("chime", A5, 300, 45)],
    "s_move":    [("organ", G4, 90, 35), ("organ", C5, 140, 35)],
    "s_grab":    [("bell", C4, 320, 55), ("organ", E5, 120, 35)],
    "s_happy":   [("bell", E5, 320, 60), ("bell", G5, 600, 60)],
    "s_done":    [("organ", C5, 90, 45), ("organ", G5, 90, 45), ("organ", C6, 260, 45)],
    "s_sad":     [("organ", A4, 220, 50), ("organ", F4, 220, 50), ("organ", D4, 420, 50)],
    "s_stop":    [("bell", 131, 900, 95)],
    "s_strike":  [("bell", C5, 450, 90)],
    "s_cheer":   [("organ", C5, 110, 50), ("organ", E5, 110, 50), ("organ", G5, 110, 50), ("bell", C6, 700, 70)],
    "s_wave":    [("bell", A5, 260, 50), ("bell", E5, 500, 50)],
    "s_bow":     [("bell", G5, 350, 55), ("bell", C5, 800, 60)],
    "s_dance":   [("organ", C5, 160, 45), ("organ", E5, 160, 45), ("organ", G5, 160, 45), ("organ", E5, 160, 45),
                  ("organ", A5, 160, 45), ("organ", G5, 160, 45), ("organ", E5, 160, 45), ("organ", C5, 160, 45),
                  ("organ", D5, 160, 45), ("organ", E5, 160, 45), ("organ", G5, 160, 45), ("organ", A5, 160, 45),
                  ("organ", G5, 160, 45), ("organ", E5, 160, 45), ("organ", D5, 160, 45), ("bell", C5, 600, 60)],
    "s_fanfare": [("organ", G4, 130, 60), ("organ", C5, 130, 60), ("organ", E5, 130, 60), ("organ", G5, 380, 65),
                  ("organ", E5, 130, 60), ("organ", G5, 500, 65), ("bell", C6, 900, 80)],
    # Copy the Bells game: the four cue notes (pitches must stay distinct)
    "s_show":    None,
    "s_think":   None,     # organ noodles, a few variants
    "s_chirp":   None,     # soft blips while working
    "s_greet":   None,     # per-kid 3-bell jingles, picked by the kid's seed
}


def sfx_variants(name):
    if SFX[name] is not None: return [seq(SFX[name])]
    if name == "s_show": return [seq([("bell", f, 420, 75)]) for f in (C5, E5, G5, C6)]
    if name == "s_think":
        return [seq([("organ", PENTA[i], 90, 30) for i in rng.integers(0, 6, 6)]) for _ in range(3)]
    if name == "s_chirp": return [seq([("organ", f, 70, 18)]) for f in PENTA[::2]]
    if name == "s_greet":
        out = []
        for k in range(3):
            h = [PENTA[(k * 7 + i * 3) % 6] for i in range(3)]
            out.append(seq([("bell", h[0], 220, 60), ("bell", h[1], 220, 60), ("bell", h[2], 400, 65)]))
        return out
    raise KeyError(name)


# ---------------------------------------------------------------- room + mastering
def hall_ir(rt60=1.5, pre_ms=18):
    n = int(rt60 * 1.2 * SR); t = t_axis(n)
    ir = rng.standard_normal(n) * np.exp(-6.9 * t / rt60)
    # darker as it decays: blend a low-passed copy in over time
    lp = signal.lfilter(*signal.butter(1, 2500 / (SR / 2)), ir)
    mix = np.minimum(1, t / (rt60 * 0.5))
    ir = (1 - mix) * ir + mix * lp
    for d, g in ((0.011, 0.5), (0.017, 0.35), (0.023, 0.3), (0.031, 0.22)):   # early reflections
        ir[int(d * SR)] += g * 4
    ir = np.concatenate([np.zeros(int(pre_ms * SR / 1000)), ir])
    return ir / np.sqrt(np.sum(ir ** 2))


IR = hall_ir()


def reverb(x, wet):
    if wet <= 0: return x
    y = signal.fftconvolve(x, IR)[:len(x) + len(IR)]
    x = np.concatenate([x, np.zeros(len(y) - len(x))])
    return (1 - wet) * x + wet * y * (np.max(np.abs(x)) / (np.max(np.abs(y)) + 1e-9))


def peaking(f0, gain_db, q):
    A = 10 ** (gain_db / 40); w = 2 * np.pi * f0 / SR; al = np.sin(w) / (2 * q)
    b = [1 + al * A, -2 * np.cos(w), 1 - al * A]; a = [1 + al / A, -2 * np.cos(w), 1 - al / A]
    return np.array(b) / a[0], np.array(a) / a[0]


def compress(x, thresh_db=-20, ratio=3.0, att=0.004, rel=0.09):
    from scipy.ndimage import maximum_filter1d
    pk = maximum_filter1d(np.abs(x), max(1, int(att * SR)), origin=-(int(att * SR) // 2))   # sees peaks coming
    a_rel = np.exp(-1 / (rel * SR))
    e = np.maximum(pk, signal.lfilter([1 - a_rel], [1, -a_rel], pk))                        # smooth release
    lvl = 20 * np.log10(e + 1e-9)
    gr = np.where(lvl > thresh_db, (thresh_db - lvl) * (1 - 1 / ratio), 0)
    return x * 10 ** (gr / 20)


def master(x, kind):
    """For a ~2 W speaker: nothing below ~170 Hz (it can't play it, it just eats headroom), a lift where
    small speakers and ears are clear, then loud and even."""
    x = signal.lfilter(*signal.butter(2, 170 / (SR / 2), "high"), x)
    x = signal.lfilter(*peaking(3000, 3.0 if kind == "voice" else 2.0, 0.9), x)
    x = signal.lfilter(*signal.butter(2, 7600 / (SR / 2)), x)         # keep clear of 8 kHz Nyquist
    rms = np.sqrt(np.mean(x[np.abs(x) > 1e-4] ** 2)) if np.any(np.abs(x) > 1e-4) else 1
    x = x * (10 ** ((-17 if kind == "voice" else -19) / 20) / rms)
    x = compress(x)
    x = x * (10 ** ((8 if kind == "voice" else 7) / 20))             # make-up: ~-15 dBFS RMS, loud on a small speaker
    x = np.tanh(x * 1.1) / np.tanh(1.1) * 0.89                        # soft limit at -1 dBFS
    y = signal.resample_poly(x, 1, SR // OUT_SR)
    # trim silence, keep 15 ms of air, short fades
    keep = np.where(np.abs(y) > 10 ** (-42 / 20))[0]
    if len(keep):
        y = y[max(0, keep[0] - 240):min(len(y), keep[-1] + 240)]
    f = min(80, len(y) // 4)
    if f: y[:f] *= np.linspace(0, 1, f); y[-f:] *= np.linspace(1, 0, f)
    return np.clip(y, -1, 1)


# ---------------------------------------------------------------- voice
_voices = {}


def load_voice(name, voice_dir):
    if name in _voices: return _voices[name]
    from piper import PiperVoice
    path = os.path.join(voice_dir, f"{name}.onnx")
    if not os.path.exists(path):
        sys.exit(f"voice model missing: {path}\n  python3 -m piper.download_voices {name} --download-dir {voice_dir}")
    _voices[name] = PiperVoice.load(path)
    return _voices[name]


def speak(text, voice_name, voice_dir, tint):
    from piper import SynthesisConfig
    v = load_voice(voice_name, voice_dir)
    cfg = SynthesisConfig(length_scale=VOICE_SPEED, noise_scale=0.6, noise_w_scale=0.8)
    chunks = list(v.synthesize(text, cfg))
    sr = chunks[0].sample_rate
    x = np.concatenate([c.audio_float_array for c in chunks]).astype(np.float64)
    g = np.gcd(SR, sr)
    x = signal.resample_poly(x, SR // g, sr // g)
    if tint > 0:                                   # faint metallic shimmer: a short swept comb
        n = np.arange(len(x)); d = (0.0045 + 0.0015 * np.sin(2 * np.pi * 0.7 * n / SR)) * SR
        idx = np.clip(n - d, 0, len(x) - 1)
        x = x + tint * 2.2 * np.interp(idx, n, x)
    return reverb(x, 0.06)


# ---------------------------------------------------------------- IMA ADPCM
STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
         107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
         876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
         4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
         22385, 24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm(pcm):
    """Standard IMA ADPCM, low nibble first. Returns (bytes, first_sample, first_index)."""
    pred, idx = int(pcm[0]), 0
    out = bytearray(); hi = None
    for s in pcm[1:]:
        step = STEPS[idx]; diff = int(s) - pred; code = 0
        if diff < 0: code = 8; diff = -diff
        dq = step >> 3
        if diff >= step: code |= 4; diff -= step; dq += step
        step >>= 1
        if diff >= step: code |= 2; diff -= step; dq += step
        step >>= 1
        if diff >= step: code |= 1; dq += step
        pred = max(-32768, min(32767, pred - dq if code & 8 else pred + dq))
        idx = max(0, min(88, idx + IDX[code & 7]))
        if hi is None: hi = code
        else: out.append(hi | (code << 4)); hi = None
    if hi is not None: out.append(hi)
    return bytes(out), int(pcm[0]), 0


# ---------------------------------------------------------------- build
def expand(group, variants):
    """-> [(group_name, [texts])]: {c} and {n} templates fan out into one group per color / number."""
    if any("{c}" in v for v in variants):
        return [(f"{group}_{c}", [v.format(c=c) for v in variants]) for c in COLORS]
    if any("{n}" in v for v in variants):
        return [(f"{group}_{i + 1}", [v.format(n=w.capitalize()) for v in variants]) for i, w in enumerate(NUMBERS)]
    return [(group, variants)]


def write_wav(path, y, rate=OUT_SR):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes((y * 32767).astype("<i2").tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", default=VOICE)
    ap.add_argument("--voice-dir", default=os.path.expanduser("~/.cache/piper"))
    ap.add_argument("--tint", type=float, default=ROBOT_TINT)
    ap.add_argument("--no-voice", action="store_true", help="silence for speech (build test without a model)")
    ap.add_argument("--preview", metavar="DIR", help="also write every clip as WAV here")
    ap.add_argument("--voices-only", action="store_true", help="with --preview: just the voice samples")
    a = ap.parse_args()

    if a.preview:
        os.makedirs(a.preview, exist_ok=True)
        if not a.no_voice:
            for vn in PREVIEW_VOICES:
                try:
                    write_wav(os.path.join(a.preview, f"voice_{vn}.wav"), master(speak(PREVIEW_TEXT, vn, a.voice_dir, a.tint), "voice"))
                    print("preview", vn)
                except SystemExit as e:
                    print("skip", vn, e)
        if a.voices_only: return

    groups, clips, rates = [], [], []       # groups: (name, first, n); clips: float arrays; rates: Hz
    def add(name, ys):
        groups.append((name, len(clips), len(ys))); clips.extend(ys)

    for name in SFX:
        ys = []
        for x in sfx_variants(name):
            y = reverb(x, 0.28)[:len(x) + int(0.45 * SR)]
            f = int(0.3 * SR); y[-f:] *= np.linspace(1, 0, f) ** 2
            ys.append(master(y, "sfx"))
        rates.extend([OUT_SR] * len(ys))
        add(name, ys)
    for group, variants in LINES.items():
        for gname, texts in expand(group, variants):
            if a.no_voice: ys = [np.zeros(1600) for _ in texts]
            else: ys = [master(speak(t, a.voice, a.voice_dir, a.tint), "voice") for t in texts]
            ys = [signal.resample_poly(y, VOICE_SR // 1000, OUT_SR // 1000) for y in ys]   # 16k -> 12k
            rates.extend([VOICE_SR] * len(ys))
            add(gname, ys)
            print(f"{gname:16s} {len(ys)} x  {sum(len(y) for y in ys) / VOICE_SR:5.1f} s")

    if a.preview:
        for gname, first, n in groups:
            for k in range(n): write_wav(os.path.join(a.preview, f"{gname}_{k}.wav"), clips[first + k], rates[first + k])

    # bank: "SNB1" u32 count | count x (u32 offset, u32 samples, i16 first, u8 index, u8 rate) | data
    #       rate: 0 = 16 kHz, 1 = 12 kHz
    head = 8 + 12 * len(clips)
    table, data = bytearray(), bytearray()
    for y, r in zip(clips, rates):
        pcm = np.round(np.clip(y, -1, 1) * 32767).astype(np.int32)
        code, first, idx = adpcm(pcm)
        table += struct.pack("<IIhBB", head + len(data), len(pcm), first, idx, 0 if r == OUT_SR else 1)
        data += code
        if len(data) % 2: data += b"\0"
    bank = b"SNB1" + struct.pack("<I", len(clips)) + table + data
    secs = sum(len(y) / r for y, r in zip(clips, rates))
    print(f"bank: {len(clips)} clips, {secs:.0f} s, {len(bank) / 1e6:.2f} MB (budget {BUDGET / 1e6:.2f} MB)")
    if len(bank) > BUDGET: sys.exit("sound bank over budget — trim LINES or variants")
    with open(os.path.join(FW, "sounds.bin"), "wb") as f: f.write(bank)

    h = ["// generated by tools/make_sounds.py — do not edit. Groups of clips in sounds.bin;",
         "// the player picks one clip of a group at random (never the same twice in a row).",
         "#pragma once", "#include <stdint.h>", "",
         f"#define SND_VOICE \"{'none' if a.no_voice else a.voice}\"",
         f"#define SND_N_COLORS {len(COLORS)}   // " + " ".join(COLORS), f"#define SND_N_NUMBERS {len(NUMBERS)}", "",
         "typedef enum {"]
    h += [f"    SND_{g.upper()}," for g, _, _ in groups]
    h += ["    SND_GROUPS", "} snd_group_t;", "",
          "static const struct { uint16_t first, n; } SND_GROUP[SND_GROUPS] = {"]
    h += [f"    {{{first}, {n}}},   // {g}" for g, first, n in groups]
    h += ["};", ""]
    with open(os.path.join(FW, "sounds_gen.h"), "w") as f: f.write("\n".join(h))


if __name__ == "__main__":
    main()
