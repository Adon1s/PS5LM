// SPDX-License-Identifier: GPL-3.0-or-later
//
// PS5LM's screen. One idea runs through it: the model is the thing, and the
// network is where it goes. Home shows the model and how fast it runs on the
// left, the address other devices use on the right, and nothing else large;
// the console's temperature and power sit in the status line, where a car
// shows the weather. Colour means state only: blue is what is active or
// chosen, green a model that serves, red a fault. Everything else is grey.
//
// Sound works the same way: one sound for each kind of thing, on every page,
// always at the same pitch, so the ear learns them (PocketVibe's set, in
// assets/audio/sfx, named after the cue):
//   focus        the highlight moved
//   error        it cannot: the end of a list, a model too big
//   tab          another page, or another view of one (search, log, range)
//   select       went in, or did it: a section, a choice, a download, a pause
//   back         came out, or cancelled
//   modal_open   a question or a list of choices opened
//   modal_close  ... and closed without an answer
//   toggle       a switch flipped
//   launch       a model starts loading
//   notify       a model is ready (not from a button)
//   complete     a download is saved (not from a button)
#include "dashboard.hpp"
#include "storage.hpp"
#include "suggested.hpp"
#include "version.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace ps5lm {

using hui::Action;
using hui::Direction;
using hui::gfx::Color;
using hui::gfx::Rect;
namespace gfx   = hui::gfx;
namespace ui    = hui::ui;
namespace tween = hui::tween;
using Cue = hui::audio::Cue;

namespace {

// ---- the design language -------------------------------------------------------

const Color kBase    = Color::rgb(0x121314);
const Color kSurface = Color::rgb(0x1c1d1f);  // sheets and panels
const Color kRaised  = Color::rgb(0x28292c);  // the focused row
const Color kHair    = Color::rgb(0x2c2e31);  // dividers
const Color kInk     = Color::rgb(0xf4f4f5);
const Color kInk2    = Color::rgb(0xa3a5aa);
const Color kInk3    = Color::rgb(0x6b6e74);
const Color kBlue    = Color::rgb(0x3e6ae1);
const Color kRed     = Color::rgb(0xe5484d);
const Color kGreen   = Color::rgb(0x3dbe7a);
const Color kBlack   = Color::rgb(0x000000);

constexpr float kLeft = 96.0f, kRight = 1824.0f;
constexpr float kTitleY = 182.0f;                 // page titles' baseline
constexpr float kDockY = 966.0f;                  // the dock's top edge
constexpr float kRailW = 330.0f, kRailPitch = 76.0f, kRailH = 64.0f;
constexpr float kListX = 486.0f, kListY = 236.0f, kRowPitch = 92.0f, kRowH = 80.0f;
constexpr int   kVisibleRows = 7;
constexpr float kTau = 6.2831853f;
constexpr const char * kDash = "\xE2\x80\x93";  // an en dash: nothing to show

// PS5LM Lean: Home is the chat first. The chat panel takes the right two
// thirds at a size read from a couch; the model, its speed, the memory and the
// console's readings stand in a rail on the left.
constexpr float kHomeRailW = 508.0f;   // the rail's width, from kLeft
constexpr float kHomeGap = 44.0f;      // between the rail and the chat
constexpr float kChatSize = 24.0f;     // the chat's text
constexpr float kChatPitch = kChatSize + 9;  // a line of it
constexpr int   kNewestLines = 14;     // the other devices' newest answer: its lines, at most
constexpr int   kBoxLines = 4;         // the message box grows to this many lines
constexpr int   kMeterBars = 56;       // the take's level, twenty a second: the last 2.8 s

Rect home_panel() {
    const float x = kLeft + kHomeRailW + kHomeGap;
    return { x, 124, kRight - x, 808 };
}
float box_text_w(float box_w) { return box_w - 84 - 214; }  // the mic's glyph on the left, Send on the right

// Another device's request the model is answering now, or null. The model
// answers one request at a time: the chat's reply waits for it.
const Exchange * running_elsewhere(const Live & L) {
    for (auto it = L.activity.rbegin(); it != L.activity.rend(); ++it) {
        if (it->status == 0 && it->client.rfind("127.0.0.1", 0) != 0) {
            return &*it;
        }
    }
    return nullptr;
}

constexpr const char * kPageNames[Dashboard::kPages] = { "Home", "Models", "API", "Console", "Settings" };
constexpr const char * kSectionNames[Dashboard::kSections] = { "On this PS5", "Suggested", "Search", "Downloads" };

// The market's searches, switched with L2/R2: Hugging Face's most downloaded
// GGUF repositories, then the families the presets know.
constexpr const char * kMarketChips[] = { "Popular", "Qwen", "Gemma", "Llama", "Mistral", "Phi", "Granite" };
constexpr int          kMarketChipCount = 7;

// ---- Settings -------------------------------------------------------------------

enum class Setting : int { auto_load, default_model, ctx_cap, kv_type, sounds, tools, introspect, scratch_limit, clear_scratch, download_dir,
                           storage, debug_report, report_kernel_log, screen_off, screen_reload, screen_sleep };

struct SettingRow {
    Setting      id;
    const char * label;
    const char * help;
};

struct SettingCategory {
    const char * name;
    SettingRow   rows[3];
    int          count;
};

constexpr SettingCategory kCategories[] = {
    // PS5LM Lean: the screen holds memory the model could use; first, so it is
    // found. The help lines are written with this console's measured figure
    // (draw_settings); these are for a console that has not measured it yet.
    { "Screen and memory",
      { { Setting::screen_off, "Turn the screen off",
          "Frees about 360 MB for the model. Hold Triangle for 2 seconds to bring it back" },
        { Setting::screen_reload, "Turn off and reload the model",
          "The freed memory in one piece, for a model that needs all of it. About 20 seconds" },
        { Setting::screen_sleep, "Sleep the screen after",
          "No button pressed for this long: the screen sleeps and frees its memory; any button wakes it" } },
      3 },
    { "Startup",
      { { Setting::auto_load, "Load a model at launch", "Off: the app opens and waits for a choice" },
        { Setting::default_model, "Model to load", "One of the models on this PS5" } },
      2 },
    { "Model",
      { { Setting::ctx_cap, "Longest context", "The longest that fits, up to this; applies at the next load" },
        { Setting::kv_type, "Context cache", "Auto picks the best that fits; a fixed type trades length for quality" } },
      2 },
    { "Chat",
      { { Setting::tools, "File tools", "The browser chat may read and write files in /data/PS5LM/scratch" },
        { Setting::scratch_limit, "Warn above", "A notification when that folder grows past this" },
        { Setting::clear_scratch, "Clear the scratch folder", "Deletes everything the chat's tools wrote" } },
      3 },
    { "Sound and storage",
      { { Setting::sounds, "Sounds", "Interface sounds on the TV" },
        { Setting::download_dir, "Save downloads to", "Internal storage, or PS5LM/models on an M.2 or USB drive" },
        { Setting::storage, "Where PS5LM Voice keeps its files", "Full storage, or its own while the HEN keeps /data closed" } },
      3 },
    { "Developer",
      { { Setting::introspect, "Logits and activations API",
          "Off: /v1/introspect is closed. On: the raw numbers. A change restarts the model" } },
      1 },
    { "Debug report",
      { { Setting::debug_report, "Create a debug report", "One file to attach to a GitHub issue, with no keys or tokens in it" },
        { Setting::report_kernel_log, "Include the kernel log", "Its last 1 MiB, read from klogsrv; off unless you switch it on" } },
      2 },
    { "Diagnostics", {}, 0 },
};
constexpr int kCategoryCount = 8;
constexpr int kDiagnostics = 7;

constexpr uint32_t     kCtxSteps[] = { 4096, 8192, 16384, 32768, 65536, 131072, 196608, 262144 };
constexpr const char * kKvSteps[] = { "auto", "f16", "q8_0", "q4_0" };
constexpr uint32_t     kScratchSteps[] = { 1, 2, 5, 10, 20 };
constexpr uint32_t     kSleepSteps[] = { 0, 5, 15, 30, 60 };  // PS5LM Lean: minutes, 0 never

constexpr int   kLogLines = 20;
constexpr float kLogLineH = 29.0f;

// ---- small helpers ----------------------------------------------------------------

float put(gfx::DrawList & l, const ui::FontRef & f, std::string_view v, float x, float y, float size, Color c,
           gfx::Align a = gfx::Align::left) {
    return ui::text(l, f, v, x, y, size, c, a);
}

std::string format(const char * fmt, double v) {
    char t[48];
    std::snprintf(t, sizeof(t), fmt, v);
    return t;
}

// "9,120"
std::string thousands(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = (int) s.size() - 3; i > 0; i -= 3) {
        s.insert((size_t) i, ",");
    }
    return s;
}

// Sizes as the console's own screens give them: binary gigabytes, called GB.
std::string gb(double gib) { return format("%.1f GB", gib); }
std::string gb_text(std::string s) {
    for (size_t at = s.find("GiB"); at != std::string::npos; at = s.find("GiB", at)) {
        s.replace(at, 3, "GB");
    }
    return s;
}

// "Qwen3.8-27B-UD-IQ2_XXS" -> "Qwen3.8 27B" and "UD-IQ2_XXS": the build starts
// at the first part that names a quantization.
bool quant_part(const std::string & p) {
    std::string u = p;
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    const auto starts = [&](const char * s) { return u.rfind(s, 0) == 0; };
    return u == "UD" || u == "BF16" || u == "F16" || u == "F32" || starts("MXFP") || u.find("_K") != std::string::npos ||
           (u.size() > 1 && (u[0] == 'Q' || starts("IQ") || starts("TQ") || starts("PQ")) &&
            std::isdigit((unsigned char) u[u[0] == 'Q' ? 1 : 2]));
}

void split_name(const std::string & label, std::string * name, std::string * build) {
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= label.size()) {
        const size_t dash = label.find('-', at);
        parts.push_back(label.substr(at, dash == std::string::npos ? std::string::npos : dash - at));
        if (dash == std::string::npos) {
            break;
        }
        at = dash + 1;
    }
    name->clear();
    build->clear();
    bool in_build = false;
    for (const auto & p : parts) {
        in_build = in_build || quant_part(p);
        std::string & to = in_build ? *build : *name;
        if (!in_build && (p == "it" || p == "Instruct" || p == "GGUF")) {
            continue;
        }
        to += (to.empty() ? "" : in_build ? "-" : " ") + p;
    }
    if (name->empty()) {
        *name = label;
    }
}

// "/data/PS5LM/models/Qwen3.8-27B-UD-Q2_K_XL.gguf" -> "Qwen3.8-27B-UD-Q2_K_XL".
std::string base_label(const std::string & path) {
    std::string s = path.substr(path.find_last_of('/') + 1);
    if (s.size() > 5 && s.compare(s.size() - 5, 5, ".gguf") == 0) {
        s.resize(s.size() - 5);
    }
    return s;
}

// A slim bar: the track, and the share filled.
void meter(gfx::DrawList & l, float x, float y, float w, float share, Color c) {
    l.rounded_rect({ x, y, w, 4 }, 2, kHair);
    if (share > 0.002f) {
        l.rounded_rect({ x, y, std::max(4.0f, w * tween::clamp01(share)), 4 }, 2, c);
    }
}

// The model's speed over the last two minutes as one line, newest on the
// right, on a faint floor; Tesla's energy graph, not a dashboard chart.
// A series of up to 120 points between lo and hi, newest at the right edge.
void plot(gfx::DrawList & l, const std::vector<float> & v, const Rect & g, Color c, float lo, float hi) {
    if (v.size() < 2 || hi <= lo) {
        return;
    }
    const float step = g.w / 119.0f;
    const auto at = [&](float x) { return g.y + g.h - tween::clamp01((x - lo) / (hi - lo)) * g.h; };
    float lx = 0, ly = 0;
    for (size_t age = 0; age < v.size(); ++age) {
        const float x = g.x + g.w - (float) age * step;
        const float y = at(v[v.size() - 1 - age]);
        if (age > 0) {
            l.line(lx, ly, x, y, 2.0f, c);
        }
        lx = x;
        ly = y;
    }
    l.circle(g.x + g.w, at(v.back()), 5, c);
}

void trace(gfx::DrawList & l, const std::vector<float> & v, const Rect & g, Color c) {
    l.line(g.x, g.y + g.h, g.x + g.w, g.y + g.h, 1.5f, kHair);
    float top = 1.0f;
    for (float x : v) {
        top = std::max(top, x);
    }
    plot(l, v, g, c, 0, top * 1.15f);
}

// The dock's line icons, 32 px, drawn with strokes like the rest.
void icon(gfx::DrawList & l, int page, float cx, float cy, Color c) {
    const float w = 2.6f;
    switch (page) {
        case Dashboard::kHomePage:
            l.line(cx - 16, cy - 1, cx, cy - 14, w, c);
            l.line(cx, cy - 14, cx + 16, cy - 1, w, c);
            l.line(cx - 11, cy - 5, cx - 11, cy + 13, w, c);
            l.line(cx + 11, cy - 5, cx + 11, cy + 13, w, c);
            l.line(cx - 11, cy + 13, cx + 11, cy + 13, w, c);
            l.line(cx - 3, cy + 13, cx - 3, cy + 4, w, c);
            l.line(cx + 3, cy + 13, cx + 3, cy + 4, w, c);
            l.line(cx - 3, cy + 4, cx + 3, cy + 4, w, c);
            break;
        case Dashboard::kModelsPage: {
            // A cube: the hexagon and the three edges that meet in front.
            const float r = 15, h = 13;
            const float px[] = { cx, cx + h, cx + h, cx, cx - h, cx - h };
            const float py[] = { cy - r, cy - r / 2, cy + r / 2, cy + r, cy + r / 2, cy - r / 2 };
            for (int i = 0; i < 6; ++i) {
                l.line(px[i], py[i], px[(i + 1) % 6], py[(i + 1) % 6], w, c);
            }
            l.line(cx, cy, cx - h, cy - r / 2, w, c);
            l.line(cx, cy, cx + h, cy - r / 2, w, c);
            l.line(cx, cy, cx, cy + r, w, c);
            break;
        }
        case Dashboard::kApiPage:
            l.line(cx - 7, cy - 10, cx - 16, cy, w, c);
            l.line(cx - 16, cy, cx - 7, cy + 10, w, c);
            l.line(cx + 7, cy - 10, cx + 16, cy, w, c);
            l.line(cx + 16, cy, cx + 7, cy + 10, w, c);
            l.line(cx + 3, cy - 13, cx - 3, cy + 13, w, c);
            break;
        case Dashboard::kConsolePage: {
            // A pulse: the line a monitor draws.
            const float px[] = { cx - 17, cx - 8, cx - 3, cx + 3, cx + 8, cx + 17 };
            const float py[] = { cy + 2, cy + 2, cy - 12, cy + 13, cy, cy };
            for (int i = 0; i < 5; ++i) {
                l.line(px[i], py[i], px[i + 1], py[i + 1], w, c);
            }
            break;
        }
        default: {
            const float knobs[] = { -6, 7, -1 };
            for (int i = 0; i < 3; ++i) {
                const float y = cy - 10 + 10.0f * (float) i;
                l.line(cx - 16, y, cx + 16, y, w, c);
                l.circle(cx + knobs[i], y, 5.5f, kBase);
                l.ring(cx + knobs[i], y, 5.0f, w, c);
            }
            break;
        }
    }
}

// A Tesla-style switch: a pill, blue and knob right when on.
void toggle_switch(gfx::DrawList & l, float right, float cy, bool on) {
    const Rect r{ right - 64, cy - 18, 64, 36 };
    l.rounded_rect(r, 18, on ? kBlue : kHair);
    l.circle(on ? r.x + r.w - 18 : r.x + 18, cy, 14, on ? kInk : kInk2);
}

// Greedy word wrap to `width`; a word longer than a line is cut.
std::vector<std::string> wrap(const ui::FontRef & f, const std::string & raw, float size, float width) {
    std::string text = raw;  // line breaks and tabs have no glyph: spaces
    std::replace_if(text.begin(), text.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
    std::vector<std::string> lines;
    std::string line;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(' ', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string word = text.substr(at, end - at);
        const std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && f.measure(next, size) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = next;
        }
        at = end + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    for (auto & l : lines) {
        l = f.font->fit(l, size, width);
    }
    return lines;
}

int first_visible(int cursor, int rows) {
    return std::clamp(cursor - kVisibleRows / 2, 0, std::max(0, rows - kVisibleRows));
}

// 2 error, 1 warning, 0 neither. llama.cpp marks its lines " E " and " W ";
// the rest is matched by words, so "failures=0" is not an error.
int log_level(const std::string & line) {
    std::string low = line;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return (char) std::tolower(ch); });
    const auto has = [&](const char * w) { return low.find(w) != std::string::npos; };
    if (line.find(" E ") != std::string::npos || has("error") || has("failed") || has("exception") || has("fatal") ||
        has("abort")) {
        return 2;
    }
    if (line.find(" W ") != std::string::npos || has("warn")) {
        return 1;
    }
    return 0;
}

// "2.1 of 10.4 GB, 3.8 MB/s, 37 min left", or the note of one that waits.
std::string download_line(const Download & d) {
    char t[200];
    const double done = d.done / 1073741824.0, all = d.bytes / 1073741824.0;
    if (d.state == DownloadState::running && d.mbps > 0.05) {
        const double left_s = (double) (d.bytes - std::min(d.done, d.bytes)) / (d.mbps * 1e6);
        if (left_s >= 3600) {
            std::snprintf(t, sizeof(t), "%.1f of %.1f GB, %.1f MB/s, %.0f h %02.0f min left", done, all, d.mbps,
                          std::floor(left_s / 3600), std::fmod(left_s / 60, 60));
        } else {
            std::snprintf(t, sizeof(t), "%.1f of %.1f GB, %.1f MB/s, %.0f min left", done, all, d.mbps, std::ceil(left_s / 60));
        }
        return d.note.empty() ? std::string(t) : std::string(t) + ", " + d.note;  // the note: written without ftpsrv
    }
    std::snprintf(t, sizeof(t), "%.1f of %.1f GB", done, all);
    if (d.state == DownloadState::running) {
        // No speed: starting, or every piece is in and the note says the SHA-256 check's progress (#30).
        return std::string(t) + ", " + (d.note.empty() ? "starting" : d.note);
    }
    if (d.state == DownloadState::done) {
        return d.note;
    }
    if (!d.note.empty()) {
        return std::string(t) + ", " + d.note;
    }
    return d.state == DownloadState::queued ? std::string(t) + ", waiting its turn" : t;
}

// The value of a setting as its row shows it, and one step of it.
std::string setting_text(const Settings & s, Setting id, const std::vector<ModelRow> & models,
                         const std::vector<Drive> & drives) {
    switch (id) {
        case Setting::default_model: {
            if (s.default_model.empty()) {
                return "None";
            }
            for (const auto & m : models) {
                if (m.file == s.default_model) {
                    std::string name, build;
                    split_name(m.label, &name, &build);
                    return name + "  " + build;
                }
            }
            return "Missing";
        }
        case Setting::ctx_cap: return format("%.0fk tokens", s.ctx_cap / 1024.0);
        case Setting::kv_type: return s.kv_type == "auto" ? "Auto" : s.kv_type;
        case Setting::scratch_limit: return format("%.0f GB", (double) s.scratch_limit_gib);
        case Setting::screen_sleep:
            return s.screen_sleep_min == 0  ? std::string("Never")
                 : s.screen_sleep_min == 60 ? std::string("1 hour")
                 : format(s.screen_sleep_min == 1 ? "%.0f minute" : "%.0f minutes", (double) s.screen_sleep_min);
        case Setting::download_dir: {
            for (const auto & d : drives) {
                if (d.models == s.download_dir) {
                    return d.name;
                }
            }
            return drive_name(s.download_dir) + ", not found";
        }
        default: return "";
    }
}

