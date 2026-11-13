# PS5LM Voice

A modified copy of [cobanov/PS5LM](https://github.com/cobanov/PS5LM) (GPL-3.0), forked from v0.11.0
(`e2a8cf4`, 2026-11-09). It installs beside the stock app under its own title ID, **PPSA99590**, and shares
its data folder (`/data/PS5LM`: models, settings, `app-args.txt`). Run one of the two at a time.

What is changed, newest last:

- 2026-11-11: own title ID and name (`ps5/app/sce_sys/param.json`), version `0.11.0-lean.N`; CI keeps the built
  title folder as a download (`.github/workflows/native-build.yml`).
- 2026-11-11: prints never wait for storage (`ps5/app/log_sink.cpp`). stdout and stderr go through a non-blocking
  pipe into a 2 MiB memory ring, and one thread writes the ring to `app.log`. Stock PS5LM writes them straight to
  `/data`, and a slow write there stopped the whole app (cobanov/PS5LM#70). The memory line in `app.log` also gives
  the sink's figures, and a memory line comes once a second while a model loads. Host test: `log_sink_test.cpp`.
- 2026-11-11: the log sink wraps the print functions at link time instead (`--wrap` for printf, fprintf, vprintf,
  vfprintf, puts, fputs, fputc, putc, putchar, fwrite in `scripts/build-app.sh`). On the console a title may not
  `dup2` over its streams (EPERM), so the pipe never started there.
- 2026-11-11: screen-off mode. `off` on the first line of `/data/PS5LM/screen.txt` starts the app without its screen
  (no GL display, dashboard, fonts or sounds), which frees the screen's ~0.42 GiB of the pool for the model. The
  TV shows nothing from the app; notifications and the PC's request files (`load`, `unload`, `memory`, `quit`)
  still work. Any other content, or no file, keeps the screen.
- 2026-11-11: Settings > Screen. "Turn the screen off" writes `screen.txt` = `off` and restarts the app (as a model
  switch does), so the model loads again with the screen's memory. "Turn the screen off now (test)" closes the GL
  display in place, no restart, and logs the memory before and after. With no screen, holding Triangle for 2 s
  writes `on` and restarts with the screen. The PC can do the same with a request file `screen` holding `off`,
  `off-now` or `on`.
  Measured: restart 1.80 GiB free when ready; in place, all free memory 1.46 -> 1.80 GiB but the largest block stays
  1.45 (the screen's memory comes back as separate blocks beside the loaded model).
- 2026-11-11: memory lines also give all free direct memory and its block count, not only the largest block.
- 2026-11-11: a projector pairs by its GGUF header when its name does not: a `general.type` `mmproj` file in the
  model's folder with the same `general.base_model.0.repo_url` or `general.basename` (Unsloth's `mmproj-F16.gguf`
  pairs with every Qwen3.8-27B build, no rename). The name rule still comes first; of several matches the largest
  file wins. Choosing a model in the library renames `app-args.txt` to `app-args.off.txt` instead of deleting it.
- 2026-11-11: the Home screen's context size comes from the server (`/slots` `n_ctx`), so a model loaded from
  `app-args.txt` no longer shows "of 0k".
- 2026-11-11: the planner gives vision models with a causal projector (Qwen's) `-ub 256`, `-b 1280` and
  `--image-max-tokens 1280` instead of the 2048 ubatch, keeping 0.5 GiB of headroom for photos. On the console that
  ubatch alone freed 0.57 GiB (0.90 -> 1.47 GiB free at load) at the same speed and answers. Other projectors keep 2048.
- 2026-11-11: the log sink's review fixes from cobanov/PS5LM#72: a `puts` line goes into the ring whole, a drop is
  always noted, and bytes storage refused count as refused, not written (the memory line says so).
- 2026-11-11: the title heap hands memory back (`patches/sdk`, `ps5/app/heap_info.cpp`). The heap is the payload
  SDK's (`mihawk-99/PS5_PayloadSDK`, pinned at `b83202be`): dlmalloc in eight arenas over direct memory. At the pin
  it maps its segments back to back, dlmalloc joins them, and a joined segment can only go back whole, so one block
  left near its end keeps everything below it. `patches/sdk` are four later commits of that fork, by its author,
  path-filtered to the heap: `8278ca5e` (calloc clears recycled memory), `3752be7e` (arena locks sleep instead of
  spin), `9d048784` (2 MiB segments) and `55a2de04` (a 64 KiB gap between mappings so segments never join,
  `ps5_heap_trim`, and the heap's in-use figure). `scripts/build-app.sh` exports the pin, applies them, and links
  the two heap objects ahead of the SDK's archive (the pinned checkout and the SDK cache stay as pinned);
  `scripts/test-native-build.sh` runs the SDK's host tests on the patched copy and records the patches' hashes.
  The app trims the heap 0.25 s after each request and every 10 s. Request files: `trim` (now, `on`, `off`) and
  `memory each` / `memory off`; the memory line ends with exact MiB, the heap's in-use and its segments.
  `/data/PS5LM/heap.txt` (MiB) sets the size from which a block gets a mapping of its own. Drop `patches/sdk` once
  PS5LM pins an SDK at or past `55a2de04`; the app still builds against a stock SDK, without the trim.
  Measured (IQ2_S, v9, screen off, 100 photos): free when ready 1,843 -> 1,942 MiB (largest block); heap after 100
  photos 1,311 -> 1,108 MiB; largest block after 100 photos 1,229 -> 1,364 MiB.
- 2026-11-11: vision warm-up (`ps5/app/vision_warmup.cpp`). Once the model is ready, with `--mmproj` and
  `--image-max-tokens N` in its arguments, the app sends its own server one image at the cap (1280x1024 for N =
  1280, 32 pixels a token side), so the vision encoder's GPU buffers are sized once. llama.cpp warms it with a square
  of at most the cap (35 x 35 = 1,225 tokens), and every photo with more tokens than any before rebuilt them bigger:
  the new buffer was cut from the largest free block and the old one left a hole. Measured from the PC on lean.10
  (one such image first, then 25 photos): largest block after 25 photos 1,364 -> 1,586 MiB, and no steps at photos
  15 and 23. `off` in `/data/PS5LM/warmup.txt` turns it off. Host test: `vision_warmup_test.cpp`.
  On the console (lean.11, Qwen3.8-27B UD-Q2_K_XL + F16 projector, full-size photos, screen off): ready with 888 MiB
  free (largest block), warm-up 11.6 s, then 434 MiB through 25 photos with no step; on lean.10 with the same image
  sent from the PC, 125 photos ran with 0 errors at a median 12.0 s, largest block 490 -> 466 MiB, all free 638 ->
  618 MiB.
- 2026-11-11 (lean.12, from ntindle's cobanov/PS5LM#73 and #74):
  - `sce_sys/param.json` declares a 256 MiB flexible pool (the system's default is 448), so the direct pool models
    use is 12.1875 GiB instead of 12 (#73 measured +192 MiB). The system libraries live in the flexible pool: watch
    the memory line's "flexible ... free" (#73 saw 111 MiB free with the app serving and its screen on). It takes
    effect when the title starts from the home screen; a restart for a model keeps the pool it started with.
  - With the screen off, the system's splash stays up (the TV shows the app's picture, not black), as #73 does for
    fleet workers.
  - `patches/0010`: llama.cpp's model file reads through an 8 MiB stdio buffer on the console. #74 measured a
    title's read calls at about 1.4 ms each (64 KiB reads at 70 MB/s, 8 MiB at 2 GB/s); our model loads read at
    about 90 MB/s (8.4 GB in ~90 s).
  - Measured on the console (Qwen3.8-27B UD-Q2_K_XL + F16 projector, screen off): pool 12.1875 GiB; the model
    loads in 20 s instead of ~1 min 40 s; largest free block 1,082 MiB when ready (lean.11: 888) and 642 after the
    vision warm-up (434), flat through 25 photos with 0 errors; flexible pool 119 MiB free throughout. With the screen
    on: 710 when ready, 270 after the warm-up. Triangle still wakes the screen while the splash is up.
  - Text only, the biggest Qwen3.8-27B quant is UD-IQ3_XXS (10.93 GB; the pool takes the file's tensors less the
    335 MiB MTP block Unsloth's files carry, `blk.64.nextn`, which this llama.cpp does not load). With an 8k context
    and f16 cache, **screen on**: 496 MiB free when ready, 382 after a 5-minute text load (25 requests of ~1,800
    prompt tokens, 0 errors, flat from the 10th), 19.3 tokens a second (IQ2_S: 20.1). Screen off (lean.11): 676 and
    552.
- 2026-11-11 (lean.13, `c1a34b4`): the screens say what the screen costs, and turning it off is easy to find.
  - Home's memory meter is one bar in three parts: what the model (or the app) holds, the screen's share in blue,
    and what is free, with a key under it; it shows with no model too. Home has an "Options: Screen off" hint;
    Options asks from any page.
  - Turning the screen off: one question (what it frees on this console, that the TV keeps the app's picture and
    the server keeps answering, Triangle held 2 s brings it back, that it stays off at the next start), then five
    seconds on the TV saying the same (Circle stays on, Cross goes at once), then the screen closes **in place**:
    the model keeps running, and `screen.txt` = `off` keeps it so at the next start. Before, the restart's message
    was on the TV for three frames.
  - With no screen, any button other than Triangle (or Triangle let go early) gets a notification on how to turn
    it on, at most every 20 s (the first 20 s after a close count the close's own notification); a start without
    a screen says what it saves. The Triangle hold is timed in microseconds.
  - Settings: "Screen and memory" is the first category: "Turn the screen off" (in place, with the measured
    figure), "Turn off and reload the model" (the restart: the memory in one piece), the memory bar, and how to
    bring the screen back. The "(test)" row is gone; the PC's `screen off-now` still does it.
  - The screen's cost is measured at each start with the screen on (largest free block before the display and
    after its first frame), logged and kept in `/data/PS5LM/screen-cost.txt`; "about 360 MB" until then.
  - Models and Search: a model too big with the screen on but not without it says "Fits with the screen off" and
    how far short it is, and Cross asks to load it with the screen off (one restart without it). A model too big
    either way says what it needs and what this PS5 has for a model. The planner's "needs" figure now includes the
    cache at 4k. A model running from `app-args.txt` says so instead of "too big". (On this console no model in
    the library falls between the two: the planner over-counts the Qwen UD files, see IQ3_XXS above.)
  - Home: "No replies yet" before the first reply; the app's own vision warm-up stays out of the request feed;
    "reads images" when a projector is loaded.
  - `Display::open` closes what it opened when a step fails, and the display opens again after `close()`: the PC's
    `screen on-now` reopens it in place when the pool has the room (else the restart). Sound starts only with a
    screen. Memory lines `display open` and `renderer and fonts ready` split the screen's cost.
  - The app asks for a surface without depth or stencil, but **the GL port ignores it**: its window always gets a
    32-bit depth + 8-bit stencil buffer (`ps5_egl.c`: one fixed visual, `Z32_FLOAT_S8X24`); the log says
    `depth 32, stencil 8`. No saving; `display.txt` = `depth` changes nothing either.
  - Measured on the console (Qwen3.8-27B UD-Q2_K_XL + F16 projector, MiB, largest free block / all free):
    - Screen on, against lean.12: 12,068 before loading (same); **712 / 723 when ready** (710 / 723); 304 / 533
      after the warm-up (274 / 530). The screen holds 372 MiB (lean.12: 370): 338 when the display opens (the GL
      port's 320 MiB render pool, its depth buffer) and 30 for the renderer and fonts. No heavier.
    - Screen off from the UI (Options, question, countdown): all free 557 -> 903 (+346) in place, `screen.txt` =
      `off`; a button pressed 20 s later sent the reminder.
    - Screen on in place (`screen on-now`): 192-195 ms; three off/on cycles during a 600-token reply: open 304 /
      555 and closed 336 / 899 in every cycle, the reply complete (HTTP 200, 28.8 s, 20.8 tokens a second).
    - Screen off by restart (the standing setting): 1,084 / 1,096 when ready (lean.12: 1,082 / 1,094).
- 2026-11-11 (lean.14): the screen comes back without a reload, and can sleep by itself.
  - Triangle held 2 s opens the screen again **in place** (about 0.2 s; the model keeps running), whether it was
    turned off from the dashboard or the app started without it, when the pool has the room: one 322 MiB block for
    the GL port's render pool, and the screen's whole cost with 256 MiB to spare. Without the room, the restart
    with the screen, as before. Asked for while a model loads, it waits for the model to be ready.
  - Settings > Screen and memory > "Sleep the screen after": Never (the default), 5, 15 or 30 minutes, or 1 hour
    without a button press (a press, a held button or a step; sticks and triggers alone do not count; a model load
    holds it). Then five seconds on the TV that any button cancels, and the screen closes in place. Asleep, **any**
    button wakes it (in place; a stray press never restarts the app: without the room it says to hold Triangle). A
    sleep is not remembered: the next start has the screen. Kept as `screen_sleep_min` in `settings.json`.
  - The PC's `screen on` is still the restart with the screen; `screen on-now` the in-place open.
  - The lean.13 depth-config request and `display.txt` are gone (the GL port ignored them).
  - Measured on the console (Q2_K_XL + F16 projector, MiB largest / all free):
    - Started without the screen: 1,084 / 1,096 when ready, 658 / 905 after the warm-up. Opened in place from
      there (the path Triangle takes, asked from the PC): 273 ms, 338 / 551 with the screen up (a start with the
      screen on leaves 304 / 533), the model serving throughout.
    - Sleep timer at 5 minutes (set from Settings with button presses): the countdown started 5 min after the
      last press, the screen slept 5 s later (338 / 539 -> 658 / 885), and one press of Square woke it in place in
      197 ms (338 / 547), on the page it was on. `screen.txt` stayed `on` throughout.
    - Steven's controller, on the standing setting (started without the screen): Triangle held 2 s brought the
      screen straight back, no reload.

- 2026-11-12 (lean.15): the planner's figures on the models and Search pages (`ps5/app/model_plan.cpp`).
  - Weights are the tensors llama.cpp loads: a file that declares MTP blocks (`nextn_predict_layers`, Unsloth's
    Qwen3.8-27B UD quants: 335 MiB) no longer counts them. Q2_K_XL's file is 1,389 MiB larger than IQ2_S's and took
    exactly 1,054 MiB more of the pool. The base reserve is 0.8 GiB (was 0.75, fitted with the MTP block counted):
    IQ3_XXS text only at 8k left 950 MiB beyond weights, cache and state after a 5-minute load at `-ub 256`.
  - A model that does not fit with its projector is planned text only when that fits, and its line says what images
    would need: UD-IQ3_XXS was "too big" (13.0 GiB) though it runs text only at 8k with the screen on.
  - The line says "of X GiB free": X is what is free before loading (the 12.2 GiB pool less the screen's share).
  - Planner on the PC's files at lean.12's budgets (screen on 11.8 GiB free / off 12.1): IQ2_S images 64k q4_0;
    Q2_K_XL images 4k q8_0 / 16k q4_0 (was too big with the screen on); IQ3_XXS text only 32k / 64k q4_0 (was too big);
    Q5_K_M too big. Host tests: MTP bytes from the tensor list, a header cut short, the text-only fallback.

Install: download the `ps5lm-title-<commit>` artifact from the Actions run, check `eboot.bin` against
`native-ci/result.json`, and copy `app-ps5/title/PPSA99590` to `/data/homebrew/` over FTP. On OnionHEN, add
PPSA99590 to `app_jailbreak.exact_title_ids` as for the stock app.
- 2026-11-12 (lean.16): a model that fits text only says whether it reads images with the screen off.
  - With lean.15's text-only plans, a vision model that fits only without its projector says so on its row ("Text
    only"), and when it would fit with its projector once the screen's memory is free, "Images with the screen
    off". Cross on that row asks: Cross loads it with images and the screen off (one restart without the screen,
    as "Fits with the screen off" does), Triangle loads it text only with the screen on.
  - The running model, when `app-args.txt` started it (no planner line), says "running with its own settings
    (app-args.txt)" instead of the planner's plan, which may not be what it runs (Q2_K_XL runs with images there).
  - The planner's log line notes "(images with the screen off)" for such models.
  - On the console (lean.16, screen on, 8k cap, f16 cache): IQ3_XXS "Text only" (images need 12.9 GiB), the running
    Q2_K_XL "running with its own settings (app-args.txt)", and the planner log marks Q2_K_XL "(images with the
    screen off)" (images need 11.9 GiB; 11.8 free with the screen on). Standing setting after it: 1,084 MiB when ready.
- 2026-11-12 (lean.17): Home puts the chat first (built in its own session on `ui/home-overhaul`, `7dfb4be`; Steven
  picked it from PC renders over a vitals strip and a console card).
  - Right, two thirds: the Requests panel (text 24 px, the newest answer up to 14 lines, the newest question always
    shown; header: answered, typical reply time, tokens in and out, failures in red only when any).
  - Left rail (508 px): the model, its state and address; the speed at 96 px with its 2-minute trace (or the load's
    cells); the memory bar with the screen's share; context; temperature, power, GPU busy and CPU use with 2-minute
    trend lines; the fan as a bar; energy since start. Home's status bar leaves out temperature and watts.
  - The vision warm-up no longer counts as an answered request (`stats.cpp`).
  - Cost: the busiest Home frame draws about 1,340 shapes instead of 1,060 (about 27 KB more GPU instance buffer);
    no fonts, textures or images.
- 2026-11-12 (lean.18): the controller's microphone records (from the mic probe session, `34d2366`, Steven's OK).
  - A request file `mic` (`ps5/app/mic_probe.cpp`) records from the DualSense microphone on a thread of its own:
    5 s by default (up to 30), every call's return code, the port's silent state and each second's level in
    `app.log`, and `mic-test.wav` in the data folder. A probe: no chat screen, no speech-to-text yet.
  - `libSceAudioIn` is a linked import through a stub made in `scripts/build-app.sh`, as for AGC: a title cannot
    look a symbol up at run time (`sceKernelDlsym` answered 0x80020003 for every name). It adds two modules at the
    title's start (libSceAudioIn, libSceSysmodule; the second only for a fallback that never ran).
  - Measured on the console as 0.11.0-lean.16-mic.1 (`7b9df03`): 16 kHz mono read directly, every read 16.0 ms;
    the mute button gives exact zeros and silent state 0x4; the model ran 20.96 tokens a second while recording
    against 21.00 alone; no memory cost (1,084 MiB when ready, flexible 119 free, as lean.16); works with the
    screen on. Write-up: `docs/handoffs/RESULT-ps5lm-mic-probe-20261111.md` in the RenoIntel tree.
  - Checked on the console as lean.18 (`7b07883`, installed over the probe build): the new Home shows on the TV
    ("0 answered" with the warm-up left out; its tokens still count in the token totals), the `mic` request
    recorded 3 s (16 kHz, 0 failed reads, every read 16.0 ms) to `mic-test-lean18.wav`. Screen on: 712 / 725 MiB when
    ready, 270 / 533 after the warm-up. Standing setting (screen off): 1,084 / 1,096 when ready, 658 / 905 after the
    warm-up, flexible pool 119 MiB free, the same as lean.16.
- 2026-11-12 (lean.19): speech-to-text on the console (from the mic probe session, `39f7fe5` + `64c8ebd`, Steven's
  OK).
  - A request file `stt` transcribes with whisper.cpp on the CPU, beside the running model: a mic-test WAV
    (`file=speech`, the default) or a new controller take (`rec=N`), with the model
    `/data/PS5LM/whisper/ggml-<name>.bin` (default `base.en-q5_1`). It logs the text, the times and memory lines.
    Muted or silent takes are skipped (whisper writes "you" for silence). Files: `ps5/app/stt.cpp`, `stt_audio.cpp`,
    `stt_audio_test.cpp` (host test, run in CI), `mic_take()` in `mic_probe.cpp`, the hook in `main.cpp`.
  - New submodule `third_party/whisper.cpp` at v1.9.5 (`d1be6fd`), compiled against llama.cpp's own ggml, so the
    title holds one ggml. `result.json` records `whisper_revision`.
  - Memory: `patches/0011-ggml-sched-one-backend-one-split.patch` changes `ggml_backend_sched_new` only for
    schedulers with one backend (llama's GPU+CPU ones are unchanged), and `patches/whisper/0001` is applied to a copy
    of whisper's source in `build/`. Without them whisper took about 650 MiB: the PS5 backs every allocated byte.
  - Measured on the console as 0.11.0-lean.18-stt.2 (`2e3d44d`, the same tree): base.en-q5_1 1.4-1.8 s a take on 4
    CPU threads (tiny 0.6 s); a live sentence with the screen on came back word for word in 1.7 s for 12 s of audio;
    +206 MiB of heap while transcribing, 294 MiB still free in one piece with the screen on, all returned after; the
    model 20.86 tokens a second during a transcription against 20.96 alone; the model still loads with 1,084 MiB free
    and warms up in 11.6-11.8 s. Write-up: section 11 of `docs/handoffs/RESULT-ps5lm-mic-probe-20261111.md` in the
    RenoIntel tree.
  - Checked on the console as lean.19 (`dd74080`, installed over stt.2): screen on 714 / 725 MiB when ready, 288 /
    533 after the warm-up (11.6 s); an `stt` of the saved 12 s live take came back word for word in 1,685 ms
    (whisper loaded in 691 ms, all free 557 -> 338 while transcribing -> 543 after; the largest block stayed 288).
    Standing setting (screen off): 1,084 / 1,096 when ready, 664 / 905 after the warm-up, flexible 119 MiB free.
- 2026-11-12 (lean.20): about 710 MiB more for the model (from the scheduler-memory session, `09f3436`, Steven's OK).
  - `patches/0011-ggml-sched-grows-with-the-graph.patch` replaces lean.19's `0011-ggml-sched-one-backend-one-split`.
    `ggml_backend_sched_new` reserved room for one split per graph node (20,160 bytes a node plus id arrays): 551 MiB
    for the model's scheduler (27,232 nodes at `-ub 256`) and 166 MiB for the projector's, of which the graphs use
    7 KB and 1.5 KB (2 splits and 10 inputs; 1 split and 2 inputs). Now the split copies go into small contexts that
    grow on demand, and the id arrays grow with the graph copy. Whisper's one-backend schedulers are covered too
    (base.en 158 -> 151 MiB after loading, the same text).
  - Checked on the PC: the same answers and top-3 log-probabilities to the last digit as the unpatched build, with
    2 splits, with 322 splits and 410 inputs a graph, and with a first context of one tensor.
  - Measured on the console as 0.11.0-lean.19-sched.1 (the same tree; Q2_K_XL v10, MiB largest / all free):
    - Screen off: **1,792 / 1,819 when ready** (lean.19: 1,084 / 1,096), 1,382 / 1,620 after the warm-up (664 /
      905), flat at 1,382 through 25 photos (lean.13: 646 -> 620). CPU heap when ready 152 MiB (lean.19: 874).
    - Screen on: 1,422 / 1,448 when ready (lean.13: 712 / 723), 980 / 1,241 after the warm-up (304 / 533), flat at
      980 through 25 photos.
    - 0 errors, median 12.0-12.1 s a photo, warm-up 11.6-11.7 s; answers within run-to-run variation (wear 24/25
      against lean.13; lean.11 against lean.13 was 23/25).
    - **IQ3_XXS with vision, screen off, now fits**: 740 when ready, 310 after the warm-up, flat through 25 photos,
      0 errors, median 12.1 s.
  - The planner (Models page) still assumes the old heap, so it now under-promises by about 700 MiB (for example
    "images need 11.9 GiB" for Q2_K_XL). Write-up: `docs/handoffs/RESULT-ps5lm-sched-memory-20261112.md` in the
    RenoIntel tree.
  - Checked on the console as lean.20 (`c03e1f9`, installed over sched.1), screen off, Q2_K_XL v10: **1,792 / 1,819
    MiB when ready**, CPU heap 0.15 GiB; 1,376 / 1,620 after the warm-up (11.8 s); an `stt` of the saved live take
    came back word for word in 1,683 ms (whisper loaded in 696 ms), the largest block untouched at 1,376.
- 2026-11-12 (lean.21): push-to-talk with chat history on Home (from the mic probe session and its chat UI
  sub-session, branch `chat/for-ci-lean` `2866f28`, Steven's OK).
  - Engine (`ps5/app/chat.cpp`, `chat_history.cpp` with a host test in the title check and CI): the conversation is
    kept and sent as `messages`; the history takes at most half the context (always leaving 512 tokens) and the
    reply gets the rest as `max_tokens`. Answers stream back through `activity.cpp`'s hooks (a chat request carries
    `"user": "ps5lm-chat-<n>"`). Push-to-talk records at once; whisper base.en-q5_1 loads beside it on first use and
    stays loaded, unloading after 10 idle minutes or when the screen goes. Muted and silent takes are skipped with a
    note. A `chat` request file drives it from the PC (`docs/CONSOLE.md`).
  - Screens (`dashboard.cpp`, from renders Steven approved): hold L2 to talk, the transcript goes into the box, R2
    sends, Triangle types, Square twice starts a new chat; the panel has two views, Chat and Other devices.
  - Tested on the TV as 0.11.0-lean.20-chat.2 (`32b2a83`, the same tree): transcripts in 1.4-1.7 s, sent and
    answered; a follow-up ("How many people live there?") answered about Paris; a long code answer (5,673 bytes,
    20.7 tokens a second) finished. Whisper (152 MiB) unloaded after 10 idle minutes and with the screen off, and
    reloads in 71-700 ms. Screen on, Q2_K_XL with vision at 8k, whisper loaded: largest free block 602 MiB, 663-715
    in all.
  - **Standing setting changed (Steven, 2026-11-12 ~08:30): app-args v12 = v10 with `-c 8192`, and the screen
    stays on** for chat. 8k costs about 380 MiB against 2k with the screen on (after the warm-up, largest 1,058 ->
    602 MiB; 64 KiB a token in the 16 attention layers). v10 is the rollback.
  - Not right yet: math shows as raw LaTeX, code lines lose their indentation, base.en is English only, and a stray
    first word sometimes appears. Write-up: section 12 of `docs/handoffs/RESULT-ps5lm-mic-probe-20261111.md` in the
    RenoIntel tree.
  - Checked on the console as lean.21 (`49d89ac`, installed over chat.2), on the new standing setting (v12 8k, screen
    on): 1,022 / 1,056 MiB when ready, 622 / 861 after the warm-up (11.7 s), CPU heap 0.17 GiB. From the PC (`chat
    say:`): "What is the capital of France?" answered "Paris", and the follow-up "How many people live there?"
    answered about Paris (4 turns kept, prompt ~122 tokens, reply room 8,038); the TV shows the conversation.
- 2026-11-12 (lean.22): the planner counts lean.20's memory, and Home leaves the vision warm-up out (`9c4656d`,
  Steven's OK).
  - `ps5/app/model_plan.cpp`: the reserve beyond weights, cache and state is 0.35 GiB, not 0.8, and a causal vision
    model (Qwen's) keeps 0.3 GiB for photos, not 0.5. The old figures held lean.20's 551 + 166 MiB of scheduler
    memory. Fitted to the console's readings after the warm-up (the encoder's buffers at their largest; 25 photos
    after it take no more), Qwen3.8-27B with its F16 projector, `-ub 256`, in largest-free-block terms: 0.84 GiB
    (Q2_K_XL, 8k, screen on, lean.21), 0.85 (Q2_K_XL, 2k, screen off, lean.20), 0.85 (IQ3_XXS, 2k, screen off). The
    planner now allows 0.97 for them, plus the checkpoint's state (0.15). Calibrated on Qwen 3.8 27B only.
  - The planner on the console's own headers (PC run, Settings cache f16, 8k cap; screen on 11.79 GiB free, off
    12.15): Q2_K_XL with images 8k f16, 11.5 of 11.8 GiB (was "text only, images need 11.9"); IQ2_S with images
    10.5; IQ3_XXS text only, images need 12.3 GiB (4k f16), so not with the screen off either while the cache is
    forced to f16. With the cache on automatic it reads "images with the screen off" (4k q4_0, 12.1 of 12.15).
  - Home: the warm-up's tokens (1,337 in, 1 out, read from the server's `usage` in its reply,
    `vision_warmup_finished`) are left out of "tokens in" and "out", and the context meter reads 0 until another
    request begins (it showed the warm-up's image as 1,337 of the context).
  - Checked on the console as lean.22 (`9c4656d`, installed over lean.21), standing setting (v12 8k, screen on): the
    log's planner lines are the PC run's to the digit; the Models page shows them (Q2_K_XL "running with its own
    settings"). Memory 1,022 / 1,056 MiB when ready (lean.21: 1,022 / 1,056), 720 / 849 after the warm-up (622 /
    861). After the start Home read "0 answered, 0 tokens in, 0 out" and "Context 0 of 8k" (the warm-up logged
    1,337 in, 1 out); after one chat message from the PC, "1 answered, 22 tokens in, 8 out" and "29 of 8k".
    Screenshots: `D:\ps5lm-work\notes\ui-lean22\`.
- 2026-11-12 (lean.23): the app is called **PS5LM Voice** (Steven's choice; it was PS5LM Lean), and the fork gets a
  README and a draft-release workflow (from the fork-publish session, `ae62e72`, Steven's OK).
  - `ps5/app/sce_sys/param.json`: titleName "PS5LM Voice". The title ID (**PPSA99590**) and content ID stay as they
    are, so it still installs beside stock PS5LM and over lean.22, with the same data folder. The app's own
    notifications say "PS5LM Voice:" too; the on-screen "PS5LM" header and the CI step names are unchanged.
  - `README.md`: what the fork adds, how to install it, the controls and its limits, for PS5LM Voice.
  - `.github/workflows/release.yml`: a tag `v<version>` (it must match `version.hpp`) builds through
    `native-build.yml` (which now also takes `workflow_call`), checks `eboot.bin` against `result.json`, and attaches
    `ps5lm-voice-<version>.zip`, its `.sha256` and `result.json` to a draft release. Nothing publishes it.
