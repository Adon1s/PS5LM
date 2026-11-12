// PS5LM Lean: the Home chat's conversation and microphone, as data. chat.cpp
// (the engine, console only) fills it; the dashboard draws it and asks for
// changes through Dashboard::Request (talk_start, talk_stop, talk_cancel,
// chat_new, and ask to send). Plain data only, so the PC preview can fill it
// with made-up states.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps5lm {

// One message of the conversation, oldest first.
struct ChatTurn {
    enum class Role { user, assistant };
    Role        role = Role::user;
    std::string text;
    bool        streaming = false;  // the model is still writing it
    bool        failed = false;     // the request failed; `text` says why
    bool        spoken = false;     // a user turn that came from the microphone
};

// The push-to-talk microphone, as the engine sees it.
enum class MicState {
    off,           // not available: no whisper model on the console, or no screen
    idle,          // ready: hold the talk button to speak
    loading,       // the talk button is down; whisper's model is loading (first use; ~0.1-0.7 s)
    recording,     // the talk button is down; recording
    transcribing,  // the button was let go; whisper is working (~1.5-2 s)
    ready,         // `transcript` holds new text (see transcript_seq)
    muted,         // the take was muted (the controller's mute button): nothing transcribed
    silent,        // nothing was heard: nothing transcribed
    failed,        // `note` says why
};

struct ChatView {
    // The conversation, as it is sent to the model (oldest first). Older turns
    // beyond the context are dropped from what is sent; `dropped` counts them.
    std::vector<ChatTurn> turns;
    int                   dropped = 0;
    bool                  answering = false;  // a reply is on its way; a new message waits

    // Push-to-talk.
    MicState    mic = MicState::off;
    double      seconds = 0;        // recording: how long so far; transcribing: of what
    float       level = 0;          // recording: the last ~100 ms, 0..1 (a meter)
    std::string note;               // muted / silent / failed / off: one line on why, for the screen
    std::string transcript;         // the latest transcript (ready, and after)
    uint32_t    transcript_seq = 0;  // goes up by one with each new transcript: the box takes it once
    double      transcribe_ms = 0;  // how long the latest took

    // Memory, for a hint: whisper's model held while the chat is in use.
    bool whisper_loaded = false;
    int  whisper_mib = 0;
};

}  // namespace ps5lm
