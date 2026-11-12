// PS5LM Lean: speech-to-text with whisper.cpp on the CPU (stt.hpp).
//
// whisper.cpp is linked against the ggml llama.cpp already brings (whisper's
// own copy is the same library, one minor version older, and the title holds
// one copy only). It runs on the CPU: the GPU and its memory are the language
// model's. whisper sets ggml's log callback, which llama.cpp shares, when it
// sets up its backends; the callback is put back after each whisper call.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "stt.hpp"

#include "heap_info.hpp"
#include "log.hpp"
#include "probes.hpp"
#include "storage.hpp"
#include "stt_audio.hpp"

#include "ggml.h"
#include "whisper.h"

#include <pthread.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace ps5lm {

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stt_busy{ false };

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Spec {
    std::string model = "base.en-q5_1";
    std::string file = "speech";
    int         rec = 0;
    int         threads = 4;
    int         beam = 5;
    int         repeat = 1;
    int         gate = 200;
};

// Letters, digits, '.', '-' and '_' only: names become paths in the data folder.
std::string safe_name(const std::string & s) {
    std::string out;
    for (const char ch : s) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
            ch == '_') {
            out += ch;
        }
    }
    return out.substr(0, 64);
}

int clamp(long v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : (int) v;
}

Spec parse_spec(const std::string & text) {
    Spec   s;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(',', at);
        end = end == std::string::npos ? text.size() : end;
        const std::string item = text.substr(at, end - at);
        at = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = item.substr(0, eq), val = item.substr(eq + 1);
        const long        num = std::strtol(val.c_str(), nullptr, 10);
        if (key == "m" && !safe_name(val).empty()) {
            s.model = safe_name(val);
        } else if (key == "file") {
            s.file = val == "-" ? "-" : safe_name(val);
        } else if (key == "rec") {
            s.rec = clamp(num, 0, 30);
        } else if (key == "t") {
            s.threads = clamp(num, 1, 8);
        } else if (key == "b") {
            s.beam = clamp(num, 1, 8);
        } else if (key == "n") {
            s.repeat = clamp(num, 1, 5);
        } else if (key == "g") {
            s.gate = clamp(num, 0, 32768);
        }
    }
    return s;
}

std::string wav_path(const std::string & name) {
    if (name == "-" || name.empty()) {
        return data_path("mic-test.wav");
    }
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".wav") == 0) {
        return data_path(name);
    }
    return data_path("mic-test-" + name + ".wav");
}

// ggml's log callback as llama.cpp set it, put back after whisper replaced it.
struct GgmlLogGuard {
    ggml_log_callback cb = nullptr;
    void *            user = nullptr;
    GgmlLogGuard() { ggml_log_get(&cb, &user); }
    ~GgmlLogGuard() { ggml_log_set(cb, user); }
};

std::string segments_text(whisper_context * ctx) {
    std::string text;
    const int   n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; ++i) {
        const char * seg = whisper_full_get_segment_text(ctx, i);
        if (seg) {
            text += seg;
        }
    }
    // One line in app.log: no newlines, no leading space, quotes kept apart.
    std::string out;
    for (const char ch : text) {
        out += (ch == '\n' || ch == '\r') ? ' ' : ch == '"' ? '\'' : ch;
    }
    const size_t first = out.find_first_not_of(' ');
    return first == std::string::npos ? std::string() : out.substr(first);
}

