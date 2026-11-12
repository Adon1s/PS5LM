// PS5LM Lean: speech-to-text with whisper.cpp on the CPU (stt.cpp). For now a
// request from the PC, to measure it beside a running model: the chat screen
// and push-to-talk come later.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

struct whisper_context;

namespace ps5lm {

// Writes one memory line to app.log, with `when` as its label (main.cpp's log_memory).
using MemoryLine = std::function<void(const char * when)>;

// The `stt` request file (scripts/ps5lm-app.sh stt): transcribes a take with a
// whisper.cpp model in /data/PS5LM/whisper (ggml-<name>.bin), logging the text,
// the time each part took and memory lines around it. `spec` is one word of
// comma-separated settings:
//   file=speech    the mic probe's /data/PS5LM/mic-test-speech.wav ("-": mic-test.wav); the default
//   rec=6          record 6 s from the controller instead (1-30)
//   m=base.en-q5_1 the model (ggml-base.en-q5_1.bin, the default)
//   t=4            CPU threads (default 4)
//   b=5            beam size (default 5, as whisper-cli; 1 = greedy)
//   n=1            transcriptions of the same audio with the model loaded once (1-5)
//   g=200          a take whose peak stays under this is silence and is skipped (0: off)
// A muted take (the mute button's silent state) and a silent one are skipped
// before whisper: whisper writes a word ("you") for silence.
// Runs on a thread of its own with a 16 MiB stack; one at a time.
void start_stt(std::string spec, MemoryLine memory);

// The same on the calling thread (start_stt's thread, and the PC check).
void run_stt(const std::string & spec, const MemoryLine & memory);

// One whisper.cpp model kept loaded, for push-to-talk (chat.cpp): load once,
// transcribe each take, unload when the chat is not in use. CPU only; English.
// Not thread-safe: one caller at a time.
class Whisper {
  public:
    Whisper() = default;
    ~Whisper();
    Whisper(const Whisper &) = delete;
    Whisper & operator=(const Whisper &) = delete;

    bool   load(const std::string & path, std::string & error);  // true when loaded (or already)
    void   unload();
    bool   loaded() const { return ctx_ != nullptr; }
    double load_ms() const { return load_ms_; }
    // 16 kHz mono floats (stt_audio.hpp's to_whisper_input). `beam` 1 = greedy.
    bool transcribe(const std::vector<float> & pcm, int threads, int beam, std::string & text, double & ms,
                    std::string & error);

  private:
    struct whisper_context * ctx_ = nullptr;
    std::string              path_;
    double                   load_ms_ = 0;
};

// /data/PS5LM/whisper/ggml-<name>.bin
std::string whisper_model_path(const std::string & name);

}  // namespace ps5lm
