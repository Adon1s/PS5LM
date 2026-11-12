// Measurements the app makes only when the PC asks (scripts/ps5lm-app.sh),
// each on a thread of its own, with its results in app.log. None of them is
// part of serving a model.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps5lm {

// Downloads a URL over HTTPS to fetch.out in the data folder: status, size, speed.
void probe_fetch(std::string url);

// Write speed by method (stdio, write, O_DIRECT) to the data folder and a USB drive.
void probe_write();

// Read speed of a file, the way streaming MoE experts from a drive would read
// it: sequentially, then at random offsets in pieces of 64 KiB to 4 MiB with
// one and four threads, through the cache and with O_DIRECT. Point it at a
// model of several GiB, so the cache can hold little of it.
void probe_read(std::string path);

// The title's memory as the system gave it: the direct pool and its largest
// free block, the flexible pool, and the largest block the app can allocate
// and map now, given back at once. `model_loaded`: skip that last part, so it
// never takes memory from a running model.
void probe_memory(bool model_loaded);

// PS5LM Lean, probe/mic (mic_probe.cpp): records from the controller's
// microphone through libSceAudioIn and writes mic-test.wav in the data folder,
// with every call's return code and each second's level in app.log. `spec` is
// the request file's one word: "-" scans the port types and rates a title may
// open and records 5 s from the first that works; "t=1,f=16000,n=256,p=0,s=5"
// records from that port (type, rate, samples a read, format, seconds);
// "out=name" writes mic-test-name.wav instead.
void probe_mic(std::string spec);

// One take from the controller's microphone for speech-to-text: the general
// port, 16 kHz mono. `silent_state` has bit 4 set when the mute button was on at
// the start or the end of the take. False with `error` set when it failed; a
// recording already running (the mic request) counts as a failure.
struct MicTake {
    std::vector<int16_t> samples;
    uint32_t             rate = 16000;
    int                  silent_state = 0;
    int                  failed_reads = 0;
    double               seconds_taken = 0;
    std::string          error;
};
bool mic_take(int seconds, MicTake & take);
// The same, open-ended for push-to-talk: records until `stop` is set or
// `max_seconds` pass; `progress` gets the level of the last ~100 ms (0..1) and
// the seconds so far, about ten times a second (on the recording thread).
bool mic_record(const std::atomic<bool> & stop, int max_seconds, MicTake & take,
                const std::function<void(float level, double seconds)> & progress);

}  // namespace ps5lm
