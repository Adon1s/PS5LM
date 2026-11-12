// PS5LM Lean: the Home chat's engine. It keeps the conversation and sends it
// to the model as `messages` (the newest turns that fit the context), and runs
// push-to-talk: the controller's microphone while the talk button is down,
// then whisper.cpp, the transcript handed to the message box (chat_view.hpp).
// The answer streams in through activity.cpp's hooks, which route a request
// whose "user" field is "ps5lm-chat-<n>" here.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "chat_view.hpp"
#include "stt.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace ps5lm {

class ChatEngine {
  public:
    // `port`: the model server's. `memory`: writes a memory line (main.cpp's
    // log_memory), around whisper's load and unload.
    ChatEngine(int port, MemoryLine memory);
    ~ChatEngine();
    ChatEngine(const ChatEngine &) = delete;
    ChatEngine & operator=(const ChatEngine &) = delete;

    // What the Home chat draws (a copy, safe from the main loop every frame).
    ChatView view() const;

    // Sends a message, typed or the checked transcript, with the conversation
    // so far, for a context of `n_ctx` tokens (0: unknown). False, and nothing
    // sent, while a reply is on its way or when the text is empty.
    bool send(const std::string & text, uint32_t n_ctx);
    // Forgets the conversation (a reply on its way is no longer shown).
    void new_chat();

    // Push-to-talk: the talk button went down, came up, or the take was
    // abandoned. Recording starts at once; whisper's model loads beside it on
    // first use and stays loaded while the chat is in use.
    void talk_start();
    void talk_stop();
    void talk_cancel();

    // About once a second from the main loop. Without the screen there is no
    // push-to-talk; whisper's model is unloaded then, and after ten minutes
    // without a take.
    void tick(bool screen_up);

    // From activity.cpp's hooks: a chat request's streamed text, and its end.
    void on_text(uint32_t seq, const std::string & text);
    void on_end(uint32_t seq, int status);

    struct State;

  private:
    std::shared_ptr<State> s_;
};

// The engine activity.cpp routes chat requests to (main.cpp sets it once).
void set_chat_sink(ChatEngine * chat);
ChatEngine * chat_sink();
// A chat request's sequence number from its "user" field ("ps5lm-chat-<n>"); false for other requests.
bool chat_seq_of(const std::string & user, uint32_t & seq);

}  // namespace ps5lm