bool is_toggle(Setting id) {
    return id == Setting::auto_load || id == Setting::sounds || id == Setting::tools || id == Setting::introspect
        || id == Setting::report_kernel_log;
}

bool & toggle_ref(Settings & s, Setting id) {
    switch (id) {
        case Setting::auto_load:         return s.auto_load;
        case Setting::sounds:            return s.sounds;
        case Setting::introspect:        return s.introspect;
        case Setting::report_kernel_log: return s.report_kernel_log;
        default:                         return s.tools;
    }
}

bool toggle_value(const Settings & s, Setting id) {
    return is_toggle(id) && toggle_ref(const_cast<Settings &>(s), id);
}

// A row that does something when pressed, with no value of its own.
bool is_action(Setting id) {
    return id == Setting::clear_scratch || id == Setting::debug_report || id == Setting::screen_off ||
           id == Setting::screen_reload;
}

// Used and whole size of the app's storage, from its drive, as the row shows it.
std::string app_storage_text(const std::vector<Drive> & drives, double size_gib) {
    for (const auto & d : drives) {
        if (d.name == "App storage" && d.total > 0) {
            return format("App storage, %.1f", d.total - d.free) + format(" of %.0f GB used", d.total);
        }
    }
    return format("App storage, %.0f GB", size_gib);
}

// A row's choices, for the list X opens under it: each with the settings it
// would make, and whether it is the one set now.
struct Choice {
    std::string label, note;
    Settings    value;
    bool        current = false;
};

std::vector<Choice> choices_of(const Settings & s, Setting id, const std::vector<ModelRow> & models,
                               const std::vector<Drive> & drives) {
    std::vector<Choice> out;
    const auto add = [&](std::string label, std::string note, bool current, auto set) {
        Choice c{ std::move(label), std::move(note), s, current };
        set(c.value);
        out.push_back(std::move(c));
    };
    switch (id) {
        case Setting::default_model:
            add("None", "Wait for a choice", s.default_model.empty(), [](Settings & v) { v.default_model.clear(); });
            for (const auto & m : models) {
                if (m.fits) {
                    std::string name, build;
                    split_name(m.label, &name, &build);
                    add(name + "  " + build, gb(m.size_gib), m.file == s.default_model,
                        [&](Settings & v) { v.default_model = m.file; });
                }
            }
            break;
        case Setting::ctx_cap:
            for (uint32_t c : kCtxSteps) {
                add(format("%.0fk tokens", c / 1024.0), "", s.ctx_cap == c, [c](Settings & v) { v.ctx_cap = c; });
            }
            break;
        case Setting::kv_type: {
            const char * notes[] = { "The best that fits", "Full quality, the most memory", "Near full quality, half the memory",
                                     "A quarter of the memory" };
            for (int i = 0; i < 4; ++i) {
                const std::string t = kKvSteps[i];
                add(i == 0 ? "Auto" : t, notes[i], s.kv_type == t, [&](Settings & v) { v.kv_type = t; });
            }
            break;
        }
        case Setting::scratch_limit:
            for (uint32_t g : kScratchSteps) {
                add(format("%.0f GB", (double) g), "", s.scratch_limit_gib == g, [g](Settings & v) { v.scratch_limit_gib = g; });
            }
            break;
        case Setting::screen_sleep:
            for (uint32_t m : kSleepSteps) {
                Settings shown = s;
                shown.screen_sleep_min = m;
                add(setting_text(shown, Setting::screen_sleep, models, drives), m == 0 ? "The screen stays on" : "",
                    s.screen_sleep_min == m, [m](Settings & v) { v.screen_sleep_min = m; });
            }
            break;
        case Setting::download_dir:
            for (const auto & d : drives) {
                add(d.name, d.total > 0 ? gb(d.free) + " free" : "", d.models == s.download_dir,
                    [&](Settings & v) { v.download_dir = d.models; });
            }
            break;
        default: break;
    }
    return out;
}

// The base name a suggestion's model file has on the console.
const char * local_name(const SuggestedFile & f) { return f.save_as ? f.save_as : f.file; }

// What Square sends from Home, in turn: something to show the model off with.
constexpr const char * kChatDemos[] = {
    "In two sentences, how can a PS5 run a language model?",
    "Write a haiku about a game console that dreams in tokens.",
    "Give me three names for a home AI server that lives under the TV.",
    "What is 17 times 23? Show the steps in one line.",
};
constexpr const char * kDecisionDemos[] = {
    "I was charged twice this month. Refund me or I cancel.",
    "Love the new update, everything is so much faster now!",
    "Where can I find the invoice for last month?",
    "Third time asking. Cancel my subscription today.",
};
constexpr int kDemos = 4;

}  // namespace

// ---- state ------------------------------------------------------------------------

Dashboard::Dashboard(const ui::Fonts & fonts) : fonts_(fonts) {
    // The message prompt in the dashboard's own colours, with Turkish letters
    // over the space bar.
    ui::Theme & t = prompt_.style.theme;
    t.page = kBase;
    t.surface = kSurface;
    t.surface_high = kRaised;
    t.text = t.page_text = kInk;
    t.text_muted = t.page_text_muted = kInk2;
    t.primary = t.accent = kBlue;
    t.on_primary = t.on_secondary = kInk;
    t.secondary = kRaised;
    t.outline = kHair;
    t.focus = kBlue;
    t.heading = ui::FontRole::semibold;
    t.radius = 14;
    t.radius_card = 24;
    prompt_.style.frosted = false;
    prompt_.style.width = 1100;
    prompt_.style.max_length = 400;
    prompt_.keyboard.style.bindings = ui::KeyboardBindings::standard();
    prompt_.keyboard.style.pitch_by_column = false;
    prompt_.keyboard.style.erase = Cue::type;
    ui::KeyboardLayout letters = ui::KeyboardLayout::letters();
    const char * tr[][2] = { { "\xC3\xA7", "\xC3\x87" }, { "\xC4\x9F", "\xC4\x9E" }, { "\xC4\xB1", "I" },
                             { "\xC4\xB0", "\xC4\xB0" }, { "\xC3\xB6", "\xC3\x96" }, { "\xC5\x9F", "\xC5\x9E" },
                             { "\xC3\xBC", "\xC3\x9C" } };
    std::vector<ui::KeyboardKey> row;
    for (const auto & k : tr) {
        row.push_back({ ui::KeyKind::character, k[0], k[1] });
    }
    letters.rows.insert(letters.rows.end() - 1, row);
    // PS5LM Lean: the keyboard types the Home chat's message; its Done key sends it.
    ui::KeyboardLayout symbols = ui::KeyboardLayout::symbols();
    for (ui::KeyboardLayout * layout : { &letters, &symbols }) {
        for (auto & keys : layout->rows) {
            for (auto & k : keys) {
                if (k.kind == ui::KeyKind::done) {
                    k.label = "Send";
                }
            }
        }
    }
    prompt_.keyboard.set_layouts({ letters, symbols });
}

void Dashboard::open_library() {
    page_from_ = page_;
    page_ = kModelsPage;
    page_age_ = 0;
    section_ = kLocal;
    in_list_ = true;
    local_cursor_ = 0;
    for (int i = 0; i < (int) live_.models.size(); ++i) {
        if (live_.models[(size_t) i].current) {
            local_cursor_ = i;
        }
    }
}

void Dashboard::show_downloads() {
    go_to(kModelsPage, nullptr);
    section_ = kDownloads;
    in_list_ = true;
}

bool Dashboard::wants_logs() const { return page_ == kSettingsPage && setting_category_ == kDiagnostics; }

void Dashboard::go_to(int page, ui::Feedback * feedback) {
    page_from_ = page_;
    page_ = page;
    page_age_ = 0;
    if (feedback) {
        feedback->play(Cue::tab);
    }
}

void Dashboard::set_logs(std::vector<std::string> app, std::vector<std::string> llama) {
    app_log_ = std::move(app);
    llama_log_ = std::move(llama);
    refilter_logs();
}

void Dashboard::refilter_logs() {
    const auto keep = [&](const std::string & line) { return !log_errors_ || log_level(line) > 0; };
    app_view_.clear();
    llama_view_.clear();
    std::copy_if(app_log_.begin(), app_log_.end(), std::back_inserter(app_view_), keep);
    std::copy_if(llama_log_.begin(), llama_log_.end(), std::back_inserter(llama_view_), keep);
    log_scroll_ = std::min(log_scroll_, std::max(0, (int) log_view().size() - kLogLines));
}

// ---- input ------------------------------------------------------------------------

void Dashboard::update(const hui::InputFrame & raw, float dt, ui::Feedback & feedback) {
    const hui::InputFrame none{};
    const bool dialog = market_confirm_ || market_space_ || picker_ || report_dialog_ || storage_dialog_;
    const hui::InputFrame & input = busy() ? none : raw;  // busy: the screen says so, buttons wait
    age_ += dt;
    clock_ += dt;
    page_age_ += dt;

    // The load's share moves on every frame, not once a second as it is measured.
    const float load = live_.state == ServerState::loading ? std::max(0.0f, live_.load_progress) : 0.0f;
    load_shown_ = load < load_shown_ ? load : load_shown_ + (load - load_shown_) * std::min(1.0f, dt * 2.5f);

    // Two sounds come from events, not buttons: a model ready, a download saved.
    const bool serving = live_.state == ServerState::ready || live_.state == ServerState::generating;
    if (serving && !was_serving_) {
        feedback.play(Cue::notify);
    }
    was_serving_ = serving;
    std::vector<std::string> unfinished;
    for (const auto & d : market_.downloads) {
        if (d.state != DownloadState::done) {
            unfinished.push_back(d.file);
        } else if (std::find(unfinished_.begin(), unfinished_.end(), d.file) != unfinished_.end()) {
            feedback.play(Cue::complete);
        }
    }
    unfinished_ = std::move(unfinished);

    // L1/R1 walk the dock, unless a question is open. A message being typed
    // takes every button until it is sent or dropped.
    const int step = input.is_pressed(Action::page_next) ? 1 : input.is_pressed(Action::page_prev) ? -1 : 0;
    if (screen_ask_ != ScreenAsk::none) {
        update_screen_off(input, dt, feedback);  // PS5LM Lean: the question, then the countdown
    } else if (storage_dialog_) {
        update_storage_dialog(input, feedback);
    } else if (prompt_.is_open()) {
        // PS5LM Lean: the keyboard edits the message box. Send sends it (or
        // holds it until the reply ends); Close keeps what was typed.
        const ui::Event e = prompt_.handle(input, feedback);
        if (e == ui::Event::activated || e == ui::Event::cancelled) {
            box_ = prompt_.text();
            box_spoken_ = box_spoken_ && !box_.empty();
            if (e == ui::Event::activated) {
                send_box(feedback);
            }
        }
    } else if (input.is_pressed(Action::menu) && !dialog) {
        ask_screen_off(ScreenAsk::off, "", feedback);  // PS5LM Lean: Options, from any page
    } else if (step != 0 && !dialog) {
        const int next = page_ + step;
        if (next >= 0 && next < kPages) {
            go_to(next, &feedback);
        } else {
            feedback.play(Cue::error);
        }
    } else if (page_ == kHomePage) {
        update_home(input, feedback);
    } else if (page_ == kModelsPage) {
        update_models(input, feedback);
    } else if (page_ == kConsolePage) {
        // L2 the last two minutes, R2 the last hour.
        const int to = input.is_pressed(Action::jump_next) ? 1 : input.is_pressed(Action::jump_prev) ? 0 : -1;
        if (to >= 0 && (to == 1) != console_hour_) {
            console_hour_ = to == 1;
            feedback.play(Cue::tab);
        } else if (to >= 0) {
            feedback.play(Cue::error);
        }
    } else if (page_ == kSettingsPage) {
        update_settings(input, feedback);
    }
    update_chat(input, dt, feedback);  // PS5LM Lean: the take, the transcript, a message that waits
    prompt_.update(dt);

    // Where the highlight goes: a section or a row on Models, a category or a
    // row on Settings.
    float target = kListY;
    if (page_ == kModelsPage) {
        int row = section_;
        if (in_list_) {
            const int cursor = section_ == kLocal ? local_cursor_ : section_ == kSuggested ? suggested_cursor_
                             : section_ == kDownloads ? dl_cursor_ : market_files_ ? market_file_ : market_repo_;
            const int count = section_ == kLocal ? (int) live_.models.size() : section_ == kSuggested ? kSuggestionCount
                            : section_ == kDownloads ? (int) market_.downloads.size()
                            : market_files_ && market_.open >= 0 && market_.open < (int) market_.repos.size()
                                ? (int) market_.repos[(size_t) market_.open].files.size() : (int) market_.repos.size();
            row = cursor - first_visible(cursor, count);
            target = kListY + (section_ == kSearch ? 72.0f : 0.0f) + (float) row * kRowPitch;
        } else {
            target = kListY + (float) row * kRailPitch;
        }
    } else if (page_ == kSettingsPage) {
        target = in_rows_ && setting_category_ != kDiagnostics ? kListY + (float) setting_row_ * kRowPitch
                                                              : kListY + (float) setting_category_ * kRailPitch;
    }
    if (age_ <= dt) {
        rows_y_.snap(target);
    }
    rows_y_.target = target;
    rows_y_.update(dt, 22.0f);
    dock_x_.target = (float) page_;
    dock_x_.update(dt, 18.0f);
}

// PS5LM Lean: Home is a chat with the loaded model. L2 held records, and
// letting go transcribes into the message box; Triangle types into the same
// box; R2 sends it. Nothing is sent by itself. Left and right switch the panel
// between the chat and the other devices' requests; up and down scroll the
// chat.
void Dashboard::update_home(const hui::InputFrame & input, ui::Feedback & feedback) {
    const bool decision = !live_.decision.empty();
    const MicState mic = chat_.mic;
    if (talking_) {
        return;  // L2 held: only letting go, or Circle, counts (update_chat)
    }
    if (devices_ && (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next) ||
                     input.is_pressed(Action::north) || input.is_pressed(Action::west))) {
        devices_ = false;  // the chat's buttons bring the chat back, and do what they do there
    }
    if (input.is_pressed(Action::confirm)) {
        open_library();
        feedback.play(Cue::select);
    } else if (input.nav == Direction::left || input.nav == Direction::right) {
        if (!input.nav_repeat && devices_ != (input.nav == Direction::right)) {
            devices_ = input.nav == Direction::right;
            feedback.play(Cue::tab);
        } else if (!input.nav_repeat) {
            feedback.play(Cue::error);
        }
    } else if (devices_) {
        // The other devices' requests: nothing more to do here.
    } else if (input.is_pressed(Action::jump_prev)) {
        // The talk button. A tap is too short to say anything.
        if (mic == MicState::off) {
            say("Talking is off", chat_.note.empty() ? std::string("There is no speech model on this PS5") : chat_.note);
            feedback.play(Cue::error);
        } else if (mic == MicState::loading || mic == MicState::recording || mic == MicState::transcribing) {
            feedback.play(Cue::error);  // the last take is still being written down
        } else if (!input.is_held(Action::jump_prev)) {
            say("Hold L2 while you speak", "Let go when you are done; the words land here to check before sending");
            feedback.play(Cue::error);
        } else {
            request_ = { Request::talk_start, {} };
            talking_ = true;
            levels_.clear();
            note_left_ = 0;
            chat_scroll_ = 0;
            feedback.play(Cue::select);
        }
    } else if (input.is_pressed(Action::jump_next)) {
        send_box(feedback);
    } else if (input.is_pressed(Action::back)) {
        if (send_waits_) {
            send_waits_ = false;  // not sent after all; the text stays
            feedback.play(Cue::back);
        } else if (!box_.empty()) {
            box_.clear();
            box_spoken_ = false;
            feedback.play(Cue::back);
        }
    } else if (input.nav == Direction::up || input.nav == Direction::down) {
        const float pitch = kChatPitch * 3;
        const Rect  view = chat_view();
        const float most = std::max(0.0f, chat_height(view.w) - view.h);
        const float next = std::clamp(chat_scroll_ + (input.nav == Direction::up ? pitch : -pitch), 0.0f, most);
        if (next != chat_scroll_) {
            chat_scroll_ = next;
            feedback.play(Cue::focus);
        } else if (!input.nav_repeat) {
            feedback.play(Cue::error);
        }
    } else if (input.is_pressed(Action::west)) {
        if (!chat_.turns.empty()) {
            // A new chat forgets this one: the second press within a few
            // seconds does it.
            if (new_chat_armed_ > 0) {
                request_ = { Request::chat_new, {} };
                new_chat_armed_ = 0;
                chat_scroll_ = 0;
                send_waits_ = false;
                note_left_ = 0;
                feedback.play(Cue::select);
            } else {
                new_chat_armed_ = 4;
                say("Press Square again to start a new chat", "The model forgets this conversation");
                feedback.play(Cue::focus);
            }
        } else if (box_.empty()) {
            box_ = (decision ? kDecisionDemos : kChatDemos)[demo_++ % kDemos];  // a question to try, sent with R2
            box_spoken_ = false;
            feedback.play(Cue::select);
        } else {
            feedback.play(Cue::error);
        }
    } else if (input.is_pressed(Action::north)) {
        prompt_.set_title(decision ? "Text to decide on" : "Message");
        prompt_.field.set_placeholder(decision ? "A customer message, a review, a ticket" : "Ask anything");
        prompt_.style.done_label = "Send";
        prompt_.style.cancel_label = "Close";
        prompt_.open(feedback, box_);
    }
}

// Every frame, whatever is on the screen: the take ends when L2 comes up (or
// is dropped when the screen moved away from the chat), a new transcript goes
// into the box once, and a message held while a reply came is sent.
void Dashboard::update_chat(const hui::InputFrame & input, float dt, ui::Feedback & feedback) {
    if (talking_) {
        const bool away = page_ != kHomePage || devices_ || prompt_.is_open() || screen_ask_ != ScreenAsk::none ||
                          storage_dialog_ || busy() || countdown_ >= 0;
        if (away || input.is_pressed(Action::back)) {
            talking_ = false;
            request_ = { Request::talk_cancel, {} };
            feedback.play(Cue::back);
        } else if (!input.is_held(Action::jump_prev)) {
            talking_ = false;
            request_ = { Request::talk_stop, {} };
        }
    }
    // The meter: the take's level twenty times a second.
    if (chat_.mic == MicState::recording) {
        level_clock_ += dt;
        if (level_clock_ >= 0.05f || levels_.empty()) {
            level_clock_ = 0;
            levels_.push_back(chat_.level);
            if (levels_.size() > (size_t) kMeterBars) {
                levels_.erase(levels_.begin());
            }
        }
    }
    if (chat_.transcript_seq != taken_seq_) {
        taken_seq_ = chat_.transcript_seq;
        if (!chat_.transcript.empty()) {
            // Spoken again before sending: the new words go after the old.
            box_ += (box_.empty() ? "" : " ") + chat_.transcript;
            box_spoken_ = true;
            feedback.play(Cue::notify);
        }
    }
    if (chat_.mic != mic_seen_) {
        switch (chat_.mic) {
            case MicState::muted:
                say("Microphone muted",
                    chat_.note.empty() ? std::string("Nothing was recorded: the controller's mute button is on (its light is "
                                                     "orange). Press it and talk again")
                                       : chat_.note, false, 10);
                feedback.play(Cue::error);
                break;
            case MicState::silent:
                say("Nothing heard", chat_.note.empty() ? std::string("Hold L2 the whole time you speak") : chat_.note, false, 10);
                feedback.play(Cue::error);
                break;
            case MicState::failed:
                say("Could not write it down", chat_.note.empty() ? std::string("Try again") : chat_.note, true, 12);
                feedback.play(Cue::error);
                break;
            default: break;
        }
        mic_seen_ = chat_.mic;
    }
    note_left_ = std::max(0.0f, note_left_ - dt);
    new_chat_armed_ = std::max(0.0f, new_chat_armed_ - dt);
    if (send_waits_ && !chat_.answering && request_.kind == Request::none) {
        send_waits_ = false;
        send_box(feedback);
    }
}

