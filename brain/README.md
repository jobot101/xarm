# MotherShip — the PC brain

The **MotherShip** build of the arm firmware (`sdkconfig.mothership`, `firmware/main/pc.c`) makes the board the arm's ears,
mouth and hands. This folder runs on the shop PC and does the thinking:

mic (from the board, echo-cancelled) → voice detector → Whisper (faster-whisper, local) → "MotherShip, …"
→ the model loaded in **LM Studio** picks one or two sentences to say and up to 3 arm commands from a fixed list
(router.c text, JSON-schema enum, so it can't invent one) → **ElevenLabs** speaks it in MotherShip's voice
(Windows' own voice if the key/voice id are missing or the call fails) → board speaker.

- `run-brain.bat`: first run builds `.venv`, then starts. LM Studio's server must be on (port 1234).
- `brain.json` (made on first run, not in git): `elevenlabs_api_key`, `elevenlabs_voice_id` (the Oracle's MotherShip
  voice), `board` (default `shop-mothership.local`), `model`, `whisper_model`.
- `python brain.py --text`: no board, type to him.
- `flash_mothership.py`: USB flash from the PC (`--muffin` puts Muffin back). `FLASH-HANDOFF.md` is the step list
  for the local model.

"Stop" never depends on the PC: MultiNet on the board keeps "stop" / "mother ship stop" and cuts speech and motion.
Board protocol: see `firmware/main/pc.h`.
