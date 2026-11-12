// PS5LM Lean, probe/mic: can a title record from the controller's microphone?
//
// The request file `mic` (probes.hpp) runs this on a thread of its own: it
// opens an audio-input port for the signed-in user, records a few seconds,
// logs every call's return code and the level of each second to app.log, and
// writes the recording as a WAV file in the data folder. Nothing here is part
// of serving a model.
//
// libSceAudioIn is a linked import, as libSceAudioOut is: a title here cannot
// look a symbol up at run time (sceKernelDlsym answers 0x80020003 for every
// name, PS5_Vulkan docs/HARDWARE_FINDINGS.md). The payload SDK ships no stub
// for it, so scripts/build-app.sh makes one naming the five calls below. An
// import the loader did not bind reads as a null slot, so every call is made
// through a pointer that was checked first.
//
// The calls, read from the console's own /system/common/lib/libSceAudioIn.sprx
// (2026-11-11; the PS4's API, kept):
//   sceAudioInOpen(user, type, index, len, freq, param) -> handle 0x300000nn, or an error
//     type   0 voice chat, 1 general, 5 voice recognition (2-4 only for system processes)
//     index  0
//     len    256 or 128 samples a read
//     freq   16000 or 48000
//     param  0 or 1 = signed 16-bit mono, 2 = signed 16-bit stereo
//   sceAudioInInput(handle, dest) waits for one buffer of len samples and copies it
//   sceAudioInGetSilentState(handle) -> bits: 1 no device, 2 low priority, 4 muted by the user
//   sceAudioInClose(handle)
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "probes.hpp"
#include "log.hpp"
#include "storage.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef __PROSPERO__

