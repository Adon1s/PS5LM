// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace ps5lm {

// stdout and stderr through a pipe into memory, and one thread of the sink's
// own that writes them to the log file (PS5LM Lean).
//
// Stock PS5LM points both streams at app.log on /data, stderr unbuffered. The
// console sometimes makes a title's write to /data wait for seconds (#41;
// thejswaggart measured 9-26 s in #69), and every thread that prints waits
// with it: the GPU driver prints with its submit lock held, the GL library
// from the render thread, llama.cpp from its log thread. That stopped the
// whole app, screen included, until the write returned (cobanov/PS5LM#70).
//
// Here a print is a write to a non-blocking pipe and returns at once. A
// drain thread copies the pipe into a memory ring; a writer thread empties the
// ring into the file in large writes, and only it waits for storage. When the
// ring is full, new text is dropped and counted, and the count goes into the
// file once there is room.
//
// Starts by truncating `path`. False when the pipe cannot be set up; the
// streams then append to `path` directly, as stock PS5LM does, and `why` says
// what failed.
bool log_sink_start(const std::string & path, std::string * why);

// Pushes the streams' buffers into the sink and waits, at most `ms`, until the
// writer has written everything it holds. For a quit or a restart.
void log_sink_flush(int ms);

struct LogSinkStats {
    bool     running       = false;
    uint64_t bytes_in      = 0;  // read from the pipe
    uint64_t bytes_written = 0;  // written to the file
    uint64_t bytes_dropped = 0;  // the ring was full
    uint64_t slowest_write_ms = 0;  // the longest single write to the file
};
LogSinkStats log_sink_stats();

// For the host test: the ring's size, set before log_sink_start.
void log_sink_set_ring_bytes(size_t bytes);

}  // namespace ps5lm
