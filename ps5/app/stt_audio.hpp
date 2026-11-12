// PS5LM Lean: the audio side of speech-to-text, with no whisper and no console
// calls, so the host test can check it (stt_audio_test.cpp).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps5lm {

// A 16-bit PCM WAV file (the mic probe's mic-test*.wav) as mono samples at
// its own rate. Stereo is averaged. Anything else is refused with `error` set.
struct WavAudio {
    std::vector<int16_t> samples;  // mono
    uint32_t             rate = 0;
    std::string          error;     // empty when read
};
WavAudio read_wav(const std::string & path);
WavAudio parse_wav(const unsigned char * data, size_t size);

// A take's loudness, for the gate below.
struct AudioLevel {
    int    peak = 0;      // largest absolute sample, 0..32768
    double rms = 0;       // root mean square
    double seconds = 0;
};
AudioLevel audio_level(const std::vector<int16_t> & samples, uint32_t rate);

// Whether a take holds anything worth transcribing. whisper writes a word
// ("you") for a silent take, so a take whose peak stays under `min_peak` is
// skipped. Measured 2026-11-11 on the DualSense: a quiet room peaks at 60-90,
// soft speech at 240-800, normal speech at full scale. 0 turns the gate off.
bool audio_worth_transcribing(const AudioLevel & level, int min_peak);

// whisper's input: 16 kHz mono floats in [-1, 1]. 48 kHz is taken down to 16
// kHz by averaging each three samples; other rates are refused (empty result).
std::vector<float> to_whisper_input(const std::vector<int16_t> & samples, uint32_t rate);

}  // namespace ps5lm
