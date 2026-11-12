// PC preview of the PS5LM dashboard: the same drawing code as the console,
// rendered off-screen through Mesa's surfaceless EGL to PNG files, with
// sample data shaped like a real run (Qwen 3.8 27B at 64k on the GPU).
//
//   scripts/preview-app-ui.sh            writes build/app-preview/*.png
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard.hpp"

#include "core/save_file.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool open_context() {
    auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                                     : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API)) {
        return false;
    }
    const EGLint attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                             EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    return context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer & r, const std::string & path, hui::gfx::Font * font, hui::ui::FontRef * ref) {
    std::string data;
    if (!hui::save::read_file(path, &data) || !font->load(data)) {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font    = font;
    ref->texture = r.batch().create_font_texture(*font);
    return true;
}

// A run as the console measured it: Qwen3.8-27B UD-Q2_K_XL, 64k, q4_0 cache.
float g_loading = -1;  // >= 0: draw the loading state at this progress
bool  g_decision = false;  // d1 Omni 600M instead of Qwen 3.8
bool  g_empty = false;     // nothing loaded
std::string g_phase;       // the app still starting
// PS5LM Lean: Home's other states.
bool  g_fresh = false;       // seconds after the start: no sensor read yet, no history
bool  g_no_replies = false;  // ready, nothing asked yet but the app's own vision warm-up
bool  g_failed = false;      // one request ended in an error
// PS5LM Lean: the Home chat's states.
bool  g_others_done = false;  // every other device's request has finished
bool  g_photo_run = false;    // the PC's photo run holds the model
bool  g_decision_self = false;  // a decision model, and a message this PS5 sent it
ps5lm::ChatView g_chat;       // the conversation and the microphone, as the engine would give them

// A voice's level, 0..1: syllables in words, words in phrases.
float speech_level(float t) {
    const float phrase = std::max(0.0f, std::sin(t * 1.1f) + 0.35f);  // pauses between phrases
    const float syllable = std::pow(std::fabs(std::sin(t * 13.0f)), 0.6f) * (0.55f + 0.45f * std::sin(t * 2.3f + 1.0f));
    return std::clamp(0.03f + 0.8f * std::min(1.0f, phrase) * syllable + 0.03f * std::sin(t * 41.0f), 0.0f, 1.0f);
}

ps5lm::ChatTurn turn(bool user, const char * text, bool spoken = false) {
    ps5lm::ChatTurn c;
    c.role = user ? ps5lm::ChatTurn::Role::user : ps5lm::ChatTurn::Role::assistant;
    c.text = text;
    c.spoken = spoken;
    return c;
}

// The roof conversation, from the live take on the console (2026-11-11).
std::vector<ps5lm::ChatTurn> roof_chat() {
    return {
        turn(true, "What should I check before buying a house that needs a new roof?", true),
        turn(false, "Start with what is under the shingles, because that is where the money goes.\n\n"
                    "- **Layers**: two or more layers of shingles mean a tear-off, which costs more than a re-roof.\n"
                    "- **Decking**: soft spots or sagging between the rafters mean rotten boards.\n"
                    "- **Attic**: water stains, daylight through the boards, or mould on the underside.\n"
                    "- **Flashing** around the chimney, vents and valleys, where most leaks start.\n\n"
                    "Get a roofer's written quote before you make an offer, and ask the seller for the roof's age and "
                    "any insurance claims."),
    };
}

// A long chat: six older messages no longer fit the context.
std::vector<ps5lm::ChatTurn> long_chat() {
    std::vector<ps5lm::ChatTurn> v = roof_chat();
    const char * qa[][2] = {
        { "How much does a tear-off usually add?",
          "Usually $1 to $2 a square foot more than shingling over, or about $2,000 to $4,000 on a 2,000 square foot "
          "roof: the labour, the dumpster and the disposal fees." },
        { "And if the decking is rotten?",
          "Replacing decking runs about $70 to $100 a sheet installed. A roofer will not know how many sheets until "
          "the old shingles are off, so ask for the per-sheet price in writing." },
        { "Is a metal roof worth it in Alabama?",
          "Often, for a house you plan to keep. Metal costs two to three times as much as shingles but lasts 40 to 70 "
          "years, sheds heat in summer, and stands up to hail better. For a flip, shingles usually win: buyers pay "
          "for a new roof, not for its material." },
        { "What about the gutters?",
          "Replace them with the roof if they are sagging or pulling away: it is cheaper while the crew and the "
          "ladders are there. Seamless aluminium gutters run about $8 to $12 a foot." },
        { "Can you sum it up in one line for my notes?",
          "Check layers, decking, attic and flashing; get a written quote with a per-sheet decking price before you "
          "offer; shingles for a flip, metal for a keeper; gutters with the roof." },
    };
    for (const auto & p : qa) {
        v.push_back(turn(true, p[0], true));
        v.push_back(turn(false, p[1]));
    }
    v[6].spoken = false;  // one of them typed
    return v;
}

ps5lm::Live sample(float seconds, bool generating) {
    ps5lm::Live L;
    L.state       = generating ? ps5lm::ServerState::generating : ps5lm::ServerState::ready;
    L.address     = "192.168.1.19:8081";
    L.model_label = "Qwen3.8-27B-UD-Q2_K_XL";
    L.model_arch  = "qwen35";
    L.preset      = "Qwen 3.8";
    L.model_gib   = 9.15;
    L.ctx         = 65536;
    L.kv_type     = "q4_0";
    L.kv_gib      = 1.13;
    L.model_file  = "/data/PS5LM/models/Qwen3.8-27B-UD-Q2_K_XL.gguf";
    L.plan        = "64k context, q4_0 cache: 11.3 of 11.4 GiB";
    L.args = { "-m", L.model_file, "-ngl", "999", "-fit", "off", "-lm", "none", "-c", "65536", "-fa", "on",
               "-ctk", "q4_0", "-ctv", "q4_0", "-b", "256", "-ub", "64", "-np", "1", "-ctxcp", "1", "-cram", "0",
               "--host", "0.0.0.0", "--port", "8081", "--temp", "0.6", "--top-p", "0.95", "--top-k", "20",
               "--min-p", "0", "--metrics", "--mmproj", "/data/PS5LM/models/mmproj-Qwen3.8-27B-F16.gguf" };
    L.soc_temp    = 61;
    L.cpu_temp    = 54;
    L.soc_power_w = 86;
    L.energy_wh   = 0.42;
    L.fan         = 0.23f;
    L.temps = { { "cpu", 54 }, { "soc0", 61 }, { "soc1", 60 }, { "soc2", 59 }, { "soc3", 62 }, { "soc4", 61 },
                { "soc5", 60 }, { "soc6", 58 } };
    L.cpu_ghz     = 3.5f;
    L.drives = { { "Internal storage", "/data/PS5LM/models", 8, 825 }, { "M.2 drive", "/mnt/ext0/PS5LM/models", 1520, 1863 }, { "USB drive", "/mnt/usb0/PS5LM/models", 455, 477 } };
    // PS5LM Lean: lean.12 on the console, screen on, Q2_K_XL ready (M, 2026-11-11).
    L.pool_gib    = 12.1875;
    L.free_gib    = 0.69;
    L.heap_gib    = 0.88;
    L.cpus        = 13;
    L.uptime_s    = 754 + seconds;
    L.tokens      = 4182;
    L.prompt_tokens = 9120;
    L.requests    = 14;
    L.served      = 14;
    L.ctx_used    = 2318;
    L.prompt_tps  = 45.4f;
    for (int i = 0; i < 120; ++i) {
        const bool on = (i / 14) % 3 != 0 || i > 104;
        const float tps = on ? 20.4f + 0.6f * std::sin(i * 0.7f) : 0.0f;
        L.gen_history.push_back(tps);
        L.gpu_history.push_back(on ? 0.92f + 0.05f * std::sin(i * 0.3f) : 0.03f);
        L.cpu_history.push_back(on ? 0.11f + 0.03f * std::sin(i * 0.5f) : 0.02f);
        L.temp_history.push_back(on ? 60.0f + 2.0f * std::sin(i * 0.2f) : 52.0f);
        L.power_history.push_back(on ? 85.0f + 1.5f * std::sin(i * 0.4f) : 23.0f);
        // An hour: idle, then a long session with the 27B from minute 20.
        const bool busy = i > 40;
        L.temp_hour.push_back(busy ? 58.0f + 3.0f * std::sin(i * 0.15f) : 50.0f + 0.5f * std::sin(i * 0.4f));
        L.power_hour.push_back(busy ? 70.0f + 12.0f * std::sin(i * 0.23f) : 24.0f);
        L.gpu_hour.push_back(busy ? 0.7f + 0.2f * std::sin(i * 0.31f) : 0.02f);
        L.cpu_hour.push_back(busy ? 0.1f + 0.03f * std::sin(i * 0.5f) : 0.02f);
    }
    {
        // Requests from the network: two answered, the newest still streaming.
        ps5lm::Exchange a;
        a.id = 12; a.client = "192.168.1.31"; a.path = "/v1/messages"; a.status = 200; a.seconds = 12.8;
        a.asked = "Rename the loop variable in parse_args to something clearer and explain why";
        a.answered = "Renamed i to arg_index: it says what the loop walks, and the next reader does not have to look up what i counts.";
        L.activity.push_back(a);
        a.id = 13; a.client = "192.168.1.24"; a.path = "/v1/chat/completions"; a.status = 200; a.seconds = 3.4;
        a.asked = "Write a haiku about a console that runs a language model";
        a.answered = "Fans hum through the night, a console dreams in tokens, words bloom on the TV.";
        L.activity.push_back(a);
        a.id = 14; a.client = "192.168.1.24"; a.path = "/v1/chat/completions"; a.status = generating ? 0 : 200; a.seconds = 9.1;
        a.asked = "How does the PS5's GPU run llama.cpp?";
        a.answered = "Through Vulkan. Mihawk-99's RADV port gives the console a full Vulkan 1.4 driver, so llama.cpp's own Vulkan "
                     "backend runs unchanged: every matrix product becomes a compute shader that ACO compiles on the console, and "
                     "the weights sit in the 12 GB the GPU and CPU share";
        L.activity.push_back(a);
    }
    L.gen_tps  = generating ? 20.7f : 0.0f;
    L.gpu_busy = generating ? 0.94f : 0.02f;
    L.cpu_use  = generating ? 0.12f : 0.02f;
    // PS5LM Lean: one model short by less than the screen holds (it fits with
    // the screen off), one too big either way.
    const char * files[] = { "Qwen3.8-27B-UD-IQ1_S", "Qwen3.8-27B-UD-IQ2_S", "Qwen3.8-27B-UD-IQ2_XXS",
                             "Qwen3.8-27B-UD-Q2_K_XL", "Mistral-Small-3.2-24B-Instruct-Q3_K_M", "gpt-oss-20b-MXFP4" };
    const char * plans[] = { "64k context, q8_0 cache: 8.9 of 11.8 GiB", "64k context, q8_0 cache: 11.0 of 11.8 GiB",
                             "text only (images need 12.0 GiB): 64k context, q8_0 cache: 9.9 of 11.8 GiB free",
                             "64k context, q4_0 cache: 11.3 of 11.8 GiB",
                             "needs 12.0 GiB with a 4k context; 11.8 GiB is free",
                             "needs 12.9 GiB with a 4k context; 11.8 GiB is free" };
    const double sizes[] = { 5.77, 7.80, 6.77, 9.15, 10.7, 11.3 };
    const double needs[] = { 0, 0, 0, 0, 12.0, 12.9 };
    for (int i = 0; i < 6; ++i) {
        ps5lm::ModelRow m;
        m.file = std::string("/data/PS5LM/models/") + files[i] + ".gguf";
        m.drive = "Internal storage";
        m.label = files[i];
        m.plan = plans[i];
        m.size_gib = sizes[i];
        m.fits = i < 4;
        m.need_gib = needs[i];
        m.fits_screen_off = i == 4;
        m.text_only = i == 2;  // lean.16: text only now, images with the screen off
        m.images_gib = i == 2 ? 12.0 : 0;
        m.images_screen_off = i == 2;
        m.current = i == 3;
        m.preset = i < 4 ? "Qwen 3.8" : i == 4 ? "Mistral" : "gpt-oss";
        L.models.push_back(m);
    }
    ps5lm::ModelRow usb;
    usb.file = "/mnt/usb0/PS5LM/models/Ornith-1.5-9B-Q4_K_M.gguf";
    usb.label = "Ornith-1.5-9B-Q4_K_M";
    usb.plan = "64k context, q8_0 cache: 8.6 of 11.0 GiB";
    usb.preset = "Ornith 1.5";
    usb.size_gib = 5.38;
    usb.fits = true;
    usb.drive = "USB drive";
    L.models.push_back(usb);
    usb.file = "/mnt/usb0/PS5LM/models/gemma-4-12b-it-Q3_K_M.gguf";
    usb.label = "gemma-4-12b-it-Q3_K_M";
    usb.plan = "64k context, q8_0 cache: 9.1 of 11.0 GiB";
    usb.preset = "Gemma 4";
    usb.size_gib = 5.30;
    L.models.push_back(usb);
    L.models[0].decision = "";
    {
        ps5lm::ModelRow d;
        d.file = "/data/PS5LM/models/d1-omni-600M-BF16.gguf";
        d.label = "d1-omni-600M-BF16";
        d.plan = "decision model (lfm2-d1-omni), 4k context, f16 cache, images: 2.9 of 11.0 GiB";
        d.decision = "lfm2-d1-omni";
        d.size_gib = 0.71;
        d.fits = true;
        d.drive = "Internal storage";
        L.models.push_back(d);
    }
    if (g_decision) {
        for (auto & m : L.models) {
            m.current = m.decision == "lfm2-d1-omni";
        }
        L.state = generating ? ps5lm::ServerState::generating : ps5lm::ServerState::ready;
        L.model_label = "d1-omni-600M-BF16";
        L.model_arch = "lfm2";
        L.decision = "lfm2-d1-omni";
        L.preset = "";
        L.model_gib = 0.71;
        L.ctx = 4096;
        L.ctx_used = 793;
        L.prompt_tps = 2140.0f;
        L.gen_tps = 0;
        L.tokens = 0;
        L.prompt_tokens = 88412;
        L.requests = 131;
        L.free_gib = 8.6;
        L.activity.clear();
        ps5lm::Exchange a;
        a.id = 130; a.client = "192.168.1.40"; a.path = "/v1/systemone"; a.status = 200; a.seconds = 0.3; a.questions = 3;
        a.asked = "3 questions: {\"from\": \"promo@cheap-meds.biz\", \"subject\": \"URGENT: you won a free iPhone!!!\"}";
        a.answered = "spam yes 1.00, intent sell, urgency 1.02";
        L.activity.push_back(a);
        a.id = 131; a.asked = "7 questions, 1 image"; a.answered = "text no 0.01, color black, ad no 0.01, people no 0.00, mood 1.00";
        a.seconds = 2.9; a.questions = 7; L.activity.push_back(a);
        if (g_decision_self) {
            // PS5LM Lean: a message sent from Home's box goes to the decision model on its own.
            a.id = 132; a.client = "127.0.0.1"; a.questions = 2; a.seconds = 0.4;
            a.asked = "2 questions: I was charged twice for my order and nobody answers my emails";
            a.answered = "intent refund, angry yes 0.91";
            L.activity.push_back(a);
        }
    }
    if (g_empty) {
        L.state = ps5lm::ServerState::no_model;
        L.model_label.clear();
        L.decision.clear();
        L.free_gib = 11.79;  // the screen and the app's own heap hold the rest
        L.requests = L.tokens = L.prompt_tokens = 0;
        for (auto & m : L.models) {
            m.current = false;
        }
    }
    if (g_loading >= 0) {
        L.state = ps5lm::ServerState::loading;
        L.load_progress = g_loading;
        L.phase = g_phase;
        L.free_gib = 11.79 - g_loading * (L.model_gib + L.kv_gib);
        L.gen_tps = 0;
        L.gpu_busy = 0;
    }
    if (g_fresh) {
        L.soc_temp = L.cpu_temp = -1000;
        L.soc_power_w = 0;
        L.cpu_ghz = 0;
        L.fan = -1;
        L.energy_wh = 0;
        L.uptime_s = 4 + seconds;
        L.temps.clear();
        L.temp_history.clear();
        L.power_history.clear();
        L.gpu_history.clear();
        L.cpu_history.clear();
        L.gen_history.clear();
        L.temp_hour.clear();
        L.power_hour.clear();
        L.gpu_hour.clear();
        L.cpu_hour.clear();
        L.activity.clear();
        L.served = L.requests = L.tokens = L.prompt_tokens = 0;
        L.cpu_use = 0.31f;
    }
    if (g_no_replies) {
        // The TV after a start with the 27B (lean.13, 2026-11-11): only the app's
        // own vision warm-up has run, which the feed and the counts leave out.
        L.activity.clear();
        ps5lm::Exchange w;
        w.id = 1; w.client = "127.0.0.1"; w.path = "/v1/chat/completions"; w.status = 200; w.seconds = 21.0;
        w.asked = "[image] Warm-up."; w.answered = "OK";
        L.activity.push_back(w);
        L.served = 0;  // stats.cpp leaves the warm-up out
        L.requests = 1;
        L.prompt_tokens = 0;  // lean.22: its 1,337 tokens in and 1 out are left out too,
        L.tokens = 0;         // and the context its image fills
        L.ctx_used = 0;
        L.uptime_s = 64 + seconds;
        L.energy_wh = 0.31;
        L.gen_history.assign(120, 0.0f);
        for (size_t i = 0; i < L.gpu_history.size(); ++i) {
            L.gpu_history[i] = i > 70 && i < 92 ? 0.95f : 0.02f;  // the warm-up, about 20 s of GPU
            L.power_history[i] = i > 70 && i < 92 ? 84.0f : 24.0f;
            L.temp_history[i] = i > 70 ? 50.0f + std::min(8.0f, (float) (i - 70) * 0.4f) : 49.0f;
            L.cpu_history[i] = 0.02f;
        }
        L.soc_temp = L.temp_history.back();
        L.soc_power_w = L.power_history.back();
    }
    if (g_others_done) {
        for (auto & e : L.activity) {
            e.status = e.status == 0 ? 200 : e.status;
        }
    }
    if (g_photo_run) {
        // The PC's photo run: a listing photo the model is reading now.
        ps5lm::Exchange p;
        p.id = 15; p.client = "192.168.1.19"; p.path = "/v1/chat/completions"; p.status = 0; p.seconds = 0;
        p.asked = "[image] Rate the kitchen's condition in this listing photo";
        L.activity.push_back(p);
    }
    if (g_failed) {
        ps5lm::Exchange f;
        f.id = 15; f.client = "192.168.1.31"; f.path = "/v1/messages"; f.status = 400; f.seconds = 0.1;
        f.asked = "Summarise this repository";
        f.answered = "The request asks for 70,412 tokens; the context holds 65,536";
        L.activity.push_back(f);
    }
    return L;
}

}  // namespace

