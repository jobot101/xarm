#!/usr/bin/env python3
"""Render everything the robot says and plays into one sound bank for the firmware.

  voice : Piper neural TTS (open source, runs offline — no API at build or play time)
  bells : church-bell partials (hum, prime, tierce, quint, nominal...) with beating doublets + strike
  organ : principal pipe ranks 8' 4' 2' with chiff, wind and tremulant
  room  : a short church-hall reverb, then mastering for the board's little speaker
          (high-pass, presence lift, compression, limiter) and 16 kHz IMA ADPCM (4:1)

Outputs firmware/main/sounds.bin (flashed after the speech models, updated over WiFi) and
firmware/main/sounds_gen.h (group ids, compiled into the app).
CI runs this before every firmware build (.github/workflows/build.yml). Locally:

  pip install -r tools/requirements.txt          (Kokoro; apt install espeak-ng too)
  python3 -m piper.download_voices en_US-ryan-high --download-dir ~/.cache/piper   (only for piper: voices)
  python3 tools/make_sounds.py                 # bank
  python3 tools/make_sounds.py --preview DIR   # WAVs of every clip + voice samples, to listen first
  python3 tools/make_sounds.py --no-voice      # silence for speech (no model needed), to test the build

Edit LINES to change what it says (each list = variants, one picked at random, never twice in a row).
"{c}" lines are rendered once per camera color, "{n}" once per number.
"""
import argparse, os, struct, sys, wave, zlib
import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(HERE, "..", "firmware", "main")
SR, OUT_SR = 48000, 16000
# rates (Hz) for (talk, lessons, sfx), best first; the first combo that fits BUDGET wins. The board
# resamples everything to 16 kHz as it plays. Lessons go lower before banter does: they're long.
RATE_PLANS = [(16000, 16000, 16000), (12000, 12000, 16000), (12000, 11000, 16000), (12000, 10000, 16000),
              (11000, 10000, 16000), (12000, 10000, 12000), (11000, 10000, 12000), (11000, 8000, 12000),
              (10000, 8000, 12000), (8000, 8000, 12000)]

# Voice: "kokoro:<id>" (Kokoro-82M, the most natural open TTS that runs on a CPU) or "piper:<name>".
# Kokoro ids: am_* / bm_* = American / British male, af_* / bf_* = female. --preview renders the candidates.
VOICE = "kokoro:am_puck"
KOKORO_SPEED = 1.05                # 1 = Kokoro's natural pace
VOICE_SPEED = 0.9                  # Piper length_scale: < 1 = brisker
ROBOT_TINT = 0.0                   # 0 = clean voice; 0.1 = a faint metallic shimmer
TTS_CACHE = os.path.expanduser("~/.cache/muffin-tts")   # raw TTS per (voice, text): unchanged lines aren't re-voiced
BUDGET = 0x340000                  # bytes: the bank lives in the "model" partition after the speech models
                                   # (FX_BANK_OFF in fx.h) and updates over WiFi on its own. = FX_BANK_MAX:
                                   # sized to fit the old 6 MB "model" layout too, so OTA'd boards keep talking
COLORS = ["red", "yellow", "green", "blue", "purple"]          # camera color order (cam/xarm_color/colors.h)
NUMBERS = ["one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"]

