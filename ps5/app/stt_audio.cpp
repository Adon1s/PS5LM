// SPDX-License-Identifier: GPL-3.0-or-later
#include "stt_audio.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ps5lm {

namespace {

uint32_t u32_at(const unsigned char * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

uint16_t u16_at(const unsigned char * p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

}  // namespace

WavAudio parse_wav(const unsigned char * data, size_t size) {
    WavAudio out;
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        out.error = "not a RIFF WAVE file";
        return out;
    }
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    bool     have_fmt = false;
    size_t   at = 12;
    while (at + 8 <= size) {
        const unsigned char * chunk = data + at;
        const uint32_t        len = u32_at(chunk + 4);
        const size_t          body = at + 8;
        if (len > size - body) {
            out.error = "a chunk runs past the end of the file";
            return out;
        }
        if (std::memcmp(chunk, "fmt ", 4) == 0 && len >= 16) {
            format = u16_at(data + body);
            channels = u16_at(data + body + 2);
            rate = u32_at(data + body + 4);
            bits = u16_at(data + body + 14);
            have_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            if (!have_fmt) {
                out.error = "data before fmt";
                return out;
            }
            if (format != 1 || bits != 16 || (channels != 1 && channels != 2) || rate == 0) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "unsupported format %u, %u bits, %u channels, %u Hz", format, bits,
                              channels, rate);
                out.error = buf;
                return out;
            }
            const size_t frames = len / (2u * channels);
            out.samples.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                const unsigned char * s = data + body + i * 2u * channels;
                int v = (int16_t) u16_at(s);
                if (channels == 2) {
                    v = (v + (int16_t) u16_at(s + 2)) / 2;
                }
                out.samples[i] = (int16_t) v;
            }
            out.rate = rate;
            return out;
        }
        at = body + len + (len & 1);  // chunks are padded to an even size
    }
    out.error = have_fmt ? "no data chunk" : "no fmt chunk";
    return out;
}

WavAudio read_wav(const std::string & path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        WavAudio out;
        out.error = "cannot open " + path;
        return out;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_wav(bytes.data(), bytes.size());
}

AudioLevel audio_level(const std::vector<int16_t> & samples, uint32_t rate) {
    AudioLevel level;
    double     sum = 0;
    for (const int16_t s : samples) {
        const int a = s < 0 ? -(int) s : (int) s;
        level.peak = a > level.peak ? a : level.peak;
        sum += (double) s * (double) s;
    }
    level.rms = samples.empty() ? 0.0 : std::sqrt(sum / (double) samples.size());
    level.seconds = rate ? (double) samples.size() / rate : 0.0;
    return level;
}

bool audio_worth_transcribing(const AudioLevel & level, int min_peak) {
    return level.seconds > 0 && level.peak >= min_peak;
}

std::vector<float> to_whisper_input(const std::vector<int16_t> & samples, uint32_t rate) {
    std::vector<float> out;
    if (rate == 16000) {
        out.reserve(samples.size());
        for (const int16_t s : samples) {
            out.push_back((float) s / 32768.0f);
        }
    } else if (rate == 48000) {
        out.reserve(samples.size() / 3);
        for (size_t i = 0; i + 2 < samples.size(); i += 3) {
            out.push_back(((float) samples[i] + (float) samples[i + 1] + (float) samples[i + 2]) / (3.0f * 32768.0f));
        }
    }
    return out;
}

}  // namespace ps5lm