int main(int argc, char ** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <kit assets dir> <output dir> <app assets dir>\n", argv[0]);
        return 2;
    }
    const std::string assets = argv[1], out = argv[2], app = argv[3];
    const int W = 1920, H = 1080;
    if (!open_context()) {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    hui::gfx::Renderer renderer;
    hui::gfx::Font regular, semibold, display, mono, pixel, hand;
    hui::ui::Fonts fonts;
    if (!renderer.init() || !load_font(renderer, app + "/fonts/manrope-500.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, app + "/fonts/manrope-600.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, app + "/fonts/manrope-200.huifont", &display, &fonts.display) ||
        !load_font(renderer, assets + "/fonts/dejavu-sans-mono.huifont", &mono, &fonts.mono)) {
        return 1;
    }
    fonts.pixel = fonts.mono;
    fonts.hand  = fonts.regular;

    GLuint fb = 0, color = 0;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);

    hui::save::ensure_directory(out);
    ps5lm::Dashboard dash(fonts);
    dash.set_screen({ 370.0 / 1024, true, 11.8 });  // PS5LM Lean: lean.12's measured screen, its budget with it on
    ps5lm::Settings settings;
    settings.default_model = "/data/PS5LM/models/Qwen3.8-27B-UD-Q2_K_XL.gguf";
    dash.set_settings(settings);
    std::vector<std::string> app_log, llama_log;
    for (int i = 0; i < 60; ++i) {
        app_log.push_back("[ps5-present-perf] calls=2500 failures=0 warmup_frames=30 idle_ms=0.000898 flip_ms=0.000889");
        app_log.push_back("22:56:01 ps5lm-app: model Qwen3.8-27B-UD-IQ2_XXS.gguf (qwen35): 64k context, q8_0 cache: 9.9 of 11.0 GiB");
        llama_log.push_back("7.15.902.312 I slot print_timing: id  0 | task 646 | n_gen =    162, tg =  20.44 t/s");
    }
    app_log.push_back("22:58:14 ps5lm-app: accept failed, errno 53");
    app_log.push_back("W srv          stop: cancel task, id_task = 22182");
    dash.set_logs(app_log, llama_log);
    ps5lm::MarketView mv;
    const char * ids[] = { "unsloth/Qwen3.8-27B-GGUF", "unsloth/gemma-4-E4B-it-GGUF", "bartowski/Llama-3.2-3B-Instruct-GGUF",
                           "ggml-org/gpt-oss-20b-GGUF", "unsloth/Qwen3.5-9B-GGUF", "bartowski/Mistral-7B-Instruct-v0.3-GGUF" };
    for (int i = 0; i < 6; ++i) {
        ps5lm::MarketRepo r;
        r.id = ids[i];
        r.downloads = 812000 / (i + 1);
        mv.repos.push_back(r);
    }
    mv.open = 0;
    const char * fnames[] = { "Qwen3.8-27B-UD-IQ2_XXS.gguf", "Qwen3.8-27B-UD-IQ2_M.gguf", "Qwen3.8-27B-Q3_K_M.gguf", "Qwen3.8-27B-Q4_K_M.gguf" };
    const double fsz[] = { 6.8, 9.9, 12.6, 15.4 };
    const int ffits[] = { 1, 0, 0, 0 };
    const char * fplans[] = { "64k context, q8_0 cache: 9.9 of 11.8 GiB", "needs 12.0 GiB with a 4k context; 11.8 GiB is free",
                              "too big: the weights alone need more than is free", "too big: the weights alone need more than is free" };
    for (int i = 0; i < 4; ++i) {
        ps5lm::MarketFile f;
        f.name = fnames[i];
        f.bytes = (uint64_t) (fsz[i] * 1073741824.0);
        f.fits = ffits[i];
        f.plan = fplans[i];
        f.fits_screen_off = i == 1;  // PS5LM Lean
        f.need_gib = i == 1 ? 12.0 : 0;
        mv.repos[0].files.push_back(f);
    }
    {
        ps5lm::Download d;
        d.repo = "unsloth/Qwen3.8-27B-GGUF";
        d.file = "Qwen3.8-27B-UD-IQ2_XXS.gguf";
        d.dir = "/data/PS5LM/models";
        d.bytes = (uint64_t) (6.8 * 1073741824.0);
        d.done = (uint64_t) (2.5 * 1073741824.0);
        d.state = ps5lm::DownloadState::running;
        d.mbps = 14.2;
        mv.downloads.push_back(d);
        d.file = "gemma-4-12b-it-Q4_K_M.gguf";
        d.repo = "unsloth/gemma-4-12b-it-GGUF";
        d.dir = "/mnt/usb0/PS5LM/models";
        d.bytes = (uint64_t) (7.1 * 1073741824.0);
        d.done = 0;
        d.mbps = 0;
        d.state = ps5lm::DownloadState::queued;
        mv.downloads.push_back(d);
        d.file = "Llama-3.2-3B-Instruct-Q4_K_M.gguf";
        d.repo = "bartowski/Llama-3.2-3B-Instruct-GGUF";
        d.bytes = 2019377696;
        d.done = 812000000;
        d.state = ps5lm::DownloadState::retrying;
        d.note = "Connection lost (sceHttp2ReadData 0x804101a3); trying again in 12 s, attempt 2";
        mv.downloads.push_back(d);
        d.file = "SmolLM2-135M-Instruct-F16.gguf";
        d.repo = "unsloth/SmolLM2-135M-Instruct-GGUF";
        d.dir = "/data/PS5LM/models";
        d.bytes = d.done = 270885952;
        d.state = ps5lm::DownloadState::done;
        d.note = "Saved to /data/PS5LM/models/SmolLM2-135M-Instruct-F16.gguf";
        mv.downloads.push_back(d);
    }
    dash.set_market(mv);
    std::vector<unsigned char> pixels((size_t) W * H * 4);
    stbi_flip_vertically_on_write(1);
    hui::ui::Feedback feedback;
    float t = 0;
    bool ok = true;

    g_chat.mic = ps5lm::MicState::idle;  // PS5LM Lean: the base.en model is on the console
    // `held`: a button kept down through the frames (L2 for a take).
    const auto run = [&](int frames, hui::InputFrame input, bool generating, uint32_t held) {
        for (int i = 0; i < frames; ++i) {
            dash.set_live(sample(t, generating));
            if (g_chat.mic == ps5lm::MicState::recording) {
                g_chat.level = speech_level(t);
                g_chat.seconds += 1.0 / 60.0;
            }
            dash.set_chat(g_chat);
            feedback.clear();
            hui::InputFrame in = i == 0 ? input : hui::InputFrame{};
            if (held) {
                in.connected = true;
                in.held |= held;
            }
            dash.update(in, 1.0f / 60.0f, feedback);
            dash.take_request();
            t += 1.0f / 60.0f;
        }
    };
    const auto step = [&](int frames, hui::InputFrame input, bool generating) { run(frames, input, generating, 0); };
    const auto shot = [&](const char * name) {
        ps5lm::DashboardFrame frame;
        frame.glass_texture = renderer.glass_texture();
        dash.draw(frame);
        renderer.begin();
        renderer.backdrop(frame.backdrop);
        renderer.draw(frame.scene);
        if (frame.glass) {
            renderer.glass();
        }
        renderer.draw(frame.overlay);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(fb, W, H);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (size_t i = 3; i < pixels.size(); i += 4) {
            pixels[i] = 255;
        }
        const std::string path = out + "/" + name + ".png";
        ok = stbi_write_png(path.c_str(), W, H, 4, pixels.data(), W * 4) != 0 && ok;
        std::fprintf(stderr, "wrote %s: %zu shapes, %zu draw calls, GL error 0x%x\n", path.c_str(),
                     renderer.last_instances(), renderer.last_draw_calls(), glGetError());
    };
    auto press = [](hui::Action a) {
        hui::InputFrame in;
        in.connected = true;
        in.pressed = hui::action_bit(a);
        return in;
    };
    auto nav = [](hui::Direction d) {
        hui::InputFrame in;
        in.connected = true;
        in.nav = d;
        return in;
    };

    using D = hui::Direction;
    using A = hui::Action;
    g_loading = 0;
    g_phase = "Opening the GPU and reading the models";
    g_fresh = true;  // PS5LM Lean: seconds in, nothing read yet
    step(50, {}, false);
    shot("00-home-starting");
    g_fresh = false;
    g_phase.clear();
    g_loading = 0.45f;
    step(90, {}, false);
    shot("00-home-loading");
    g_loading = -1;
    step(120, {}, true);
    shot("01-home");
    g_failed = true;  // PS5LM Lean: one request refused, for the error count
    step(60, {}, false);
    shot("02-home-idle");
    g_failed = false;
    g_no_replies = true;
    step(60, {}, false);
    shot("02b-home-no-replies");
    g_no_replies = false;
    g_decision = true;
    step(60, {}, true);
    shot("03-home-decision");
    g_decision = false;
    g_empty = true;
    step(60, {}, false);
    shot("04-home-empty");
    g_empty = false;
    step(40, press(A::confirm), false);
    shot("05-models-local");
    step(20, press(A::back), false);
    step(20, nav(D::down), false);
    step(20, press(A::confirm), false);
    step(20, nav(D::down), false);
    step(30, nav(D::down), false);
    shot("06-models-suggested");
    step(20, press(A::back), false);
    step(20, nav(D::down), false);
    step(20, press(A::confirm), false);
    step(30, nav(D::down), false);
    shot("07-models-search");
    step(30, press(A::confirm), false);
    dash.set_market(mv);
    step(20, nav(D::down), false);
    shot("08-models-files");
    step(30, press(A::confirm), false);
    shot("09-models-space");
    step(20, press(A::back), false);
    step(20, press(A::back), false);
    step(20, press(A::back), false);
    step(20, nav(D::down), false);
    step(20, press(A::confirm), false);
    step(30, nav(D::down), false);
    shot("10-models-downloads");
    step(40, press(A::page_next), false);
    shot("11-api");
    g_decision = true;
    step(40, {}, false);
    shot("12-api-decision");
    g_decision = false;
    step(40, press(A::page_next), false);
    shot("13-console");
    step(30, press(A::jump_next), false);
    shot("14-console-hour");
    step(40, press(A::page_next), false);
    shot("15-settings");
    // PS5LM Lean: Screen and memory comes first; its rows, then on to Startup.
    step(30, press(A::confirm), false);
    shot("15-settings-screen-rows");
    step(20, press(A::back), false);
    step(20, nav(D::down), false);
    step(20, press(A::confirm), false);
    step(30, nav(D::down), false);
    shot("16-settings-row");
    step(30, press(A::confirm), false);
    shot("17-settings-picker");
    step(20, press(A::back), false);
    step(20, press(A::back), false);
    step(20, nav(D::down), false);
    step(20, press(A::confirm), false);
    step(30, press(A::confirm), false);
    shot("18-settings-context");
    step(20, press(A::back), false);
    step(20, press(A::back), false);
    for (int i = 0; i < 2; ++i) {
        step(15, nav(D::down), false);
    }
    // Sound and storage, on a console where the HEN kept /data closed.
    dash.set_storage(false, 16, false);
    step(30, press(A::confirm), false);
    step(20, nav(D::down), false);
    step(20, nav(D::down), false);
    shot("18-settings-app-storage");
    step(20, press(A::back), false);
    dash.set_storage(true, 16, false);
    step(15, nav(D::down), false);
    step(30, press(A::confirm), false);
    shot("19-settings-report");
    step(30, press(A::confirm), false);
    shot("19-settings-report-dialog");
    step(20, press(A::back), false);
    dash.set_report_status(false, "Saved to /data/PS5LM/debug-report.txt and /mnt/usb0/PS5LM-debug-report.txt, 214 KB");
    step(20, {}, false);
    shot("19-settings-report-saved");
    step(20, press(A::back), false);
    step(15, nav(D::down), false);
    step(30, press(A::confirm), false);
    shot("19-settings-diagnostics");
    step(20, press(A::back), false);
    for (int i = 0; i < 4; ++i) {
        step(15, press(A::page_prev), false);
    }
    step(30, press(A::north), true);
    step(60, {}, true);
    shot("20-home-message");
    step(30, press(A::back), true);
    // The first start without full storage.
    dash.set_storage(false, 16, true);
    step(30, {}, false);
    shot("20-storage-closed");
    step(20, press(A::north), false);
    shot("20-storage-howto");
    step(20, press(A::back), false);
    step(20, press(A::confirm), false);
    dash.set_busy("Loading Qwen3.8 27B", "Restarting with the whole memory for it, about 90 seconds");
    step(30, {}, false);
    shot("21-busy");
    dash.set_busy("");
    step(30, {}, false);

    // PS5LM Lean: the screen-off question from Home (Options), its countdown,
    // and a model that fits only with the screen off.
    step(30, press(A::menu), false);
    shot("22-screen-question");
    step(20, press(A::confirm), false);
    step(60, {}, false);
    shot("23-screen-countdown");
    step(20, press(A::back), false);
    step(30, press(A::confirm), false);  // Home's Cross: the library, on the running model
    step(30, nav(D::down), false);
    shot("24-models-screen-off");
    step(30, press(A::confirm), false);
    shot("25-models-load-question");
    step(20, press(A::back), false);
    // PS5LM Lean (lean.14): the sleep timer's countdown, which any button cancels.
    dash.start_sleep_countdown();
    step(80, {}, false);
    shot("26-sleep-countdown");
    step(20, press(A::west), false);
    // lean.16: a model that fits text only, and reads images with the screen off.
    step(30, nav(D::up), false);  // still in the library, on Mistral: up to IQ2_XXS
    step(30, nav(D::up), false);
    shot("27-models-text-only");
    step(30, press(A::confirm), false);
    shot("28-models-images-question");
    step(20, press(A::back), false);

    // PS5LM Lean: the Home chat. Push-to-talk on L2 into the message box, R2
    // sends; the conversation from the engine's ChatView.
    using M = ps5lm::MicState;
    const uint32_t L2 = hui::action_bit(A::jump_prev);
    step(30, press(A::page_prev), false);  // back to Home
    g_others_done = true;
    step(40, {}, false);
    shot("30-chat-empty");
    step(30, press(A::west), false);  // Square: an example question into the box
    shot("31-chat-example");
    step(20, press(A::back), false);  // Circle clears it
    g_chat.turns = roof_chat();
    run(20, press(A::jump_prev), false, L2);  // L2 down: the engine loads whisper first
    g_chat.mic = M::loading;
    run(20, {}, false, L2);
    shot("32-chat-loading");
    g_chat.mic = M::recording;
    g_chat.seconds = 0;
    run(250, {}, false, L2);
    shot("33-chat-recording");
    step(2, {}, false);  // L2 up: talk_stop
    g_chat.mic = M::transcribing;
    step(40, {}, false);
    shot("34-chat-transcribing");
    g_chat.mic = M::ready;
    g_chat.transcript = "How much does a tear-off usually add?";
    g_chat.transcript_seq = 1;
    g_chat.transcribe_ms = 1690;
    g_chat.whisper_loaded = true;
    g_chat.whisper_mib = 160;
    step(30, {}, false);
    shot("35-chat-transcript");
    step(30, press(A::jump_next), false);  // R2 sends; the engine adds the turn and answers
    g_chat.mic = M::idle;
    g_chat.answering = true;
    g_chat.turns.push_back(turn(true, "How much does a tear-off usually add?", true));
    g_chat.turns.push_back(turn(false, "Usually $1 to $2 a square foot more than shingling over, or about $2,000 to "
                                       "$4,000 on a 2,000 square foot roof: the labour, the dumpster and"));
    g_chat.turns.back().streaming = true;
    step(40, {}, true);
    shot("36-chat-answering");
    g_chat.transcript = "And if the decking is rotten?";
    g_chat.transcript_seq = 2;
    step(20, {}, true);
    step(30, press(A::jump_next), true);  // R2 while it answers: the message waits
    shot("37-chat-waits-for-reply");
    g_chat.answering = false;
    g_chat.turns.back().streaming = false;
    g_chat.turns.back().text += " the disposal fees.";
    step(2, {}, false);  // the reply ended: the message that waited goes
    g_chat.turns.push_back(turn(true, "And if the decking is rotten?", true));
    g_chat.turns.push_back(turn(false, ""));
    g_chat.turns.back().streaming = true;
    g_chat.answering = true;
    g_photo_run = true;  // ... but the PC's photo run has the model
    g_others_done = false;
    step(60, {}, true);
    shot("38-chat-other-device-first");
    g_photo_run = false;
    g_others_done = true;
    g_chat.answering = false;
    g_chat.turns.back().streaming = false;
    g_chat.turns.back().text = "Replacing decking runs about $70 to $100 a sheet installed. A roofer will not know how "
                               "many sheets until the old shingles are off, so ask for the per-sheet price in writing.";
    // A take with nothing in it: muted, silent, failed; then no speech model.
    run(10, press(A::jump_prev), false, L2);
    g_chat.mic = M::recording;
    run(60, {}, false, L2);
    step(2, {}, false);
    g_chat.mic = M::muted;
    g_chat.note = "Your controller's microphone is muted (its mute button)";  // chat.cpp's wording
    step(40, {}, false);
    shot("39-chat-muted");
    g_chat.mic = M::silent;
    g_chat.note = "Nothing heard. Hold the button while you speak";
    step(40, {}, false);
    shot("40-chat-silent");
    g_chat.mic = M::failed;
    g_chat.note = "Not enough memory for the speech model: it needs 210 MB, 150 MB is free";
    step(40, {}, false);
    shot("41-chat-failed");
    g_chat.mic = M::off;
    g_chat.note = "No speech model on this console (/data/PS5LM/whisper/ggml-base.en-q5_1.bin)";
    step(760, {}, false);  // the note has gone
    shot("42-chat-off");
    g_chat.mic = M::idle;
    g_chat.note.clear();
    // A long chat: six older messages no longer fit the context.
    g_chat.turns = long_chat();
    g_chat.dropped = 6;
    step(40, {}, false);
    shot("43-chat-long");
    for (int i = 0; i < 24; ++i) {
        step(8, nav(D::up), false);
    }
    step(20, {}, false);
    shot("44-chat-long-scrolled-up");
    for (int i = 0; i < 24; ++i) {
        step(4, nav(D::down), false);
    }
    // Right: the other devices' requests.
    g_others_done = false;
    step(30, nav(D::right), true);
    shot("45-other-devices");
    step(30, nav(D::left), false);
    g_others_done = true;
    // Typing: Triangle opens the keyboard on the box's text; Send or Close.
    g_chat.transcript = "Which is cheaper over 20 years, metal or shingles";
    g_chat.transcript_seq = 3;
    step(20, {}, false);
    step(40, press(A::north), false);
    shot("46-chat-keyboard");
    step(30, press(A::back), false);
    step(20, press(A::back), false);  // Circle clears the box
    // A new chat: Square twice.
    step(30, press(A::west), false);
    shot("47-chat-new-chat-again");
    step(240, {}, false);
    // No model loaded: the chat says so.
    g_chat.turns.clear();
    g_chat.dropped = 0;
    g_empty = true;
    step(40, {}, false);
    shot("48-chat-no-model");
    g_empty = false;
    // A decision model: no conversation; each message is one request, shown in the chat view.
    g_decision = true;
    g_decision_self = true;
    step(40, {}, false);
    shot("49-chat-decision");
    g_decision = false;
    g_decision_self = false;
    return ok ? 0 : 1;
}
