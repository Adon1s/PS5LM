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

Install: download the `ps5lm-title-<commit>` artifact from the Actions run, check `eboot.bin` against
`native-ci/result.json`, and copy `app-ps5/title/PPSA99590` to `/data/homebrew/` over FTP. On OnionHEN, add
PPSA99590 to `app_jailbreak.exact_title_ids` as for the stock app.