extern "C" {
int sceAudioInInit(void);
int sceAudioInOpen(int user, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioInInput(int handle, void * dest);
int sceAudioInClose(int handle);
int sceAudioInGetSilentState(int handle);
int sceUserServiceInitialize(const void * params);
int sceUserServiceGetInitialUser(int * user);
int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceKernelLoadStartModule(const char * path, size_t args, const void * argp, uint32_t flags, const void * option,
                             int * result);
int sceKernelDlsym(int handle, const char * symbol, void ** address);
int sceKernelUsleep(unsigned int us);
}

namespace ps5lm {

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_mic_busy{ false };

// An import's address as the loader left it: through a volatile, so the
// compiler cannot assume a declared function is never null.
template <class F>
F * bound(F * f) {
    F * volatile p = f;
    return p;
}

struct Calls {
    int (*init)(void) = nullptr;
    int (*open)(int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
    int (*input)(int, void *) = nullptr;
    int (*close)(int) = nullptr;
    int (*silent)(int) = nullptr;

    bool usable() const { return open && input && close; }
};

Calls linked_calls() {
    Calls c;
    c.init = bound(&sceAudioInInit);
    c.open = bound(&sceAudioInOpen);
    c.input = bound(&sceAudioInInput);
    c.close = bound(&sceAudioInClose);
    c.silent = bound(&sceAudioInGetSilentState);
    return c;
}

void log_calls(const char * when, const Calls & c) {
    log_line("mic: imports %s: Init %p, Open %p, Input %p, Close %p, GetSilentState %p\n", when, (void *) c.init,
             (void *) c.open, (void *) c.input, (void *) c.close, (void *) c.silent);
}

// The library's functions: the linked imports, or, when the loader left them
// unbound, whatever loading the module at run time makes reachable. Every
// step is logged; a title that cannot reach them says which step answered what.
Calls find_calls() {
    Calls c = linked_calls();
    log_calls("as linked", c);
    if (c.usable()) {
        return c;
    }
    constexpr uint32_t kAudioInInternal = 0x80000002;  // the payload SDK's crt/rtld_sprx.c
    if (auto load = bound(&sceSysmoduleLoadModuleInternal)) {
        const int rc = load(kAudioInInternal);
        log_line("mic: sceSysmoduleLoadModuleInternal(0x%08x) = 0x%08x\n", kAudioInInternal, (unsigned) rc);
        c = linked_calls();
        log_calls("after the sysmodule load", c);
        if (c.usable()) {
            return c;
        }
    } else {
        log_line("mic: sceSysmoduleLoadModuleInternal is not bound\n");
    }
    auto start = bound(&sceKernelLoadStartModule);
    auto dlsym = bound(&sceKernelDlsym);
    if (!start) {
        log_line("mic: sceKernelLoadStartModule is not bound\n");
        return c;
    }
    for (const char * path : { "libSceAudioIn.sprx", "/system/common/lib/libSceAudioIn.sprx" }) {
        int status = 0;
        const int handle = start(path, 0, nullptr, 0, nullptr, &status);
        log_line("mic: sceKernelLoadStartModule(%s) = 0x%08x, status 0x%08x\n", path, (unsigned) handle, (unsigned) status);
        if (handle < 0) {
            continue;
        }
        c = linked_calls();
        log_calls("after the module load", c);
        if (c.usable() || !dlsym) {
            return c;
        }
        struct Name {
            const char * name;
            void **      to;
        } names[] = {
            { "sceAudioInInit", (void **) &c.init },     { "sceAudioInOpen", (void **) &c.open },
            { "sceAudioInInput", (void **) &c.input },   { "sceAudioInClose", (void **) &c.close },
            { "sceAudioInGetSilentState", (void **) &c.silent },
        };
        for (const Name & n : names) {
            void * at = nullptr;
            const int rc = dlsym(handle, n.name, &at);
            log_line("mic: sceKernelDlsym(%s) = 0x%08x, %p\n", n.name, (unsigned) rc, at);
            *n.to = rc == 0 ? at : nullptr;
        }
        return c;
    }
    return c;
}

struct Port {
    uint32_t type = 1, freq = 16000, len = 256, param = 0;

    uint32_t channels() const { return (param & 0xf) == 2 ? 2 : 1; }
};

const char * type_name(uint32_t type) {
    switch (type) {
        case 0: return "voice chat";
        case 1: return "general";
        case 5: return "voice recognition";
        default: return "system";
    }
}

std::string silent_text(int state) {
    if (state < 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "error 0x%08x", (unsigned) state);
        return buf;
    }
    std::string s;
    if (state & 1) s += " no-device";
    if (state & 2) s += " low-priority";
    if (state & 4) s += " muted-by-user";
    if (state & ~7) s += " other";
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%x", (unsigned) state);
    return std::string(buf) + (s.empty() ? " (none)" : " (" + s.substr(1) + ")");
}

struct Level {
    int      peak = 0;
    uint64_t sum = 0;
    uint64_t count = 0;

    void add(const int16_t * s, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const int a = std::abs((int) s[i]);
            peak = a > peak ? a : peak;
            sum += (uint64_t) a;
        }
        count += n;
    }
    double average() const { return count ? (double) sum / (double) count : 0.0; }
    double peak_db() const { return peak > 0 ? 20.0 * std::log10(peak / 32768.0) : -99.0; }
};

int open_port(const Calls & c, int user, const Port & p) {
    const int handle = c.open(user, p.type, 0, p.len, p.freq, p.param);
    log_line("mic: sceAudioInOpen(user 0x%x, type %u %s, index 0, len %u, freq %u, param %u) = 0x%08x%s\n", (unsigned) user,
             p.type, type_name(p.type), p.len, p.freq, p.param, (unsigned) handle, handle < 0 ? " FAILED" : "");
    return handle;
}

void close_port(const Calls & c, int handle) {
    const int rc = c.close(handle);
    log_line("mic: sceAudioInClose(0x%08x) = 0x%08x\n", (unsigned) handle, (unsigned) rc);
}

// Opens each port in turn, reads a quarter of a second from the ones that
// open, and closes them: which types and rates this console gives a title.
// Returns the first that opened and read.
bool scan_ports(const Calls & c, int user, bool system_types, Port & first) {
    std::vector<Port> ports = {
        { 1, 16000, 256, 0 }, { 0, 16000, 256, 0 }, { 5, 16000, 256, 0 },
        { 1, 48000, 256, 0 }, { 1, 48000, 256, 2 }, { 1, 16000, 128, 0 },
    };
    if (system_types) {
        for (uint32_t t : { 2u, 3u, 4u }) {
            ports.push_back({ t, 16000, 256, 0 });
        }
    }
    bool found = false;
    for (const Port & p : ports) {
        const int handle = open_port(c, user, p);
        if (handle < 0) {
            continue;
        }
        const int state = c.silent ? c.silent(handle) : 0;
        std::vector<int16_t> buf((size_t) p.len * p.channels());
        Level level;
        int   first_rc = 0, errors = 0;
        const int reads = (int) (p.freq / p.len / 4);
        const auto t0 = Clock::now();
        for (int i = 0; i < reads; ++i) {
            const int rc = c.input(handle, buf.data());
            if (i == 0) {
                first_rc = rc;
            }
            if (rc < 0) {
                ++errors;
                sceKernelUsleep(5000);
                continue;
            }
            level.add(buf.data(), buf.size());
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        log_line("mic:   %d reads in %.0f ms (audio %.0f ms), first read = 0x%08x, %d failed, peak %d (%.1f dBFS), average %.1f, "
                 "silent state %s\n",
                 reads, ms, 1000.0 * reads * p.len / p.freq, (unsigned) first_rc, errors, level.peak, level.peak_db(),
                 level.average(), silent_text(state).c_str());
        close_port(c, handle);
        if (!found && errors == 0) {
            first = p;
            found = true;
        }
    }
    return found;
}

bool write_wav(const std::string & path, const std::vector<int16_t> & samples, uint32_t freq, uint32_t channels) {
    FILE * f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const uint32_t bytes = (uint32_t) (samples.size() * sizeof(int16_t));
    unsigned char h[44];
    auto u32 = [&](size_t at, uint32_t v) { std::memcpy(h + at, &v, 4); };
    auto u16 = [&](size_t at, uint16_t v) { std::memcpy(h + at, &v, 2); };
    std::memcpy(h, "RIFF", 4);
    u32(4, 36 + bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1);  // PCM
    u16(22, (uint16_t) channels);
    u32(24, freq);
    u32(28, freq * channels * 2);
    u16(32, (uint16_t) (channels * 2));
    u16(34, 16);
    std::memcpy(h + 36, "data", 4);
    u32(40, bytes);
    bool ok = std::fwrite(h, 1, sizeof(h), f) == sizeof(h);
    ok = ok && (bytes == 0 || std::fwrite(samples.data(), 1, bytes, f) == bytes);
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

// Records `seconds` from one port: a level line for each second, then the WAV.
void record(const Calls & c, int user, const Port & p, int seconds, const std::string & out) {
    const int handle = open_port(c, user, p);
    if (handle < 0) {
        log_line("mic: FAILED: sceAudioInOpen = 0x%08x, nothing recorded\n", (unsigned) handle);
        return;
    }
    const uint32_t channels = p.channels();
    const int state0 = c.silent ? c.silent(handle) : 0;
    log_line("mic: recording %d s at %u Hz, %u channel%s, %u samples a read; silent state %s\n", seconds, p.freq, channels,
             channels == 1 ? "" : "s", p.len, silent_text(state0).c_str());
    std::vector<int16_t> all;
    all.reserve((size_t) seconds * p.freq * channels);
    std::vector<int16_t> buf((size_t) p.len * channels);
    Level  second, whole;
    int    errors = 0, run = 0, last_error = 0, first_rc = 0;
    bool   first = true;
    double slowest_ms = 0, second_slowest_ms = 0;
    const auto t0 = Clock::now();
    auto       second_t0 = t0;
    uint64_t   frames = 0, second_frames = 0;
    int        n = 0;
    const uint64_t want = (uint64_t) seconds * p.freq;
    while (frames < want) {
        const auto r0 = Clock::now();
        const int  rc = c.input(handle, buf.data());
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - r0).count();
        if (first) {
            first_rc = rc;
            first = false;
        }
        if (rc < 0) {
            ++errors;
            last_error = rc;
            if (++run >= 200) {  // about a second of nothing but errors
                log_line("mic: FAILED: sceAudioInInput = 0x%08x, 200 times in a row; stopping\n", (unsigned) rc);
                break;
            }
            sceKernelUsleep(5000);
            continue;
        }
        run = 0;
        slowest_ms = ms > slowest_ms ? ms : slowest_ms;
        second_slowest_ms = ms > second_slowest_ms ? ms : second_slowest_ms;
        second.add(buf.data(), buf.size());
        whole.add(buf.data(), buf.size());
        all.insert(all.end(), buf.begin(), buf.end());
        frames += p.len;
        second_frames += p.len;
        if (second_frames >= p.freq || frames >= want) {
            const auto   now = Clock::now();
            const double took = std::chrono::duration<double>(now - second_t0).count();
            const int    state = c.silent ? c.silent(handle) : 0;
            log_line("mic: second %d: peak %d (%.1f dBFS), average %.1f, %.2f s of audio in %.2f s, slowest read %.1f ms, "
                     "silent state %s\n",
                     ++n, second.peak, second.peak_db(), second.average(), (double) second_frames / p.freq, took,
                     second_slowest_ms, silent_text(state).c_str());
            second = Level();
            second_frames = 0;
            second_slowest_ms = 0;
            second_t0 = now;
        }
    }
    const double took = std::chrono::duration<double>(Clock::now() - t0).count();
    char         last[24] = "";
    if (errors) {
        std::snprintf(last, sizeof(last), " (last 0x%08x)", (unsigned) last_error);
    }
    log_line("mic: recorded %.2f s of audio in %.2f s; first read = 0x%08x, %d failed reads%s, slowest read %.1f ms; whole take "
             "peak %d (%.1f dBFS), average %.1f\n",
             (double) frames / p.freq, took, (unsigned) first_rc, errors, last, slowest_ms, whole.peak, whole.peak_db(),
             whole.average());
    close_port(c, handle);
    const std::string path = data_path(out);
    const bool        ok = write_wav(path, all, p.freq, channels);
    log_line("mic: %s %s, %zu bytes of samples (%u Hz, %u channel%s, 16-bit)\n", ok ? "wrote" : "FAILED to write", path.c_str(),
             all.size() * sizeof(int16_t), p.freq, channels, channels == 1 ? "" : "s");
}

// "t=1,f=16000,n=256,p=0,s=5,u=0x1234,scan=1,x=1,out=mute": one word, as a
// request file holds. Unknown keys are ignored.
struct Spec {
    Port        port;
    bool        port_given = false;
    bool        scan = true;
    bool        system_types = false;
    bool        user_given = false;
    int         user = 0;
    int         seconds = 5;
    std::string out = "mic-test.wav";
};

Spec parse_spec(const std::string & text) {
    Spec   s;
    bool   scan_given = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(',', at);
        end = end == std::string::npos ? text.size() : end;
        const std::string item = text.substr(at, end - at);
        at = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = item.substr(0, eq), val = item.substr(eq + 1);
        const long        num = std::strtol(val.c_str(), nullptr, 0);
        if (key == "t") {
            s.port.type = (uint32_t) num;
            s.port_given = true;
        } else if (key == "f") {
            s.port.freq = (uint32_t) num;
        } else if (key == "n") {
            s.port.len = (uint32_t) num;
        } else if (key == "p") {
            s.port.param = (uint32_t) num;
        } else if (key == "s") {
            s.seconds = num < 1 ? 1 : num > 30 ? 30 : (int) num;
        } else if (key == "u") {
            s.user = (int) num;
            s.user_given = true;
        } else if (key == "scan") {
            s.scan = num != 0;
            scan_given = true;
        } else if (key == "x") {
            s.system_types = num != 0;
        } else if (key == "out") {
            std::string name;
            for (char ch : val) {
                if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-') {
                    name += ch;
                }
            }
            if (!name.empty()) {
                s.out = "mic-test-" + name.substr(0, 24) + ".wav";
            }
        }
    }
    if (s.port_given && !scan_given) {
        s.scan = false;  // a named port is recorded as asked
    }
    return s;
}

}  // namespace

