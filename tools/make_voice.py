#!/usr/bin/env python3
"""Render robot voice lines to 16 kHz mono PCM for the firmware (firmware/main/voice_*.pcm).
espeak-ng (pip: espeakng-loader) + a little ring-mod for a robot timbre.
usage: python3 tools/make_voice.py"""
import array, ctypes, math, os, wave
import espeakng_loader as E

LINES = {"rumble": ("Ready to rumble!", 120, 35)}   # name: (text, words/min, pitch 0-99)
OUT = os.path.join(os.path.dirname(__file__), "..", "firmware", "main")

lib = ctypes.cdll.LoadLibrary(E.get_library_path())
CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_short), ctypes.c_int, ctypes.c_void_p)
buf = []
@CB
def cb(wav, n, ev):
    if wav and n > 0: buf.extend(wav[:n])
    return 0
rate = lib.espeak_Initialize(2, 0, E.get_data_path().encode(), 0)   # AUDIO_OUTPUT_SYNCHRONOUS
lib.espeak_SetSynthCallback(cb)
lib.espeak_SetVoiceByName(b"en-us")

for name, (text, wpm, pitch) in LINES.items():
    buf.clear()
    lib.espeak_SetParameter(1, wpm, 0); lib.espeak_SetParameter(3, pitch, 0)
    t = text.encode()
    lib.espeak_Synth(t, len(t) + 1, 0, 1, 0, 0, None, None)
    lib.espeak_Synchronize()
    # resample to 16 kHz (linear), ring-modulate at 55 Hz blended 45% for the robot buzz, normalize
    src = buf; n = int(len(src) * 16000 / rate)
    out = []
    for i in range(n):
        x = i * rate / 16000; k = int(x); f = x - k
        s = src[k] * (1 - f) + (src[k + 1] if k + 1 < len(src) else 0) * f
        s = 0.55 * s + 0.45 * s * math.sin(2 * math.pi * 55 * i / 16000) * 2
        out.append(s)
    peak = max(1, max(abs(v) for v in out))
    pcm = array.array("h", (int(v / peak * 26000) for v in out))
    with open(os.path.join(OUT, f"voice_{name}.pcm"), "wb") as fh: pcm.tofile(fh)
    with wave.open(f"/tmp/voice_{name}.wav", "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(pcm.tobytes())
    print(name, len(pcm) / 16000, "s")