// R2, or Send on the keyboard: the box goes to the model, unless a reply is
// still coming (then it waits for it) or nothing could answer.
void Dashboard::send_box(ui::Feedback & feedback) {
    const bool serving = live_.state == ServerState::ready || live_.state == ServerState::generating;
    if (box_.empty()) {
        say("Nothing to send yet", "Hold L2 to talk, or press Triangle to type");
        feedback.play(Cue::error);
    } else if (!serving) {
        say(live_.state == ServerState::loading ? "The model is still loading" : "No model is loaded",
            live_.state == ServerState::loading ? "Send it when the model is ready; it stays here"
                                                : "Cross chooses one; your message stays here");
        feedback.play(Cue::error);
    } else if (chat_.answering) {
        send_waits_ = true;  // sent when the reply ends; Circle takes it back
        feedback.play(Cue::select);
    } else {
        request_ = { Request::ask, box_ };
        request_.repo = box_spoken_ ? 1 : 0;  // for the engine: some of it was spoken
        box_.clear();
        box_spoken_ = false;
        send_waits_ = false;
        chat_scroll_ = 0;
        note_left_ = 0;
        feedback.play(Cue::select);
    }
}

void Dashboard::say(std::string head, std::string detail, bool fault, float seconds) {
    // The engine's note may start with what the heading says ("Nothing
    // heard. Hold the button ..."): said once.
    if (detail.size() > head.size() + 1 && detail.compare(0, head.size(), head) == 0 &&
        (detail[head.size()] == '.' || detail[head.size()] == ':')) {
        detail.erase(0, detail.find_first_not_of(" .:", head.size()));
    }
    note_head_ = std::move(head);
    note_detail_ = std::move(detail);
    note_fault_ = fault;
    note_left_ = seconds;
}

void Dashboard::update_models(const hui::InputFrame & input, ui::Feedback & feedback) {
    if (market_confirm_ || market_space_) {
        update_market(input, feedback);
        return;
    }
    if (!in_list_) {
        if (input.nav == Direction::up || input.nav == Direction::down) {
            const int next = section_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < kSections) {
                section_ = next;
                feedback.play(Cue::focus);
                if (section_ == kSearch && !market_searched_) {
                    market_searched_ = true;
                    request_ = { Request::market_search, "" };
                }
            } else if (!input.nav_repeat) {
                feedback.play(Cue::error);
            }
        }
        if (input.nav == Direction::right || input.is_pressed(Action::confirm)) {
            in_list_ = true;
            feedback.play(Cue::select);
        }
        return;
    }
    // Left from a list's top level, or Circle there, goes back to the sections.
    const bool top = !(section_ == kSearch && market_files_);
    if (top && (input.nav == Direction::left || input.is_pressed(Action::back))) {
        in_list_ = false;
        dl_confirm_ = -1;
        feedback.play(Cue::back);
        return;
    }
    switch (section_) {
        case kLocal: update_local(input, feedback); break;
        case kSuggested: update_suggested(input, feedback); break;
        case kSearch: update_market(input, feedback); break;
        default: update_downloads(input, feedback); break;
    }
}

namespace {

// Up and down through a list: true when the cursor moved.
bool walk(const hui::InputFrame & input, int & cursor, int count, ui::Feedback & feedback) {
    if (input.nav != Direction::up && input.nav != Direction::down) {
        return false;
    }
    const int next = cursor + (input.nav == Direction::down ? 1 : -1);
    if (next >= 0 && next < count) {
        cursor = next;
        feedback.play(Cue::focus);
        return true;
    }
    if (!input.nav_repeat) {
        feedback.play(Cue::error);
    }
    return false;
}

}  // namespace

void Dashboard::update_local(const hui::InputFrame & input, ui::Feedback & feedback) {
    const int rows = (int) live_.models.size();
    local_cursor_ = std::clamp(local_cursor_, 0, std::max(0, rows - 1));
    walk(input, local_cursor_, rows, feedback);
    if (input.is_pressed(Action::confirm) && rows > 0) {
        const ModelRow & m = live_.models[(size_t) local_cursor_];
        if (m.current) {
            request_ = { Request::unload, m.file };
            feedback.play(Cue::select);
        } else if (m.fits && m.images_screen_off) {
            ask_screen_off(ScreenAsk::load_images, m.file, feedback);  // PS5LM Lean: images, or text only
        } else if (m.fits) {
            request_ = { Request::load, m.file };
            feedback.play(Cue::launch);
        } else if (m.fits_screen_off) {
            ask_screen_off(ScreenAsk::load, m.file, feedback);  // PS5LM Lean
        } else {
            feedback.play(Cue::error);
        }
    }
}

void Dashboard::update_suggested(const hui::InputFrame & input, ui::Feedback & feedback) {
    walk(input, suggested_cursor_, kSuggestionCount, feedback);
    if (input.is_pressed(Action::confirm)) {
        const Suggestion & s = kSuggestions[suggested_cursor_];
        bool here = false;
        for (const auto & m : live_.models) {
            here = here || m.label + ".gguf" == local_name(s.model);
        }
        if (here) {
            feedback.play(Cue::error);
        } else {
            request_ = { Request::suggested_download, settings_.download_dir, suggested_cursor_ };
            feedback.play(Cue::select);
        }
    }
}

// After the space check: ask about a loaded model first, or start.
void Dashboard::start_download(ui::Feedback & feedback) {
    if (!live_.model_label.empty() && live_.free_gib < 2.0) {
        market_confirm_ = true;  // a model holds the memory: ask first
        feedback.play(Cue::modal_open);
    } else {
        request_ = { Request::market_download, market_dir_, market_.open, market_file_ };
        feedback.play(Cue::select);
    }
}

void Dashboard::update_market(const hui::InputFrame & input, ui::Feedback & feedback) {
    // The download does not fit where it would go.
    if (market_space_) {
        if (input.is_pressed(Action::confirm) && !market_dir_.empty()) {
            market_space_ = false;
            start_download(feedback);
        } else if (input.is_pressed(Action::back)) {
            market_space_ = false;
            feedback.play(Cue::modal_close);
        }
        return;
    }
    // The question before a download while a model holds the memory.
    if (market_confirm_) {
        if (input.is_pressed(Action::north)) {
            request_ = { Request::market_download_unload, market_dir_, market_.open, market_file_ };
            market_confirm_ = false;
            feedback.play(Cue::select);
        } else if (input.is_pressed(Action::confirm)) {
            request_ = { Request::market_download, market_dir_, market_.open, market_file_ };
            market_confirm_ = false;
            feedback.play(Cue::select);
        } else if (input.is_pressed(Action::back)) {
            market_confirm_ = false;
            feedback.play(Cue::modal_close);
        }
        return;
    }
    const int chip = input.is_pressed(Action::jump_next) ? 1 : input.is_pressed(Action::jump_prev) ? -1 : 0;
    if (chip != 0) {
        const int next = market_chip_ + chip;
        if (next >= 0 && next < kMarketChipCount) {
            market_chip_ = next;
            market_repo_ = 0;
            market_files_ = false;
            request_ = { Request::market_search, next == 0 ? std::string() : std::string(kMarketChips[next]) };
            feedback.play(Cue::tab);
        } else {
            feedback.play(Cue::error);
        }
        return;
    }
    const int repos = (int) market_.repos.size();
    const MarketRepo * open = market_.open >= 0 && market_.open < repos ? &market_.repos[(size_t) market_.open] : nullptr;
    const int files = open ? (int) open->files.size() : 0;
    int & cursor = market_files_ ? market_file_ : market_repo_;
    if (walk(input, cursor, market_files_ ? files : repos, feedback) && market_files_) {
        request_ = { Request::market_plan, "", market_.open, cursor };
    }
    if (input.is_pressed(Action::confirm)) {
        if (!market_files_ && cursor < repos) {
            request_ = { Request::market_open, "", cursor, -1 };
            market_files_ = true;
            market_file_ = 0;
            feedback.play(Cue::select);
        } else if (market_files_ && open && cursor < files) {
            const MarketFile & f = open->files[(size_t) cursor];
            const double need = f.bytes / 1073741824.0 + 1.0;  // and 1 GiB spare, as Market::download wants
            const Drive * to = nullptr;
            const Drive * other = nullptr;  // the roomiest other drive it fits on
            for (const auto & d : live_.drives) {
                if (d.models == settings_.download_dir) {
                    to = &d;
                } else if (d.free >= need && (!other || d.free > other->free)) {
                    other = &d;
                }
            }
            market_dir_.clear();
            if (f.fits == 0 && !f.fits_screen_off) {
                feedback.play(Cue::error);  // too big, even with the screen off
            } else if (!to || (to->total > 0 && to->free < need)) {  // unknown space (no exporter) does not block
                market_space_ = true;
                market_dir_ = other ? other->models : "";
                feedback.play(Cue::modal_open);
            } else {
                start_download(feedback);
            }
        }
    }
    // Square: read this file's header now and say whether it fits.
    if (input.is_pressed(Action::west) && market_files_ && open && cursor < files) {
        const MarketFile & f = open->files[(size_t) cursor];
        if (f.fits == -1 || f.fits == -2) {
            request_ = { Request::market_plan, "", market_.open, cursor };
            feedback.play(Cue::select);
        } else {
            feedback.play(Cue::error);  // checked already
        }
    }
    if (input.is_pressed(Action::back) && market_files_) {
        market_files_ = false;
        feedback.play(Cue::back);
    }
    // The first file of a freshly opened repository gets planned too.
    if (request_.kind == Request::none && market_files_ && files > 0 && market_file_ == 0 && open->files[0].fits == -1 &&
        !open->files[0].checking && !market_.busy) {
        request_ = { Request::market_plan, "", market_.open, 0 };
    }
}

void Dashboard::update_downloads(const hui::InputFrame & input, ui::Feedback & feedback) {
    const int rows = (int) market_.downloads.size();
    dl_cursor_ = std::clamp(dl_cursor_, 0, std::max(0, rows - 1));
    if (walk(input, dl_cursor_, rows, feedback)) {
        dl_confirm_ = -1;
    }
    if (rows == 0) {
        return;
    }
    const Download & d = market_.downloads[(size_t) dl_cursor_];
    if (input.is_pressed(Action::confirm)) {
        if (d.state == DownloadState::running || d.state == DownloadState::retrying || d.state == DownloadState::queued) {
            request_ = { Request::download_pause, "", dl_cursor_ };
            feedback.play(Cue::select);
        } else if (d.state == DownloadState::paused || d.state == DownloadState::failed) {
            request_ = { Request::download_resume, "", dl_cursor_ };
            feedback.play(Cue::select);
        } else {
            feedback.play(Cue::error);
        }
    }
    // Square: a finished one leaves the list; any other is cancelled and its
    // partial file deleted, after a second press.
    if (input.is_pressed(Action::west)) {
        if (d.state == DownloadState::done || dl_confirm_ == dl_cursor_) {
            request_ = { Request::download_remove, "", dl_cursor_ };
            dl_confirm_ = -1;
            feedback.play(Cue::select);
        } else {
            dl_confirm_ = dl_cursor_;
            feedback.play(Cue::modal_open);
        }
    }
}

void Dashboard::update_settings(const hui::InputFrame & input, ui::Feedback & feedback) {
    if (picker_) {
        update_picker(input, feedback);
        return;
    }
    // What a debug report holds, before the first one.
    if (report_dialog_) {
        if (input.is_pressed(Action::confirm)) {
            report_dialog_ = false;
            request_ = { Request::debug_report, "" };
            feedback.play(Cue::select);
        } else if (input.is_pressed(Action::back)) {
            report_dialog_ = false;
            feedback.play(Cue::modal_close);
        }
        return;
    }
    if (!in_rows_) {
        if (input.nav == Direction::up || input.nav == Direction::down) {
            const int next = setting_category_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < kCategoryCount) {
                setting_category_ = next;
                setting_row_ = 0;
                log_scroll_ = 0;
                feedback.play(Cue::focus);
            } else if (!input.nav_repeat) {
                feedback.play(Cue::error);
            }
        }
        if (input.nav == Direction::right || input.is_pressed(Action::confirm)) {
            in_rows_ = true;
            feedback.play(Cue::select);
        }
        return;
    }
    if (input.is_pressed(Action::back) || input.nav == Direction::left) {
        in_rows_ = false;
        feedback.play(Cue::back);
        return;
    }
    if (setting_category_ == kDiagnostics) {
        update_logs(input, feedback);
        return;
    }
    const SettingCategory & c = kCategories[setting_category_];
    walk(input, setting_row_, c.count, feedback);
    const Setting id = c.rows[setting_row_].id;
    if (!input.is_pressed(Action::confirm)) {
        return;
    }
    // Cross does what the row is: clears, flips a switch, or opens its choices.
    if (id == Setting::screen_off || id == Setting::screen_reload) {
        ask_screen_off(id == Setting::screen_off ? ScreenAsk::off : ScreenAsk::off_reload, "", feedback);
    } else if (id == Setting::clear_scratch) {
        request_ = { Request::clear_scratch, "" };
        feedback.play(Cue::select);
    } else if (id == Setting::storage) {
        if (full_storage_) {
            feedback.play(Cue::error);  // nothing to choose
        } else {
            storage_dialog_ = true;
            storage_howto_ = false;
            feedback.play(Cue::modal_open);
        }
    } else if (id == Setting::debug_report) {
        if (report_busy_) {
            feedback.play(Cue::error);  // one is being written
        } else if (!settings_.report_explained) {
            report_dialog_ = true;
            feedback.play(Cue::modal_open);
        } else {
            request_ = { Request::debug_report, "" };
            feedback.play(Cue::select);
        }
    } else if (is_toggle(id)) {
        bool & on = toggle_ref(settings_, id);
        on = !on;
        request_ = { Request::settings, "" };
        feedback.play(Cue::toggle);
    } else {
        const auto all = choices_of(settings_, id, live_.models, live_.drives);
        picker_ = true;
        picker_cursor_ = 0;
        for (int i = 0; i < (int) all.size(); ++i) {
            picker_cursor_ = all[(size_t) i].current ? i : picker_cursor_;
        }
        feedback.play(Cue::modal_open);
    }
}

void Dashboard::update_picker(const hui::InputFrame & input, ui::Feedback & feedback) {
    const auto all = choices_of(settings_, kCategories[setting_category_].rows[setting_row_].id, live_.models, live_.drives);
    picker_cursor_ = std::clamp(picker_cursor_, 0, std::max(0, (int) all.size() - 1));
    walk(input, picker_cursor_, (int) all.size(), feedback);
    if (input.is_pressed(Action::confirm) && !all.empty()) {
        settings_ = all[(size_t) picker_cursor_].value;
        request_ = { Request::settings, "" };
        picker_ = false;
        feedback.play(Cue::select);
    } else if (input.is_pressed(Action::back)) {
        picker_ = false;
        feedback.play(Cue::modal_close);
    }
}

void Dashboard::update_logs(const hui::InputFrame & input, ui::Feedback & feedback) {
    const int most = std::max(0, (int) log_view().size() - kLogLines);
    if (input.nav == Direction::up || input.nav == Direction::down) {
        const int next = std::clamp(log_scroll_ + (input.nav == Direction::up ? 3 : -3), 0, most);
        if (next != log_scroll_) {
            log_scroll_ = next;
            feedback.play(Cue::focus);
        } else if (!input.nav_repeat) {
            feedback.play(Cue::error);
        }
    }
    if (input.is_pressed(Action::west)) {
        log_llama_ = !log_llama_;
        log_scroll_ = 0;
        feedback.play(Cue::tab);
    }
    if (input.is_pressed(Action::north)) {
        log_errors_ = !log_errors_;
        refilter_logs();
        log_scroll_ = 0;
        feedback.play(Cue::toggle);
    }
    if (input.is_pressed(Action::confirm)) {
        log_scroll_ = 0;  // back to the newest
        feedback.play(Cue::select);
    }
}

// ---- drawing: the frame ---------------------------------------------------------------

void Dashboard::draw(DashboardFrame & frame) const {
    frame.backdrop.mode = gfx::BackdropMode::gradient;
    frame.backdrop.colors[0] = Color::rgb(0x151618);
    frame.backdrop.colors[1] = kBase;
    frame.backdrop.colors[2] = kBase;
    frame.backdrop.time = clock_;

    gfx::DrawList & scene = frame.scene;
    // A page fades in and settles from a little below, the way a card rises
    // out of a car's dock.
    const float in = tween::cubic_out(tween::clamp01(page_age_ / 0.32f));
    scene.push_transform(1.0f, 960, 540, 0, 18.0f * (1.0f - in));
    scene.push_opacity(in);
    switch (page_) {
        case kHomePage: draw_home(scene); break;
        case kModelsPage: draw_models(scene); break;
        case kApiPage: draw_api(scene); break;
        case kConsolePage: draw_console(scene); break;
        default: draw_settings(scene); break;
    }
    scene.pop_opacity();
    scene.pop_transform();
    draw_status(scene);
    draw_dock(scene);
    draw_hints(scene);
    draw_dialogs(frame.overlay);
    if (picker_ && page_ == kSettingsPage) {
        draw_picker(frame.overlay);
    }
    if (prompt_.visible()) {
        ui::Canvas canvas{ frame.overlay, fonts_, 0, clock_ };
        prompt_.draw(canvas);
    }
    if (countdown_ >= 0) {
        draw_countdown(frame.overlay);
    }
    if (busy()) {
        draw_busy(frame.overlay);
    }
}

void Dashboard::draw_status(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const float y = 72;
    float w = put(l, f.semibold, "PS5", kLeft, y, 28, kInk);
    w += put(l, f.semibold, "LM", kLeft + w, y, 28, kBlue);
    put(l, f.semibold, " Voice", kLeft + w, y, 28, kInk);  // PS5LM Voice (lean.24)
    // Right to left: how long it has been up, power, temperature. PS5LM Lean:
    // Home shows the last two with the other readings.
    std::vector<std::string> items;
    const bool readings = page_ != kHomePage;
    if (readings && live_.soc_temp > -100) {
        items.push_back(format("%.0f\xC2\xB0" "C", live_.soc_temp));
    }
    if (readings && live_.soc_power_w > 0) {
        items.push_back(format("%.0f W", live_.soc_power_w));
    }
    if (live_.uptime_s > 0) {
        const double m = live_.uptime_s / 60.0;
        items.push_back(m < 60 ? format("Up %.0f min", std::floor(m)) : format("Up %.1f h", m / 60.0));
    }
    float x = kRight;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        x -= put(l, f.regular, *it, x, y, 22, kInk2, gfx::Align::right) + 40;
    }
    put(l, f.regular, "v" PS5LM_VERSION, x, y, 22, kInk3, gfx::Align::right);
}