void transcribe(const Spec & spec, const std::vector<float> & pcm, double audio_s, const MemoryLine & memory) {
    const std::string path = data_path("whisper/ggml-" + spec.model + ".bin");
    struct stat       st = {};
    if (stat(path.c_str(), &st) != 0) {
        log_line("stt: FAILED: no model at %s\n", path.c_str());
        return;
    }
    log_line("stt: model %s (%.1f MB), %d threads, %s, whisper %s\n", path.c_str(), st.st_size / 1e6, spec.threads,
             spec.beam > 1 ? ("beam " + std::to_string(spec.beam)).c_str() : "greedy", whisper_version());
    memory("stt before loading");
    whisper_context * ctx = nullptr;
    const auto        t_load = Clock::now();
    {
        GgmlLogGuard          keep;
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = false;  // the GPU is the language model's
        ctx = whisper_init_from_file_with_params(path.c_str(), cparams);
    }
    const double load_ms = ms_since(t_load);
    if (!ctx) {
        log_line("stt: FAILED: whisper could not load %s (%.0f ms)\n", path.c_str(), load_ms);
        memory("stt after a failed load");
        return;
    }
    log_line("stt: model loaded in %.0f ms\n", load_ms);
    memory("stt after loading");

    whisper_full_params p =
        whisper_full_default_params(spec.beam > 1 ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    if (spec.beam > 1) {
        p.beam_search.beam_size = spec.beam;
    }
    p.n_threads = spec.threads;
    p.language = "en";
    p.translate = false;
    p.no_context = true;
    p.print_progress = false;
    p.print_realtime = false;
    p.print_timestamps = false;
    p.print_special = false;
    for (int run = 1; run <= spec.repeat; ++run) {
        GgmlLogGuard keep;
        whisper_reset_timings(ctx);
        const auto   t0 = Clock::now();
        const int    rc = whisper_full(ctx, p, pcm.data(), (int) pcm.size());
        const double ms = ms_since(t0);
        if (rc != 0) {
            log_line("stt: FAILED: whisper_full = %d after %.0f ms\n", rc, ms);
            break;
        }
        whisper_timings * tm = whisper_get_timings(ctx);
        const std::string text = segments_text(ctx);
        log_line("stt: run %d: %.0f ms for %.2f s of audio (%.2f s per second of audio; mel+encode %.0f, decode %.1f "
                 "a token, batch %.1f, prompt %.1f, sample %.2f ms): \"%s\"\n",
                 run, ms, audio_s, audio_s > 0 ? ms / 1000.0 / audio_s : 0.0, tm ? tm->encode_ms : -1.0f,
                 tm ? tm->decode_ms : -1.0f, tm ? tm->batchd_ms : -1.0f, tm ? tm->prompt_ms : -1.0f,
                 tm ? tm->sample_ms : -1.0f, text.c_str());
        delete tm;  // whisper_get_timings hands over a new struct
    }
    memory("stt after transcribing");
    {
        GgmlLogGuard keep;
        whisper_free(ctx);
    }
    const double trim_ms = heap_trim();
    log_line("stt: model freed; heap trim %.1f ms\n", trim_ms);
    memory("stt after freeing");
}

}  // namespace

void run_stt(const std::string & spec_text, const MemoryLine & memory) {
    if (g_stt_busy.exchange(true)) {
        log_line("stt: a transcription is already running; request '%s' dropped\n", spec_text.c_str());
        return;
    }
    struct Release {
        ~Release() { g_stt_busy = false; }
    } release;
    const Spec spec = parse_spec(spec_text);
    log_line("stt: start, request '%s'\n", spec_text.c_str());

    std::vector<int16_t> samples;
    uint32_t             rate = 0;
    int                  silent_state = 0;
    std::string          source;
    if (spec.rec > 0) {
        MicTake    take;
        const bool ok = mic_take(spec.rec, take);
        log_line("stt: recorded %.2f s in %.2f s from the controller, %d failed reads, silent state 0x%x%s%s\n",
                 take.rate ? (double) take.samples.size() / take.rate : 0.0, take.seconds_taken, take.failed_reads,
                 (unsigned) take.silent_state, take.error.empty() ? "" : ": ", take.error.c_str());
        if (!ok) {
            log_line("stt: FAILED: no recording\n");
            return;
        }
        samples = std::move(take.samples);
        rate = take.rate;
        silent_state = take.silent_state;
        source = "the controller";
    } else {
        const std::string path = wav_path(spec.file);
        WavAudio          wav = read_wav(path);
        if (!wav.error.empty()) {
            log_line("stt: FAILED: %s: %s\n", path.c_str(), wav.error.c_str());
            return;
        }
        samples = std::move(wav.samples);
        rate = wav.rate;
        source = path;
    }
    const AudioLevel level = audio_level(samples, rate);
    log_line("stt: audio from %s: %.2f s at %u Hz, peak %d (%.1f dBFS), rms %.1f\n", source.c_str(), level.seconds, rate,
             level.peak, level.peak > 0 ? 20.0 * std::log10(level.peak / 32768.0) : -99.0, level.rms);
    if (silent_state & 4) {
        log_line("stt: skipped: the microphone is muted (the controller's mute button; silent state 0x%x)\n",
                 (unsigned) silent_state);
        log_line("stt: done\n");
        return;
    }
    if (!audio_worth_transcribing(level, spec.gate)) {
        log_line("stt: skipped: near silence (peak %d, under %d); whisper would write a word for it\n", level.peak,
                 spec.gate);
        log_line("stt: done\n");
        return;
    }
    const std::vector<float> pcm = to_whisper_input(samples, rate);
    if (pcm.empty()) {
        log_line("stt: FAILED: %u Hz audio (whisper takes 16 kHz; 48 kHz is converted)\n", rate);
        return;
    }
    transcribe(spec, pcm, pcm.size() / 16000.0, memory);
    log_line("stt: done\n");
}

void start_stt(std::string spec, MemoryLine memory) {
    auto * job = new std::pair<std::string, MemoryLine>(std::move(spec), std::move(memory));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);  // the console's default stack is small
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t  t{};
    const int  rc = pthread_create(&t, &attr, [](void * p) -> void * {
        auto * j = static_cast<std::pair<std::string, MemoryLine> *>(p);
        run_stt(j->first, j->second);
        delete j;
        return nullptr;
    }, job);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        log_line("stt: FAILED: no thread (%d)\n", rc);
        delete job;
    }
}

}  // namespace ps5lm
