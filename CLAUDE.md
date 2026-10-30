# CLAUDE.md — xarm-voice

Voice and web control for a Hiwonder xArm 1S. The brain is a Waveshare ESP32-S3-AUDIO-Board running ESP-IDF; a Hiwonder ESP32-S3 vision module (color mode) gives it eyes. The goal is "pick up the red" → the arm finds the red object, grabs it, and holds it at Home. README.md covers wiring and user setup; read it first.

## Status

- `core.c` passes its host tests. The frames match the byte examples in Hiwonder's protocol docs.
- The web app builds and has been checked against the mock API.
- **The firmware compiles clean in CI** (ESP-IDF v5.5.1, first try) and the `latest` release is published. It has **not been run on hardware yet**.
- The partition table already has two OTA app slots (`ota_0`/`ota_1`), and rollback is on in `sdkconfig.defaults`. The OTA code itself is task 3 below.

## The job, in order

The owner wants to **flash and update this board from an Android phone**. A PC is only used for building.

**1. Build.** Run the host tests, then `idf.py set-target esp32s3 && idf.py build`. Fix API drift (ESP-IDF 5.5, esp-sr 2.4.7, esp_codec_dev 1.6) until the build is clean, without restructuring anything.

**2. Phone flash kit (CI).** The owner works from a phone, so **GitHub Actions is the build machine** (`.github/workflows/build.yml`). Every push to `main` builds and publishes the GitHub release `latest` with the files below. Do the compile-fix loop by pushing and reading the Actions log (`gh run watch`, `gh run view --log-failed`); don't install ESP-IDF on the phone. The release contains:

| file | offset | used for |
|---|---|---|
| `bootloader.bin` | 0x0 | first USB flash |
| `partition-table.bin` | 0x8000 | first USB flash |
| `ota_data_initial.bin` | 0xd000 | first USB flash |
| `xarm-voice.bin` | 0x10000 | first USB flash, **and** every OTA update after that |
| `srmodels.bin` | 0x710000 | first USB flash (speech models: MultiNet7 + VADNet; only reflash if the models change) |
| `sounds.bin` | 0xA90000 | first USB flash (Muffin's voice; after that it syncs over WiFi) |

CI also writes `OFFSETS.txt` and an ESP Web Tools `manifest.json`, taking the offsets from `build/flasher_args.json`. Files land at `https://github.com/jobot101/xarm/releases/download/latest/<file>`.

The first flash happens from the phone over USB-C using **`flasher/`** (modes: *Full install, keep settings* = every part, no chip erase, so NVS + `store` survive; *Full install, erase*; *App only*), a one-tap page that CI deploys to https://jobot101.github.io/xarm/ (gh-pages branch) with that build's files baked in. Android Chrome can't open the S3's built-in USB-Serial-JTAG (303A:1001) through Web Serial, and esptool-js's own WebUSB mode only supports CH340. So the flasher hands esptool-js a port from Google's `web-serial-polyfill`, which drives the board's CDC-ACM interfaces over WebUSB. Keep baud at 115200 (USB-JTAG ignores it, and a baud change would reopen the port). Every flash after the first is OTA.

**Any phone (done):** the flasher picks its transport from the browser: Android Chrome → WebUSB (above); desktop Chrome/Edge → Web Serial (the OS driver owns CDC there, so WebUSB can't claim it); no USB at all (every iOS browser, Safari, Firefox) → USB tools hidden, a WiFi card links to `http://muffin.local/#update` (iOS) / `http://muffin/#update` (others) and `http://192.168.4.1/#update`. `#update` opens the web app in Workshop scrolled to Firmware, whose **Install latest** relays the build through the phone: fetch `manifest.json`, `sounds.bin` and `xarm-voice.bin` from GitHub Pages (CORS `*`), then `POST /api/sounds?v=<hash>` (update.c, same erase-ahead write as the WiFi sync, shares `s_bank_lock` with it, sets NVS `sndver`) and `POST /api/ota`. That's how an iPhone updates a board that has no internet. `/api/state` `fw.sounds` = the bank hash in flash. First installs, srmodels, partition changes and the camera stay USB-only.

**3. OTA from the phone.** Add this, following the existing patterns:
- `ota.c`: `POST /api/ota` takes the raw `.bin` body and streams it with `esp_ota_begin` / `esp_ota_write` (4 KB chunks, heap buffer) / `esp_ota_end`, then `esp_ota_set_boot_partition`, replies `{ok}`, and restarts after 500 ms. Refuse while `pick_busy()`, and call `pick_stop()` first. Raise httpd `recv_wait_timeout` for the upload.
- In `app_main`, after `web_start()` succeeds, call `esp_ota_mark_app_valid_cancel_rollback()`. A build that can't reach the web server rolls itself back.
- Add `fw: {version, partition}` to `/api/state`. `version` comes from `esp_app_get_description()`; set `PROJECT_VER` in the top-level CMakeLists.
- Web app: a **Firmware** panel with the current version, a `.bin` file picker, an upload progress bar (XHR `upload.onprogress`; fetch has no upload progress), then "rebooting…" and a poll of `/api/state` until it answers with the new version. Add a matching handler to `scripts/mock.js`.
- The web app is embedded in the firmware, so one `.bin` updates both the UI and the firmware.

**Auto-update loop (done):** with home WiFi set (web app → WiFi), `update.c` polls `https://jobot101.github.io/xarm/fw/manifest.json` every 60 s. When the `version` there (the short commit SHA; CI stamps the same value into `firmware/version.txt` → `PROJECT_VER`) differs from the running build, it installs `xarm-voice.bin` over HTTPS and reboots. Each version is tried once, so a build that rolls back isn't reflashed in a loop. **So shipping a change is just: push to `main`.** About 3 min of CI plus up to 1 min for the board's check. Don't break this path. If a change needs `srmodels.bin` or the partition table reflashed, say so, because the phone flasher has to do those.

**Play / Workshop (done):** the web app opens in **Play** mode for kids: big picture tiles, plain-words status, recording their own moves, and the Copy the Bells game. Hold the rose window for 3 s to reach **Workshop** (all the setup panels). Keep Play jargon-free. New tricks need a router phrase (voice), a Play tile, and an `OP_` in `pick.c`.

**Head-mounted camera (done, default):** `cfg.cam_on_head`. Picks start from the taught **Look** pose, then sweep `scan[]` base offsets. Calibration is taught from Look, and a base offset just rotates the picture, so it gets added to the grasp base.

**Ragamuffin Hall theme:** rose-window LED ring in stained-glass jewels, bell and organ sounds, a talking robot, and oak/brass/jewel UI. Keep new features in that voice.

**Muffin — sound, voice, conversation (done):** the robot is **Muffin**, the self-appointed star of Ragamuffin Hall: cocky, dramatic, sarcastic, but the joke is always on itself or the objects, never the kids. Everything it says or plays is pre-rendered by `tools/make_sounds.py` (Kokoro-82M neural TTS by default, `VOICE = "kokoro:<id>"`; `piper:<name>` still works. Rendered in CI, played offline, no API on the board; already-voiced lines come from the `~/.cache/muffin-tts` cache, so builds only voice new text; church bells / pipe organ with hall reverb; mastered for the speaker) into an IMA ADPCM bank, `sounds.bin` + `sounds_gen.h` (both generated, gitignored; CI renders them before every build). **The bank is not in the app:** it lives in the `model` partition at `FX_BANK_OFF` (after the speech models), the phone flasher writes it on a full install, and `update.c` re-downloads it over WiFi whenever the manifest's `sounds` hash changes (no reboot). A layout CRC keeps a mismatched app/bank pair silent instead of wrong. Lines live in `LINES` (variants per group; `{c}` per camera color, `{n}` per number); firmware says them with `fx_say(SND_X)` / `fx_say_color()` / `fx_say_n()`, mostly from `pick.c`. Conversation (name, small talk, jokes, mood, yes/no follow-ups, quiet mode, milestones) is `chat.c`, reached through the router like any command. **Teaching:** `LESSONS` (sentences, in order), `FACTS` and `QUIZ` in make_sounds.py; `pick.c do_teach()` says a lesson sentence by sentence and demonstrates on the arm (`teach_move()` matches moves by lesson + sentence number — keep them in step when editing a lesson); chat.c handles "teach me" / "tell me more" / topic questions / facts / the true-false quiz. Lessons render at a lower rate than banter if the bank runs short (`RATE_PLANS`). The bank starts at `FX_BANK_OFF` 0x380000 inside the 7 MB `model` (0x2C0000 on boards still on the old 6 MB layout: `fx_bank_off()` picks by partition size, so an OTA'd app keeps talking until the next USB install). `BUDGET` = `FX_BANK_MAX` 0x340000 fits both. CI fails if srmodels.bin ever grows into the bank. The `voice-preview` branch renders WAVs of candidate voices and every clip, committed back to that branch.

**Robot School (done, not yet run on hardware):** `school.c` turns Muffin into a self-running, voice-only teacher with a daily time limit (Workshop → School; default 20 min per kid per day, Kconfig `XARM_SCHOOL_MIN`). The first thing a kid says starts class: greeting (+ "who's here?" if several kids, answered with "hi I'm …"), streak, a warm-up quiz on past lessons (missed questions first, `recap_pick`), the next lesson, a two-question check, "another lesson?" while ≥ 6 min remain (max 3 new lessons a day), then free play. At the limit it finishes the sentence, says tomorrow's teaser, bows and sleeps (dim amethyst ring) until the next day; asleep it answers only "stop" and the grown-up passphrase (NVS `sc_pass`, also a MultiNet phrase; +10 min). "Stop" or two unanswered prompts pause class (clock stops) and the next word resumes it. **App commands (`/api/cmd`, `router_exec_ex(app=true)`) are never limited: the app is the grown-up's controller.** The router asks `school_filter()` first (sleep, start, true/false, yes/no, names, passphrase) and every action branch goes through `RUN(K_*, …)` → `school_allow()`, which during "your turn" accepts only the asked-for `cmd_kind_t` and otherwise says "class first". Lessons are `SCHOOL` in **`tools/school_lessons.py`** (53 lessons: sentences with an arm act from `ACTS` and an optional kid's turn named after `cmd_kind_t` in core.h, which make_sounds.py parses; three true/false questions; a teaser). They're **SD-only groups**: in `sounds_gen.h` and in the flash bank's group table with no clips; the SD bank stores them as 16 kHz IMA ADPCM (rate byte bit 7) next to the PCM clips. `fx_has()` tells School whether they're playable; without the card it says so (flash line `sc_nocard`) and the class is timed free play. `pick.c do_lesson()` (OP_LESSON, slot = first step | end << 16) plays a lesson part up to the next turn, running each step's act while the sentence plays. Progress per kid + guest is an NVS blob in "store"/"school" (`PROG_MAGIC`); kids.c resets a slot's progress when it's freed or reused. Days: SNTP when there's WiFi (writes the PCF85063 RTC at 0x51), else the RTC if it kept time, else each boot is a new day; TZ from Kconfig `XARM_TZ`. Pure helpers (`civil_days`, `streak_next`, `pass_heard`, `recap_pick`, `kind_fits`, `rank_of`) are in core.c with host tests. Web: `GET/POST /api/school`, Workshop → **School** panel (minutes, passphrase, per-kid progress, +10 min, reset today, start over).

**WiFi joining (done):** the station config allows WPA3 (`pmf_cfg.capable`, `sae_pwe_h2e` both) and picks the strongest AP of that name. If home WiFi isn't joined in 15 s the "Muffin" AP carries the app and home WiFi retries every 45 s (APSTA stays on). `net_why()` turns the last disconnect reason into plain words (not found / 5 GHz, password, security type, weak signal, refused) → `/api/state` `net.why` → WiFi card.

**Camera switch (default OFF):** the owner runs Muffin with no camera. `cam_init()` reads NVS `cam_on` (Workshop → Settings → Camera, `/api/fx {cam}`, restarts) and with it off never polls the module: `cam_ok()` stays false so the app hides camera tiles and the Vision panel, `router_phrases()` drops pick / sort / stack / tower phrases, and those commands get `SND_NO_EYES`. Look/scan acts become plain moves. Lessons teach robot cameras in general, never as Muffin's own. The wireless-camera work was reverted. One child uses Muffin, so School never asks "who's here?" (only with 2+ kids set up).

**Lifelike motion (done, not yet run on hardware):** travel between poses goes through `pick.c travel()` instead of `go()`: a wind-up (a small breath up along `cfg.lift` and a turn away), a look-first glance (base 30% toward the target, wrist/twist aiming early, a beat), then an arc streamed at 25 Hz by `arc_pose()` (core.c, tested: per-joint min-jerk with big joints leading by `LAG[]` and a mid-flight rise along `cfg.lift`), then follow-through (base overshoots 6-18 units and settles). Timings get ±10% jitter. Moves under 90 units stay plain `go()`, and fine moves (onto an object, setting it down) always do. `react()` adds a quick proud bob after a good grab or a right quiz answer and a slump + head shake after a miss or not-found (`OP_REACT` for School). Idle life (pick task, between jobs, only resting at Home, torque on, not holding, and no app jog/drive/limp since the last job: `arm_manual_us()`): breathing and the odd glance for 90 s after the last job or "Hey Muffin" (`pick_attention()` perks it up), then still; head down with slow breaths while School has it asleep. **Facing the speaker:** with "Use both mics" on, voice.c keeps the last second of MIC1 and MIC2 (PSRAM) and on a wake phrase cross-correlates the loudest frames over ±5 samples (`doa_estimate()`, -1..1); `pick_face()` turns the resting pose's base up to ±140 units that way (Workshop → Voice → Turn toward voices, NVS `face` 1/-1 flipped/0). **Mood:** `chat_mood()` scales every flourish (`mood_k` 0.64-1.36) and tempo (`mood_ms` 1.18x-0.82x); grumpy rests slumped, delighted perky and glances more. **Listening/thinking:** while the voice window is open (`pick_listening()` from voice.c) the resting pose leans in along `g_reach` and cocks the head (wrist rotate +45); `think()` ("hmm": up, off to one side, a beat) runs before School's verdict on each answer (`OP_REACT` slot 4, then 1/0). **Talking gestures:** fx.c meters its own speech while it plays (`fx_talk_level()`, speech = groups ≥ `SND_N_SFX`); `talk_step()` nods the wrist on loud onsets (every 4th adds a base sway and head turn) around the current pose. Lessons use `talk_along()` instead of `wait_quiet()`; idle life nods along to anything it says between jobs. **Boredom:** 90 s with nobody around → stretch up and over, yawn (`SND_YAWN`), droop into a nap with slow breaths; after 10 min it holds the nap pose; its name or ears opening wakes it. **Natural wobble:** breathing depth, pace and a little base/wrist drift ride on pink noise (`pink_next`, core.c, tested). **Celebrations:** `OP_CELEBRATE` (up, a spin each way with bells, jazz hands, a bow; fanfare + `SND_CELEBRATE`) on School rank-ups, finishing the course, streaks of 3 and every 5, and a new tower record (`kids_score()` now returns true on a new best). **Dancing on the beat:** voice.c keeps 8 s of MIC1 loudness in 16 ms hops; on "dance" `voice_beat()` runs `beat_find()` (core.c, tested: onset autocorrelation 70-180 bpm + comb phase) and, if there's a clear beat in the room, `beat_moves()` lands 16 moves on it (state "grooving": party lights, no tune of its own); otherwise the usual dance. Workshop → Settings → Lifelike motion (NVS `anim`, `/api/fx {anim}`, `state.arm.anim`) turns it all off. Sources: Takayama, Dooley & Ju 2011 (anticipation + reaction), Dragan, Lee & Srinivasa 2013 (legible motion, within a trust region), Ribeiro & Paiva 2012 (Disney's principles on robots).

**SD card voice (done):** the TF slot is SDMMC 1-bit, CLK 40 / CMD 42 / D0 41, D3 on TCA9555 EXIO3 (driven high so the card starts in SD mode); `sd.c` mounts it at `/sdcard` with `format_if_mount_failed = false` (no exFAT in this FatFs: 64 GB+ cards are reported, never formatted). CI also renders `sounds_hq.bin` (same groups + layout CRC, magic `SNP1`, 16 kHz 16-bit PCM, ~25 MB) and lists `sounds_hq` / `sounds_hq_size` in manifest.json; it's never flashed. With **Workshop → Voice → Store voice on SD card** on (NVS `sd_voice`, off by default), `update.c sync_sd()` downloads it to `/sdcard/muffin/sounds.tmp` (Range resume, fsync every MB, sha256 check), swaps it in as `sounds.bin`, and `fx.c` plays from the card in preference to flash; a failed card read falls back to the flash bank mid-session. Muffin writes nothing outside `/sdcard/muffin/`.

**Camera firmware (done):** `cam/xarm_color` is Hiwonder's color sketch (originals in `cam/hiwonder`, fetched by `fetch-cam.yml`), patched with:
- the two arm colors set in `colors.h` (0 red, 1 yellow, 2 green, 3 blue, 4 purple), on regs 0x00/0x01, plus all five colors on 0x10–0x14;
- a fix for their max-area bug.

The CI `camera` job builds it with arduino-esp32 2.0.17 (bundled esp-dl), 4 MB flash, huge_app, for both quad and octal PSRAM. The flasher's **Camera** button handles it. HSV thresholds live in `color_detection.cpp`.

**4. Hardware bring-up:** work through the unverified list below with the owner.

The phone gets its files from the `latest` release.

## Build / test

```
cd firmware/test && cc -I../main test_core.c ../main/core.c -lm -o t && ./t     # pure logic
cd web && npm i && npm run build      # also writes firmware/main/www/index.html.gz
cd web && npm run mock & ARM=http://localhost:8787 npm run dev                    # UI with no hardware
cd firmware && idf.py set-target esp32s3 && idf.py build flash monitor          # IDF >= 5.5, PC only
git push                              # CI builds + publishes release "latest"
```

## Architecture rules

- **One command vocabulary.** `router.c` turns text into actions. Voice phrases come from `router_phrases()`, and the web app posts plain text to `/api/cmd`. Add a command in the router once and both inputs get it. Don't add a voice-only or web-only action path.
- **Pure logic goes in `core.c`** (protocol, parsing, math) with a test in `firmware/test`. No ESP-IDF includes there.
- **Motion is streamed, never sent whole.** `arm.c` runs a 25 Hz task that walks each joint along a curve (min-jerk for `arm_move`, constant speed for `arm_move_lin` path segments, accel-limited chase for `arm_follow_one`, velocity for `arm_drive`) and sends each setpoint with a 60 ms move time, so servos never stop between frames. Don't write servo move frames anywhere else. `arm_hold()` brakes in place. The curve math lives in `core.c` (tested).
- **`pick.c` owns motion sequences.** One job at a time (queue length 1). `pick_stop()` must stay callable from any task and preempt within ~20 ms. Every new wait inside a sequence goes through `wait_ms()`.
- **Config lives in `cfg_t` (store.h).** If you change the struct layout, bump `MAGIC` in store.c, otherwise old NVS blobs load as garbage.
- **One shared I2C bus** (`i2c_master` driver, created in main.c) carries the codecs, TCA9555, RTC and the cam. Don't call the legacy `driver/i2c.h` API, because mixing the two drivers aborts at boot.
- **Web app:** keep it one gzipped file served from flash. Rebuild the web app after UI changes, since the firmware embeds `www/index.html.gz`. Home and Drop poses are taught on the Teach panel; Settings must never post them back.
- Servo index `i` = bus ID `i+1`. On the 1S: 1 gripper, 2 wrist rotate, 3 wrist, 4 elbow, 5 shoulder, 6 base. Positions run 0–1000.

## Unverified: check on hardware

1. **esp-sr / esp_codec_dev API and sdkconfig names.** `voice.c` follows xiaozhi-esp32 (esp-sr 2.4.7). Check that `CONFIG_SR_MN_EN_MULTINET7_QUANT` and friends exist under menuconfig → ESP Speech Recognition, and that the `model` partition gets flashed.
2. ~~Vision module data format~~ **Resolved from Hiwonder's source:** 4 × uint8 x,y,w,h on a 240×240 frame.
3. **Camera PSRAM type.** The flasher picks the octal or quad build from the chip features. Check on first flash that the cam boots (Boot log).
4. **Mic channels + AFE.** `audio.c` reads ES7210 TDM slots 0 (MIC1) and 1 (MIC3 = speaker reference, per xiaozhi's `AUDIO_INPUT_REFERENCE`) as AFE format `"MR"`; "Use both mics" adds slot 2 (MIC2) as `"MRM"`. The Voice panel shows which VAD loaded (VADNet expected after a keep-settings full install). Boot log prints the AFE pipeline. Check that AEC actually cancels the bells (say "stop" during a trick) and that the level meter peaks around −12 to −6 dBFS when talking at arm's length (gain 24 dB default, AGC on).
5. **Gripper direction and lift sign.** Defaults are open 200 / close 650 and shoulder lift −120. Users fix these in Settings.
6. **Controller TX logic level** (3.3 V expected), and that the vision module doesn't pull SDA/SCL up to 5 V.
7. **MultiNet phrase rejection.** `load_commands()` logs rejected phrases. Keep phrases short and plain English.

## Pin facts (from xiaozhi-esp32 `boards/waveshare/esp32-s3-audio-board`)

- I2C: SDA 11, SCL 10.
- I2S: MCLK 12, BCLK 13, WS 14, DIN 15, DOUT 16.
- WS2812 ring: GPIO38 (6 LEDs).
- TCA9555: EXIO8 = speaker PA enable, EXIO0/1 = LCD/touch reset, EXIO5 = DVP camera reset.
- TF card: CLK GPIO40, CMD GPIO42, D0 GPIO41, D3 = TCA9555 EXIO3, D1/D2 not connected (1-bit SDMMC).
- Header IO3/4/5/7/8/9 = LCD SPI pins, free with no LCD. Arm UART is TX 5 / RX 6. IO19/20 = USB, don't use.

## Next features (natural extension points)

- **Feedback:** LED ring states (listening/heard/moving/fail) via `led_strip` on GPIO38. Spoken or beep replies via ES8311: init per xiaozhi `box_audio_codec.cc`, PA on TCA9555 EXIO8. Hook both in `pick.c set()` and in the voice detect handler.
- **Wrist alignment:** use the blob's w/h to set wrist rotate (id 2) for long objects.
- **Hiwonder action groups:** `xf_action()` exists. Wire router phrases like "wave" to `XCMD_ACTION_RUN`.
- **Better recognition (done):** esp-sr 2.6.0 AFE (AEC, AGC, **VADNet**, falling back to WebRTC VAD if srmodels.bin predates it; BSS when "Use both mics" is on, an NVS setting read at boot, Kconfig `XARM_MIC_DUAL` is only the default) sits in front of MultiNet7. A command only fires if the VAD heard speech within `XARM_VOICE_VAD_HOLD_MS`; weak matches while the speaker plays are dropped. Voice phrases are 3+ syllables and acoustically distinct; keep new ones that way.
- **"Hey Muffin" (done):** no WakeNet (a custom WakeNet word is an Espressif training job). Instead every phrase containing "muffin" opens an 8 s window (`voice_engage()`); commands heard inside it extend it; in a live conversation (engaged in the last 60 s) it also stays open while Muffin talks + 6 s, so answers to Muffin's questions need no name. Stop phrases always pass. App commands (`/api/cmd`) and idle-chatter questions call `voice_engage()` too. The ring glows sapphire while open (`fx_listening`). Workshop → Voice switches the gate off (NVS `wake`). Ignored-while-asleep matches show in "What Muffin heard" as `asleep - say Hey Muffin`.
- **Cloud ears (done):** `cloud.c` sends each utterance heard while the "Hey Muffin" window is open (VAD-segmented in `voice.c seg_feed`, 320 ms pre-roll, ≤ 8 s) as a WAV to Groq Whisper (`XARM_STT_URL` / `XARM_STT_MODEL`, OpenAI-compatible, `verbose_json` so Whisper's no-speech verdict drops noise). The key is NVS-only (`stt_key`, pasted in Workshop → Voice; never in the repo, `/api/state` shows only `gsk_…wxyz`). Text → `text_clean()` (core.c, tested: strips wake word / "can you" / "please", folds yes/no) → `router_exec`. MultiNet keeps "stop" (instant, and the cloud's "stop" for the same clip is skipped), the wake word, and its guess as the fallback when the call fails. 401 → key flagged bad; 429 → 60 s on-board; network error → 20 s on-board; `XARM_STT_DAILY_MAX` caps requests. Unknown text → `SND_HUH` (max once per 8 s). Clips that start during Muffin's playback and end right after it are dropped as echo.
- **Smarter cloud ears (done, not yet on hardware):** Whisper model default is full `whisper-large-v3` (better with kids). `cloud_hint()` puts what Muffin is waiting for (School: "That's true. That's false.", the turn's phrase, kid names) at the front of Whisper's prompt. Low-confidence transcripts (`avg_logprob` < -0.9) are flagged `unsure`. `voice.c run_cloud()`: the router first (unless unsure); anything it doesn't understand goes to `cloud_intent()` — a Groq chat model (`XARM_INTENT_MODEL`, llama-3.1-8b-instant, same key) that must answer with exactly one line: a phrase from `router_phrases()`, `LESSON <n>` for a question about robots / machines / electronics / math / physics (lesson topics from `SCHOOL_TOPIC` in sounds_gen.h), `OFFTOPIC` for any other question, or `NONE`. Anything else it writes is discarded; it never authors speech (Muffin's voice is pre-rendered). A command runs through the router; a lesson plays whole via `school_explain()` (`OP_LESSON | LESSON_EXPLAIN`: no kid's turns; not mid-lesson; needs the SD lessons); off-topic says `SND_OFFTOPIC` ("I only talk about robots, math, and physics"); NONE says "Huh?". During a School class `voice_hold_open(true)` keeps the ears open: no "Hey Muffin" needed until it pauses or sleeps. True/false answers also accept yes/no, right/correct, wrong/not true.
- **Next: Muffin's new personality (agreed, not built yet; its own change after the current push has run on hardware).** Replaces the cocky-star voice everywhere: `LINES`, `LESSONS`, `FACTS`, `QUIZ` in make_sounds.py and all of `tools/school_lessons.py` (all in: every lesson rewritten in character, teaching kept intact). The character: **lovable mischief-maker** — playful troublemaker, teases objects, sneaks in surprises. **Humor:** silly and absurd (talks to the bell, wild exaggeration, odd theories). **Talk:** chatty storyteller, tangents and side comments (still short sentences: while it talks it only hears "stop"). **Running bit:** it hid a snack stash and can't remember where; drops clues, suspects objects, never solves it. **Toward the kid:** neutral, not gushy, but clearly wants them to learn and thrive. **Right answer:** calm and steady ("That's right. On we go."). **Wrong answer:** normalizes it ("Engineers get it wrong all the time. That's how they learn."). Voice stays Kokoro `am_puck`.
- **Open-vocabulary questions (next, not built):** BabyTalk (Citrinet-256, 4-bit, 6 MB, ESP-IDF C API) as a second ear for questions after "Hey Muffin", transcript fuzzy-matched to Muffin's answer lines. It doesn't fit 16 MB alongside two app slots + MultiNet + the sound bank; needs a storage decision first.
- **LLM layer:** anything that can produce router text (xiaozhi MCP, a server) can drive the arm through `router_exec()`.
- **Push instead of poll:** a websocket for `/api/state` if 4 Hz polling gets in the way.

## Wakes up wanting company; Learning page
- Power-on: ~8 s after boot Muffin perks up, waves, and (School on, time left) says a wake line (sc_wake) with its
  ears open 20 s: any answer starts class. School off: wave + greet. Between classes, nobody talking for 4 min,
  it invites them back (sc_invite), up to 3 times, then lets the boredom arc take it to a nap. No clock runs
  until class starts; skipped while the app moved the arm recently. (school.c reach_out)
- Voice pace: KOKORO_SPEED 0.92 (slowed for a kid).
- App main page = Learning page (web/src/panels/Learn.jsx): course by part, progress, rank, done lessons open
  to re-read, next lesson's teaser, later ones title only. No robot controls; STOP stays. Data comes from
  tools/lessons_json.py -> web/src/lessons.json (CI runs it before the web build).
- Both mics: the ES7210 opens all 4 TDM slots; voice.c drops slot 3 before the AFE (MRM).
- Answer pop-up: while School waits for an answer (true/false, yes/no, who's here, a "your turn" command, "shout
  stop"), /api/school carries `ask` {kind, q, options[{label, say}]} (school.c ask_json) and the Learning page pops
  up those buttons. A tap posts /api/cmd {text: say, voice: true}: router_exec_ex(app=false), so it goes through
  school_filter/school_allow exactly like speech. STOP stays above the pop-up.
- Taps only (School setting `tap`, NVS sc_tap, default ON): with School on, the ears are shut (voice.c window_open
  false; router_exec drops everything but "stop" and the passphrase); the kid only uses the Learning page.
  Idle/paused: ask {kind start|resume} -> a Start class / Keep going button on the top card. Free play: ask
  {kind free} -> a pop-up of tricks. Lines with a _tap twin (sc_tf_how, sc_turn_again, sc_more, sc_free, sc_away,
  sc_wake, sc_invite) say "tap" instead of "say" (TAP() in school.c). Lesson stories still say "say, a little up".
- Borrowing an object: lesson turns "fetch" / "giveback" (K_FETCH / K_GIVEBACK, School-only kinds) with act
  "handoff" (reach toward the kid, hand open unless holding). Fetch: pop-up Clamp! (= "grab it", K_CLOSE), then
  pick_caught() reads the gripper: missed -> open, ask again (3 tries), then pretend. Giveback: pop-up Let go
  (K_OPEN); nobody takes it -> put it down at the drop spot. wrap() never sleeps holding something. Used in
  lessons 5 torque, 6 balance, 7 payload, 8 grip, 9 inertia (smooth carry, jerky demo stays empty-handed),
  10 gravity, 25 safety (safe handoff), 45 pick and place, 46 assembly line, 50 inspection, 53 teamwork.
  Between fetch and giveback: no jerky/fast/dance acts (flinging); wave/dance/open acts skip while holding anyway.
- Start lesson (Learning page): POST /api/school {lesson: i} -> pick_lesson(): any lesson the kid has reached
  (done ones replay: "Do it again" inside an opened lesson; the next one: "Start lesson"). When class is waiting
  it starts class straight into that lesson (no warm-up); in free play it ends play, runs it, then back to play.
  Replays don't move progress or give the finish star (quiz stars count). /api/school can_pick says when.
