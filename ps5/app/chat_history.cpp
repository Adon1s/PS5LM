// SPDX-License-Identifier: GPL-3.0-or-later
#include "chat_history.hpp"

namespace ps5lm {

namespace {
constexpr int kPerMessage = 8;     // the template's role markers and line breaks
constexpr int kTemplate = 64;      // the generation prompt and a margin
constexpr int kMinBudget = 128;
}  // namespace

int estimate_tokens(const std::string & text) {
    return (int) ((text.size() * 2 + 6) / 7);  // ceil(bytes / 3.5)
}

int history_budget(uint32_t n_ctx, int max_reply) {
    const int ctx = n_ctx > 0 ? (int) n_ctx : 2048;
    const int budget = ctx - max_reply - kTemplate;
    return budget > kMinBudget ? budget : kMinBudget;
}

bool turn_is_sent(const ChatTurn & turn) {
    return !turn.failed && !turn.streaming && !turn.text.empty();
}

size_t first_turn_to_send(const std::vector<ChatTurn> & turns, int budget) {
    // The newest user turn goes whatever it costs.
    size_t last_user = turns.size();
    for (size_t i = turns.size(); i-- > 0;) {
        if (turns[i].role == ChatTurn::Role::user && turn_is_sent(turns[i])) {
            last_user = i;
            break;
        }
    }
    if (last_user == turns.size()) {
        return turns.size();  // nothing to send
    }
    int    used = 0;
    size_t first = turns.size();
    for (size_t i = turns.size(); i-- > 0;) {
        if (!turn_is_sent(turns[i])) {
            continue;
        }
        const int cost = estimate_tokens(turns[i].text) + kPerMessage;
        if (i < last_user && used + cost > budget) {
            break;
        }
        used += cost;
        first = i;
    }
    // Start on a user turn: an answer without its question reads oddly.
    while (first < last_user && (turns[first].role != ChatTurn::Role::user || !turn_is_sent(turns[first]))) {
        ++first;
    }
    return first;
}

int prompt_tokens(const std::vector<ChatTurn> & turns, size_t first) {
    int used = kTemplate;
    for (size_t i = first; i < turns.size(); ++i) {
        if (turn_is_sent(turns[i])) {
            used += estimate_tokens(turns[i].text) + kPerMessage;
        }
    }
    return used;
}

int reply_tokens(uint32_t n_ctx, int prompt, int floor) {
    const int ctx = n_ctx > 0 ? (int) n_ctx : 2048;
    const int left = ctx - prompt - 32;  // the estimate's margin
    return left > floor ? left : floor;
}

}  // namespace ps5lm
