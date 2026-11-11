// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the vision warm-up (PS5LM Lean): the image's size for a token
// cap, and that the request it sends carries a whole BMP at that size.
//   c++ -std=c++17 -O1 -pthread ps5/app/vision_warmup_test.cpp ps5/app/vision_warmup.cpp -o build/vision_warmup_test
#include "vision_warmup.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace ps5lm {
// Stands in for stats.cpp's loopback POST: keeps the body, answers like the server.
std::string g_body;
std::string http_post(int, const char *, const std::string & body, int) {
    g_body = body;
    return "{\"choices\":[]}";
}
}  // namespace ps5lm

static int failures = 0;

static void check(bool ok, const char * what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

static std::string unbase64(const std::string & in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::string out;
    uint32_t    acc = 0;
    int         bits = 0;
    for (char c : in) {
        const int v = val(c);
        if (v < 0) {
            break;
        }
        acc = (acc << 6) | (uint32_t) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char) ((acc >> bits) & 0xff);
        }
    }
    return out;
}

static uint32_t le(const std::string & s, size_t at, int bytes) {
    uint32_t v = 0;
    for (int i = bytes - 1; i >= 0; i--) {
        v = (v << 8) | (uint8_t) s[at + i];
    }
    return v;
}

int main() {
    int w = 0, h = 0;
    ps5lm::vision_warmup_size(1280, w, h);
    check(w == 1280 && h == 1024, "a 1280-token cap gives 1280x1024, 40 x 32 tokens");
    ps5lm::vision_warmup_size(1024, w, h);
    check(w == 1024 && h == 1024, "a 1024-token cap gives 1024x1024");
    ps5lm::vision_warmup_size(256, w, h);
    check(w == 512 && h == 512, "a 256-token cap gives 512x512");
    ps5lm::vision_warmup_size(1000, w, h);
    check((w / 32) * (h / 32) <= 1000 && (w / 32) * (h / 32) >= 990 && w <= 2 * h, "a 1000-token cap stays under it");

    const std::vector<std::string> text_only = { "-m", "x.gguf", "--image-max-tokens", "1280" };
    check(!ps5lm::vision_warmup_start(8081, text_only, "/nonexistent/warmup.txt"), "no projector, no warm-up");
    const std::vector<std::string> no_cap = { "-m", "x.gguf", "--mmproj", "p.gguf" };
    check(!ps5lm::vision_warmup_start(8081, no_cap, "/nonexistent/warmup.txt"), "no image-token cap, no warm-up");

    const std::vector<std::string> args = { "-m", "x.gguf", "--mmproj", "p.gguf", "--image-max-tokens", "1280" };
    check(ps5lm::vision_warmup_start(8081, args, "/nonexistent/warmup.txt"), "a projector and a cap start it");
    std::string summary;
    for (int i = 0; i < 200 && !ps5lm::vision_warmup_finished(summary); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(summary.find("1280x1024 image (1280 tokens) answered") != std::string::npos, summary.c_str());
    const std::string prefix = "data:image/bmp;base64,";
    const size_t      at = ps5lm::g_body.find(prefix);
    check(at != std::string::npos, "the request carries a BMP data URL");
    const std::string bmp = unbase64(ps5lm::g_body.substr(at + prefix.size()));
    check(bmp.size() == 54 + 1280 * 3 * 1024 && bmp[0] == 'B' && bmp[1] == 'M' && le(bmp, 2, 4) == bmp.size(),
          "the BMP is whole");
    check(le(bmp, 18, 4) == 1280 && le(bmp, 22, 4) == 1024 && le(bmp, 28, 2) == 24, "1280x1024, 24-bit");
    check(ps5lm::g_body.find("\"max_tokens\":1") != std::string::npos, "one token asked for");
    std::printf("vision_warmup_test: %s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
