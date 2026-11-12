// Host test for chat_history.cpp: c++ -std=c++17 chat_history_test.cpp chat_history.cpp && ./a.out
// SPDX-License-Identifier: GPL-3.0-or-later
#include "chat_history.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace ps5lm;
using Role = ChatTurn::Role;

static int failures = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

static ChatTurn turn(Role r, size_t bytes, bool failed = false, bool streaming = false) {
    ChatTurn t;
    t.role = r;
    t.text = std::string(bytes, 'a');
    t.failed = failed;
    t.streaming = streaming;
    return t;
}

int main() {
    // Estimates and the budget.
    CHECK(estimate_tokens("") == 0);
    CHECK(estimate_tokens("abcdefg") == 2);  // 7 bytes = 2 tokens
    CHECK(estimate_tokens(std::string(35, 'x')) == 10);
    CHECK(history_budget(2048, 512) == 2048 - 512 - 64);
    CHECK(history_budget(0, 512) == 2048 - 512 - 64);  // unknown: the console's 2048
    CHECK(history_budget(300, 512) == 128);             // never below the floor

    // Nothing, or only answers: nothing to send.
    CHECK(first_turn_to_send({}, 1000) == 0);
    CHECK(first_turn_to_send({ turn(Role::assistant, 10) }, 1000) == 1);

    // Everything fits: from the first turn.
    {
        std::vector<ChatTurn> t = { turn(Role::user, 35), turn(Role::assistant, 35), turn(Role::user, 35) };
        CHECK(first_turn_to_send(t, 1000) == 0);
    }
    // Only the newest pair and question fit (each 35 bytes = 10 + 8 = 18 tokens).
    {
        std::vector<ChatTurn> t = { turn(Role::user, 35), turn(Role::assistant, 35), turn(Role::user, 35),
                                    turn(Role::assistant, 35), turn(Role::user, 35) };
        CHECK(first_turn_to_send(t, 18 * 3) == 2);
        // Room for two: the answer at 3 would come first, so it starts at the question at 4.
        CHECK(first_turn_to_send(t, 18 * 2) == 4);
    }
    // The newest question goes even alone over the budget.
    {
        std::vector<ChatTurn> t = { turn(Role::user, 35), turn(Role::assistant, 35), turn(Role::user, 3500) };
        CHECK(first_turn_to_send(t, 100) == 2);
    }
    // A failed answer and a streaming one are skipped, not counted.
    {
        std::vector<ChatTurn> t = { turn(Role::user, 35), turn(Role::assistant, 35, true), turn(Role::user, 35),
                                    turn(Role::assistant, 35, false, true) };
        CHECK(first_turn_to_send(t, 18 * 2) == 0);  // users 0 and 2 fit; 1 and 3 are not sent
        CHECK(!turn_is_sent(t[1]) && !turn_is_sent(t[3]) && turn_is_sent(t[2]));
    }
    // The prompt's estimate and the reply's limit.
    {
        std::vector<ChatTurn> t = { turn(Role::user, 35), turn(Role::assistant, 35, true), turn(Role::user, 35) };
        CHECK(prompt_tokens(t, 0) == 64 + 18 + 18);  // the failed answer is not counted
        CHECK(prompt_tokens(t, 2) == 64 + 18);
        CHECK(reply_tokens(2048, 100, 256) == 2048 - 100 - 32);
        CHECK(reply_tokens(2048, 2000, 256) == 256);  // never under the floor
        CHECK(reply_tokens(0, 100, 256) == 2048 - 100 - 32);
    }
    if (failures) {
        std::fprintf(stderr, "chat_history_test: %d failed\n", failures);
        return 1;
    }
    std::printf("chat_history_test: ok\n");
    return 0;
}
