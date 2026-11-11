// SPDX-License-Identifier: GPL-3.0-or-later
// PS5LM Lean: prints that never wait for storage (log_sink.hpp).
#include "log_sink.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ps5lm {

namespace {

using Clock = std::chrono::steady_clock;

struct Sink {
    std::mutex              mtx;
    std::condition_variable has_text;  // the writer waits on it
    std::condition_variable written;   // log_sink_flush waits on it
    std::vector<char>       ring;
    size_t                  head = 0;  // the oldest byte held
    size_t                  held = 0;
    bool                    writing = false;  // the writer holds a chunk outside the lock
    uint64_t                unreported = 0;   // dropped and not yet noted in the file
    LogSinkStats            stats;
    int                     read_fd = -1;
    int                     out_fd  = -1;
};

Sink   g;
size_t g_ring_bytes = 2u << 20;

// Appends to the ring; the caller holds the lock and checked the room.
void put_locked(const char * p, size_t n) {
    const size_t cap  = g.ring.size();
    size_t       tail = (g.head + g.held) % cap;
    while (n > 0) {
        const size_t run = std::min(n, cap - tail);
        std::memcpy(g.ring.data() + tail, p, run);
        p += run;
        n -= run;
        g.held += run;
        tail = (tail + run) % cap;
    }
}

// One line where text went missing, in its place in the file.
void note_drops_locked(size_t room_needed_after) {
    if (g.unreported == 0) {
        return;
    }
    char      note[96];
    const int len = std::snprintf(note, sizeof(note), "\n[log] %llu bytes dropped here: the log file fell behind\n",
                                  (unsigned long long) g.unreported);
    if (len > 0 && g.ring.size() - g.held >= (size_t) len + room_needed_after) {
        put_locked(note, (size_t) len);
        g.unreported = 0;
    }
}

// Pipe to ring. Never touches storage, so it keeps the pipe empty and every
// print returns at once.
void drain() {
    std::vector<char> buf(64u << 10);
    for (;;) {
        const ssize_t n = read(g.read_fd, buf.data(), buf.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return;  // the read end stays open, so a print can never raise SIGPIPE
        }
        std::lock_guard<std::mutex> lock(g.mtx);
        g.stats.bytes_in += (uint64_t) n;
        note_drops_locked((size_t) n);
        const size_t take = std::min((size_t) n, g.ring.size() - g.held);
        put_locked(buf.data(), take);
        if (take < (size_t) n) {
            g.stats.bytes_dropped += (uint64_t) n - take;
            g.unreported += (uint64_t) n - take;
        }
        g.has_text.notify_one();
    }
}

void write_all(int fd, const char * p, size_t n) {
    while (n > 0) {
        const ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return;  // storage refused it: the text is lost, the app goes on
        }
        p += w;
        n -= (size_t) w;
    }
}

// Ring to file, in large writes. The only thread that waits for storage.
void writer() {
    std::vector<char>            chunk(1u << 20);
    std::unique_lock<std::mutex> lock(g.mtx);
    for (;;) {
        g.has_text.wait(lock, [] { return g.held > 0; });
        const size_t n   = std::min(g.held, chunk.size());
        const size_t cap = g.ring.size();
        const size_t run = std::min(n, cap - g.head);
        std::memcpy(chunk.data(), g.ring.data() + g.head, run);
        std::memcpy(chunk.data() + run, g.ring.data(), n - run);
        g.head = (g.head + n) % cap;
        g.held -= n;
        g.writing = true;
        lock.unlock();

        const Clock::time_point t0 = Clock::now();
        write_all(g.out_fd, chunk.data(), n);
        const uint64_t ms =
            (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();

        lock.lock();
        g.writing = false;
        g.stats.bytes_written += n;
        g.stats.slowest_write_ms = std::max(g.stats.slowest_write_ms, ms);
        if (g.held == 0) {
            g.written.notify_all();
        }
    }
}

// Stock PS5LM's streams: both append to the file, stderr unbuffered.
bool fallback(const std::string & path, std::string * why, const char * what, int err) {
    if (std::FILE * f = std::fopen(path.c_str(), "w")) {
        std::fclose(f);
    }
    if (std::freopen(path.c_str(), "a", stdout) != nullptr) {
        setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    if (std::freopen(path.c_str(), "a", stderr) != nullptr) {
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    if (why) {
        *why = std::string(what) + ": " + std::strerror(err);
    }
    return false;
}

}  // namespace

void log_sink_set_ring_bytes(size_t bytes) {
    g_ring_bytes = std::max<size_t>(bytes, 4096);
}

bool log_sink_start(const std::string & path, std::string * why) {
    int p[2];
    if (pipe(p) != 0) {
        return fallback(path, why, "pipe", errno);
    }
    const int fl = fcntl(p[1], F_GETFL);
    if (fl == -1 || fcntl(p[1], F_SETFL, fl | O_NONBLOCK) == -1) {
        const int err = errno;
        close(p[0]);
        close(p[1]);
        return fallback(path, why, "fcntl", err);
    }
    const int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
    if (out < 0) {
        const int err = errno;
        close(p[0]);
        close(p[1]);
        return fallback(path, why, "open", err);
    }
    // Each stream gets a descriptor of its own, which the pipe then replaces.
    // Mode "w", not "a": an append stream seeks before each write, and a seek
    // on a pipe fails. The buffering is set after, while each stream is a pipe.
    std::fflush(stdout);
    std::fflush(stderr);
    if (std::freopen(path.c_str(), "w", stdout) == nullptr || std::freopen(path.c_str(), "w", stderr) == nullptr ||
        dup2(p[1], fileno(stdout)) < 0 || dup2(p[1], fileno(stderr)) < 0) {
        const int err = errno;
        close(p[0]);
        close(p[1]);
        close(out);
        return fallback(path, why, "freopen or dup2", err);
    }
    close(p[1]);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    g.ring.assign(g_ring_bytes, '\0');
    g.read_fd = p[0];
    g.out_fd  = out;
    try {
        std::thread(drain).detach();
        std::thread(writer).detach();
    } catch (...) {
        // No thread: put the streams back on the file. The pipe's read end
        // stays open, so nothing already written to it can raise SIGPIPE.
        close(out);
        return fallback(path, why, "thread", EAGAIN);
    }
    std::lock_guard<std::mutex> lock(g.mtx);
    g.stats.running = true;
    return true;
}

void log_sink_flush(int ms) {
    std::fflush(stdout);
    std::fflush(stderr);
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        if (!g.stats.running) {
            return;
        }
    }
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(ms);
    // What is still in the pipe reaches the ring within a moment.
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(ms, 50)));
    std::unique_lock<std::mutex> lock(g.mtx);
    note_drops_locked(0);
    if (g.held > 0) {
        g.has_text.notify_one();
    }
    g.written.wait_until(lock, deadline, [] { return g.held == 0 && !g.writing; });
}

LogSinkStats log_sink_stats() {
    std::lock_guard<std::mutex> lock(g.mtx);
    return g.stats;
}

}  // namespace ps5lm
