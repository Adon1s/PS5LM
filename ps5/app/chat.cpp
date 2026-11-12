// PS5LM Lean: the Home chat's engine (chat.hpp).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "chat.hpp"

#include "chat_history.hpp"
#include "heap_info.hpp"
#include "log.hpp"
#include "probes.hpp"
#include "stats.hpp"
#include "stt_audio.hpp"

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ps5lm {

namespace {

using Clock = std::chrono::steady_clock;
using json = nlohmann::ordered_json;

constexpr const char * kModel = "base.en-q5_1";   // picked on the PC and the console (RESULT-ps5lm-mic-probe §8, §11)
constexpr int          kMinReply = 512;            // tokens: the reply always has at least this
constexpr int          kMaxTalkSeconds = 30;       // whisper's window
constexpr int          kThreads = 4;               // measured: 1.4-1.8 s a take beside the model
constexpr int          kBeam = 5;
constexpr int          kSilentPeak = 200;          // a quiet room peaks at 60-90, soft speech at 240-800
constexpr double       kMinTalkSeconds = 0.3;      // a tap is not a take
constexpr int          kIdleUnloadSeconds = 600;   // whisper's model goes after ten minutes without a take
const char * const     kUserTag = "ps5lm-chat-";

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

bool file_exists(const std::string & path) {
    struct stat st = {};
    return stat(path.c_str(), &st) == 0;
}

// whisper's own words for "nothing said", which are not a message.
bool blank_transcript(const std::string & text) {
    std::string t;
    for (const char c : text) {
        if (c != ' ' && c != '.' && c != '\n') {
            t += c;
        }
    }
    return t.empty() || t == "[BLANK_AUDIO]" || t == "(silence)" || t == "[silence]" || t == "[Music]";
}

ChatEngine * g_chat = nullptr;

}  // namespace

struct ChatEngine::State {
    int        port = 0;
    MemoryLine memory;

    mutable std::mutex    m;
    std::vector<ChatTurn> turns;
    int                   dropped = 0;
    bool                  answering = false;
    uint32_t              answer_seq = 0;  // the request whose text goes to the last turn
    uint32_t              next_seq = 0;

    MicState    mic = MicState::off;
    double      seconds = 0;
    float       level = 0;
    std::string note;
    std::string transcript;
    uint32_t    transcript_seq = 0;
    double      transcribe_ms = 0;
    bool        screen_up = false;
    bool        model_present = false;
    int         checked_model_at = -1000;

    // whisper's model: loaded and used on the talk threads, one at a time.
    std::mutex             wm;
    Whisper                whisper;
    std::atomic<bool>      whisper_loaded{ false };
    int                    whisper_mib = 0;
    Clock::time_point      last_take = Clock::now();
    std::atomic<bool>      talking{ false };  // a take or its transcription is running
    std::atomic<bool>      stop{ false };
    std::atomic<bool>      cancel{ false };
    int                    ticks = 0;

    void set_mic(MicState state, std::string why = {}) {
        std::lock_guard<std::mutex> lock(m);
        mic = state;
        note = std::move(why);
    }
};

ChatEngine::ChatEngine(int port, MemoryLine memory) : s_(std::make_shared<State>()) {
    s_->port = port;
    s_->memory = std::move(memory);
    s_->model_present = file_exists(whisper_model_path(kModel));
    s_->mic = s_->model_present ? MicState::idle : MicState::off;
    if (!s_->model_present) {
        s_->note = "No speech model on this console (" + whisper_model_path(kModel) + ")";
    }
}

ChatEngine::~ChatEngine() = default;

ChatView ChatEngine::view() const {
    std::lock_guard<std::mutex> lock(s_->m);
    ChatView v;
    v.turns = s_->turns;
    v.dropped = s_->dropped;
    v.answering = s_->answering;
    v.mic = s_->mic;
    v.seconds = s_->seconds;
    v.level = s_->level;
    v.note = s_->note;
    v.transcript = s_->transcript;
    v.transcript_seq = s_->transcript_seq;
    v.transcribe_ms = s_->transcribe_ms;
    v.whisper_loaded = s_->whisper_loaded.load();
    v.whisper_mib = s_->whisper_mib;
    return v;
}

bool ChatEngine::send(const std::string & text, uint32_t n_ctx, bool spoken) {
    std::string body;
    uint32_t    seq = 0;
    {
        std::lock_guard<std::mutex> lock(s_->m);
        if (s_->answering || text.empty()) {
            return false;
        }
        ChatTurn user;
        user.role = ChatTurn::Role::user;
        user.text = text;
        user.spoken = spoken || (!s_->transcript.empty() && text == s_->transcript);
        s_->turns.push_back(user);
        // The history takes at most half the context (and leaves kMinReply); the reply gets the rest.
        const int    ctx = n_ctx > 0 ? (int) n_ctx : 2048;
        const size_t first = first_turn_to_send(s_->turns, history_budget(n_ctx, std::max(kMinReply, ctx / 2)));
        const int    max_reply = reply_tokens(n_ctx, prompt_tokens(s_->turns, first), kMinReply);
        json         messages = json::array();
        int          dropped = 0;
        for (size_t i = 0; i < s_->turns.size(); ++i) {
            const ChatTurn & t = s_->turns[i];
            if (!turn_is_sent(t)) {
                continue;
            }
            if (i < first) {
                ++dropped;
                continue;
            }
            messages.push_back({ { "role", t.role == ChatTurn::Role::user ? "user" : "assistant" }, { "content", t.text } });
        }
        ChatTurn answer;
        answer.role = ChatTurn::Role::assistant;
        answer.streaming = true;
        s_->turns.push_back(answer);
        s_->dropped = dropped;
        s_->answering = true;
        seq = s_->answer_seq = ++s_->next_seq;
        json b;
        b["messages"] = std::move(messages);
        b["stream"] = true;
        b["max_tokens"] = max_reply;
        b["chat_template_kwargs"] = { { "enable_thinking", false } };  // an answer on the TV in seconds
        b["user"] = kUserTag + std::to_string(seq);                     // activity.cpp routes it back here
        body = b.dump();
        log_line("chat: message %u sent (%zu bytes, %s), %zu turns, %d older not sent, context %u, prompt ~%d tokens, "
                 "reply up to %d\n", seq, text.size(), user.spoken ? "spoken" : "typed", s_->turns.size() - 1, dropped,
                 n_ctx, prompt_tokens(s_->turns, first), max_reply);
    }
    std::thread([s = s_, seq, body = std::move(body)] {
        const std::string reply = http_post(s->port, "/v1/chat/completions", body, 300);
        // The hooks end the request; give them a moment when the connection closed first.
        for (int i = 0; i < 20; ++i) {
            {
                std::lock_guard<std::mutex> lock(s->m);
                if (!s->answering || s->answer_seq != seq) {
                    return;
                }
            }
            if (reply.empty()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::lock_guard<std::mutex> lock(s->m);
        if (s->answering && s->answer_seq == seq && !s->turns.empty()) {
            ChatTurn & t = s->turns.back();
            t.streaming = false;
            t.failed = true;
            if (t.text.empty()) {
                t.text = reply.empty() ? "No answer: the model server did not take the message" : "No answer came back";
            }
            s->answering = false;
            log_line("chat: message %u failed (%s)\n", seq, reply.empty() ? "no connection" : "no end from the server");
        }
    }).detach();
    return true;
}

void ChatEngine::on_text(uint32_t seq, const std::string & text) {
    std::lock_guard<std::mutex> lock(s_->m);
    if (s_->answering && seq == s_->answer_seq && !s_->turns.empty()) {
        s_->turns.back().text += text;
    }
}

void ChatEngine::on_end(uint32_t seq, int status) {
    std::lock_guard<std::mutex> lock(s_->m);
    if (!s_->answering || seq != s_->answer_seq || s_->turns.empty()) {
        return;
    }
    ChatTurn & t = s_->turns.back();
    t.streaming = false;
    if (status != 200 || t.text.empty()) {
        t.failed = true;
        if (t.text.empty()) {
            t.text = status == 200 ? "The model sent an empty answer" : "The request failed (HTTP " + std::to_string(status) + ")";
        }
    }
    s_->answering = false;
    log_line("chat: message %u answered, %zu bytes, status %d\n", seq, t.text.size(), status);
}

void ChatEngine::new_chat() {
    std::lock_guard<std::mutex> lock(s_->m);
    s_->turns.clear();
    s_->dropped = 0;
    s_->answering = false;
    s_->answer_seq = 0;  // a reply still on its way is no longer shown
    log_line("chat: new chat\n");
}

void ChatEngine::talk_start() {
    {
        std::lock_guard<std::mutex> lock(s_->m);
        if (!s_->model_present) {
            s_->mic = MicState::off;
            return;
        }
        if (!s_->screen_up || s_->talking.exchange(true)) {
            return;
        }
        s_->mic = s_->whisper_loaded ? MicState::recording : MicState::loading;
        s_->note.clear();
        s_->seconds = 0;
        s_->level = 0;
    }
    s_->stop = false;
    s_->cancel = false;
    std::thread([s = s_] {
        // whisper's model loads beside the recording, on first use.
        std::thread loader;
        if (!s->whisper_loaded) {
            loader = std::thread([s] {
                std::lock_guard<std::mutex> wl(s->wm);
                if (s->whisper.loaded()) {
                    return;
                }
                const HeapFigures before = heap_figures();
                std::string       error;
                if (s->whisper.load(whisper_model_path(kModel), error)) {
                    const HeapFigures after = heap_figures();
                    s->whisper_mib = after.has_in_use && after.in_use > before.in_use
                                         ? (int) ((after.in_use - before.in_use) >> 20) : 0;
                    s->whisper_loaded = true;
                    log_line("chat: whisper %s loaded in %.0f ms, %d MiB\n", kModel, s->whisper.load_ms(), s->whisper_mib);
                    if (s->memory) {
                        s->memory("chat whisper loaded");
                    }
                    std::lock_guard<std::mutex> lock(s->m);
                    if (s->mic == MicState::loading) {
                        s->mic = MicState::recording;
                    }
                } else {
                    log_line("chat: FAILED: %s\n", error.c_str());
                }
            });
        }
        MicTake    take;
        const bool ok = mic_record(s->stop, kMaxTalkSeconds, take, [s](float level, double seconds) {
            std::lock_guard<std::mutex> lock(s->m);
            s->level = level;
            s->seconds = seconds;
        });
        const double seconds = take.rate ? (double) take.samples.size() / take.rate : 0.0;
        if (loader.joinable()) {
            loader.join();
        }
        struct Done {
            State * s;
            ~Done() {
                s->last_take = Clock::now();
                s->talking = false;
            }
        } done{ s.get() };
        if (s->cancel) {
            s->set_mic(MicState::idle);
            log_line("chat: take abandoned after %.1f s\n", seconds);
            return;
        }
        if (!ok) {
            s->set_mic(MicState::failed, "The microphone did not record: " + take.error);
            log_line("chat: FAILED: no recording: %s\n", take.error.c_str());
            return;
        }
        if (take.silent_state & 4) {
            s->set_mic(MicState::muted, "Your controller's microphone is muted (its mute button)");
            log_line("chat: take of %.1f s muted (silent state 0x%x)\n", seconds, (unsigned) take.silent_state);
            return;
        }
        const AudioLevel level = audio_level(take.samples, take.rate);
        if (seconds < kMinTalkSeconds || !audio_worth_transcribing(level, kSilentPeak)) {
            s->set_mic(MicState::silent, "Nothing heard. Hold the button while you speak");
            log_line("chat: take of %.1f s skipped as silence (peak %d)\n", seconds, level.peak);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(s->m);
            s->mic = MicState::transcribing;
            s->seconds = seconds;
            s->level = 0;
            s->note.clear();
        }
        std::string text, error;
        double      ms = 0;
        bool        transcribed = false;
        {
            std::lock_guard<std::mutex> wl(s->wm);
            if (!s->whisper.loaded()) {
                error = "the speech model did not load";
            } else {
                transcribed = s->whisper.transcribe(to_whisper_input(take.samples, take.rate), kThreads, kBeam, text, ms, error);
            }
        }
        if (!transcribed) {
            s->set_mic(MicState::failed, "Speech-to-text failed: " + error);
            log_line("chat: FAILED: transcription: %s\n", error.c_str());
            return;
        }
        if (blank_transcript(text)) {
            s->set_mic(MicState::silent, "Nothing heard. Hold the button while you speak");
            log_line("chat: take of %.1f s had no words (%.0f ms)\n", seconds, ms);
            return;
        }
        std::lock_guard<std::mutex> lock(s->m);
        s->mic = MicState::ready;
        s->transcript = text;
        ++s->transcript_seq;
        s->transcribe_ms = ms;
        s->note.clear();
        log_line("chat: take of %.1f s transcribed in %.0f ms (peak %d): \"%s\"\n", seconds, ms, level.peak, text.c_str());
    }).detach();
}

void ChatEngine::talk_stop() {
    s_->stop = true;
}

void ChatEngine::talk_cancel() {
    s_->cancel = true;
    s_->stop = true;
}

void ChatEngine::tick(bool screen_up) {
    State & s = *s_;
    {
        std::lock_guard<std::mutex> lock(s.m);
        s.screen_up = screen_up;
        if (++s.ticks - s.checked_model_at >= 10) {  // the model may be uploaded while the app runs
            s.checked_model_at = s.ticks;
            const bool present = file_exists(whisper_model_path(kModel));
            if (present != s.model_present) {
                s.model_present = present;
                if (present && s.mic == MicState::off) {
                    s.mic = MicState::idle;
                    s.note.clear();
                } else if (!present && !s.talking) {
                    s.mic = MicState::off;
                    s.note = "No speech model on this console (" + whisper_model_path(kModel) + ")";
                }
            }
        }
    }
    if (!s.whisper_loaded || s.talking) {
        return;
    }
    const double idle_s = std::chrono::duration<double>(Clock::now() - s.last_take).count();
    if (screen_up && idle_s < kIdleUnloadSeconds) {
        return;
    }
    // Not on the main loop: freeing takes a moment and the heap trim after it more.
    s.talking = true;
    std::thread([sp = s_, screen_up, idle_s] {
        {
            std::lock_guard<std::mutex> wl(sp->wm);
            sp->whisper.unload();
            sp->whisper_loaded = false;
            sp->whisper_mib = 0;
        }
        const double trim_ms = heap_trim();
        log_line("chat: whisper unloaded (%s), heap trim %.1f ms\n",
                 screen_up ? ("idle " + std::to_string((int) idle_s) + " s").c_str() : "no screen", trim_ms);
        if (sp->memory) {
            sp->memory("chat whisper unloaded");
        }
        sp->talking = false;
    }).detach();
}

void set_chat_sink(ChatEngine * chat) {
    g_chat = chat;
}

// For activity.cpp: the engine and the sequence number of a chat request.
ChatEngine * chat_sink() {
    return g_chat;
}

bool chat_seq_of(const std::string & user, uint32_t & seq) {
    const std::string tag = kUserTag;
    if (user.compare(0, tag.size(), tag) != 0) {
        return false;
    }
    seq = (uint32_t) std::strtoul(user.c_str() + tag.size(), nullptr, 10);
    return seq != 0;
}

}  // namespace ps5lm