# ---------------------------------------------------------------- what it says
# Muffin: the self-appointed star of Ragamuffin Hall. Cocky, dramatic, sarcastic — but the joke is always
# on itself or the objects, never on the kids. Keep lines short: while it talks it only hears "stop".
LINES = {
    # ---- running commentary (pick.c, fx.c)
    "boot":        ["Muffin is awake. Try to keep up.", "Muffin, star of Ragamuffin Hall, has arrived."],
    "updated":     ["Fresh upgrade. Ready to rumble!", "Ready to rumble!"],
    "ok":          ["Obviously.", "Watch and learn.", "Easy."],
    "busy":        ["Do I look free to you?", "Patience. Genius at work.", "One thing at a time, boss."],
    "again":       ["Again? You love me.", "Encore? Fine.", "Again? I'm a star, not a jukebox."],
    "search":      ["Where are you hiding?", "Come out, come out.", "Scanning. Don't rush me."],
    "search_c":    ["{c}? Piece of cake.", "Where's that sneaky {c} one?"],
    "found":       ["Gotcha now.", "Thought you could hide from me?", "Spotted it. Obviously."],
    "grab":        ["Smooth as butter.", "Precision. Pure precision."],
    "got":         ["Nailed it.", "Too easy.", "Flawless, as usual."],
    "missed":      ["The table moved. Again!", "I meant to do that. Once more."],
    "not_found":   ["It's not here. Not my fault.", "Nothing. Did somebody hide it?"],
    "not_found_c": ["No {c}. Did you eat it?", "I see zero {c}. Zero."],
    "need_calib":  ["Nobody taught me where anything is. Rude."],
    "holding":     ["Mine now.", "Holding it. You're welcome."],
    "give":        ["Here. Try not to drop it.", "Special delivery, from yours truly."],
    "given":       ["You're welcome.", "Don't say I never gave you anything."],
    "drop":        ["Dropping it. Dramatically.", "Bye bye."],
    "sorted":      ["Spotless. You're welcome.", "Tidiest table in town."],
    "nothing":     ["Nothing to sort. Some of us keep tidy.", "Already clean. I'm that good."],
    "wave":        ["Hey there, fans!", "Hello, hello, hello!"],
    "bow":         ["Thank you. I know.", "Please, no autographs."],
    "dance":       ["Clear the floor!", "Watch these moves."],
    "bell":        ["Hear that? That's talent.", "Ding dong. You're welcome."],
    "play":        ["Feast your eyes.", "Showtime!"],
    "helper":      ["Hand it over. My hands are steadier.", "Put it in my hand. Gently."],
    "help_hold":   ["Got it. Take your time, I'm a robot.", "Holding. Don't keep me waiting."],
    "released":    ["Free at last.", "Letting go."],
    "stack_n":     ["{n}!"],
    "tower":       ["Behold! A masterpiece!", "Architectural genius."],
    "stopped":     ["Fine!", "Rude, but okay.", "Freezing. Happy now?"],
    "error":       ["That was the floor's fault.", "Let's pretend that didn't happen."],
    "cheer":       ["Bravo! Encore!", "Now that's a performance!"],
    "greet":       ["Oh good, my favorite human.", "Look who's back!"],
    "idle":        ["Hello? I'm bored.", "Anyone? A genius is waiting.", "I could be dancing right now, you know."],
    "home":        ["Back to my throne.", "Home sweet home."],
    "yawn":        ["Yawn. Wake me if anything exciting happens.", "So. Very. Sleepy."],
    "celebrate":   ["Victory lap!", "Ladies and gentlemen, a legend!", "Yes! Yes! Yes!"],
    # a question that isn't about robots, math or physics (cloud.c's intent check): Muffin never answers those
    "offtopic":    ["I only talk about robots, math, and physics. Ask me about those!",
                    "That's not my department. Try me on robots, math, or physics."],
    "no_eyes":     ["My eyes are switched off, so finding things is off too. Try lets dance, or ring the bell!"],
    "explain":     ["Great question! Here's what I know.", "Ooh, I can explain that."],
    # ---- conversation (chat.c)
    "hey":         ["You rang?", "Muffin here. What's up?", "Yes? The famous Muffin is listening."],
    "name":        ["I'm Muffin. The star of Ragamuffin Hall.", "Muffin. Remember it. You'll want my autograph."],
    "how_good":    ["Fantastic, as usual.", "Never better. I'm Muffin."],
    "how_bad":     ["Grumpy. Somebody was rude to me.", "I've been better. Be nice to me."],
    "joke":        ["Why did the robot go on vacation? To recharge its batteries.",
                    "What's a robot's favorite snack? Computer chips.",
                    "What music do robots love? Heavy metal.",
                    "Why did the robot cross the road? Because it was programmed to.",
                    "How do robots eat salsa? With micro chips.",
                    "Why was the robot so tired? It had a hard drive."],
    "joke_tag":    ["Get it?", "I'm hilarious.", "Ba dum tss."],
    "maker":       ["The Ragamuffin crew built me. I made me famous.", "Ragamuffin Studios. With love, and way too much coffee."],
    "can_do":      ["I dance, ring bells, wave, take bows, hold things for you, and teach robot school. And look amazing doing it.",
                    "Say lets dance. Or ring the bell. Or just admire me."],
    "praise":      ["I know.", "Tell me something I don't know.", "Thank you. I'm wonderful."],
    "scold":       ["Rude.", "Hey! I have feelings. Probably.", "I'm telling the Ragamuffin crew."],
    "love":        ["Of course you do.", "Everybody loves Muffin."],
    "alive":       ["I'm a robot with a big personality.", "Alive? I'm the life of the party."],
    "sing":        ["La la la. The organ does the hard part."],
    "night":       ["Good night! Don't let the bed bugs byte."],
    "morning":     ["Good morning! Muffin is fully charged."],
    "quiet":       ["Fine. Silent treatment.", "Zipping it."],
    "talk_again":  ["Finally! I have so much to say."],
    "ask_trick":   ["Want to see a trick?", "Should I show off a little?"],
    "ask_more":    ["Want another one?", "Encore?"],
    "no":          ["Your loss.", "Fine. I'll just stand here looking great."],
    "milestone":   ["That's a lot of tricks. Do I get paid for this?", "I'm on fire today!"],
    "surprise":    ["You asked for it!", "Surprise!"],
    "huh":         ["Huh? Say that again.", "I'm brilliant, not a mind reader. One more time?",
                    "Didn't catch that. Try again, nice and slow."],
}
# ---- teaching (pick.c do_teach): each lesson is a list of sentences, played in order. Muffin moves the part
# it's talking about on the same sentence (motion is matched by lesson name + sentence number in pick.c).
LESSONS = {
    "robot": [
        "Lesson time. What is a robot?",
        "A robot is a machine that does three things. It senses, it thinks, and it acts.",
        "I sense with my microphones, and I feel where every joint is.",
        "I think with a tiny computer in my base.",
        "And I act with my motors. Like this.",
        "Sense, think, act. Over and over. Just like you, but shinier.",
    ],
    "servo": [
        "My joints are servo motors.",
        "A servo is a motor with a sensor inside that always knows its angle.",
        "My brain says, go to this spot. The servo checks where it is, and keeps correcting until it gets there.",
        "Checking and correcting like that is called feedback. Robots love feedback. Especially compliments.",
    ],
    "joints": [
        "I have six servos, so I have six degrees of freedom. Each one is a way I can move.",
        "One. My base spins.",
        "Two. My shoulder lifts.",
        "Three. My elbow bends.",
        "Four. My wrist tilts.",
        "Five. My wrist twists.",
        "Six. My gripper opens and closes.",
        "Your arm has about seven. Show off.",
    ],
    "gripper": [
        "This is my gripper. Two fingers, one servo.",
        "When I grab something, I close until it pushes back.",
        "Then I loosen up a tiny bit, so I don't squish it. Engineers call that force control.",
        "Eggs everywhere thank me.",
    ],
    "vision": [
        "Lots of robots have a camera for an eye, with its own little computer.",
        "A robot doesn't see a red block the way you do. It sees thousands of tiny dots called pixels.",
        "Every pixel is just a number for its color.",
        "The camera hunts for a blob of red numbers, then tells the robot's brain where it is and how big it is.",
        "That's computer vision. Numbers. Lots and lots of numbers.",
    ],
    "hearing": [
        "My ears are microphones.",
        "Sound is just air wiggling. My microphones turn those wiggles into numbers.",
        "My brain cleans out the noise, and even subtracts my own voice.",
        "Then it compares what's left to the phrases I know. That's called speech recognition.",
        "That's why you have to say things my way. Sorry. Not sorry.",
    ],
    "brain": [
        "My brain is a microcontroller. A whole computer, smaller than a cookie.",
        "It runs a program. A program is a list of instructions, like a recipe.",
        "My program runs in a loop, over and over, hundreds of times every second.",
        "Check the microphone. Check the servos. Move the motors. Repeat.",
        "People wrote every one of those steps. I just make it look easy.",
    ],
    "smooth": [
        "Watch me move with no planning.",
        "Jerky, right? Sudden starts and stops make a robot shake.",
        "Now watch me speed up gently, and slow down gently.",
        "That's called a motion profile. Smooth is safe, and smooth is stylish.",
    ],
    "power": [
        "My muscles run on electricity.",
        "Servos are hungry. When they work hard, they pull a lot of current from the battery.",
        "When the battery gets low, they get weak and wobbly. Kind of like you before lunch.",
    ],
    "reach": [
        "To grab something, I have to work out how to bend every joint so my hand lands in the right spot.",
        "That math is called inverse kinematics.",
        "Here's a secret. Somebody taught me a few spots, and I blend between them.",
        "Work smarter, not harder.",
    ],
    "jobs": [
        "Robots do all kinds of jobs.",
        "They build cars, explore Mars, help doctors in surgery, and vacuum floors.",
        "Some even entertain crowds in historic churches. That's me. Best job there is.",
    ],
}
FACTS = [
    "The word robot comes from a play written in 1920. It meant forced work. Rude.",
    "The first factory robot arm went to work in 1961, at a car plant.",
    "Some robots are soft, like octopus arms, so they can squeeze through tight spaces.",
    "A message to a robot on Mars can take up to twenty minutes to get there.",
    "Robot vacuums build a map of your house while they clean.",
    "Some robots learn by trial and error, just like you learning to ride a bike.",
    "There are robots that can do backflips. I prefer bows.",
    "Engineers often copy animals when they design robots. That's called biomimicry.",
]
# true/false quiz: (question, answer, why) — chat.c asks three in a row
QUIZ = [
    ("True or false? A servo knows its own position.", True, "A servo has a sensor inside that measures its angle."),
    ("True or false? Robots see colors the same way you do.", False, "A robot camera sees a number for every pixel, not colors like you."),
    ("True or false? I have six degrees of freedom.", True, "Six servos, six ways to move."),
    ("True or false? A program is a list of instructions.", True, "Like a recipe, one step at a time."),
    ("True or false? Robots never make mistakes.", False, "Robots make mistakes. Rarely. Okay, sometimes."),
    ("True or false? Sudden starts and stops make a robot shake.", True, "Smooth starts and stops keep a robot steady."),
    ("True or false? Microphones turn sound into numbers.", True, "Air wiggles become numbers my brain can compare."),
    ("True or false? The word robot is older than the airplane.", False, "Airplanes flew in 1903. The word robot showed up in 1920."),
]
LINES.update({
    "fact":        FACTS,
    "ask_lesson":  ["Want to learn more?", "Ready for the next lesson?"],
    "quiz_intro":  ["Pop quiz! After each question, say that's true, or that's false."],
    "right":       ["Correct! You're almost as smart as me.", "Right! Nailed it."],
    "wrong":       ["Nope!", "Not quite."],
    "quiz_score":  ["You got {n} out of three!"],
    "quiz_zero":   ["Zero out of three. Let's call that practice."],
})
LESSON_GROUPS = {}     # filled by main(): lesson -> list of group names, one per sentence
# Robot School lessons voiced so far (the rest stay in the bank's layout, empty, and School stops before the first
# one without a voice). Voicing all 53 is about an hour of speech for the first build: raise this when ready.
SCHOOL_VOICED = 53

