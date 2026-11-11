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
- 2026-11-11: a projector pairs by its GGUF header when its name does not: a `general.type` `mmproj` file in the
  model's folder with the same `general.base_model.0.repo_url` or `general.basename` (Unsloth's `mmproj-F16.gguf`
  pairs with every Qwen3.8-27B build, no rename). The name rule still comes first; of several matches the largest
  file wins. Choosing a model in the library renames `app-args.txt` to `app-args.off.txt` instead of deleting it.
- 2026-11-11: the Home screen's context size comes from the server (`/slots` `n_ctx`), so a model loaded from
  `app-args.txt` no longer shows "of 0k".
- 2026-11-11: the planner gives vision models with a causal projector (Qwen's) `-ub 256`, `-b 1280` and
  `--image-max-tokens 1280` instead of the 2048 ubatch, keeping 0.5 GiB of headroom for photos. On the console that
  ubatch alone freed 0.57 GiB (0.90 -> 1.47 GiB free at load) at the same speed and answers. Other projectors keep 2048.

Install: download the `ps5lm-title-<commit>` artifact from the Actions run, check `eboot.bin` against
`native-ci/result.json`, and copy `app-ps5/title/PPSA99590` to `/data/homebrew/` over FTP. On OnionHEN, add
PPSA99590 to `app_jailbreak.exact_title_ids` as for the stock app.