void probe_mic(std::string spec_text) {
    if (g_mic_busy.exchange(true)) {
        log_line("mic: a recording is already running; request '%s' dropped\n", spec_text.c_str());
        return;
    }
    const Spec spec = parse_spec(spec_text);
    log_line("mic: start, request '%s'\n", spec_text.c_str());
    const Calls c = find_calls();
    if (!c.usable()) {
        log_line("mic: FAILED: the audio-input library's calls are not reachable from this title (Open %p, Input %p, Close %p)\n",
                 (void *) c.open, (void *) c.input, (void *) c.close);
        g_mic_busy = false;
        return;
    }
    int user = -1;
    int rc = sceUserServiceGetInitialUser(&user);
    if (rc != 0) {
        const int init = sceUserServiceInitialize(nullptr);
        log_line("mic: sceUserServiceGetInitialUser = 0x%08x; sceUserServiceInitialize = 0x%08x\n", (unsigned) rc, (unsigned) init);
        rc = sceUserServiceGetInitialUser(&user);
    }
    log_line("mic: sceUserServiceGetInitialUser = 0x%08x, user 0x%x\n", (unsigned) rc, (unsigned) user);
    if (spec.user_given) {
        user = spec.user;
        log_line("mic: using user 0x%x from the request\n", (unsigned) user);
    } else if (rc != 0) {
        log_line("mic: FAILED: no signed-in user (sceUserServiceGetInitialUser = 0x%08x)\n", (unsigned) rc);
        g_mic_busy = false;
        return;
    }
    if (c.init) {
        const int init = c.init();
        log_line("mic: sceAudioInInit = 0x%08x\n", (unsigned) init);
    }
    Port port = spec.port;
    bool have = spec.port_given;
    if (spec.scan) {
        Port first;
        const bool found = scan_ports(c, user, spec.system_types, first);
        if (!found) {
            log_line("mic: scan: no port opened and read\n");
        } else {
            log_line("mic: scan: first port that opened and read: type %u %s, %u Hz, len %u, param %u\n", first.type,
                     type_name(first.type), first.freq, first.len, first.param);
        }
        if (!spec.port_given) {
            port = first;
            have = found;
        }
    }
    if (have) {
        record(c, user, port, spec.seconds, spec.out);
    } else {
        log_line("mic: FAILED: nothing to record from\n");
    }
    log_line("mic: done\n");
    g_mic_busy = false;
}