void Dashboard::draw_dock(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    l.line(kLeft, kDockY, kRight, kDockY, 1.5f, kHair);
    constexpr float pitch = 168;
    for (int i = 0; i < kPages; ++i) {
        const float cx = kLeft + 44 + pitch * (float) i;
        const float lit = tween::clamp01(1.0f - std::fabs(dock_x_.value - (float) i));
        const Color c = gfx::mix(kInk3, kInk, lit);
        icon(l, i, cx, 1004, c);
        put(l, f.regular, kPageNames[i], cx, 1056, 20, c, gfx::Align::center);
    }
    // The current tab's marker slides between them.
    const float mx = kLeft + 44 + pitch * dock_x_.value;
    l.rounded_rect({ mx - 18, 1068, 36, 4 }, 2, kBlue);
}

void Dashboard::draw_hints(gfx::DrawList & l) const {
    ui::GlyphStyle style = ui::GlyphStyle::dark();
    style.label = kInk2;
    ui::HintLayout layout;
    layout.size = 32;
    layout.text_size = 20;
    layout.cy = 1018;
    layout.item_gap = 36;
    std::vector<ui::Hint> h;
    if (market_confirm_ || market_space_ || report_dialog_ || storage_dialog_ || prompt_.visible() ||
        screen_ask_ != ScreenAsk::none) {
        return;  // the dialog has its own
    }
    if (picker_ && page_ == kSettingsPage) {
        h = { { ui::Button::circle, "Cancel" }, { ui::Button::cross, "Choose" } };
    } else if (page_ == kHomePage) {
        // PS5LM Lean: the chat's buttons for what the box holds; the screen's
        // memory, offered where the memory is shown.
        const MicState mic = chat_.mic;
        const bool can_talk = mic != MicState::off && mic != MicState::loading && mic != MicState::recording &&
                              mic != MicState::transcribing;
        const char * model = live_.model_label.empty() ? "Choose a model" : "Change model";
        if (devices_) {
            h = { { ui::Button::options, "Screen off" }, { ui::Button::l1, "Pages", ui::Button::r1 },
                  { ui::Button::dpad, "Chat" }, { ui::Button::cross, model } };
        } else if (talking_ || mic == MicState::loading || mic == MicState::recording) {
            h = { { ui::Button::l2, "Let go to finish" }, { ui::Button::circle, "Drop it" } };
        } else if (!box_.empty()) {
            if (can_talk) {
                h.push_back({ ui::Button::l2, "Talk more" });
            }
            h.push_back({ ui::Button::triangle, "Edit" });
            h.push_back({ ui::Button::circle, send_waits_ ? "Don't send" : "Clear" });
            if (!send_waits_) {
                h.push_back({ ui::Button::r2, "Send" });
            }
            h.push_back({ ui::Button::cross, model });
        } else {
            // No room for L1/R1 here: the dock shows the pages.
            h = { { ui::Button::options, "Screen off" } };
            if (can_talk) {
                h.push_back({ ui::Button::l2, "Hold to talk" });
            }
            h.push_back({ ui::Button::triangle, "Type" });
            h.push_back({ ui::Button::square, !chat_.turns.empty() ? (new_chat_armed_ > 0 ? "Again: new chat" : "New chat")
                                                                   : "Example" });
            h.push_back({ ui::Button::cross, model });
        }
    } else if (page_ == kConsolePage) {
        h = { { ui::Button::l1, "Pages", ui::Button::r1 }, { ui::Button::l2, "Range", ui::Button::r2 } };
    } else if (page_ == kModelsPage) {
        if (!in_list_) {
            h = { { ui::Button::dpad, "Choose" }, { ui::Button::cross, "Open" } };
        } else if (section_ == kLocal) {
            const ModelRow * m = local_cursor_ < (int) live_.models.size() ? &live_.models[(size_t) local_cursor_] : nullptr;
            h = { { ui::Button::circle, "Back" } };
            if (m && !m->current && m->fits && m->images_screen_off) {
                h.push_back({ ui::Button::cross, "Load" });  // asks: images with the screen off, or text only
            } else if (m && (m->current || m->fits)) {
                h.push_back({ ui::Button::cross, m->current ? "Unload" : "Load" });
            } else if (m && m->fits_screen_off) {
                h.push_back({ ui::Button::cross, "Load, screen off" });
            }
        } else if (section_ == kSuggested) {
            h = { { ui::Button::circle, "Back" }, { ui::Button::cross, "Download" } };
        } else if (section_ == kSearch) {
            h = market_files_ ? std::vector<ui::Hint>{ { ui::Button::circle, "Back" }, { ui::Button::square, "Check fit" },
                                                       { ui::Button::cross, "Download" } }
                              : std::vector<ui::Hint>{ { ui::Button::l2, "Search", ui::Button::r2 }, { ui::Button::circle, "Back" },
                                                       { ui::Button::cross, "Open" } };
        } else {
            const Download * d = dl_cursor_ < (int) market_.downloads.size() ? &market_.downloads[(size_t) dl_cursor_] : nullptr;
            h = { { ui::Button::circle, "Back" } };
            if (d) {
                const bool runs = d->state == DownloadState::running || d->state == DownloadState::retrying ||
                                  d->state == DownloadState::queued;
                h.push_back({ ui::Button::square, d->state == DownloadState::done ? "Clear" : dl_confirm_ == dl_cursor_ ? "Yes, delete it" : "Cancel" });
                if (d->state != DownloadState::done) {
                    h.push_back({ ui::Button::cross, runs ? "Pause" : d->state == DownloadState::failed ? "Try again" : "Resume" });
                }
            }
        }
    } else if (page_ == kApiPage) {
        h = { { ui::Button::l1, "Pages", ui::Button::r1 } };
    } else if (!in_rows_) {
        h = { { ui::Button::dpad, "Choose" }, { ui::Button::cross, "Open" } };
    } else if (setting_category_ == kDiagnostics) {
        h = { { ui::Button::dpad, "Scroll" }, { ui::Button::square, log_llama_ ? "App log" : "Server log" },
              { ui::Button::triangle, log_errors_ ? "All lines" : "Errors only" } };
    } else {
        const Setting id = kCategories[setting_category_].rows[setting_row_].id;
        if (id == Setting::screen_off || id == Setting::screen_reload) {
            h = { { ui::Button::circle, "Back" }, { ui::Button::cross, "Turn off" } };
        } else if (id == Setting::clear_scratch) {
            h = { { ui::Button::circle, "Back" }, { ui::Button::cross, "Clear" } };
        } else if (id == Setting::debug_report) {
            h = { { ui::Button::circle, "Back" }, { ui::Button::cross, report_busy_ ? "Writing it" : "Create" } };
        } else if (id == Setting::storage) {
            h = { { ui::Button::circle, "Back" } };
            if (!full_storage_) {
                h.push_back({ ui::Button::cross, "Options" });
            }
        } else if (is_toggle(id)) {
            h = { { ui::Button::circle, "Back" }, { ui::Button::cross, "Switch" } };
        } else {
            h = { { ui::Button::circle, "Back" }, { ui::Button::cross, "Change" } };
        }
    }
    ui::draw_hints(l, fonts_, style, h.data(), (int) h.size(), kRight, true, layout);
}

// ---- drawing: Home -------------------------------------------------------------------

namespace {

// A console reading on Home: its value now and its last two minutes.
struct Vital {
    const char *               name;
    std::string                value;   // empty: not read yet
    const std::vector<float> * series;  // the last two minutes, or null
    bool                       share;   // the series runs from 0 to 1
    float                      bar;     // 0..1: a bar instead of a line (the fan keeps no history), else < 0
};

std::vector<Vital> vitals_of(const Live & L) {
    const auto degrees = [](float t) { return format("%.0f\xC2\xB0" "C", t); };
    return {
        { "Temperature", L.soc_temp > -100 ? degrees(L.soc_temp) : std::string(), &L.temp_history, false, -1 },
        { "Power", L.soc_power_w > 0 ? format("%.0f W", L.soc_power_w) : std::string(), &L.power_history, false, -1 },
        { "GPU busy", format("%.0f%%", 100.0 * L.gpu_busy), &L.gpu_history, true, -1 },
        { "CPU use", format("%.0f%%", 100.0 * L.cpu_use), &L.cpu_history, true, -1 },
        { "Fan", L.fan >= 0 ? format("%.0f%%", 100.0 * L.fan) : std::string(), nullptr, true, L.fan },
        { "Energy since start", L.soc_power_w > 0 || L.energy_wh > 0 ? format("%.1f Wh", L.energy_wh) : std::string(), nullptr,
          false, -1 },
    };
}

// A small trend line: the last two minutes in 40 points of 3 seconds, the
// newest at the right edge and dotted, scaled to what it saw (a share from 0
// to 100%). A history shorter than two minutes fills only the right part.
void spark(gfx::DrawList & l, const std::vector<float> & v, const Rect & g, bool share) {
    constexpr int    kPoints = 40;
    constexpr size_t kGroup = 3;
    float pts[kPoints];  // newest first
    int   n = 0;
    for (size_t end = v.size(); end > 0 && n < kPoints;) {
        const size_t begin = end > kGroup ? end - kGroup : 0;
        float sum = 0;
        for (size_t i = begin; i < end; ++i) {
            sum += v[i];
        }
        pts[n++] = sum / (float) (end - begin);
        end = begin;
    }
    l.line(g.x, g.y + g.h, g.x + g.w, g.y + g.h, 1.0f, kHair);
    if (n < 2) {
        return;
    }
    float lo = pts[0], hi = pts[0];
    for (int i = 1; i < n; ++i) {
        lo = std::min(lo, pts[i]);
        hi = std::max(hi, pts[i]);
    }
    if (share) {
        lo = 0;
        hi = 1;
    } else {
        const float pad = std::max(2.0f, (hi - lo) * 0.2f);
        lo -= pad;
        hi += pad;
    }
    const float step = g.w / (float) (kPoints - 1);
    const auto at = [&](float x) { return g.y + g.h - tween::clamp01((x - lo) / (hi - lo)) * g.h; };
    for (int i = 1; i < n; ++i) {
        l.line(g.x + g.w - step * (float) (i - 1), at(pts[i - 1]), g.x + g.w - step * (float) i, at(pts[i]), 2.0f,
               kInk.with_alpha(0.8f));
    }
    l.circle(g.x + g.w, at(pts[0]), 4, kInk);
}

// One reading on a row: its name, its trend line (the fan's duty as a bar),
// its value now.
void vital_row(gfx::DrawList & l, const ui::Fonts & f, const Vital & v, float x, float w, float y) {
    put(l, f.regular, v.name, x, y, 22, kInk2);
    if (v.value.empty()) {
        put(l, f.regular, "Not read yet", x + w, y, 20, kInk3, gfx::Align::right);
        return;
    }
    put(l, f.regular, v.value, x + w, y, 24, kInk, gfx::Align::right);
    const Rect g{ x + 160, y - 24, w - 160 - 100, 28 };
    if (v.bar >= 0) {
        const float by = g.y + g.h - 8;
        l.rounded_rect({ g.x, by, g.w, 6 }, 3, kHair);
        l.rounded_rect({ g.x, by, std::max(6.0f, g.w * tween::clamp01(v.bar)), 6 }, 3, kInk.with_alpha(0.8f));
    } else if (v.series) {
        spark(l, *v.series, g, v.share);
    }
}

// The latest requests the feed keeps (six), in two numbers: how many failed,
// and the typical time of the rest (the median), < 0 before any finished.
struct RequestSummary {
    int    failed = 0;
    double typical = -1;
};

RequestSummary summary_of(const Live & L) {
    RequestSummary s;
    std::vector<double> t;
    for (const auto & e : L.activity) {
        if (is_warm_up(e) || e.status == 0) {
            continue;
        }
        if (e.status >= 400) {
            ++s.failed;
        } else {
            t.push_back(e.seconds);
        }
    }
    if (!t.empty()) {
        std::sort(t.begin(), t.end());
        const size_t m = t.size() / 2;
        s.typical = t.size() % 2 ? t[m] : 0.5 * (t[m - 1] + t[m]);
    }
    return s;
}

// The model's one large number: its speed while it serves (the last reply's
// when idle), a decision model's time per decision, the load's progress.
struct Hero {
    float       value = 0;
    std::string unit;   // "tokens per second"
    std::string note;   // "last reply", or how much of the load is done
    bool        known = true;
};

Hero hero_of(const Live & L, float load_shown) {
    Hero h;
    if (L.state == ServerState::loading) {
        h.value = load_shown * 100.0f;
        h.unit = "% on the GPU";
        const double total = L.model_gib + L.kv_gib;
        if (total > 0) {
            h.note = format("%.1f", load_shown * total) + " of " + gb(total);
        }
    } else if (!L.decision.empty()) {
        // Time per decision (one question answered) over the latest requests:
        // what a decision model is judged by, as Ollaya reports it.
        double seconds = 0;
        int decisions = 0;
        for (const auto & e : L.activity) {
            if (e.status == 200 && e.questions > 0) {
                seconds += e.seconds;
                decisions += e.questions;
            }
        }
        h.value = decisions ? (float) (1000.0 * seconds / decisions) : 0;
        h.unit = "ms per decision";
        h.known = decisions > 0;
    } else {
        h.value = L.gen_tps;
        h.unit = "tokens per second";
        h.known = L.gen_tps > 0;
        if (!h.known) {
            // Idle: the last reply's speed, quieter.
            for (auto it = L.gen_history.rbegin(); it != L.gen_history.rend(); ++it) {
                if (*it > 0) {
                    h.value = *it;
                    h.note = "last reply";
                    h.known = true;
                    break;
                }
            }
        }
    }
    return h;
}

}  // namespace

void Dashboard::draw_home(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const bool loaded = !live_.model_label.empty() && live_.state != ServerState::no_model;
    const bool decision = !live_.decision.empty();
    const float x = kLeft, w = kHomeRailW;  // the rail
    const float hero_y = 416;               // the large number's baseline
    const Rect  tr{ x, 438, w, 44 };        // the speed over two minutes, or the load's cells

    // Left, the rail: the model.
    if (!loaded) {
        put(l, f.regular, "Nothing is loaded", x, 164, 22, kInk2);
        put(l, f.regular, "Choose a model", x, 216, 44, kInk);
        float y = 254;
        for (const auto & s : wrap(f.regular, "It runs on the PS5's GPU and answers every device on your network.", 20, w)) {
            put(l, f.regular, s, x, y, 20, kInk2);
            y += 28;
        }
        put(l, f.regular, format("%.0f models on this PS5", (double) live_.models.size()), x, y + 6, 20, kInk3);
        if (!live_.address.empty()) {
            put(l, f.regular, "On your network", x, hero_y - 40, 20, kInk3);
            put(l, f.regular, "http://" + live_.address, x, hero_y, 28, kInk2);
        }
    } else {
        std::string name, build;
        split_name(live_.model_label, &name, &build);
        put(l, f.regular, !live_.phase.empty() ? "Starting" : decision ? "Decision model" : "Language model", x, 164, 22, kInk2);
        put(l, f.regular, f.regular.font->fit(name, 44, w), x, 216, 44, kInk);
        std::string detail = build;
        const auto add = [&](const std::string & s) { detail += (detail.empty() ? "" : "   ") + s; };
        if (live_.ctx > 0) {
            add(format("%.0fk context", live_.ctx / 1024.0));
        }
        if (live_.model_gib > 0) {
            add(gb(live_.model_gib));
        }
        if (std::find(live_.args.begin(), live_.args.end(), "--mmproj") != live_.args.end()) {
            add("reads images");
        }
        put(l, f.regular, f.regular.font->fit(detail, 20, w), x, 250, 20, kInk3);

        // The state, as a car shows its gear: one word, coloured only when it matters.
        const char * word = live_.state == ServerState::loading ? "Loading"
                          : live_.state == ServerState::generating ? (decision ? "Deciding" : "Generating")
                          : live_.state == ServerState::failed ? "Stopped" : "Ready";
        const Color dot = live_.state == ServerState::failed ? kRed
                        : live_.state == ServerState::generating ? kBlue
                        : live_.state == ServerState::loading ? kInk2.with_alpha(0.5f + 0.5f * std::sin(clock_ * 4.0f)) : kGreen;
        l.circle(x + 7, 290, 7, dot);
        put(l, f.regular, word, x + 26, 298, 24, kInk);
        // Where the network reaches it, on the same line.
        if (!live_.address.empty()) {
            put(l, f.regular, "http://" + live_.address, x + w, 298, 22, kInk2, gfx::Align::right);
        }
        l.line(x, 324, x + w, 324, 1.5f, kHair);

        // The large number: speed while serving, progress while loading.
        const Hero h = hero_of(live_, load_shown_);
        const bool quiet = live_.state != ServerState::generating && live_.state != ServerState::loading;
        if (!h.known) {
            // Nothing answered yet, said in words (a lone dash and "tokens per
            // second" read as broken).
            put(l, f.display, decision ? "No decisions yet" : "No replies yet", x, hero_y - 24, 56, kInk2);
            put(l, f.regular, decision ? "The time per decision shows here" : "The speed of each reply shows here", x, hero_y + 8,
                20, kInk3);
        } else {
            const std::string number = h.value >= 100 || live_.state == ServerState::loading ? format("%.0f", h.value)
                                                                                             : format("%.1f", h.value);
            const float nw = put(l, f.display, number, x - 3, hero_y, 96, quiet ? kInk2 : kInk);
            put(l, f.regular, h.unit, x + nw + 14, h.note.empty() ? hero_y : hero_y - 28, 22, kInk2);
            if (!h.note.empty()) {
                put(l, f.regular, h.note, x + nw + 14, hero_y, 20, kInk3);
            }
        }
        if (live_.state == ServerState::loading) {
            // The model's memory as cells filling column by column, the newest
            // lit and a slow wave over the rest; before the load starts, a light
            // sweeping the empty ones. It never looks stopped.
            constexpr int cols = 30, rows = 4;
            const float cw = (w - 3.0f * (cols - 1)) / cols, ch = (tr.h - 3.0f * (rows - 1)) / rows;
            const int filled = (int) (load_shown_ * cols * rows);
            const float sweep = (0.5f + 0.5f * std::sin(clock_ * 1.7f)) * (cols - 1);
            for (int c = 0; c < cols; ++c) {
                for (int r = 0; r < rows; ++r) {
                    const int i = c * rows + r;
                    const Color cell = i < filled ? kInk.with_alpha(0.45f + 0.4f * (0.5f + 0.5f * std::sin(clock_ * 3.0f - (float) c * 0.44f)))
                                     : i == filled ? kBlue.with_alpha(0.6f + 0.4f * std::sin(clock_ * 8.0f))
                                     : filled == 0 ? gfx::mix(kHair, kInk3, std::max(0.0f, 1.0f - std::fabs((float) c - sweep) / 3.0f))
                                                   : kHair;
                    l.rounded_rect({ x + (cw + 3) * (float) c, tr.y + (ch + 3) * (float) r, cw, ch }, 3, cell);
                }
            }
            put(l, f.regular,
                f.regular.font->fit(!live_.phase.empty() ? live_.phase
                                    : live_.load_progress < 0.985f ? std::string("Copying the weights into GPU memory")
                                                                   : std::string("Preparing the context"),
                                    20, w),
                x, tr.y + tr.h + 26, 20, kInk2);
        } else if (!decision) {
            trace(l, live_.gen_history, tr, kInk.with_alpha(0.75f));
        }
    }

    // What the model holds and the screen's share, and how much of the context
    // is used.
    l.line(x, 522, x + w, 522, 1.5f, kHair);
    draw_memory(l, x, 562, w);
    if (loaded) {
        put(l, f.regular, "Context", x, 658, 22, kInk2);
        put(l, f.regular, thousands(live_.ctx_used) + " of " + format("%.0fk", live_.ctx / 1024.0), x + w, 658, 22, kInk,
            gfx::Align::right);
        meter(l, x, 674, w, live_.ctx > 0 ? (float) live_.ctx_used / (float) live_.ctx : 0, kInk.with_alpha(0.8f));
    }

    // The console's readings, each with its last two minutes: what the Console
    // page charts, at a glance.
    l.line(x, 700, x + w, 700, 1.5f, kHair);
    float y = 742;
    for (const auto & v : vitals_of(live_)) {
        vital_row(l, f, v, x, w, y);
        y += 36;
    }

    const float cx = x + w + kHomeGap;
    draw_requests(l, { cx, 124, kRight - cx, 808 });
}