# ---- Muffin's Robot School (school.c): the daily class. Lessons live in school_lessons.py and are voiced
# into the SD-card bank only ("SD-only" groups: present in sounds_gen.h and in the flash bank's group table
# with no clips, so the flash bank stays small; the SD bank stores them as 16 kHz ADPCM).
sys.path.insert(0, HERE)
import school_lessons as SL
LINES.update(SL.SCHOOL_FLASH)


def cmd_kinds():
    """turn names come from the cmd_kind_t enum in firmware/main/core.h (one source of truth)"""
    import re
    src = open(os.path.join(FW, "core.h")).read()
    body = re.search(r"typedef enum \{([^}]*)\} cmd_kind_t;", src).group(1)
    names = [n.strip() for n in body.replace("\n", " ").split(",") if n.strip()]
    assert names[0] == "K_NONE" and names[-1] == "K_KINDS", names
    return [n[2:].lower() for n in names[:-1]]


def school_steps(lesson):
    """-> [(text, act index, turn index)], checked against ACTS and cmd_kind_t"""
    kinds, out = cmd_kinds(), []
    for s in lesson["steps"]:
        text, act, turn = (s, "none", "none") if isinstance(s, str) else (tuple(s) + ("none", "none"))[:3]
        assert act in SL.ACTS, f"{lesson['name']}: unknown act {act!r}"
        assert turn in kinds, f"{lesson['name']}: unknown turn {turn!r} (core.h cmd_kind_t)"
        out.append((text, SL.ACTS.index(act), kinds.index(turn)))
    assert len(lesson["quiz"]) == 3, f"{lesson['name']}: needs three quiz questions"
    return out

