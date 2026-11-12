// SPDX-License-Identifier: GPL-3.0-or-later
#include "vision_warmup.hpp"

#include "stats.hpp"  // http_post

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

namespace ps5lm {
namespace {

std::mutex        g_mutex;
std::string       g_summary;
uint64_t          g_prompt_tokens = 0, g_reply_tokens = 0;  // the server's usage for it
std::atomic<bool> g_finished{ false };

// A 24-bit BMP, bottom-up rows: stb_image (llama.cpp's mtmd) reads it, and it
// needs no encoder. The content does not matter, only the size.
std::string bmp(int w, int h) {
    const uint32_t row = (uint32_t) (w * 3 + 3) & ~3u;
    const uint32_t data = row * (uint32_t) h, file = 54 + data;
    std::string    out(file, '\0');
    auto           put = [&](size_t at, uint32_t v, int bytes) {
        for (int i = 0; i < bytes; i++) {
            out[at + i] = (char) ((v >> (8 * i)) & 0xff);
        }
    };
    out[0] = 'B';
    out[1] = 'M';
    put(2, file, 4);
    put(10, 54, 4);
    put(14, 40, 4);
    put(18, (uint32_t) w, 4);
    put(22, (uint32_t) h, 4);
    put(26, 1, 2);
    put(28, 24, 2);
    put(34, data, 4);
    for (int y = 0; y < h; y++) {
        char * p = &out[54 + (size_t) y * row];
        for (int x = 0; x < w; x++) {
            p[3 * x + 0] = (char) ((x * 7 + y * 3) & 0xff);
            p[3 * x + 1] = (char) ((x * 3 + y * 5) & 0xff);
            p[3 * x + 2] = (char) (((x ^ y) * 13) & 0xff);
        }
    }
    return out;
}

std::string base64(const std::string & in) {
    static const char * k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string         out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = ((uint8_t) in[i] << 16) | ((uint8_t) in[i + 1] << 8) | (uint8_t) in[i + 2];
        out += k[v >> 18];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += k[v & 63];
    }
    if (i < in.size()) {
        const bool     two = i + 1 < in.size();
        const uint32_t v = ((uint8_t) in[i] << 16) | (two ? (uint8_t) in[i + 1] << 8 : 0);
        out += k[v >> 18];
        out += k[(v >> 12) & 63];
        out += two ? k[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string arg_after(const std::vector<std::string> & args, const char * name) {
    const auto it = std::find(args.begin(), args.end(), name);
    return it == args.end() || it + 1 == args.end() ? std::string() : *(it + 1);
}

void run(int port, int width, int height, int n_tokens) {
    const auto  t0 = std::chrono::steady_clock::now();
    std::string body;
    {
        const std::string image = base64(bmp(width, height));
        body = "{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":"
               "\"data:image/bmp;base64," + image + "\"}},{\"type\":\"text\",\"text\":\"Warm-up.\"}]}],"
               "\"max_tokens\":1,\"stream\":false}";
    }
    const std::string reply = http_post(port, "/v1/chat/completions", body, 300);
    body.clear();
    body.shrink_to_fit();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    char         line[256];
    std::snprintf(line, sizeof(line), "vision warm-up: %dx%d image (%d tokens) %s in %.1f s", width, height,
                  n_tokens, reply.find("\"choices\"") != std::string::npos ? "answered" : "NOT answered", s);
    // What the server counted for it ("usage"), so Home's totals can leave it out.
    const auto count = [&](const char * key) -> uint64_t {
        const std::string k = std::string("\"") + key + "\":";
        const size_t      at = reply.find(k);
        return at == std::string::npos ? 0 : std::strtoull(reply.c_str() + at + k.size(), nullptr, 10);
    };
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_summary = line;
        g_prompt_tokens = count("prompt_tokens");
        g_reply_tokens = count("completion_tokens");
    }
    g_finished.store(true, std::memory_order_release);
}

}  // namespace

void vision_warmup_size(int n_tokens, int & width, int & height) {
    int best = 0, best_a = 1, best_b = 1;
    for (int b = 1; b * b <= n_tokens; b++) {
        const int a = std::min(n_tokens / b, 2 * b);
        if (a >= b && a * b >= best) {  // ties go to the squarer one (larger b)
            best = a * b;
            best_a = a;
            best_b = b;
        }
    }
    width = 32 * best_a;
    height = 32 * best_b;
}

bool vision_warmup_start(int port, const std::vector<std::string> & server_args, const std::string & warmup_txt) {
    if (std::find(server_args.begin(), server_args.end(), "--mmproj") == server_args.end()) {
        return false;
    }
    const int n_tokens = std::atoi(arg_after(server_args, "--image-max-tokens").c_str());
    if (n_tokens <= 0) {
        return false;
    }
    std::string first;
    std::ifstream(warmup_txt) >> first;
    if (first == "off") {
        return false;
    }
    int width = 0, height = 0;
    vision_warmup_size(n_tokens, width, height);
    std::thread(run, port, width, height, n_tokens).detach();
    return true;
}

bool vision_warmup_finished(std::string & summary, uint64_t * prompt_tokens, uint64_t * reply_tokens) {
    if (!g_finished.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    summary = g_summary;
    if (prompt_tokens) {
        *prompt_tokens = g_prompt_tokens;
    }
    if (reply_tokens) {
        *reply_tokens = g_reply_tokens;
    }
    return true;
}

}  // namespace ps5lm