// ---- drawing: the Home chat (PS5LM Lean) ------------------------------------------------

namespace {

// A microphone, `s` tall, centred on (cx, cy); struck through when off.
void mic_icon(gfx::DrawList & l, float cx, float cy, float s, Color c, bool slash = false) {
    const float t = std::max(2.0f, s * 0.09f);
    l.rounded_rect({ cx - s * 0.17f, cy - s * 0.5f, s * 0.34f, s * 0.58f }, s * 0.17f, c);
    l.arc(cx, cy - s * 0.1f, s * 0.32f, t, 1.5708f, 3.1416f, c);  // the cradle: 3 o'clock round the bottom to 9
    l.line(cx, cy + s * 0.22f, cx, cy + s * 0.42f, t, c);
    l.line(cx - s * 0.18f, cy + s * 0.43f, cx + s * 0.18f, cy + s * 0.43f, t, c);
    if (slash) {
        l.line(cx - s * 0.42f, cy - s * 0.46f, cx + s * 0.42f, cy + s * 0.46f, t, c);
    }
}

// Three dots that rise in turn: something is on its way.
float dots(gfx::DrawList & l, float x, float cy, Color c, float clock) {
    for (int i = 0; i < 3; ++i) {
        l.circle(x + 5 + 16.0f * (float) i, cy, 4, c.with_alpha(0.3f + 0.7f * (0.5f + 0.5f * std::sin(clock * 6.0f - (float) i))));
    }
    return 42;
}

// A line of the model's answer as the TV shows it: Markdown's emphasis,
// code and heading marks left out, a list's dashes as bullets.
std::string plain_line(std::string s) {
    for (const char * mark : { "**", "`" }) {
        for (size_t at = s.find(mark); at != std::string::npos; at = s.find(mark, at)) {
            s.erase(at, std::char_traits<char>::length(mark));
        }
    }
    const size_t lead = s.find_first_not_of(" \t\r");
    if (lead == std::string::npos) {
        return {};
    }
    s.erase(0, lead);
    if (s[0] == '#') {
        s.erase(0, std::min(s.size(), s.find_first_not_of("# ")));
    } else if (s.size() > 1 && (s[0] == '-' || s[0] == '*') && s[1] == ' ') {
        s = "\xE2\x80\xA2 " + s.substr(2);
    }
    return s;
}

// The chat's text wrapped, keeping its line breaks; an empty line (half a
// line tall) between paragraphs.
std::vector<std::string> chat_wrap(const ui::FontRef & f, const std::string & text, float width) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = plain_line(text.substr(at, end - at));
        if (line.empty()) {
            if (!out.empty() && !out.back().empty()) {
                out.emplace_back();
            }
        } else {
            for (auto & s : wrap(f, line, kChatSize, width)) {
                out.push_back(std::move(s));
            }
        }
        at = end + 1;
    }
    while (!out.empty() && out.back().empty()) {
        out.pop_back();
    }
    return out;
}

float lines_h(const std::vector<std::string> & v) {
    float h = 0;
    for (const auto & s : v) {
        h += s.empty() ? kChatPitch * 0.5f : kChatPitch;
    }
    return h;
}

// One message laid out: what was said, on the right in a bubble, and the
// model's answer under it on the left, as in any chat. No turn: the line that
// says the oldest messages are no longer sent.
constexpr float kBubblePad = 20, kAfterAsk = 18, kAfterAnswer = 36, kMetaH = 34, kDroppedH = 70;
float bubble_text_w(float w) { return w * 0.78f - 40; }

struct ChatBlock {
    const ChatTurn *         turn = nullptr;
    std::vector<std::string> lines;
    float                    h = 0;
};

ChatBlock chat_block(const ui::FontRef & f, const ChatTurn & t, float w) {
    ChatBlock b;
    b.turn = &t;
    if (t.role == ChatTurn::Role::user) {
        b.lines = chat_wrap(f, t.text, bubble_text_w(w));
        if (b.lines.empty()) {
            b.lines.push_back(kDash);
        }
        b.h = lines_h(b.lines) + kBubblePad + kAfterAsk;
    } else {
        b.lines = chat_wrap(f, t.text, w);
        b.h = std::max(kChatPitch, lines_h(b.lines)) + (t.streaming ? kMetaH : 0) + kAfterAnswer;
    }
    return b;
}

}  // namespace

Rect Dashboard::chat_box() const {
    const Rect p = home_panel();
    const float bx = p.x + 28, bw = p.w - 56;
    int lines = 1;
    if (!box_.empty()) {
        lines = std::clamp((int) wrap(fonts_.regular, box_, kChatSize, box_text_w(bw)).size(), 1, kBoxLines);
    }
    const float bh = 76 + kChatPitch * (float) (lines - 1);
    return { bx, p.y + p.h - 28 - bh, bw, bh };
}

Rect Dashboard::chat_view() const {
    const Rect p = home_panel(), b = chat_box();
    const float top = p.y + 90 + 22 + (running_elsewhere(live_) ? 50.0f : 0.0f);
    const float bottom = b.y - 22 - (note_left_ > 0 ? 46.0f : 0.0f);
    return { p.x + 56, top, p.w - 112, bottom - top };
}

float Dashboard::chat_height(float w) const {
    float h = chat_.dropped > 0 ? kDroppedH : 0;
    for (const auto & t : chat_.turns) {
        h += chat_block(fonts_.regular, t, w).h;
    }
    return h;
}

// Right: the chat with the loaded model, or what the other devices on the
// network ask of it (left and right switch). One panel, as a car gives the
// map.
void Dashboard::draw_requests(gfx::DrawList & l, const Rect & p) const {
    const ui::Fonts & f = fonts_;
    l.rounded_rect(p, 24, kSurface);
    const float x = p.x + 56, w = p.w - 112, hy = p.y + 62;
    const Exchange * other = running_elsewhere(live_);

    // The two views as tabs; a blue dot while another device is being answered.
    float tx = x;
    for (int i = 0; i < 2; ++i) {
        const bool on = devices_ == (i == 1);
        const float tw = put(l, f.regular, i == 0 ? "Chat" : "Other devices", tx, hy, 22, on ? kInk : kInk3);
        if (on) {
            l.rounded_rect({ tx, p.y + 86, tw, 4 }, 2, kBlue);
        }
        tx += tw;
        if (i == 1 && other) {
            l.circle(tx + 14, hy - 8, 5, kBlue);
            tx += 20;
        }
        tx += 36;
    }
    ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
    ui::draw_button(l, f, glyphs, ui::Button::dpad, tx - 8, hy - 8, 26);

    // On the right: the chat's length, or the trip meter of every request.
    std::vector<std::pair<std::string, Color>> trip;
    if (devices_) {
        const RequestSummary s = summary_of(live_);
        trip.push_back({ thousands(live_.served) + " answered", kInk2 });
        if (s.typical >= 0) {
            trip.push_back({ format(s.typical < 10 ? "%.1f s typical" : "%.0f s typical", s.typical), kInk2 });
        }
        trip.push_back({ thousands(live_.prompt_tokens) + " tokens in", kInk3 });
        if (live_.decision.empty()) {
            trip.push_back({ thousands(live_.tokens) + " out", kInk3 });
        }
        if (s.failed > 0) {
            trip.push_back({ format("%.0f failed", (double) s.failed), kRed });
        }
    } else {
        if (!chat_.turns.empty()) {
            const double n = (double) chat_.turns.size();
            trip.push_back({ format(n == 1 ? "%.0f message" : "%.0f messages", n), kInk2 });
        }
        if (chat_.dropped > 0) {
            trip.push_back({ format("%.0f older left out", (double) chat_.dropped), kInk3 });
        }
        if (chat_.whisper_loaded && chat_.whisper_mib > 0) {
            trip.push_back({ format("speech model %.0f MB", (double) chat_.whisper_mib), kInk3 });
        }
    }
    float rx = x + w;
    for (auto it = trip.rbegin(); it != trip.rend(); ++it) {
        rx -= put(l, f.regular, it->first, rx, hy, 20, it->second, gfx::Align::right) + 28;
    }
    l.line(x, p.y + 90, x + w, p.y + 90, 1.5f, kHair);

    if (devices_) {
        draw_feed(l, x, w, p.y + 110, p.y + p.h - 26);
        return;
    }
    if (other) {
        // The model answers one request at a time: say who has it now.
        const float sy = p.y + 90 + 38;
        const std::string who = other->client;
        l.circle(x + 6, sy - 7, 6, kBlue.with_alpha(0.55f + 0.45f * std::sin(clock_ * 4.0f)));
        float sx = x + 24;
        sx += put(l, f.regular, who + " is using the model", sx, sy, 20, kInk2);
        const std::string after = chat_.answering ? "Your reply comes after it" : "";
        const float aw = after.empty() ? 0 : put(l, f.regular, after, x + w, sy, 20, kInk, gfx::Align::right);
        const std::string asked = other->asked.empty() ? std::string() : "\xE2\x80\x9C" + other->asked + "\xE2\x80\x9D";
        put(l, f.regular, f.regular.font->fit(asked, 20, std::max(0.0f, x + w - aw - 40 - (sx + 16))), sx + 16, sy, 20, kInk3);
        l.line(x, p.y + 90 + 50, x + w, p.y + 90 + 50, 1.0f, kHair);
    }
    const Rect view = chat_view();
    if (!live_.decision.empty() && chat_.turns.empty()) {
        draw_feed(l, view.x, view.w, view.y, view.y + view.h, true);  // a decision model: each message on its own
    } else {
        draw_chat(l, view);
    }
    if (note_left_ > 0) {
        draw_chat_note(l, x, w, chat_box().y - 24);
    }
    draw_box(l);
}

// The conversation, newest at the bottom; up and down scroll it.
void Dashboard::draw_chat(gfx::DrawList & l, const Rect & v) const {
    const ui::Fonts & f = fonts_;
    const float x = v.x, w = v.w, top = v.y, bottom = v.y + v.h;
    const bool serving = live_.state == ServerState::ready || live_.state == ServerState::generating;

    if (chat_.turns.empty()) {
        // What the chat is for, and how: until the first message.
        std::string name, build;
        split_name(live_.model_label, &name, &build);
        std::vector<std::string> say;
        std::string title;
        if (live_.model_label.empty() || live_.state == ServerState::no_model) {
            title = "No model is loaded";
            say = { "Cross chooses one. You can talk or type now; the message waits in the box." };
        } else {
            title = serving ? "Talk to " + name : name + " is loading";
            say.push_back(chat_.mic == MicState::off
                              ? "Press Triangle to type a message, then R2 to send it."
                              : "Hold L2 and speak. Let go, check the words in the box, and press R2 to send. Or press "
                                "Triangle to type.");
            say.push_back("It remembers what was said until you start a new chat.");
            if (!serving) {
                say.push_back("You can talk or type now; send when it is ready.");
            }
        }
        float y = top + 64;
        put(l, f.regular, f.regular.font->fit(title, 36, w), x, y, 36, kInk);
        y += 52;
        for (const auto & s : say) {
            for (const auto & line : wrap(f.regular, s, kChatSize, w * 0.82f)) {
                put(l, f.regular, line, x, y, kChatSize, kInk2);
                y += kChatPitch;
            }
            y += 10;
        }
        return;
    }

    // Newest first, as many as the view and the scroll need.
    std::vector<ChatBlock> blocks;
    float total = 0;
    bool oldest = false;
    for (size_t i = chat_.turns.size(); i-- > 0;) {
        blocks.push_back(chat_block(f.regular, chat_.turns[i], w));
        total += blocks.back().h;
        if (i == 0) {
            oldest = true;
        } else if (total > chat_scroll_ + v.h + kChatPitch) {
            break;
        }
    }
    if (oldest && chat_.dropped > 0) {
        blocks.push_back({});
        blocks.back().h = kDroppedH;
        total += kDroppedH;
    }
    const float scroll = oldest ? std::min(chat_scroll_, std::max(0.0f, total - v.h)) : chat_scroll_;

    l.push_clip({ x - 56, top, w + 112, v.h });
    float yb = bottom + scroll;
    for (const auto & b : blocks) {
        const float y = yb - b.h;
        yb = y;
        if (y > bottom || y + b.h < top) {
            continue;
        }
        if (!b.turn) {
            // The oldest messages no longer fit the context.
            const std::string s = std::to_string(chat_.dropped) + (chat_.dropped == 1 ? " earlier message is" : " earlier messages are") +
                                  " no longer sent to the model";
            const float sw = f.regular.measure(s, 20);
            const float cy = y + 28;
            put(l, f.regular, s, x + w / 2, cy + 7, 20, kInk3, gfx::Align::center);
            l.line(x, cy, x + w / 2 - sw / 2 - 20, cy, 1.0f, kHair);
            l.line(x + w / 2 + sw / 2 + 20, cy, x + w, cy, 1.0f, kHair);
            continue;
        }
        const ChatTurn & t = *b.turn;
        if (t.role == ChatTurn::Role::user) {
            float bw = 0;
            for (const auto & s : b.lines) {
                bw = std::max(bw, f.regular.measure(s, kChatSize));
            }
            bw += 40;
            const float bh = lines_h(b.lines) + kBubblePad;
            l.rounded_rect({ x + w - bw, y, bw, bh }, 20, t.failed ? kRed.with_alpha(0.6f) : kBlue);
            float ly = y + kChatSize + 10;
            for (const auto & s : b.lines) {
                if (!s.empty() && ly + 8 > top && ly - kChatSize < bottom) {  // only the lines in view cost shapes
                    put(l, f.regular, s, x + w - bw + 20, ly, kChatSize, kInk);
                }
                ly += s.empty() ? kChatPitch * 0.5f : kChatPitch;
            }
            if (t.spoken) {
                mic_icon(l, x + w - bw - 26, y + 26, 24, kInk3);  // said, not typed
            }
        } else {
            float ly = y + kChatSize;
            if (b.lines.empty() && t.streaming) {
                dots(l, x + 3, y + 14, kInk2, clock_);  // nothing yet, as a chat shows the other side typing
            }
            for (const auto & s : b.lines) {
                if (!s.empty() && ly + 8 > top && ly - kChatSize < bottom) {
                    put(l, f.regular, s, x, ly, kChatSize, t.failed ? kRed : kInk);
                }
                ly += s.empty() ? kChatPitch * 0.5f : kChatPitch;
            }
            if (t.streaming && !b.lines.empty()) {
                const float mw = put(l, f.regular, "Writing", x, y + std::max(kChatPitch, lines_h(b.lines)) + 24, 20, kInk3);
                dots(l, x + mw + 10, y + std::max(kChatPitch, lines_h(b.lines)) + 17, kInk3, clock_);
            }
        }
    }
    l.pop_clip();

    // More above: the top fades. Scrolled up: a way back to the newest.
    if (yb < top - 1) {
        l.gradient_rect({ x - 56, top, w + 112, 44 }, 0, kSurface, kSurface.with_alpha(0));
    }
    if (scroll > 0) {
        l.gradient_rect({ x - 56, bottom - 56, w + 112, 56 }, 0, kSurface.with_alpha(0), kSurface);
        const float pw = 168, ph = 42;
        const Rect pill{ x + w - pw, bottom - ph - 4, pw, ph };
        l.rounded_rect(pill, ph / 2, kRaised);
        ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
        ui::draw_button(l, f, glyphs, ui::Button::dpad, pill.x + 16, pill.cy(), 24);
        put(l, f.regular, "Newest", pill.x + 52, pill.cy() + 7, 20, kInk2);
    }
}

// The message box: what will be sent, a take being recorded or written down,
// or what the box is for.
void Dashboard::draw_box(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const Rect b = chat_box();
    const MicState mic = chat_.mic;
    const bool take = talking_ || mic == MicState::loading || mic == MicState::recording;
    const float gx = b.x + 42, gy = b.y + 38;                                // the microphone
    const float tx = b.x + 84, tw = box_text_w(b.w), base = b.y + 47;  // the text
    if (take) {
        l.bordered_rect(b, 22, kRaised, 2, kBlue);
    } else {
        l.rounded_rect(b, 22, kRaised);
    }

    if (take) {
        const float lv = levels_.empty() || mic != MicState::recording ? 0 : tween::clamp01(levels_.back());
        l.circle(gx, gy, 22 + 7 * std::sqrt(lv), mic == MicState::recording ? kBlue
                                                    : kBlue.with_alpha(0.45f + 0.35f * std::sin(clock_ * 6.0f)));
        mic_icon(l, gx, gy, 24, kInk);
        if (mic != MicState::recording) {
            const float nw = put(l, f.regular, "Starting the microphone", tx, base, kChatSize, kInk2);
            dots(l, tx + nw + 12, base - 8, kInk2, clock_);
            return;
        }
        const double s = chat_.seconds;
        const float nw = put(l, f.regular, "Listening", tx, base, kChatSize, kInk);
        put(l, f.regular, format("0:%02.0f", std::floor(s)), tx + nw + 18, base, kChatSize, s >= 25 ? kInk : kInk2);
        // The level of the last few seconds, newest on the right.
        const float mx = tx + 250, mw = b.x + b.w - 36 - mx, step = mw / (float) kMeterBars;
        l.line(mx, gy, mx + mw, gy, 1.0f, kHair);
        for (size_t i = 0; i < levels_.size(); ++i) {
            const float v = std::sqrt(tween::clamp01(levels_[levels_.size() - 1 - i]));
            const float h = 4 + 40 * v;
            l.rounded_rect({ mx + mw - step * (float) (i + 1) + step * 0.2f, gy - h / 2, step * 0.6f, h }, step * 0.3f,
                           i == 0 ? kInk : kInk2);
        }
        return;
    }
    if (mic == MicState::transcribing) {
        l.circle(gx, gy, 22, kBlue.with_alpha(0.25f));
        l.arc(gx, gy, 25, 3, clock_ * 5.0f, 1.8f, kBlue);
        mic_icon(l, gx, gy, 24, kInk);
        const float nw = put(l, f.regular, "Writing it down", tx, base, kChatSize, kInk);
        dots(l, tx + nw + 12, base - 8, kInk, clock_);
        if (chat_.seconds > 0) {
            put(l, f.regular, format("%.0f s of speech", chat_.seconds), b.x + b.w - 30, base, 20, kInk3, gfx::Align::right);
        }
        return;
    }

    mic_icon(l, gx, gy, 24, mic == MicState::off ? kInk3 : kInk2, mic == MicState::off);
    if (box_.empty()) {
        if (mic == MicState::off) {
            const float nw = put(l, f.regular, "Press Triangle to type", tx, base, kChatSize, kInk3);
            const std::string why = "Talking is off: " + (chat_.note.empty() ? std::string("no speech model") : chat_.note);
            put(l, f.regular, f.regular.font->fit(why, 20, b.x + b.w - 30 - (tx + nw + 28)), tx + nw + 28, base, 20, kInk3);
        } else {
            put(l, f.regular, "Hold L2 to talk, or press Triangle to type", tx, base, kChatSize, kInk3);
        }
        return;
    }
    std::vector<std::string> lines = wrap(f.regular, box_, kChatSize, tw);
    if (lines.size() > (size_t) kBoxLines) {
        lines.resize(kBoxLines);
        lines.back() = f.regular.font->fit(lines.back() + " \xE2\x80\xA6", kChatSize, tw);
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        put(l, f.regular, lines[i], tx, base + kChatPitch * (float) i, kChatSize, kInk);
    }
    // How it goes: R2 sends, or it waits for the reply.
    const float rx = b.x + b.w - 30;
    if (send_waits_) {
        const float ww = put(l, f.regular, "Waits for the reply", rx, base, 20, kInk2, gfx::Align::right);
        l.circle(rx - ww - 16, base - 7, 6, kBlue.with_alpha(0.55f + 0.45f * std::sin(clock_ * 4.0f)));
    } else {
        const float sw = put(l, f.regular, "Send", rx, base, 22, kInk, gfx::Align::right);
        ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
        ui::draw_button(l, f, glyphs, ui::Button::r2, rx - sw - 12 - ui::button_width(ui::Button::r2, 30), base - 8, 30);
    }
}

