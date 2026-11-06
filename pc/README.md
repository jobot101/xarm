# Johnny — the PC brain

The **Johnny** build of the arm firmware (`sdkconfig.johnny`, `firmware/main/pc.c`) makes the board the arm's ears,
mouth and hands. This folder runs on the shop PC and does the thinking:

mic (from the board, echo-cancelled) → voice detector → Whisper (faster-whisper, local) → "Johnny, …"
→ the model loaded in **LM Studio** picks one or two sentences to say and up to 3 arm commands from a fixed list
(router.c text, JSON-schema enum, so it can't invent one) → **ElevenLabs** speaks it in Johnny's voice
(Windows' own voice if the key/voice id are missing or the call fails) → board speaker.

- `run-johnny.bat`: first run builds `.venv`, then starts. LM Studio's server must be on (port 1234).
- `johnny.json` (made on first run, not in git): `elevenlabs_api_key`, `elevenlabs_voice_id` (the Oracle's Johnny
  voice), `board` (default `johnny.local`), `model`, `whisper_model`.
- `python johnny.py --text`: no board, type to him.
- `flash_johnny.py`: USB flash from the PC (`--muffin` puts Muffin back). `FLASH-HANDOFF.md` is the step list
  for the local model.

"Stop" never depends on the PC: MultiNet on the board keeps "stop" / "stop johnny" and cuts speech and motion.
Board protocol: see `firmware/main/pc.h`.