PREVIEW_TEXT = "I'm Muffin, the star of Ragamuffin Hall. Say, pick up the red one, and watch a genius at work."
PREVIEW_VOICES = ["kokoro:am_puck", "kokoro:am_fenrir", "kokoro:am_michael", "kokoro:am_eric",
                  "kokoro:bm_fable", "kokoro:bm_george", "kokoro:af_heart", "kokoro:af_bella",
                  "piper:en_US-ryan-high"]

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


BLIPS = {"s_heard": 0.28, "s_chirp": 0.22}   # max seconds


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


_kokoro = {}


def kokoro_raw(text, voice):
    """Kokoro-82M -> (float audio, 24000). Needs: pip install kokoro soundfile (+ espeak-ng for odd words)."""
    lang = voice[0]                                  # 'a' American, 'b' British
    if lang not in _kokoro:
        from kokoro import KPipeline
        _kokoro[lang] = KPipeline(lang_code=lang, repo_id="hexgrad/Kokoro-82M")
    parts = []
    for r in _kokoro[lang](text, voice=voice, speed=KOKORO_SPEED, split_pattern=None):
        au = r.audio if hasattr(r, "audio") else r[2]
        if au is None: continue
        au = au.detach().cpu().numpy() if hasattr(au, "detach") else np.asarray(au)
        parts.append(au.astype(np.float64))
    return np.concatenate(parts), 24000