// One take for speech-to-text (stt.cpp, chat.cpp): the general port at 16 kHz
// mono, as the probe measured it, without its logging. Records until `stop` is
// set or `max_seconds` pass; `progress` (optional) gets the level of the last
// ~100 ms (0..1) and the seconds so far, about ten times a second. The silent
// state is read at the start and at the end, so a mute pressed during the take
// is seen.
bool mic_record(const std::atomic<bool> & stop, int max_seconds, MicTake & take,
                const std::function<void(float level, double seconds)> & progress) {
    take = MicTake{};
    if (g_mic_busy.exchange(true)) {
        take.error = "a recording is already running";
        return false;
    }
    struct Release {
        ~Release() { g_mic_busy = false; }
    } release;
    const Calls c = linked_calls();
    if (!c.usable()) {
        take.error = "the audio-input calls are not bound";
        return false;
    }
    int user = -1;
    int rc = sceUserServiceGetInitialUser(&user);
    if (rc != 0) {
        sceUserServiceInitialize(nullptr);
        rc = sceUserServiceGetInitialUser(&user);
    }
    char err[96];
    if (rc != 0) {
        std::snprintf(err, sizeof(err), "no signed-in user (sceUserServiceGetInitialUser = 0x%08x)", (unsigned) rc);
        take.error = err;
        return false;
    }
    const Port p{ 1, 16000, 256, 0 };
    const int  handle = c.open(user, p.type, 0, p.len, p.freq, p.param);
    if (handle < 0) {
        std::snprintf(err, sizeof(err), "sceAudioInOpen = 0x%08x", (unsigned) handle);
        take.error = err;
        return false;
    }
    take.rate = p.freq;
    const int start_state = c.silent ? c.silent(handle) : 0;
    take.silent_state = start_state > 0 ? start_state : 0;
    const uint64_t       want = (uint64_t) (max_seconds > 0 ? max_seconds : 1) * p.freq;
    std::vector<int16_t> buf(p.len);
    take.samples.reserve(std::min<uint64_t>(want, (uint64_t) 10 * p.freq));
    int        run = 0, peak = 0, reads = 0;
    const auto t0 = Clock::now();
    while (take.samples.size() < want && !stop.load()) {
        const int got = c.input(handle, buf.data());
        if (got < 0) {
            ++take.failed_reads;
            if (++run >= 200) {
                std::snprintf(err, sizeof(err), "sceAudioInInput = 0x%08x, 200 times in a row", (unsigned) got);
                take.error = err;
                break;
            }
            sceKernelUsleep(5000);
            continue;
        }
        run = 0;
        take.samples.insert(take.samples.end(), buf.begin(), buf.end());
        for (const int16_t v : buf) {
            const int a = v < 0 ? -(int) v : (int) v;
            peak = a > peak ? a : peak;
        }
        if (++reads % 6 == 0) {  // ~100 ms
            if (progress) {
                progress(std::min(1.0f, peak / 32767.0f), (double) take.samples.size() / p.freq);
            }
            peak = 0;
        }
    }
    take.seconds_taken = std::chrono::duration<double>(Clock::now() - t0).count();
    const int end_state = c.silent ? c.silent(handle) : 0;
    take.silent_state |= end_state > 0 ? end_state : 0;
    c.close(handle);
    return take.error.empty();
}

bool mic_take(int seconds, MicTake & take) {
    const std::atomic<bool> never{ false };
    return mic_record(never, seconds, take, {});
}

}  // namespace ps5lm

#else  // not the console: the preview and host tests have no microphone

namespace ps5lm {

void probe_mic(std::string spec_text) {
    log_line("mic: only on the console (request '%s')\n", spec_text.c_str());
}

bool mic_take(int, MicTake & take) {
    take = MicTake{};
    take.error = "no microphone off the console";
    return false;
}

bool mic_record(const std::atomic<bool> &, int, MicTake & take, const std::function<void(float, double)> &) {
    take = MicTake{};
    take.error = "no microphone off the console";
    return false;
}

}  // namespace ps5lm

#endif
