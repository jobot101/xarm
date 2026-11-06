# Handoff for Claude Code: Johnny (xArm with its brain on the shop PC)

Read `CLAUDE.md` first (repo rules: router is the one command vocabulary, pick.c owns motion, push to `main` ships).
This file covers the Johnny work started on Nov 5 2026.

## What Johnny is

The owner retired **Muffin** (the kid's robotics teacher) from the arm and put **Johnny** on it: a **shop helper**
voiced like the Oracle phone's Johnny character, driven from the shop PC. Muffin's code stays in the repo
untouched; Johnny is a second build of the same firmware. The owner can go back with `flash_johnny.py --muffin`.

- **Board = hands, ears, mouth.** Mic (after the AFE's AEC/AGC) streams to the PC; speech from the PC plays on the
  speaker; plain router text from the PC moves the arm. MultiNet on the board keeps only "stop" / "stop johnny" /
  "johnny stop", so stop works with the PC down.
- **PC = brain** (`pc/johnny.py`): webrtcvad segments → faster-whisper (large-v3-turbo on GPU, small.en on CPU)
  → wake word "Johnny" (regex `WAKE`, plus a follow-up window after each answer) → LM Studio (OpenAI-compatible,
  localhost:1234, JSON-schema reply `{say, actions[]}` where actions is an **enum** of router phrases, max 3)
  → ElevenLabs streaming `pcm_16000` (Windows SAPI voice via pyttsx3 as fallback) → board, paced ≤ 2 s ahead.
  Half duplex: mic is ignored while he talks or thinks.
- **Persona rule carried over from the Oracle:** Johnny never says the surname "Cash". `Brain.think()` also strips it.

## Where things are

| piece | file |
|---|---|
| Kconfig switch `XARM_JOHNNY`, `XARM_NAME` (mDNS/DHCP/AP name) | `firmware/main/Kconfig.projbuild` |
| Johnny overlay (name `johnny`, update URL `fw/johnny/`) | `firmware/sdkconfig.johnny` |
| WebSocket link, protocol in the header comment | `firmware/main/pc.c`, `pc.h` |
| `JOHNNY` macro (0/1, for C expressions; an off bool Kconfig is undefined) | `pc.h` |
| `/ws` registered before the `/*` catch-all; `close_fn` → `pc_sock_closed()` | `web.c` |
| Mic tap: `pc_mic()` on every AFE fetch; no cloud ears; PC drives ring + lean-in | `voice.c detect_task` |
| Stop-only MultiNet list; `chat_exec` skipped; `pc_stopped()` on stop | `router.c` |
| Bank never plays (`enqueue` returns); `fx_talk_level`/`fx_speaking` read the PC speech so `talk_step` nods along; ring: dim gold = no PC, sapphire = listening, spinner = thinking, gold glow follows his voice | `fx.c` (`fx_pc()`) |
| School forced off, `school_set` refuses | `school.c` |
| Web app: header says Johnny + PC status pill, no Learning page/School panel when `state.brain` exists | `web/src/App.jsx` |
| CI job `johnny` (parallel; appends `sdkconfig.johnny` to `sdkconfig.defaults` because esp-idf-ci-action mangles quoted `-D` args; `make_sounds.py --no-voice` for the header only) → Pages `fw/johnny/` (own manifest, no sounds.bin) | `.github/workflows/build.yml` |
| PC brain, flasher, local-model step list | `pc/johnny.py`, `pc/flash_johnny.py`, `pc/FLASH-HANDOFF.md`, `pc/run-johnny.bat` |

`pc/**` is in `paths-ignore`: PC-only changes don't rebuild firmware. On the shop PC the same files live in
`C:\Users\mark\Documents\Ragamuffin Workshop\johnny` (copied, not a git checkout: re-copy after changes).

## Status (Nov 5 2026, evening)

- Johnny firmware **compiles clean in CI** (commit `fc33e8d`, first try after the CI quoting fix) and is published
  at https://jobot101.github.io/xarm/fw/johnny/. **Never run on hardware.**
- `johnny.py` was smoke-tested against a mock board (hello → greeting streamed, wake → think → cmd with id/reply →
  speech → listen). **Never run with real Whisper, LM Studio or ElevenLabs.**
- Flashing: the board is on **COM4** on the shop PC (Windows). The owner's local model (Cline + Qwen in LM Studio)
  was flashing it with `python flash_johnny.py --port COM4`. Result unknown: ask. Gotchas seen: Cline ran in the
  wrong folder, then left a path with a space unquoted in PowerShell. The owner's own MCP flasher (`flash_s3`) has
  a serial-number lock (`uv run ports.py mark-muffin COM4` registers the board) and flashes Muffin's files, so don't use it for Johnny.
- Muffin boards also took this build (shared code); Muffin behavior is unchanged except the hostname/AP now come
  from `CONFIG_XARM_NAME` (default `muffin`) and `CONFIG_HTTPD_WS_SUPPORT=y`.

## Open items, in order

1. **Get Johnny on the board and talking.** After flashing: ring breathes dim gold. WiFi: board keeps saved WiFi
   (keep-settings flash); else WiFi `Johnny` / `ragamuffin` → http://192.168.4.1 → ClarkShop. Then on the PC:
   LM Studio server on with a model loaded, `johnny.json` filled (`elevenlabs_api_key`,
   `elevenlabs_voice_id` = the Oracle's Johnny voice: ask the owner, it's in his ElevenLabs account), `run-johnny.bat`.
   If `johnny.local` doesn't resolve on Windows, use the board's IP (`--board 192.168.x.x`, shown on the board's web page).
2. **Add Johnny to the Ragamuffin Flasher** (owner asked; not done). Repo `jobot101/Ragamuffin-Flasher`: a
   "xArm Johnny" button next to Muffin's that reads `https://jobot101.github.io/xarm/fw/johnny/manifest.json` the same
   way it reads `fw/`. Touch only the flasher page (owner's rule: flasher additions never rebuild other projects).
3. **Hardware checks for Johnny** (all unverified):
   - The PC gets clean mic audio: AFE output is the AEC'd channel; Whisper accuracy at bench distance.
   - Speech plays without gaps: `pc.c` stream buffer is 256 KB PSRAM; PC paces ≤ 2 s ahead; a 200 ms gap ends a reply (`spoke` event).
   - httpd LRU purge never drops the `/ws` socket while the phone app polls (PC sends `{"ping":true}` every 2 s).
   - "stop" on the board cuts motion and speech mid-reply and the PC hears `{"ev":"stop"}`.
   - The model actually picks sensible actions; tune `PERSONA`/`COMMANDS` in `johnny.py`, never free text to the arm.
4. **Nice next steps once it works:** barge-in (full duplex once AEC is proven), a "say" path for long answers
   split by sentence for faster first audio, recorded moves ("do the <name>") already come from the board's hello.

## Owner preferences that matter here

Short, direct answers; no hedging; push straight to `main` (no test branches); additive changes, never swap things out
unless he says so; he wants to slow down a bit rather than pile on features. He works from a Pixel 4a and the shop PC.