// Over the box: why a take gave nothing, why a message did not go, or what a
// second press does. It stays a few seconds.
void Dashboard::draw_chat_note(gfx::DrawList & l, float x, float w, float y) const {
    const ui::Fonts & f = fonts_;
    const float a = std::min(1.0f, note_left_ / 0.4f);  // fades out over its last moment
    const float hw = put(l, f.regular, note_head_, x, y, 22, (note_fault_ ? kRed : kInk).with_alpha(a));
    if (!note_detail_.empty()) {
        put(l, f.regular, f.regular.font->fit(note_detail_, 20, std::max(0.0f, w - hw - 24)), x + hw + 24, y, 20,
            kInk2.with_alpha(a));
    }
}

// The other devices on the network: what each asked, on the right as a
// message sent, and the model's answer under it on the left, newest at the
// bottom. This PS5's own chat has the other view. `self`: only this PS5's
// own requests instead (a decision model has no conversation: each message
// is one request, and the chat view shows them so).
void Dashboard::draw_feed(gfx::DrawList & l, float x, float w, float top, float bottom, bool self) const {
    const ui::Fonts & f = fonts_;
    constexpr float ts = kChatSize, ask_pitch = ts + 8, answer_pitch = ts + 9;  // a question's lines, an answer's
    struct Turn {
        const Exchange *         e;
        std::vector<std::string> asked, answered;
        float                    h;
    };
    const float bubble = w * 0.78f;  // the widest a request's bubble gets
    const auto clip = [&](std::vector<std::string> & lines, size_t keep, bool tail, float width) {
        if (lines.size() <= keep) {
            return;
        }
        if (tail) {
            lines.erase(lines.begin(), lines.end() - (long) keep);  // streaming: the newest lines
        } else {
            lines.resize(keep);
            lines.back() = f.regular.font->fit(lines.back() + " \xE2\x80\xA6", ts, width);
        }
    };
    std::vector<Turn> turns;
    float total = 0;
    for (auto it = live_.activity.rbegin(); it != live_.activity.rend(); ++it) {
        // This PS5's own requests are the chat (and the vision warm-up, which
        // nobody asked for): they stay out of the other devices' view.
        if (is_warm_up(*it) || (it->client.rfind("127.0.0.1", 0) == 0) != self) {
            continue;
        }
        const bool newest = turns.empty();
        Turn t{ &*it, wrap(f.regular, it->asked.empty() ? std::string(kDash) : it->asked, ts, bubble - 40),
                wrap(f.regular, it->answered, ts, w), 0 };
        clip(t.asked, newest ? 3 : 2, false, bubble - 40);
        // The newest answer gets the lines the panel has left under its question.
        const float room = bottom - top - (26 + (float) t.asked.size() * ask_pitch + 20 + 14 + 34 + 26);
        clip(t.answered, newest ? (size_t) std::clamp(std::floor(room / answer_pitch), 1.0f, (float) kNewestLines) : 3,
             it->status == 0, w);
        t.h = 26 + (float) t.asked.size() * ask_pitch + 20 + 14 + (float) std::max<size_t>(1, t.answered.size()) * answer_pitch + 34 + 26;
        total += t.h;
        turns.push_back(std::move(t));
        if (total > bottom - top) {
            break;  // the oldest shown runs off the top, as a chat scrolls
        }
    }
    if (turns.empty() && self) {
        std::string name, build;
        split_name(live_.model_label, &name, &build);
        put(l, f.regular, f.regular.font->fit("Decide with " + name, 36, w), x, top + 64, 36, kInk);
        float y = top + 116;
        for (const auto & s : wrap(f.regular, "Hold L2 and speak, or press Triangle to type, then R2: the decision model "
                                              "answers each message on its own, here.", ts, w * 0.82f)) {
            put(l, f.regular, s, x, y, ts, kInk2);
            y += ts + 9;
        }
    } else if (turns.empty()) {
        put(l, f.regular, "Requests from other devices on your network appear here.", x, top + 40, ts, kInk3);
        if (!live_.address.empty()) {
            put(l, f.regular, "They reach the model at http://" + live_.address, x, top + 40 + ts + 14, ts, kInk3);
        }
    }
    float y = bottom - total;
    for (auto t = turns.rbegin(); t != turns.rend(); ++t) {
        const Exchange & e = *t->e;
        const bool running = e.status == 0;
        const bool fault = e.status >= 400;
        const std::string who = self ? std::string("This PS5") : e.client;
        if (y >= top) {
            put(l, f.regular, who + "    " + e.path, x + w, y + 19, 20, kInk3, gfx::Align::right);
        }
        y += 26;
        float bw = 0;
        for (const auto & s : t->asked) {
            bw = std::max(bw, f.regular.measure(s, ts));
        }
        bw += 40;
        const float bh = (float) t->asked.size() * ask_pitch + 20;
        if (y >= top) {
            l.rounded_rect({ x + w - bw, y, bw, bh }, 20, kBlue);
            for (size_t i = 0; i < t->asked.size(); ++i) {
                put(l, f.regular, t->asked[i], x + w - bw + 20, y + ts + 10 + ask_pitch * (float) i, ts, kInk);
            }
        }
        y += bh + 14;
        if (t->answered.empty() && running) {
            // Nothing yet: three dots, as a chat shows the other side typing.
            for (int i = 0; i < 3; ++i) {
                l.circle(x + 8 + 20.0f * (float) i, y + 14, 5, kInk2.with_alpha(0.35f + 0.65f * (0.5f + 0.5f * std::sin(clock_ * 6.0f - (float) i))));
            }
        }
        for (size_t i = 0; i < t->answered.size(); ++i) {
            if (y + answer_pitch * (float) i >= top) {
                put(l, f.regular, t->answered[i], x, y + ts + answer_pitch * (float) i, ts, fault ? kRed : kInk);
            }
        }
        if (t->answered.empty() && !running && y >= top) {
            put(l, f.regular, kDash, x, y + ts, ts, kInk3);
        }
        y += (float) std::max<size_t>(1, t->answered.size()) * answer_pitch + 6;
        std::string meta = running ? "Answering" : fault ? format("Error %.0f", (double) e.status)
                                                         : format(e.seconds < 10 ? "%.1f s" : "%.0f s", e.seconds);
        if (!running && !fault && e.questions > 0) {
            meta += format(", %.0f ms per decision", 1000.0 * e.seconds / e.questions);
        }
        if (y >= top) {
            put(l, f.regular, meta, x, y + 23, 20, fault ? kRed : kInk3);
        }
        y += 28 + 26;
    }
}

// ---- drawing: Models ------------------------------------------------------------------

void Dashboard::draw_models(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    put(l, f.semibold, "Models", kLeft, kTitleY, 44, kInk);

    // The highlight: on a section, or on a row of the list.
    const float hx = in_list_ ? kListX : kLeft - 20;
    const float hw = in_list_ ? kRight - kListX : kRailW;
    const float hh = in_list_ ? kRowH : kRailH;
    l.rounded_rect({ hx, rows_y_.value, hw, hh }, 14, kRaised);

    for (int i = 0; i < kSections; ++i) {
        const float y = kListY + kRailPitch * (float) i;
        const bool on = i == section_;
        if (on) {
            l.rounded_rect({ kLeft - 20, y + 16, 4, kRailH - 32 }, 2, kBlue);
        }
        put(l, f.regular, kSectionNames[i], kLeft, y + 42, 26, on ? kInk : kInk2);
        std::string count;
        if (i == kLocal) {
            count = std::to_string(live_.models.size());
        } else if (i == kDownloads && !market_.downloads.empty()) {
            int active = 0;
            for (const auto & d : market_.downloads) {
                active += d.state != DownloadState::done;
            }
            count = active ? std::to_string(active) : "";
        }
        put(l, f.regular, count, kLeft + kRailW - 44, y + 42, 22, kInk3, gfx::Align::right);
    }

    const Rect area{ kListX, kListY, kRight - kListX, kDockY - 40 - kListY };
    switch (section_) {
        case kLocal: draw_local(l, area); break;
        case kSuggested: draw_suggested(l, area); break;
        case kSearch: draw_market(l, area); break;
        default: draw_downloads(l, area); break;
    }
}

void Dashboard::draw_local(gfx::DrawList & l, const Rect & a) const {
    const ui::Fonts & f = fonts_;
    const int rows = (int) live_.models.size();
    if (rows == 0) {
        put(l, f.regular, "No models yet", a.x + 28, a.y + 46, 30, kInk);
        put(l, f.regular, "Get one from Suggested, or copy a .gguf to /data/PS5LM/models.", a.x + 28, a.y + 90, 22, kInk2);
        return;
    }
    const int first = first_visible(local_cursor_, rows);
    for (int i = first; i < std::min(rows, first + kVisibleRows); ++i) {
        const ModelRow & m = live_.models[(size_t) i];
        const float y = a.y + kRowPitch * (float) (i - first);
        std::string name, build;
        split_name(m.label, &name, &build);
        const bool can = m.fits || m.current || m.fits_screen_off;
        const float nw = put(l, f.regular, f.regular.font->fit(name, 26, 640), a.x + 28, y + 36, 26, can ? kInk : kInk3);
        put(l, f.regular, build, a.x + 28 + nw + 14, y + 36, 21, kInk3);
        std::string meta = (m.decision.empty() ? "Language" : "Decision") + std::string("    ") + gb(m.size_gib);
        if (m.drive != "Internal storage" && !m.drive.empty()) {
            meta += "    " + m.drive;
        }
        std::string plan = gb_text(m.plan);
        if (plan.rfind("decision model (", 0) == 0 && plan.find("), ") != std::string::npos) {
            plan = plan.substr(plan.find("), ") + 3);  // the kind is said already
        }
        // PS5LM Lean: why it does not fit, in a sentence: short by a little
        // (and the screen's memory would do), or how far over this PS5 it is.
        if (m.current && live_.plan.empty()) {
            plan = "running with its own settings (app-args.txt)";
        } else if (m.current && !m.fits) {
            plan = "loaded with its own settings (app-args.txt)";
        } else if (!m.fits && m.fits_screen_off) {
            plan = format("%.1f GB short with the screen on", std::max(0.1, m.need_gib - screen_.budget_gib));
        } else if (!m.fits && m.need_gib > 0) {
            plan = "needs " + gb(m.need_gib) + "; this PS5 has " + gb(screen_.budget_gib + screen_.cost_gib) + " for a model";
        }
        meta += "    " + plan;
        put(l, f.regular, f.regular.font->fit(meta, 20, a.w - 360), a.x + 28, y + 66, 20, kInk3);
        const char * state = m.current ? "Running"
                           : m.fits && m.images_screen_off ? "Images with the screen off"
                           : m.fits && m.text_only ? "Text only"
                           : m.fits ? "" : m.fits_screen_off ? "Fits with the screen off" : "Too big";
        const bool blue = m.current || m.fits_screen_off || (m.fits && m.images_screen_off);
        put(l, f.semibold, state, a.x + a.w - 28, y + 50, 22, blue ? kBlue : kInk3, gfx::Align::right);
    }
}

void Dashboard::draw_suggested(gfx::DrawList & l, const Rect & a) const {
    const ui::Fonts & f = fonts_;
    const int first = first_visible(suggested_cursor_, kSuggestionCount);
    for (int i = first; i < std::min(kSuggestionCount, first + kVisibleRows); ++i) {
        const Suggestion & s = kSuggestions[i];
        const float y = a.y + kRowPitch * (float) (i - first);
        const float nw = put(l, f.regular, s.name, a.x + 28, y + 36, 26, kInk);
        const double bytes = (double) s.model.bytes + (double) (s.mmproj.file ? s.mmproj.bytes : 0);
        put(l, f.regular, format("%.1f GB", bytes / 1073741824.0), a.x + 28 + nw + 14, y + 36, 21, kInk3);
        std::string about = s.decision ? "Decision" : s.mmproj.file ? "Language, images" : "Language";
        if (s.tps > 0) {
            about += format("    %.0f tokens a second here", s.tps);
        }
        about += std::string("    ") + s.about;
        put(l, f.regular, f.regular.font->fit(about, 20, a.w - 260), a.x + 28, y + 66, 20, kInk3);
        // On the console already, on its way, or ready to get.
        std::string state;
        Color c = kInk3;
        for (const auto & m : live_.models) {
            if (m.label + ".gguf" == local_name(s.model)) {
                state = m.current ? "Running" : "On this PS5";
                c = m.current ? kBlue : kInk3;
            }
        }
        for (const auto & d : market_.downloads) {
            if (state.empty() && d.repo == s.repo && d.file == s.model.file && d.state != DownloadState::done) {
                state = d.bytes ? format("%.0f%%", 100.0 * (double) d.done / (double) d.bytes) : "Waiting";
                c = kInk2;
            }
        }
        if (state.empty() && in_list_ && i == suggested_cursor_) {
            state = "Get";
            c = kBlue;
        }
        put(l, f.semibold, state, a.x + a.w - 28, y + 50, 22, c, gfx::Align::right);
    }
}

void Dashboard::draw_market(gfx::DrawList & l, const Rect & a) const {
    const ui::Fonts & f = fonts_;
    // The searches, as tabs.
    float x = a.x + 28;
    for (int i = 0; i < kMarketChipCount; ++i) {
        const bool on = i == market_chip_;
        const float w = put(l, f.regular, kMarketChips[i], x, a.y + 34, 22, on ? kInk : kInk3);
        if (on) {
            l.rounded_rect({ x, a.y + 48, w, 3 }, 1.5f, kBlue);
        }
        x += w + 36;
    }
    put(l, f.regular, market_.busy ? "Searching" : market_.status, a.x + a.w - 28, a.y + 34, 20, kInk3, gfx::Align::right);
    const float top = a.y + 72;
    const int repos = (int) market_.repos.size();
    const MarketRepo * open = market_.open >= 0 && market_.open < repos ? &market_.repos[(size_t) market_.open] : nullptr;
    if (!market_files_) {
        const int first = first_visible(market_repo_, repos);
        for (int i = first; i < std::min(repos, first + kVisibleRows); ++i) {
            const MarketRepo & r = market_.repos[(size_t) i];
            const float y = top + kRowPitch * (float) (i - first);
            const size_t slash = r.id.find('/');
            put(l, f.regular, f.regular.font->fit(slash == std::string::npos ? r.id : r.id.substr(slash + 1), 26, a.w - 300), a.x + 28,
                 y + 36, 26, kInk);
            put(l, f.regular, (slash == std::string::npos ? std::string() : r.id.substr(0, slash)) + "    " +
                                   thousands(r.downloads) + " downloads", a.x + 28, y + 66, 20, kInk3);
        }
        if (repos == 0 && !market_.busy) {
            put(l, f.regular, "Nothing found", a.x + 28, top + 46, 26, kInk2);
        }
        return;
    }
    if (!open) {
        return;
    }
    put(l, f.regular, open->id, a.x + a.w - 28, top - 4, 20, kInk2, gfx::Align::right);
    const int files = (int) open->files.size();
    const int first = first_visible(market_file_, files);
    for (int i = first; i < std::min(files, first + kVisibleRows); ++i) {
        const MarketFile & m = open->files[(size_t) i];
        const float y = top + kRowPitch * (float) (i - first);
        const bool can = m.fits != 0 || m.fits_screen_off;
        put(l, f.regular, f.regular.font->fit(m.name, 26, a.w - 360), a.x + 28, y + 36, 26, can ? kInk : kInk3);
        std::string plan = m.checking ? "Reading its header" : m.fits == -1 ? "Square checks whether it fits" : gb_text(m.plan);
        // PS5LM Lean: the same verdicts as the library.
        if (m.fits == 0 && m.fits_screen_off) {
            plan = format("%.1f GB short with the screen on", std::max(0.1, m.need_gib - screen_.budget_gib));
        } else if (m.fits == 0 && m.need_gib > 0) {
            plan = "needs " + gb(m.need_gib) + "; this PS5 has " + gb(screen_.budget_gib + screen_.cost_gib) + " for a model";
        }
        put(l, f.regular, f.regular.font->fit(format("%.1f GB", m.bytes / 1073741824.0) + "    " + plan, 20, a.w - 360), a.x + 28, y + 66,
             20, kInk3);
        put(l, f.semibold, m.fits == 1 ? "Fits" : m.fits_screen_off ? "Fits with the screen off" : m.fits == 0 ? "Too big" : "",
             a.x + a.w - 28, y + 50, 22, m.fits_screen_off ? kBlue : m.fits == 1 ? kInk2 : kInk3, gfx::Align::right);
    }
}

void Dashboard::draw_downloads(gfx::DrawList & l, const Rect & a) const {
    const ui::Fonts & f = fonts_;
    const int rows = (int) market_.downloads.size();
    if (rows == 0) {
        put(l, f.regular, "No downloads", a.x + 28, a.y + 46, 30, kInk);
        put(l, f.regular, "Models you get from Suggested or Search wait here until they are saved.", a.x + 28, a.y + 90, 22, kInk2);
        return;
    }
    const int first = first_visible(dl_cursor_, rows);
    for (int i = first; i < std::min(rows, first + kVisibleRows); ++i) {
        const Download & d = market_.downloads[(size_t) i];
        const float y = a.y + kRowPitch * (float) (i - first);
        put(l, f.regular, f.regular.font->fit(d.file, 26, a.w - 320), a.x + 28, y + 34, 26,
             d.state == DownloadState::done ? kInk2 : kInk);
        const bool fault = d.state == DownloadState::failed;
        put(l, f.regular, f.regular.font->fit(download_line(d), 20, a.w - 320), a.x + 28, y + 64, 20, fault ? kRed : kInk3);
        const char * state = d.state == DownloadState::running ? "" : d.state == DownloadState::paused ? "Paused"
                           : d.state == DownloadState::failed ? "Failed" : d.state == DownloadState::done ? "Saved"
                           : d.state == DownloadState::retrying ? "Retrying" : "Waiting";
        if (d.state == DownloadState::running && d.bytes) {
            const float share = (float) ((double) d.done / (double) d.bytes);
            meter(l, a.x + a.w - 248, y + 42, 220, share, kBlue);
        } else {
            put(l, f.semibold, state, a.x + a.w - 28, y + 50, 22, fault ? kRed : kInk3, gfx::Align::right);
        }
    }
}

// ---- drawing: API -----------------------------------------------------------------