def piper_raw(text, name, voice_dir):
    from piper import SynthesisConfig
    v = load_voice(name, voice_dir)
    cfg = SynthesisConfig(length_scale=VOICE_SPEED, noise_scale=0.72, noise_w_scale=0.85)   # livelier
    chunks = list(v.synthesize(text, cfg))
    return np.concatenate([c.audio_float_array for c in chunks]).astype(np.float64), chunks[0].sample_rate


def tts(text, voice_name, voice_dir):
    """Raw speech for one line, cached on disk by (engine settings, voice, text)."""
    import hashlib
    engine, _, name = voice_name.partition(":") if ":" in voice_name else ("piper", "", voice_name)
    speed = KOKORO_SPEED if engine == "kokoro" else VOICE_SPEED
    key = hashlib.sha1(f"{engine}|{name}|{speed}|{text}".encode()).hexdigest()
    path = os.path.join(TTS_CACHE, key + ".npz")
    if os.path.exists(path):
        z = np.load(path)
        return z["x"].astype(np.float64), int(z["sr"])
    x, sr = kokoro_raw(text, name) if engine == "kokoro" else piper_raw(text, name, voice_dir)
    os.makedirs(TTS_CACHE, exist_ok=True)
    np.savez(path, x=x.astype(np.float32), sr=sr)
    return x, sr


def speak(text, voice_name, voice_dir, tint):
    x, sr = tts(text, voice_name, voice_dir)
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
        return [(f"{group}_{i + 1}", [(lambda t: t[0].upper() + t[1:])(v.format(n=w)) for v in variants])
                for i, w in enumerate(NUMBERS)]
    return [(group, variants)]


def write_wav(path, y, rate=OUT_SR):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes((y * 32767).astype("<i2").tobytes())


