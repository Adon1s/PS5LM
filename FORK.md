# PS5LM Lean

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

Install: download the `ps5lm-title-<commit>` artifact from the Actions run, check `eboot.bin` against
`native-ci/result.json`, and copy `app-ps5/title/PPSA99590` to `/data/homebrew/` over FTP. On OnionHEN, add
PPSA99590 to `app_jailbreak.exact_title_ids` as for the stock app.
