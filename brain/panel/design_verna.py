"""design_verna.py — create Verna's voice (the Oracle phone's operator) with ElevenLabs Voice Design and give it to
Shop MotherShip. Uses the API key in ../brain.json (never printed).

    python design_verna.py previews        # makes 3 takes of Verna, saves them as verna_take_1..3.mp3 to listen to
    python design_verna.py pick 2          # turns take 2 into a saved voice; writes its voice_id to brain.json and
                                           # to the Oracle phone's render/voices.yaml (verna.voice_id)
    python design_verna.py render          # pre-renders MotherShip's fixed lines in Verna's voice (lines/*.pcm)

Verna's design and settings come from Oracle Phone/oracle-characters/oracle-characters/render/voices.yaml.
"""
import base64, json, os, re, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                          # the brain folder
CFG = os.path.join(ROOT, "brain.json")
ORACLE_VOICES = r"C:\Users\mark\Documents\Ragamuffin Workshop\Oracle Phone\oracle-characters\oracle-characters\render\voices.yaml"
DESIGN = ("woman, mid-30s, alto, quick and dry, rural central Pennsylvania with a light Pennsylvania Dutch lilt, "
          "clipped and efficient, warm underneath, never rushed")
SAMPLE = ("Number please. Hold the line a moment, I'm putting you through. There, you're connected now, go ahead. "
          "MotherShip online, and the bench is ready whenever you are.")
LINES = {"online": "MotherShip online.", "hello1": "Hello.", "hello2": "Evening, friend.", "hello3": "Welcome back.",
         "hello4": "Hey there.", "brain_down": "Brain's down. Check LM Studio."}
STATE = os.path.join(HERE, "verna_previews.json")


def cfg(): return json.load(open(CFG, encoding="utf-8"))


def call(path, body=None, raw=False):
    c = cfg()
    if not c.get("elevenlabs_api_key"): sys.exit("Put your ElevenLabs API key in brain.json (elevenlabs_api_key) first.")
    req = urllib.request.Request("https://api.elevenlabs.io" + path, data=json.dumps(body).encode() if body is not None else None,
                                 method="POST" if body is not None else "GET",
                                 headers={"xi-api-key": c["elevenlabs_api_key"], "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=120) as r: return r.read() if raw else json.loads(r.read())
    except urllib.error.HTTPError as e:
        sys.exit(f"ElevenLabs said {e.code}: {e.read()[:300].decode(errors='replace')}")


def previews():
    r = call("/v1/text-to-voice/create-previews", {"voice_description": DESIGN, "text": SAMPLE})
    keep = []
    for i, p in enumerate(r.get("previews", []), 1):
        f = os.path.join(HERE, f"verna_take_{i}.mp3")
        open(f, "wb").write(base64.b64decode(p["audio_base_64"]))
        keep.append(p["generated_voice_id"])
        print(f"take {i}: {f}")
    json.dump(keep, open(STATE, "w"))
    print("Listen, then: python design_verna.py pick <n>")


def pick(n):
    ids = json.load(open(STATE))
    r = call("/v1/text-to-voice/create-voice-from-preview",
             {"voice_name": "Verna Kauffman (Oracle operator)", "voice_description": DESIGN, "generated_voice_id": ids[n - 1]})
    vid = r["voice_id"]
    c = cfg(); c["elevenlabs_voice_id"] = vid
    json.dump(c, open(CFG, "w", encoding="utf-8"), indent=2)
    print("brain.json -> elevenlabs_voice_id set")
    if os.path.exists(ORACLE_VOICES):                 # the phone shares the same Verna
        s = open(ORACLE_VOICES, encoding="utf-8").read()
        s2 = re.sub(r"(?m)^(verna:\n(?:  .*\n)*?  voice_id: )''", lambda m: m.group(1) + f"'{vid}'", s, count=1)
        if s2 != s: open(ORACLE_VOICES, "w", encoding="utf-8").write(s2); print("Oracle voices.yaml -> verna.voice_id set")
    print("voice_id", vid)


def render():
    c = cfg()
    out = os.path.join(HERE, "lines"); os.makedirs(out, exist_ok=True)
    settings = c.get("elevenlabs_settings") or {"stability": 0.5, "similarity_boost": 0.8, "style": 0.2, "use_speaker_boost": True, "speed": 1.04}
    for key, text in LINES.items():
        pcm = call(f"/v1/text-to-speech/{c['elevenlabs_voice_id']}?output_format=pcm_16000",
                   {"text": text, "model_id": c.get("elevenlabs_model", "eleven_flash_v2_5"), "voice_settings": settings}, raw=True)
        open(os.path.join(out, key + ".pcm"), "wb").write(pcm)
        print(f"{key}: {len(pcm) / 32000:.1f} s")


if __name__ == "__main__":
    a = sys.argv[1:] or ["previews"]
    {"previews": lambda: previews(), "pick": lambda: pick(int(a[1])), "render": lambda: render()}[a[0]]()