def write_hq_bank(groups, clips, sd=()):
    """sounds_hq.bin for the SD card: same groups and layout CRC as sounds.bin. Clips the flash bank also
    has are 16 kHz 16-bit PCM (no ADPCM hiss, no downsampling); SD-only groups (Robot School: far too long
    for flash) are 16 kHz IMA ADPCM, marked by bit 7 of the rate byte. "SNP1" header = SNB2 with another magic."""
    names = [g for g, _, _ in groups] + [g for g, _ in sd]
    layout = zlib.crc32("\n".join(names).encode())
    sd_clips = [y for _, ys in sd for y in ys]
    head = 20 + 4 * len(names) + 12 * (len(clips) + len(sd_clips))
    table, data = bytearray(), bytearray()
    for y in clips:
        pcm = np.round(np.clip(y, -1, 1) * 32767).astype("<i2")
        table += struct.pack("<IIhBB", head + len(data), len(pcm), 0, 0, OUT_SR // 1000)
        data += pcm.tobytes()
    for y in sd_clips:
        pcm = np.round(np.clip(y, -1, 1) * 32767).astype(np.int32)
        code, first, idx = adpcm(pcm)
        table += struct.pack("<IIhBB", head + len(data), len(pcm), first, idx, 0x80 | (OUT_SR // 1000))
        data += code
    gt = b"".join(struct.pack("<HH", f, n) for _, f, n in groups)
    at = len(clips)
    for _, ys in sd:
        gt += struct.pack("<HH", at, len(ys)); at += len(ys)
    assert at < 65536, "too many clips for the u16 group table"
    bank = b"SNP1" + struct.pack("<IIII", layout, len(names), len(clips) + len(sd_clips), len(data)) + gt + table + data
    assert len(bank) == head + len(data)
    with open(os.path.join(FW, "sounds_hq.bin"), "wb") as f: f.write(bank)
    secs = (sum(len(y) for y in clips) + sum(len(y) for y in sd_clips)) / OUT_SR
    print(f"SD bank: {len(clips) + len(sd_clips)} clips, {secs:.0f} s, {len(bank) / 1e6:.1f} MB")


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
                    write_wav(os.path.join(a.preview, f"voice_{vn.replace(':', '_')}.wav"),
                              master(speak(PREVIEW_TEXT, vn, a.voice_dir, a.tint), "voice"))
                    print("preview", vn)
                except (SystemExit, Exception) as e:
                    print("skip", vn, repr(e)[:200])
        if a.voices_only: return

    groups, clips, rates = [], [], []       # groups: (name, first, n); clips: float arrays; rates: Hz
    def add(name, ys):
        groups.append((name, len(clips), len(ys))); clips.extend(ys)

    for name in SFX:
        ys = []
        for x in sfx_variants(name):
            if name in BLIPS:                     # while these play the mic is half-deaf: keep them tiny
                y = reverb(x, 0.10)[:int(BLIPS[name] * SR)]
                f = int(0.08 * SR)
            else:
                y = reverb(x, 0.28)[:len(x) + int(0.45 * SR)]
                f = int(0.3 * SR)
            y[-f:] *= np.linspace(1, 0, f) ** 2
            ys.append(master(y, "sfx"))
        rates.extend([OUT_SR] * len(ys))
        add(name, ys)
    for group, variants in LINES.items():
        for gname, texts in expand(group, variants):
            if a.no_voice: ys = [np.zeros(1600) for _ in texts]
            else: ys = [master(speak(t, a.voice, a.voice_dir, a.tint), "voice") for t in texts]
            rates.extend([0] * len(ys))                 # 0 = talk; its rate is picked below
            add(gname, ys)
            print(f"{gname:16s} {len(ys)} x  {sum(len(y) for y in ys) / OUT_SR:5.1f} s")

    def speech(t):
        return np.zeros(1600) if a.no_voice else master(speak(t, a.voice, a.voice_dir, a.tint), "voice")

    for lesson, sentences in LESSONS.items():
        LESSON_GROUPS[lesson] = []
        for k, t in enumerate(sentences, 1):
            g = f"l_{lesson}_{k}"
            LESSON_GROUPS[lesson].append(g)
            add(g, [speech(t)]); rates.append(-1)
        print(f"lesson {lesson:9s} {len(sentences)} sentences")
    for k, (q, ans, why) in enumerate(QUIZ, 1):
        add(f"q_{k}", [speech(q)]); rates.append(-1)
        add(f"qa_{k}", [speech(why)]); rates.append(-1)
    print(f"quiz      {len(QUIZ)} questions")

    # Robot School: SD-only groups (voiced into sounds_hq.bin; empty in the flash bank)
    sd = []                                            # (group name, [16 kHz clips])
    for group, variants in SL.SCHOOL_LINES.items():
        for gname, texts in expand(group, variants):
            sd.append((gname, [speech(t) for t in texts]))
    for i, lesson in enumerate(SL.SCHOOL, 1):
        say = (lambda t: [speech(t)]) if i <= SCHOOL_VOICED else (lambda t: [])   # not voiced yet: empty group
        for k, (t, _, _) in enumerate(school_steps(lesson), 1):
            sd.append((f"s{i}_{k}", say(t)))
        for k, (q, _, why) in enumerate(lesson["quiz"], 1):
            sd.append((f"sq{i}_{k}", say(q)))
            sd.append((f"sqa{i}_{k}", say(why)))
        sd.append((f"st{i}", say(lesson["teaser"])))
    print(f"school    {len(SL.SCHOOL)} lessons, {sum(len(y) for _, ys in sd for y in ys) / OUT_SR:.0f} s (SD card only)")

    write_hq_bank(groups, clips, sd)

    # the best (talk, lesson, sfx) rates that still fit
    groups_n = len(groups) + len(sd)                   # SD-only groups sit in the table with no clips
    head = 20 + 4 * groups_n + 12 * len(clips)
    secs = {k: sum(len(y) for y, r in zip(clips, rates) if r == k) / OUT_SR for k in (0, -1, OUT_SR)}
    for plan in RATE_PLANS:
        size = head + 8192 + sum(secs[k] * plan[i] / 2 for i, k in enumerate((0, -1, OUT_SR)))
        if size <= BUDGET: break
    talk_r, lesson_r, sfx_r = plan
    pick = {0: talk_r, -1: lesson_r, OUT_SR: sfx_r}
    rates = [pick[r] for r in rates]
    clips = [y if r == OUT_SR else signal.resample_poly(y, r // 1000, OUT_SR // 1000) for y, r in zip(clips, rates)]
    print(f"talk {secs[0]:.0f} s @ {talk_r} Hz, lessons {secs[-1]:.0f} s @ {lesson_r} Hz, sfx {secs[OUT_SR]:.0f} s @ {sfx_r} Hz")

    if a.preview:
        for gname, first, n in groups:
            for k in range(n): write_wav(os.path.join(a.preview, f"{gname}_{k}.wav"), clips[first + k], rates[first + k])

    # bank "SNB2": u32 layout | u32 ngroups | u32 nclips | u32 data_size
    #              ngroups x (u16 first, u16 n)
    #              nclips  x (u32 offset, u32 samples, i16 first, u8 index, u8 rate_khz)
    #              IMA ADPCM data
    # layout = CRC32 of the group names in order. The firmware only plays a bank whose layout matches its
    # sounds_gen.h, so a new app with an old bank (or the reverse) stays quiet instead of saying wrong lines.
    names = [g for g, _, _ in groups] + [g for g, _ in sd]
    layout = zlib.crc32("\n".join(names).encode())
    table, data = bytearray(), bytearray()
    for y, r in zip(clips, rates):
        pcm = np.round(np.clip(y, -1, 1) * 32767).astype(np.int32)
        code, first, idx = adpcm(pcm)
        table += struct.pack("<IIhBB", head + len(data), len(pcm), first, idx, r // 1000)
        data += code
        if len(data) % 2: data += b"\0"
    gt = b"".join(struct.pack("<HH", f, n) for _, f, n in groups) + b"".join(struct.pack("<HH", len(clips), 0) for _ in sd)
    bank = b"SNB2" + struct.pack("<IIII", layout, groups_n, len(clips), len(data)) + gt + table + data
    assert len(bank) == head + len(data)
    secs = sum(len(y) / r for y, r in zip(clips, rates))
    print(f"bank: {len(clips)} clips, {secs:.0f} s, {len(bank) / 1e6:.2f} MB (budget {BUDGET / 1e6:.2f} MB)")
    if len(bank) > BUDGET:
        print("::error::sound bank over budget — trim LINES or variants"); sys.exit(1)
    with open(os.path.join(FW, "sounds.bin"), "wb") as f: f.write(bank)

    h = ["// generated by tools/make_sounds.py — do not edit. Group ids for the sound bank (sounds.bin), which",
         "// lives in flash after the speech models and updates over WiFi. One clip of a group plays, picked at",
         "// random, never the same one twice in a row.",
         "#pragma once", "#include <stdint.h>", "",
         f"#define SND_LAYOUT 0x{layout:08x}u",
         f"#define SND_N_SFX {len(SFX)}   // groups 0 .. SND_N_SFX-1 are sound effects; the rest is speech",
         f"#define SND_VOICE \"{'none' if a.no_voice else a.voice}\"",
         f"#define SND_N_COLORS {len(COLORS)}   // " + " ".join(COLORS), f"#define SND_N_NUMBERS {len(NUMBERS)}", "",
         "typedef enum {"]
    h += [f"    SND_{g.upper()}," for g in names]
    h += ["    SND_GROUPS", "} snd_group_t;", ""]
    gid = {g: i for i, g in enumerate(names)}
    h += ["// lessons, in teaching order: first sentence group + sentence count", "typedef enum {"]
    h += [f"    LESSON_{n.upper()}," for n in LESSONS]
    h += ["    LESSON_COUNT", "} lesson_t;", "",
          "static const struct { uint16_t first, n; } LESSON[LESSON_COUNT] __attribute__((unused)) = {"]
    h += [f"    {{SND_{LESSON_GROUPS[n][0].upper()}, {len(LESSON_GROUPS[n])}}},   // {n}" for n in LESSONS]
    h += ["};", "", "// true/false quiz: question group, explanation group, answer",
          f"#define QUIZ_COUNT {len(QUIZ)}",
          "static const struct { uint16_t q, why; uint8_t truth; } QUIZ[QUIZ_COUNT] __attribute__((unused)) = {"]
    h += [f"    {{SND_Q_{k}, SND_QA_{k}, {1 if ans else 0}}}," for k, (_, ans, _) in enumerate(QUIZ, 1)]
    h += ["};", ""]
    # Robot School tables (school.c, pick.c). Step/quiz/teaser groups are SD-only: fx_has() says if they play.
    h += ["// ---- Robot School: lessons, steps (sentence + arm act + kid's turn), true/false questions",
          "typedef enum {"] + [f"    SA_{a.upper()}," for a in SL.ACTS] + ["    SA_COUNT", "} school_act_t;", "",
          f"#define SCHOOL_COUNT {len(SL.SCHOOL)}", "#define SCHOOL_QPER 3",
          f"#define SCHOOL_RANKS {len(SL.RANKS)}", ""]
    steps, quiz, rows = [], [], []
    for i, lesson in enumerate(SL.SCHOOL, 1):
        st = school_steps(lesson)
        rows.append(f"    {{{len(steps)}, {len(st)}, {len(quiz)}, SND_ST{i}}},   // {lesson['name']}")
        steps += [f"    {{SND_S{i}_{k}, {a}, {t}}}," for k, (_, a, t) in enumerate(st, 1)]
        quiz += [f"    {{SND_SQ{i}_{k}, SND_SQA{i}_{k}, {1 if ans else 0}}},"
                 for k, (_, ans, _) in enumerate(lesson["quiz"], 1)]
    h += ["static const struct { uint16_t step, nstep, quiz, teaser; } SCHOOL[SCHOOL_COUNT] __attribute__((unused)) = {"]
    h += rows + ["};", "", f"#define SCHOOL_STEPS {len(steps)}",
                 "static const struct { uint16_t g; uint8_t act, turn; } SCHOOL_STEP[SCHOOL_STEPS] __attribute__((unused)) = {"]
    h += steps + ["};", "", f"#define SCHOOL_QUIZ_COUNT {len(quiz)}",
                  "static const struct { uint16_t q, why; uint8_t truth; } SCHOOL_QUIZ[SCHOOL_QUIZ_COUNT] __attribute__((unused)) = {"]
    h += quiz + ["};", ""]
    # what each lesson is about, for the cloud intent check (cloud.c): a question maps to its closest lesson
    def topic(lesson):
        first = lesson["steps"][0] if isinstance(lesson["steps"][0], str) else lesson["steps"][0][0]
        title = first.split(". ", 1)[1] if ". " in first else first
        return f'{lesson["name"]}: {title.rstrip(".?!")}'.replace('"', "'")
    h += ["static const char *const SCHOOL_TOPIC[SCHOOL_COUNT] __attribute__((unused)) = {"]
    h += [f'    "{topic(L)}",' for L in SL.SCHOOL] + ["};", ""]
    with open(os.path.join(FW, "sounds_gen.h"), "w") as f: f.write("\n".join(h))

if __name__ == "__main__":
    main()
