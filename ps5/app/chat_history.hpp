// PS5LM Lean: which turns of the Home chat go to the model, so the
// conversation fits the context. No console calls, so the host test can check
// it (chat_history_test.cpp).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "chat_view.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps5lm {

// About how many tokens a text takes: one per 3.5 bytes, rounded up. Qwen's
// tokenizer runs about four characters a token on English; the margin covers
// the rest without a tokenizer call.
int estimate_tokens(const std::string & text);

// What the history may take of a context of `n_ctx` tokens, with `max_reply`
// kept for the answer and some room for the chat template. An unknown context
// (0) counts as 2048, the console's setting.
int history_budget(uint32_t n_ctx, int max_reply);

// The index of the first turn sent: the newest turns that fit `budget` (each
// costs its text and 8 for the template), without failed or still-streaming
// turns, and starting on a user turn. Turns before it are not sent. The newest
// user turn always goes, alone if it is over the budget by itself.
size_t first_turn_to_send(const std::vector<ChatTurn> & turns, int budget);

// Whether a turn is sent at all (failed answers and the one being written are not).
bool turn_is_sent(const ChatTurn & turn);

}  // namespace ps5lm