void Dashboard::draw_api(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const bool decision = !live_.decision.empty();
    const bool loaded = !live_.model_label.empty();
    put(l, f.semibold, "API", kLeft, kTitleY, 44, kInk);
    const std::string base = live_.address.empty() ? std::string("http://<PS5 address>:8081") : "http://" + live_.address;
    put(l, f.regular, "Base address", kLeft, 262, 24, kInk2);
    put(l, f.regular, base + "/v1", kLeft, 316, 44, kInk);
    put(l, f.regular, "Any device on this network can call it, with any model name: the loaded one answers.", kLeft, 360, 22,
         kInk3);

    struct Ep { const char * method; const char * path; const char * about; bool decision; };
    const Ep all[] = {
        { "POST", "/v1/chat/completions", "OpenAI chat, streaming and tools", false },
        { "POST", "/v1/responses", "OpenAI Responses, for Codex and the Agents SDK", false },
        { "POST", "/v1/messages", "Anthropic Messages, for Claude Code and its SDKs", false },
        { "POST", "/v1/completions", "Plain text completion", false },
        { "POST", "/v1/systemone", "TypeSafe decisions: choice, score and yes-or-no questions", true },
        { "GET", "/v1/models", "The loaded model", false },
        { "GET", "/health", "Ready or loading", false },
    };
    float y = 452;
    for (const auto & e : all) {
        const bool here = loaded && (e.decision == decision || std::string(e.method) == "GET");
        put(l, f.mono, e.method, kLeft, y, 24, kInk3);
        put(l, f.mono, e.path, kLeft + 96, y, 24, here ? kInk : kInk3);
        put(l, f.regular, e.about, kLeft + 96, y + 34, 20, here ? kInk2 : kInk3);
        y += 72;
    }

    // An example for what is loaded, in a quiet panel.
    const Rect p{ 1000, 236, kRight - 1000, 688 };
    l.rounded_rect(p, 24, kSurface);
    put(l, f.regular, "Try it", p.x + 48, p.y + 68, 24, kInk2);
    std::vector<std::string> code;
    if (decision) {
        code = { "curl " + base + "/v1/systemone \\", "  -H 'Content-Type: application/json' \\", "  -d '{", "    \"state\": \"Refund me now or I cancel.\",",
                 "    \"questions\": {", "      \"angry\": {", "        \"type\": \"noul\",", "        \"instructions\": \"Is the customer angry?\"",
                 "      }", "    }", "  }'" };
    } else {
        code = { "curl " + base + "/v1/chat/completions \\", "  -H 'Content-Type: application/json' \\", "  -d '{", "    \"messages\": [",
                 "      {\"role\": \"user\",", "       \"content\": \"Hello from my PS5\"}", "    ]", "  }'" };
    }
    float cy = p.y + 130;
    for (const auto & line : code) {
        put(l, f.mono, f.mono.font->fit(line, 21, p.w - 96), p.x + 48, cy, 21, kInk);
        cy += 34;
    }
    l.line(p.x + 48, p.y + p.h - 150, p.x + p.w - 48, p.y + p.h - 150, 1.5f, kHair);
    put(l, f.regular, decision ? "Ollaya and TypeSafe clients:" : "OpenAI SDKs:", p.x + 48, p.y + p.h - 100, 22, kInk2);
    put(l, f.mono, decision ? "base_url=" + base : "base_url=" + base + "/v1", p.x + 48 + (decision ? 300 : 170), p.y + p.h - 100, 21, kInk);
    put(l, f.regular, decision ? "" : "Claude Code:", p.x + 48, p.y + p.h - 56, 22, kInk2);
    put(l, f.mono, decision ? "" : "ANTHROPIC_BASE_URL=" + base, p.x + 218, p.y + p.h - 56, 21, kInk);
}

// ---- drawing: Console -----------------------------------------------------------------

// Four charts on the left, the instruments' readings now on the right, as a
// car shows its energy page: the line first, the numbers beside it.
void Dashboard::draw_console(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    put(l, f.semibold, "Console", kLeft, kTitleY, 44, kInk);
    const char * ranges[] = { "Last 2 minutes", "Last hour" };
    float rx = 1224;
    for (int i = 1; i >= 0; --i) {
        const bool on = (i == 1) == console_hour_;
        rx -= f.regular.measure(ranges[i], 22);
        const float tw = put(l, f.regular, ranges[i], rx, kTitleY - 8, 22, on ? kInk : kInk3);
        if (on) {
            l.rounded_rect({ rx, kTitleY + 6, tw, 3 }, 1.5f, kBlue);
        }
        rx -= 40;
    }

    struct Chart {
        const char *               name;
        const std::vector<float> & v;
        const char *               unit;
        bool                       share;   // 0..1, shown as a percentage
        bool                       sensor;  // read through ps5-exporter
    };
    const bool hour = console_hour_;
    const Chart charts[] = {
        { "Temperature", hour ? live_.temp_hour : live_.temp_history, "\xC2\xB0" "C", false, true },
        { "Power", hour ? live_.power_hour : live_.power_history, " W", false, true },
        { "GPU", hour ? live_.gpu_hour : live_.gpu_history, "%", true, false },
        { "CPU", hour ? live_.cpu_hour : live_.cpu_history, "%", true, false },
    };
    const float cw = (1224 - kLeft - 32) / 2, ch = (926 - kListY - 32) / 2;
    for (int i = 0; i < 4; ++i) {
        const Chart & c = charts[i];
        const Rect r{ kLeft + (cw + 32) * (float) (i % 2), kListY + (ch + 32) * (float) (i / 2), cw, ch };
        l.rounded_rect(r, 20, kSurface);
        put(l, f.regular, c.name, r.x + 32, r.y + 50, 22, kInk2);
        if (c.v.empty()) {
            put(l, f.regular, hour ? "The first point comes after 30 seconds" : c.sensor ? "Not read" : kDash, r.x + 32,
                r.y + r.h * 0.5f + 8, 22, kInk3);
            if (c.sensor && !hour) {
                put(l, f.regular, "With elfldr running, the app loads ps5-exporter to read it", r.x + 32, r.y + r.h * 0.5f + 44,
                    19, kInk3);
            }
            continue;
        }
        float lo = c.v[0], hi = c.v[0], sum = 0;
        for (float x : c.v) {
            lo = std::min(lo, x);
            hi = std::max(hi, x);
            sum += x;
        }
        const auto show = [&](float x) { return c.share ? format("%.0f%%", 100.0 * x) : format("%.0f", x) + c.unit; };
        put(l, f.regular, show(c.v.back()), r.x + r.w - 32, r.y + 54, 34, kInk, gfx::Align::right);
        // The scale: a share runs 0 to 100%; the rest a little past what was seen, in fives.
        const float pad = std::max(4.0f, (hi - lo) * 0.25f);
        const float bot = c.share ? 0.0f : std::max(0.0f, std::floor((lo - pad) / 5) * 5);
        const float top = c.share ? 1.0f : std::ceil((hi + pad) / 5) * 5;
        const Rect g{ r.x + 100, r.y + 96, r.w - 132, r.h - 96 - 70 };
        for (int k = 0; k < 3; ++k) {
            const float gy = g.y + g.h * (float) k * 0.5f;
            l.line(g.x, gy, g.x + g.w, gy, 1.0f, kHair);
            put(l, f.regular, show(top - (top - bot) * 0.5f * (float) k), g.x - 14, gy + 6, 16, kInk3, gfx::Align::right);
        }
        plot(l, c.v, g, kInk.with_alpha(0.85f), bot, top);
        put(l, f.regular, "Low " + show(lo) + "    Average " + show(sum / (float) c.v.size()) + "    High " + show(hi),
            r.x + 32, r.y + r.h - 26, 19, kInk3);
    }

    // The readings now, as a table.
    const Rect p{ 1264, kListY, kRight - 1264, 926 - kListY };
    l.rounded_rect(p, 24, kSurface);
    put(l, f.regular, "Now", p.x + 40, p.y + 52, 22, kInk2);
    const auto deg = [&](float t) { return t > -100 ? format("%.0f\xC2\xB0" "C", t) : std::string(kDash); };
    float soc_lo = 1000, soc_hi = -1000;
    for (const auto & [name, t] : live_.temps) {
        if (name.rfind("soc", 0) == 0) {
            soc_lo = std::min(soc_lo, t);
            soc_hi = std::max(soc_hi, t);
        }
    }
    std::vector<std::pair<std::string, std::string>> rows = {
        { "SoC", deg(live_.soc_temp) },
        { "CPU", deg(live_.cpu_temp) },
        { "SoC sensors", soc_hi > -100 ? format("%.0f", soc_lo) + " to " + deg(soc_hi) : std::string(kDash) },
        { "Fan", live_.fan >= 0 ? format("%.0f%%", 100.0 * live_.fan) : std::string(kDash) },
        { "", "" },
        { "Power", live_.soc_power_w > 0 ? format("%.0f W", live_.soc_power_w) : std::string(kDash) },
        { "Energy since start", format("%.1f Wh", live_.energy_wh) },
        { "CPU clock", live_.cpu_ghz > 0 ? format("%.1f GHz", live_.cpu_ghz) : std::string(kDash) },
        { "", "" },
        { "GPU busy", format("%.0f%%", 100.0 * live_.gpu_busy) },
        { "CPU use", format("%.0f%%", 100.0 * live_.cpu_use) },
        { "Memory", format("%.1f", live_.pool_gib - live_.free_gib) + " of " + gb(live_.pool_gib) },
        { "", "" },
    };
    for (const auto & d : live_.drives) {
        rows.push_back({ d.name, d.total > 0 ? format("%.0f", d.free) + " of " + format("%.0f GB free", d.total) : std::string(kDash) });
    }
    float y = p.y + 104;
    for (const auto & [name, value] : rows) {
        if (name.empty()) {
            l.line(p.x + 40, y - 14, p.x + p.w - 40, y - 14, 1.0f, kHair);
            y += 14;
            continue;
        }
        put(l, f.regular, name, p.x + 40, y, 20, kInk2);
        put(l, f.regular, value, p.x + p.w - 40, y, 20, kInk, gfx::Align::right);
        y += 38;
    }
}

// ---- drawing: Settings ----------------------------------------------------------------

void Dashboard::draw_settings(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    put(l, f.semibold, "Settings", kLeft, kTitleY, 44, kInk);
    const bool rows = in_rows_ && setting_category_ != kDiagnostics;
    l.rounded_rect({ rows ? kListX : kLeft - 20, rows_y_.value, rows ? kRight - kListX : kRailW, rows ? kRowH : kRailH }, 14,
                   in_rows_ && setting_category_ == kDiagnostics ? kSurface : kRaised);
    for (int i = 0; i < kCategoryCount; ++i) {
        const float y = kListY + kRailPitch * (float) i;
        const bool on = i == setting_category_;
        if (on) {
            l.rounded_rect({ kLeft - 20, y + 16, 4, kRailH - 32 }, 2, kBlue);
        }
        put(l, f.regular, kCategories[i].name, kLeft, y + 42, 26, on ? kInk : kInk2);
    }
    if (setting_category_ == kDiagnostics) {
        draw_logs(l, { kListX, kListY, kRight - kListX, kDockY - 40 - kListY });
        return;
    }
    const SettingCategory & c = kCategories[setting_category_];
    for (int i = 0; i < c.count; ++i) {
        const SettingRow & r = c.rows[i];
        const float y = kListY + kRowPitch * (float) i;
        put(l, f.regular, r.label, kListX + 28, y + 36, 26, kInk);
        // The debug report's row says where the last one went, in place of its help.
        const bool status = r.id == Setting::debug_report && !report_status_.empty();
        std::string help = status ? report_status_ : std::string(r.help);
        if (r.id == Setting::screen_off) {
            help = "Frees " + screen_mb() + " for the model. Hold Triangle for 2 seconds to bring it back";
        }
        put(l, f.regular, f.regular.font->fit(help, 20, kRight - kListX - 340),
             kListX + 28, y + 66, 20, status && report_status_.rfind("Failed", 0) == 0 ? kRed : status ? kInk2 : kInk3);
        const float cy = y + kRowH * 0.5f;
        if (r.id == Setting::screen_off) {
            // PS5LM Lean: which mode it is in, and what it costs.
            put(l, f.regular, "On, " + screen_mb(), kRight - 28, y + 50, 24, in_rows_ && i == setting_row_ ? kInk : kInk2,
                 gfx::Align::right);
        } else if (is_toggle(r.id)) {
            toggle_switch(l, kRight - 28, cy, toggle_value(settings_, r.id));
        } else if (r.id == Setting::storage) {
            put(l, f.regular, full_storage_ ? "Full storage, /data/PS5LM" : app_storage_text(live_.drives, app_storage_gib_),
                 kRight - 28, y + 50, 24, in_rows_ && i == setting_row_ ? kInk : kInk2, gfx::Align::right);
        } else if (!is_action(r.id)) {
            const bool focus = in_rows_ && i == setting_row_;
            const Color c = focus ? kInk : kInk2;
            put(l, f.regular, setting_text(settings_, r.id, live_.models, live_.drives), kRight - 68, y + 50, 24, c,
                 gfx::Align::right);
            // A chevron down: X opens a list of the choices.
            l.line(kRight - 44, cy - 4, kRight - 36, cy + 4, 2.4f, c);
            l.line(kRight - 36, cy + 4, kRight - 28, cy - 4, 2.4f, c);
        } else if (r.id == Setting::clear_scratch && live_.scratch_gib > 0) {
            put(l, f.regular, format("%.1f GB in it", live_.scratch_gib), kRight - 28, y + 50, 22, kInk2,
                 gfx::Align::right);
        }
    }
    // PS5LM Lean: under the screen's rows, where the memory goes, and what
    // turning it off does.
    if (kCategories[setting_category_].rows[0].id == Setting::screen_off) {
        const float x = kListX + 28, w = kRight - kListX - 56;
        float y = kListY + kRowPitch * (float) c.count + 96;
        draw_memory(l, x, y, w);
        y += 120;
        const char * lines[] = {
            "With the screen off, the TV shows the PS5LM Voice picture and the server keeps answering on your network.",
            "It stays off when the app starts again, until you bring it back:",
        };
        for (const char * line : lines) {
            put(l, f.regular, line, x, y, 22, kInk2);
            y += 36;
        }
        wake_line(l, x, y, 22, kInk, false);
    }
}

// A row's choices in a list that drops from it, the one set now ticked.
void Dashboard::draw_picker(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const auto all = choices_of(settings_, kCategories[setting_category_].rows[setting_row_].id, live_.models, live_.drives);
    const int   n = (int) all.size(), shown = std::min(n, 7);
    const float pitch = 64, w = 700, h = (float) shown * pitch + 24;
    const float below = kListY + kRowPitch * (float) setting_row_ + kRowH + 8;
    const Rect  r{ kRight - w, std::min(below, kDockY - 24 - h), w, h };
    l.shadow({ r.x, r.y + 12, r.w, r.h }, 20, 40, kBlack.with_alpha(0.55f));
    l.rounded_rect({ r.x - 1.5f, r.y - 1.5f, r.w + 3, r.h + 3 }, 21.5f, kHair);
    l.rounded_rect(r, 20, kSurface);
    const int first = std::clamp(picker_cursor_ - shown / 2, 0, std::max(0, n - shown));
    for (int i = first; i < first + shown; ++i) {
        const Choice & c = all[(size_t) i];
        const float y = r.y + 12 + pitch * (float) (i - first);
        if (i == picker_cursor_) {
            l.rounded_rect({ r.x + 10, y, r.w - 20, pitch }, 14, kRaised);
        }
        put(l, f.regular, f.regular.font->fit(c.label, 24, r.w - 330), r.x + 32, y + 41, 24, kInk);
        put(l, f.regular, f.regular.font->fit(c.note, 19, 260), r.x + r.w - 76, y + 40, 19, kInk3, gfx::Align::right);
        if (c.current) {
            const float cx = r.x + r.w - 46, cy = y + pitch * 0.5f;
            l.line(cx - 9, cy, cx - 3, cy + 7, 2.6f, kBlue);
            l.line(cx - 3, cy + 7, cx + 9, cy - 7, 2.6f, kBlue);
        }
    }
    if (n > shown) {
        // How far down the list is, as a thin bar.
        const float track = h - 32, thumb = track * (float) shown / (float) n;
        l.rounded_rect({ r.x + r.w - 8, r.y + 16, 3, track }, 1.5f, kHair);
        l.rounded_rect({ r.x + r.w - 8, r.y + 16 + (track - thumb) * (float) first / (float) (n - shown), 3, thumb }, 1.5f, kInk3);
    }
}

void Dashboard::draw_logs(gfx::DrawList & l, const Rect & a) const {
    const ui::Fonts & f = fonts_;
    float x = a.x + 28;
    const char * tabs[] = { "App", "Server" };
    for (int i = 0; i < 2; ++i) {
        const bool on = (i == 1) == log_llama_;
        const float w = put(l, f.regular, tabs[i], x, a.y + 40, 24, on ? kInk : kInk3);
        if (on) {
            l.rounded_rect({ x, a.y + 54, w, 3 }, 1.5f, kBlue);
        }
        x += w + 36;
    }
    put(l, f.regular, log_errors_ ? "Errors and warnings only" : "Every line", a.x + a.w - 28, a.y + 40, 20, kInk3,
         gfx::Align::right);
    const auto & v = log_view();
    const int last = (int) v.size() - log_scroll_;
    const int first = std::max(0, last - kLogLines);
    float y = a.y + 96;
    for (int i = first; i < last; ++i) {
        const int level = log_level(v[(size_t) i]);
        put(l, f.mono, f.mono.font->fit(v[(size_t) i], 18, a.w - 56), a.x + 28, y, 18,
             level == 2 ? kRed : level == 1 ? kInk : kInk2);
        y += kLogLineH;
    }
    if (v.empty()) {
        put(l, f.regular, "Nothing here yet", a.x + 28, a.y + 120, 24, kInk3);
    }
}

// ---- drawing: dialogs and the busy sheet -------------------------------------------------

namespace {

void sheet(gfx::DrawList & l, const Rect & r) {
    l.rounded_rect({ 0, 0, 1920, 1080 }, 0, kBlack.with_alpha(0.62f));
    l.shadow({ r.x, r.y + 16, r.w, r.h }, 24, 48, kBlack.with_alpha(0.5f));
    l.rounded_rect(r, 24, kSurface);
}

}  // namespace

void Dashboard::draw_dialogs(gfx::DrawList & l) const {
    if (screen_ask_ != ScreenAsk::none) {
        if (countdown_ < 0) {
            draw_screen_question(l);  // the countdown covers the whole screen (draw)
        }
        return;
    }
    if (storage_dialog_) {
        draw_storage_dialog(l);
        return;
    }
    if (page_ == kSettingsPage && report_dialog_) {
        draw_report_dialog(l);
        return;
    }
    if (!(page_ == kModelsPage && (market_confirm_ || market_space_))) {
        return;
    }
    const ui::Fonts & f = fonts_;
    const Rect r{ 960 - 440, 540 - 170, 880, 340 };
    sheet(l, r);
    ui::GlyphStyle style = ui::GlyphStyle::dark();
    style.label = kInk2;
    ui::HintLayout layout;
    layout.size = 32;
    layout.text_size = 22;
    layout.cy = r.y + r.h - 56;
    if (market_confirm_) {
        put(l, f.semibold, "Unload the model first?", r.x + 56, r.y + 86, 34, kInk);
        put(l, f.regular, "The download is faster with the memory free. The model can load again after.", r.x + 56, r.y + 136, 22, kInk2);
        const ui::Hint h[] = { { ui::Button::circle, "Cancel" }, { ui::Button::cross, "Keep it loaded" }, { ui::Button::triangle, "Unload and download" } };
        ui::draw_hints(l, fonts_, style, h, 3, r.x + r.w - 56, true, layout);
    } else {
        put(l, f.semibold, "Not enough space there", r.x + 56, r.y + 86, 34, kInk);
        put(l, f.regular, market_dir_.empty() ? "No drive has room for it. Free some space and try again."
                                               : "It fits on " + drive_name(market_dir_) + ".",
             r.x + 56, r.y + 136, 22, kInk2);
        const ui::Hint h[] = { { ui::Button::circle, "Cancel" }, { ui::Button::cross, "Save it there" } };
        ui::draw_hints(l, fonts_, style, h, market_dir_.empty() ? 1 : 2, r.x + r.w - 56, true, layout);
    }
}

