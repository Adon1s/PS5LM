// Host test for stt_audio.cpp: c++ -std=c++17 stt_audio_test.cpp stt_audio.cpp && ./a.out
// SPDX-License-Identifier: GPL-3.0-or-later
#include "stt_audio.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ps5lm;

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static void put32(std::vector<unsigned char> & b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((unsigned char) (v >> (8 * i)));
}

static void put16(std::vector<unsigned char> & b, uint16_t v) {
    b.push_back((unsigned char) v);
    b.push_back((unsigned char) (v >> 8));
}

// A WAV as the mic probe writes it, optionally with an extra chunk before data.
static std::vector<unsigned char> wav(const std::vector<int16_t> & s, uint16_t channels, uint32_t rate, bool extra,
                                      uint16_t bits = 16, uint16_t format = 1) {
    std::vector<unsigned char> b;
    const uint32_t             bytes = (uint32_t) (s.size() * 2);
    b.insert(b.end(), { 'R', 'I', 'F', 'F' });
    put32(b, 36 + bytes + (extra ? 8 + 3 + 1 : 0));
    b.insert(b.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
    put32(b, 16);
    put16(b, format);
    put16(b, channels);
    put32(b, rate);
    put32(b, rate * channels * 2);
    put16(b, (uint16_t) (channels * 2));
    put16(b, bits);
    if (extra) {  // an odd-sized chunk, padded to even
        b.insert(b.end(), { 'L', 'I', 'S', 'T' });
        put32(b, 3);
        b.insert(b.end(), { 'a', 'b', 'c', 0 });
    }
    b.insert(b.end(), { 'd', 'a', 't', 'a' });
    put32(b, bytes);
    for (int16_t v : s) put16(b, (uint16_t) v);
    return b;
}

int main() {
    // Mono, 16 kHz, read back exactly.
    {
        const std::vector<int16_t> s = { 0, 1000, -1000, 32767, -32768 };
        const auto                 b = wav(s, 1, 16000, false);
        const WavAudio             a = parse_wav(b.data(), b.size());
        CHECK(a.error.empty());
        CHECK(a.rate == 16000);
        CHECK(a.samples == s);
    }
    // Stereo is averaged; an odd-sized chunk before data is skipped.
    {
        const std::vector<int16_t> s = { 100, 300, -200, -400 };
        const auto                 b = wav(s, 2, 48000, true);
        const WavAudio             a = parse_wav(b.data(), b.size());
        CHECK(a.error.empty());
        CHECK(a.rate == 48000);
        CHECK((a.samples == std::vector<int16_t>{ 200, -300 }));
    }
    // Refused: 8-bit, float, truncated, not a WAV.
    {
        const std::vector<int16_t> s = { 1, 2 };
        auto                       b8 = wav(s, 1, 16000, false, 8);
        CHECK(!parse_wav(b8.data(), b8.size()).error.empty());
        auto bf = wav(s, 1, 16000, false, 16, 3);
        CHECK(!parse_wav(bf.data(), bf.size()).error.empty());
        auto bt = wav(s, 1, 16000, false);
        bt.resize(bt.size() - 2);
        CHECK(!parse_wav(bt.data(), bt.size()).error.empty());
        const unsigned char junk[16] = { 'n', 'o', 'p', 'e' };
        CHECK(!parse_wav(junk, sizeof(junk)).error.empty());
    }
    // Level and the gate: zeros (the mute button) and a quiet room are skipped, soft speech is not.
    {
        const std::vector<int16_t> zeros(16000, 0);
        const AudioLevel           z = audio_level(zeros, 16000);
        CHECK(z.peak == 0 && z.rms == 0 && z.seconds == 1.0);
        CHECK(!audio_worth_transcribing(z, 200));
        CHECK(audio_worth_transcribing(z, 0) == true);  // gate off: anything with length goes
        std::vector<int16_t> room(16000, 0);
        for (size_t i = 0; i < room.size(); ++i) room[i] = (int16_t) ((i % 7) * 12 - 36);  // peaks at 36
        CHECK(!audio_worth_transcribing(audio_level(room, 16000), 200));
        std::vector<int16_t> soft = room;
        soft[8000] = 400;
        CHECK(audio_worth_transcribing(audio_level(soft, 16000), 200));
        CHECK(!audio_worth_transcribing(audio_level({}, 16000), 0));  // nothing at all
    }
    // whisper input: 16 kHz scaled, 48 kHz averaged by three, other rates refused.
    {
        const std::vector<int16_t> s = { 16384, -16384, 0 };
        const auto                 f = to_whisper_input(s, 16000);
        CHECK(f.size() == 3 && f[0] == 0.5f && f[1] == -0.5f && f[2] == 0.0f);
        const auto g = to_whisper_input({ 3, 6, 9, 30, 60, 90, 1 }, 48000);
        CHECK(g.size() == 2);
        CHECK(std::abs(g[0] - 6.0f / 32768.0f) < 1e-9f);
        CHECK(to_whisper_input(s, 44100).empty());
    }
    if (failures) {
        std::fprintf(stderr, "stt_audio_test: %d failed\n", failures);
        return 1;
    }
    std::printf("stt_audio_test: ok\n");
    return 0;
}