// Full storage closed: the app's own storage, or how to open /data to it.
void Dashboard::update_storage_dialog(const hui::InputFrame & input, ui::Feedback & feedback) {
    if (!storage_howto_) {
        if (input.is_pressed(Action::confirm)) {
            storage_dialog_ = false;
            request_ = { Request::storage_app, "" };
            feedback.play(Cue::select);
        } else if (input.is_pressed(Action::north)) {
            storage_howto_ = true;
            feedback.play(Cue::tab);
        } else if (input.is_pressed(Action::back)) {
            storage_dialog_ = false;  // asked again at the next start
            feedback.play(Cue::modal_close);
        }
        return;
    }
    if (input.is_pressed(Action::confirm)) {
        storage_dialog_ = false;
        request_ = { Request::restart, "" };
        feedback.play(Cue::launch);
    } else if (input.is_pressed(Action::back)) {
        storage_howto_ = false;
        feedback.play(Cue::back);
    }
}

void Dashboard::draw_storage_dialog(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const Rect r{ 960 - 580, 540 - 300, 1160, 600 };
    sheet(l, r);
    const float text_w = r.w - 112;
    float y = r.y + 150;
    const auto para = [&](const std::string & text, Color c) {
        for (const auto & line : wrap(f.regular, text, 23, text_w)) {
            put(l, f.regular, line, r.x + 56, y, 23, c);
            y += 34;
        }
        y += 18;
    };
    std::vector<ui::Hint> h;
    if (!storage_howto_) {
        put(l, f.semibold, "Full storage is closed to PS5LM Voice", r.x + 56, r.y + 86, 34, kInk);
        para("The HEN did not open the console's storage (/data) to the app, so PS5LM Voice cannot keep its models, "
             "settings and logs there.", kInk2);
        para("It can use its own storage instead: up to " + format("%.0f GB", app_storage_gib_) +
             ", enough for one large model or a few small ones. Downloads that do not fit are refused before they start.",
             kInk2);
        para("Or open full storage to it and start again; models saved in the app's storage stay in the library.", kInk3);
        h = { { ui::Button::circle, "Not now" }, { ui::Button::triangle, "How to open it" },
              { ui::Button::cross, "Use the app's storage" } };
    } else {
        put(l, f.semibold, "Open full storage to PS5LM Voice", r.x + 56, r.y + 86, 34, kInk);
        static const std::string title = title_id();  // read once: this sheet redraws every frame
        para("OnionHEN: over FTP, add " + title + " to exact_title_ids in the [app_jailbreak] section of "
             "/data/OnionHEN/config.ini.", kInk2);
        para("etaHEN opens /data to homebrew before it starts; if it did not, load etaHEN again, up to date.", kInk2);
        para("Other HENs: allow " + title + " in their app jailbreak list.", kInk2);
        para("Then start PS5LM Voice again: it asks the HEN for /data at every start.", kInk3);
        h = { { ui::Button::circle, "Back" }, { ui::Button::cross, "Start again" } };
    }
    ui::GlyphStyle style = ui::GlyphStyle::dark();
    style.label = kInk2;
    ui::HintLayout layout;
    layout.size = 32;
    layout.text_size = 22;
    layout.cy = r.y + r.h - 56;
    ui::draw_hints(l, fonts_, style, h.data(), (int) h.size(), r.x + r.w - 56, true, layout);
}

// Before the first debug report: what goes in it, and what never does.
void Dashboard::draw_report_dialog(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const Rect r{ 960 - 560, 540 - 330, 1120, 660 };
    sheet(l, r);
    put(l, f.semibold, "What a debug report holds", r.x + 56, r.y + 86, 34, kInk);
    const char * items[] = {
        "The app's version and the console's system software",
        "Where PS5LM Voice keeps its files, the drives and their free space",
        "The models on this PS5, their plans, and the downloads",
        "Your settings and the server's arguments",
        "The last 256 KiB of app.log and of llama.log",
        "Which payload ports answer on this console (ELF loader, FTP, kernel log, ...)",
    };
    float y = r.y + 150;
    for (const char * it : items) {
        l.circle(r.x + 66, y - 8, 4, kInk3);
        put(l, f.regular, it, r.x + 86, y, 22, kInk2);
        y += 40;
    }
    put(l, f.regular, settings_.report_kernel_log ? "And the last 1 MiB of the kernel log, as you switched on."
                                                   : "Not the kernel log, unless you switch it on below.",
         r.x + 86, y, 22, kInk2);
    y += 56;
    for (const auto & line : wrap(f.regular, "API keys and tokens are taken out. The logs can still name your models and the "
                                             "addresses of the devices that used the server; read the file before you post it.",
                                  22, r.w - 112)) {
        put(l, f.regular, line, r.x + 56, y, 22, kInk3);
        y += 32;
    }
    ui::GlyphStyle style = ui::GlyphStyle::dark();
    style.label = kInk2;
    ui::HintLayout layout;
    layout.size = 32;
    layout.text_size = 22;
    layout.cy = r.y + r.h - 56;
    const ui::Hint h[] = { { ui::Button::circle, "Cancel" }, { ui::Button::cross, "Create the report" } };
    ui::draw_hints(l, fonts_, style, h, 2, r.x + r.w - 56, true, layout);
}

// Slow work, often the app restarting for a model: the whole screen, the
// name and what is happening, and a bar that keeps moving so a pause never
// looks like a hang.
void Dashboard::draw_busy(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    l.rounded_rect({ 0, 0, 1920, 1080 }, 0, kBase);
    const float ps5 = f.semibold.measure("PS5", 120), lm = f.semibold.measure("LM", 120),
                voice = f.semibold.measure(" Voice", 120);
    const float x = 960 - (ps5 + lm + voice) * 0.5f;
    put(l, f.semibold, "PS5", x, 500, 120, kInk);
    put(l, f.semibold, "LM", x + ps5, 500, 120, kBlue);
    put(l, f.semibold, " Voice", x + ps5 + lm, 500, 120, kInk);
    put(l, f.regular, f.regular.font->fit(busy_title_, 32, 1400), 960, 600, 32, kInk, gfx::Align::center);
    put(l, f.regular, f.regular.font->fit(busy_text_, 24, 1400), 960, 646, 24, kInk2, gfx::Align::center);
    const float track = 360, seg = 96;
    const float t = 0.5f + 0.5f * std::sin(clock_ * 2.4f);
    l.rounded_rect({ 960 - track * 0.5f, 700, track, 3 }, 1.5f, kHair);
    l.rounded_rect({ 960 - track * 0.5f + (track - seg) * t, 700, seg, 3 }, 1.5f, kInk);
}

// ---- PS5LM Lean: the screen's memory, and turning the screen off --------------------------

namespace {
constexpr float kCountdown = 5.0f;  // seconds on the TV before the screen goes off
}  // namespace

// The screen's share, in the words the console's own screens use: "370 MB",
// rounded to tens (one start measures a few MB differently from the next).
std::string Dashboard::screen_mb() const {
    const double mb = std::round(screen_.cost_gib * 1024.0 / 10.0) * 10.0;
    return (screen_.measured ? "" : "about ") + format("%.0f MB", mb);
}

// "Hold (Triangle) for 2 seconds to bring the screen back", with the glyph.
float Dashboard::wake_line(gfx::DrawList & l, float x, float y, float size, Color c, bool center) const {
    const ui::Fonts & f = fonts_;
    const char * before = "Hold ";
    const char * after = " for 2 seconds to bring the screen back";
    const float glyph = size * 1.25f;
    const float w = f.regular.measure(before, size) + glyph + f.regular.measure(after, size);
    float at = center ? x - w * 0.5f : x;
    at += put(l, f.regular, before, at, y, size, c);
    ui::GlyphStyle style = ui::GlyphStyle::dark();
    ui::draw_button(l, fonts_, style, ui::Button::triangle, at, y - size * 0.36f, glyph);
    at += glyph;
    put(l, f.regular, after, at, y, size, c);
    return w;
}

// One bar: what the model (and the app) holds, the screen's share in blue,
// and what is free, with a key under it. Narrow, the key leaves out the model:
// the line above it says how much is used.
void Dashboard::draw_memory(gfx::DrawList & l, float x, float y, float w) const {
    const ui::Fonts & f = fonts_;
    const double pool = std::max(0.0, live_.pool_gib), free = std::clamp(live_.free_gib, 0.0, pool);
    const double used = pool - free;
    const double scr = std::min(used, screen_.cost_gib), held = used - scr;
    put(l, f.regular, "Memory", x, y, 22, kInk2);
    put(l, f.regular, format("%.1f", used) + " of " + gb(pool), x + w, y, 22, kInk, gfx::Align::right);
    const float by = y + 16, bh = 6;
    l.rounded_rect({ x, by, w, bh }, 3, kHair);
    if (pool > 0) {
        const float wm = w * (float) (held / pool), ws = w * (float) (scr / pool);
        if (wm > 2) {
            l.rounded_rect({ x, by, wm, bh }, 3, kInk.with_alpha(0.8f));
        }
        if (ws > 2) {
            l.rounded_rect({ x + wm + 3, by, std::max(4.0f, ws - 3), bh }, 3, kBlue);
        }
    }
    const float ky = y + 54;
    float kx = x;
    const auto key = [&](Color swatch, const std::string & label, Color text) {
        l.rounded_rect({ kx, ky - 13, 12, 12 }, 3, swatch);
        kx += 20;
        kx += put(l, f.regular, label, kx, ky, 20, text) + 30;
    };
    if (w >= 600 && held >= 0.05) {
        key(kInk.with_alpha(0.8f), (live_.model_label.empty() ? "App " : "Model ") + gb(held), kInk2);
    }
    key(kBlue, "Screen " + format("%.0f MB", std::round(screen_.cost_gib * 1024.0 / 10.0) * 10.0), kInk);
    key(kHair, "Free " + gb(free), kInk2);
}

bool Dashboard::start_sleep_countdown() {
    if (screen_ask_ != ScreenAsk::none || busy()) {
        return false;
    }
    screen_ask_ = ScreenAsk::sleep;
    screen_model_.clear();
    countdown_ = kCountdown;
    return true;
}

void Dashboard::ask_screen_off(ScreenAsk what, std::string model, ui::Feedback & feedback) {
    screen_ask_ = what;
    screen_model_ = std::move(model);
    countdown_ = -1;
    feedback.play(Cue::modal_open);
}

// The question, then five seconds on the TV that say how to bring the screen
// back, then the request. Circle steps back at either stage; Cross skips the wait.
void Dashboard::update_screen_off(const hui::InputFrame & input, float dt, ui::Feedback & feedback) {
    if (busy()) {
        screen_ask_ = ScreenAsk::none;  // a restart began meanwhile: nothing to turn off
        countdown_ = -1;
        return;
    }
    if (countdown_ < 0) {
        if (input.is_pressed(Action::confirm)) {
            countdown_ = kCountdown;
            feedback.play(Cue::select);
        } else if (screen_ask_ == ScreenAsk::load_images && input.is_pressed(Action::north)) {
            request_ = { Request::load, screen_model_ };  // text only, the screen stays
            screen_ask_ = ScreenAsk::none;
            feedback.play(Cue::launch);
        } else if (input.is_pressed(Action::back) || input.is_pressed(Action::menu)) {
            screen_ask_ = ScreenAsk::none;
            feedback.play(Cue::modal_close);
        }
        return;
    }
    // The sleep timer's countdown: any button keeps the screen on.
    const bool cancel = screen_ask_ == ScreenAsk::sleep ? (input.pressed != 0 || input.nav != Direction::none)
                                                        : input.is_pressed(Action::back);
    if (cancel) {
        screen_ask_ = ScreenAsk::none;
        countdown_ = -1;
        feedback.play(Cue::back);
        return;
    }
    const float was = countdown_;
    countdown_ -= dt;
    if (std::ceil(countdown_) < std::ceil(was) && countdown_ > 0) {
        feedback.play(Cue::focus);  // a tick each second
    }
    if (input.is_pressed(Action::confirm) || countdown_ <= 0) {
        request_ = screen_ask_ == ScreenAsk::load || screen_ask_ == ScreenAsk::load_images
                                                        ? Request{ Request::load_screen_off, screen_model_ }
                 : screen_ask_ == ScreenAsk::off_reload ? Request{ Request::screen_off_restart, "" }
                 : screen_ask_ == ScreenAsk::sleep      ? Request{ Request::screen_sleep, "" }
                                                        : Request{ Request::screen_off, "" };
        screen_ask_ = ScreenAsk::none;
        countdown_ = -1;
        feedback.play(Cue::select);
    }
}

void Dashboard::draw_screen_question(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const Rect r{ 960 - 600, 540 - 235, 1200, 470 };
    sheet(l, r);
    const float text_w = r.w - 112;
    float y = r.y + 150;
    const auto para = [&](const std::string & text, Color c) {
        for (const auto & line : wrap(f.regular, text, 24, text_w)) {
            put(l, f.regular, line, r.x + 56, y, 24, c);
            y += 36;
        }
        y += 16;
    };
    std::string name, build;
    split_name(base_label(screen_model_), &name, &build);
    const std::string where = live_.address.empty() ? std::string("on port 8081") : "at http://" + live_.address;
    if (screen_ask_ == ScreenAsk::load) {
        put(l, f.semibold, f.semibold.font->fit("Load " + name + " with the screen off?", 34, text_w), r.x + 56, r.y + 86, 34, kInk);
        double short_by = 0.1;
        for (const auto & m : live_.models) {
            if (m.file == screen_model_) {
                short_by = std::max(0.1, m.need_gib - screen_.budget_gib);
            }
        }
        para(format("It is %.1f GB short with the screen on. ", short_by) + "The screen holds " + screen_mb() +
                 "; with it off, the model fits.", kInk2);
    } else if (screen_ask_ == ScreenAsk::load_images) {
        put(l, f.semibold, f.semibold.font->fit("Load " + name + " with images?", 34, text_w), r.x + 56, r.y + 86, 34, kInk);
        double images = 0;
        for (const auto & m : live_.models) {
            if (m.file == screen_model_) {
                images = m.images_gib;
            }
        }
        para("With images it needs " + gb(images) + ", and with the screen on this PS5 has " + gb(screen_.budget_gib) +
                 " for a model, so it would load text only. The screen holds " + screen_mb() +
                 "; with it off, the model reads images.", kInk2);
    } else if (screen_ask_ == ScreenAsk::off_reload) {
        put(l, f.semibold, "Turn the screen off and reload the model?", r.x + 56, r.y + 86, 34, kInk);
        para("The app restarts without the screen, so the model loads again with the screen's " + screen_mb() +
                 " in one piece: for a model that needs all of it. About 20 seconds.", kInk2);
    } else {
        put(l, f.semibold, "Turn the screen off?", r.x + 56, r.y + 86, 34, kInk);
        para("The screen holds " + screen_mb() + " of this PS5's " + gb(live_.pool_gib) +
                 ". With it off, the model gets that memory.", kInk2);
    }
    para("The TV shows the PS5LM Voice picture. The server keeps answering " + where + ".", kInk2);
    wake_line(l, r.x + 56, y, 24, kInk, false);
    y += 52;
    put(l, f.regular, "It stays off when the app starts again.", r.x + 56, y, 22, kInk3);
    ui::GlyphStyle style = ui::GlyphStyle::dark();
    style.label = kInk2;
    ui::HintLayout layout;
    layout.size = 32;
    layout.text_size = 22;
    layout.cy = r.y + r.h - 56;
    if (screen_ask_ == ScreenAsk::load_images) {
        const ui::Hint h[] = { { ui::Button::circle, "Cancel" }, { ui::Button::triangle, "Text only, screen on" },
                               { ui::Button::cross, "Images, screen off" } };
        ui::draw_hints(l, fonts_, style, h, 3, r.x + r.w - 56, true, layout);
        return;
    }
    const ui::Hint h[] = { { ui::Button::circle, "Cancel" },
                           { ui::Button::cross, screen_ask_ == ScreenAsk::load ? "Load, screen off" : "Turn off" } };
    ui::draw_hints(l, fonts_, style, h, 2, r.x + r.w - 56, true, layout);
}

// The whole screen for five seconds: when it goes, and how to bring it back.
void Dashboard::draw_countdown(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    l.rounded_rect({ 0, 0, 1920, 1080 }, 0, kBase);
    const int left = std::max(1, (int) std::ceil(countdown_));
    if (screen_ask_ == ScreenAsk::sleep) {
        // The sleep timer: any button keeps it on, and any button wakes it.
        put(l, f.display, format("Screen sleeps in %.0f", (double) left), 960, 470, 88, kInk, gfx::Align::center);
        put(l, f.regular, "Press any button to keep it on", 960, 560, 32, kInk, gfx::Align::center);
        put(l, f.regular, "Asleep, it gives the model " + screen_mb() + "; any button wakes it.", 960, 612, 24, kInk2,
            gfx::Align::center);
        const float track = 360, done = tween::clamp01(1.0f - countdown_ / kCountdown);
        l.rounded_rect({ 960 - track * 0.5f, 660, track, 3 }, 1.5f, kHair);
        l.rounded_rect({ 960 - track * 0.5f, 660, std::max(3.0f, track * done), 3 }, 1.5f, kInk);
        return;
    }
    put(l, f.display, format("Screen off in %.0f", (double) left), 960, 470, 88, kInk, gfx::Align::center);
    wake_line(l, 960, 560, 32, kInk, true);
    std::string then;
    if (screen_ask_ == ScreenAsk::load || screen_ask_ == ScreenAsk::load_images) {
        std::string name, build;
        split_name(base_label(screen_model_), &name, &build);
        then = "Then " + name + (screen_ask_ == ScreenAsk::load_images ? " loads with images, with the screen's memory."
                                                                        : " loads with the screen's memory.");
    } else if (screen_ask_ == ScreenAsk::off_reload) {
        then = "Then the model loads again with the screen's memory, about 20 seconds.";
    } else {
        then = live_.address.empty() ? std::string("The model keeps running.")
                                     : "The model keeps running at http://" + live_.address + ".";
    }
    put(l, f.regular, then, 960, 612, 24, kInk2, gfx::Align::center);
    const float track = 360, done = tween::clamp01(1.0f - countdown_ / kCountdown);
    l.rounded_rect({ 960 - track * 0.5f, 660, track, 3 }, 1.5f, kHair);
    l.rounded_rect({ 960 - track * 0.5f, 660, std::max(3.0f, track * done), 3 }, 1.5f, kInk);
    ui::GlyphStyle style = ui::GlyphStyle::dark();
    style.label = kInk2;
    ui::HintLayout layout;
    layout.size = 32;
    layout.text_size = 22;
    layout.cy = 760;
    const ui::Hint h[] = { { ui::Button::circle, "Stay on" }, { ui::Button::cross, "Off now" } };
    const float hw = ui::measure_hints(fonts_, h, 2, layout);
    ui::draw_hints(l, fonts_, style, h, 2, 960 + hw * 0.5f, true, layout);
}

}  // namespace ps5lm
