// Host check for model_plan.cpp:
//   c++ -std=c++17 -O1 ps5/app/model_plan_test.cpp ps5/app/model_plan.cpp -o build/model_plan_test
//   build/model_plan_test          synthetic headers only
//   build/model_plan_test models   also the real GGUF files in models/
// Uses the budget the console reported (11.38 GiB free) and the two
// configurations measured there.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "model_plan.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// A GGUF header with string and uint32 metadata, no tensors.
struct Gguf {
    std::string kvs;
    uint64_t    n = 0;
    template <class T> static std::string raw(T v) { return std::string((const char *) &v, sizeof(v)); }
    static std::string str(const std::string & v) { return raw<uint64_t>(v.size()) + v; }
    Gguf & kv(const std::string & k, const std::string & v) { kvs += str(k) + raw<uint32_t>(8) + str(v); n++; return *this; }
    Gguf & kv(const std::string & k, uint32_t v) { kvs += str(k) + raw<uint32_t>(4) + raw(v); n++; return *this; }
    Gguf & kv(const std::string & k, const std::vector<bool> & v) {  // a bool array
        kvs += str(k) + raw<uint32_t>(9) + raw<uint32_t>(7) + raw<uint64_t>(v.size());
        for (bool b : v) {
            kvs += (char) b;
        }
        n++;
        return *this;
    }
    // PS5LM Lean: tensor entries (one dimension, type 0) at data offsets.
    std::string tensors;
    uint64_t    n_tensors = 0;
    Gguf & tensor(const std::string & name, uint64_t offset) {
        tensors += str(name) + raw<uint32_t>(1) + raw<uint64_t>(1) + raw<uint32_t>(0) + raw(offset);
        n_tensors++;
        return *this;
    }
    // magic, version 3, tensor count, metadata count, metadata, tensor list
    std::string bytes() const { return "GGUF" + raw<uint32_t>(3) + raw(n_tensors) + raw(n) + kvs + tensors; }
};

bool has_args(const ps5lm::Plan & p, const char * flag, const char * value) {
    auto it = std::find(p.args.begin(), p.args.end(), flag);
    return it != p.args.end() && it + 1 != p.args.end() && *(it + 1) == value;
}

}  // namespace

int main(int argc, char ** argv) {
    const double budget = 11.38;

    // A decision model (Clef is Qwen 3.5 9B with a decision head): found from
    // "<arch>.decision.type", a wide ubatch, a 4k cap and no chat sampling.
    Gguf clef;
    clef.kv("general.architecture", "qwen35").kv("general.name", "Clef Flash Qwen3.5 9B")
        .kv("qwen35.block_count", 32u).kv("qwen35.attention.head_count", 16u)
        .kv("qwen35.attention.head_count_kv", 4u).kv("qwen35.embedding_length", 4096u)
        .kv("qwen35.context_length", 262144u).kv("qwen35.decision.type", "clef");
    const ps5lm::ModelInfo d = ps5lm::read_model_header("clef-flash-Q4_K_M.gguf", clef.bytes(), 5ull << 30);
    assert(d.ok && d.decision == "clef");
    assert(!ps5lm::find_preset(d));
    const ps5lm::Plan dp = ps5lm::plan_model(d, budget);
    assert(dp.fits && dp.ctx == 4096 && has_args(dp, "-ub", "2048") && has_args(dp, "-b", "2048"));
    assert(dp.why.find("decision model (clef)") == 0);

    // The same model without the decision key is a chat model with the narrow batch.
    Gguf chat;
    chat.kv("general.architecture", "qwen35").kv("general.name", "Qwen3.5 9B")
        .kv("qwen35.block_count", 32u).kv("qwen35.attention.head_count", 16u)
        .kv("qwen35.attention.head_count_kv", 4u).kv("qwen35.embedding_length", 4096u)
        .kv("qwen35.context_length", 262144u);
    const ps5lm::ModelInfo c = ps5lm::read_model_header("Qwen3.5-9B-Q4_K_M.gguf", chat.bytes(), 5ull << 30);
    assert(c.ok && c.decision.empty() && ps5lm::find_preset(c));
    const ps5lm::Plan cp = ps5lm::plan_model(c, budget);
    assert(cp.fits && cp.ctx > 4096 && has_args(cp, "-ub", "64"));

    // PS5LM Lean: a model that does not fit says the least it needs, its cache
    // at 4k included, and fits once the screen's memory is added.
    {
        const ps5lm::Plan q4 = ps5lm::plan_model(c, 64.0, 4096, "q4_0");
        assert(q4.fits && q4.ctx == 4096);
        const double least = q4.total_gib;  // 4k, q4_0: the least this model can run with
        assert(least > q4.weights_gib + q4.state_gib + q4.reserve_gib);
        const ps5lm::Plan short_by = ps5lm::plan_model(c, least - 0.2);
        assert(!short_by.fits);
        assert(short_by.total_gib > least - 0.001 && short_by.total_gib < least + 0.001);  // the cache is counted
        assert(short_by.why.find("needs") == 0);
        assert(ps5lm::plan_model(c, least - 0.2 + 0.36).fits);  // the screen's memory makes it fit
    }

    // A larger prompt batch costs compute buffer, counted by the feed-forward width.
    Gguf wide_ff = chat;
    wide_ff.kv("qwen35.feed_forward_length", 12288u);
    const ps5lm::ModelInfo w = ps5lm::read_model_header("Qwen3.5-9B-Q4_K_M.gguf", wide_ff.bytes(), 5ull << 30);
    const ps5lm::Plan w64 = ps5lm::plan_model(w, budget), w512 = ps5lm::plan_model(w, budget, 65536, nullptr, 512);
    assert(w.n_ff == 12288 && has_args(w64, "-ub", "64") && has_args(w512, "-ub", "512") && has_args(w512, "-b", "512"));
    assert(w512.reserve_gib - w64.reserve_gib > 0.18 && w512.reserve_gib - w64.reserve_gib < 0.21);

    // Bonsai 2: upstream's Q2_0 loads with its preset (64k at most, PrismML's
    // sampling); PrismML's own PQ2_0 and PTQ1_0 say what to do instead.
    const auto bonsai = [](uint32_t ftype) {
        Gguf g;
        g.kv("general.architecture", "qwen35").kv("general.name", "Ternary-Bonsai-2-27B")
            .kv("qwen35.block_count", 64u).kv("qwen35.attention.head_count", 24u)
            .kv("qwen35.attention.head_count_kv", 4u).kv("qwen35.embedding_length", 5120u)
            .kv("qwen35.attention.key_length", 256u).kv("qwen35.attention.value_length", 256u)
            .kv("qwen35.full_attention_interval", 4u).kv("qwen35.context_length", 262144u)
            .kv("prism.hadamard.version", 1u).kv("general.file_type", ftype);
        return g.bytes();
    };
    const ps5lm::ModelInfo b = ps5lm::read_model_header("Ternary-Bonsai-2-27B-Q2_0.gguf", bonsai(41), 7626008928ull);
    const ps5lm::Preset * bp = ps5lm::find_preset(b);
    assert(b.ok && bp && std::string(bp->label) == "Bonsai 2");
    const ps5lm::Plan bpl = ps5lm::plan_model(b, 12.0, 131072);
    assert(bpl.fits && bpl.ctx == 65536 && has_args(bpl, "--min-p", "0.05"));
    const ps5lm::ModelInfo pq = ps5lm::read_model_header("Ternary-Bonsai-2-27B-PQ2_0.gguf", bonsai(141), 7206168928ull);
    assert(!pq.ok && pq.error.find("bonsai-pq2-to-q2.py") != std::string::npos);
    const ps5lm::ModelInfo pt = ps5lm::read_model_header("Ternary-Bonsai-2-27B-PTQ1_0.gguf", bonsai(143), 5946648928ull);
    assert(!pt.ok && pt.error.find("PTQ1_0") != std::string::npos);

    // Sliding-window attention (#57): Spark-X2.6 4B's header. 27 of its 36
    // layers keep a 512-token window, so 256k fits; counted as full attention
    // it stopped at 128k.
    std::vector<bool> swa;
    for (int i = 0; i < 36; i++) {
        swa.push_back(i % 4 != 3);
    }
    Gguf spark;
    spark.kv("general.architecture", "spark2_5").kv("spark2_5.block_count", 36u)
        .kv("spark2_5.attention.head_count", 16u).kv("spark2_5.attention.head_count_kv", 4u)
        .kv("spark2_5.embedding_length", 2560u).kv("spark2_5.attention.key_length", 256u)
        .kv("spark2_5.attention.value_length", 256u).kv("spark2_5.context_length", 1048576u)
        .kv("spark2_5.attention.sliding_window", 512u).kv("spark2_5.attention.sliding_window_pattern", swa);
    const ps5lm::ModelInfo s = ps5lm::read_model_header("Spark-X2.6-4B-Q8_0.gguf", spark.bytes(), 4375021824ull);
    assert(s.ok && s.n_swa_layers == 27 && s.swa_window == 512);
    assert(kv_bytes_per_token(s, "q8_0") == 9 * 4 * 512 * 34.0 / 32);
    const ps5lm::Plan sp = ps5lm::plan_model(s, budget, 262144);
    assert(sp.fits && sp.ctx == 262144 && sp.kv_type == "q8_0");
    ps5lm::ModelInfo full = s;
    full.n_swa_layers = 0;
    assert(ps5lm::plan_model(full, budget, 262144).ctx == 131072);

    // PS5LM Lean: a projector pairs by its header when its name does not
    // ("mmproj-F16.gguf", as Unsloth names every one), the larger of two
    // matches wins, and another model's projector never pairs.
    {
        char dir[] = "/tmp/model_plan_test.XXXXXX";
        assert(mkdtemp(dir));
        const std::string at = dir;
        const auto write = [&](const std::string & name, const Gguf & g, size_t pad) {
            std::ofstream(at + "/" + name, std::ios::binary) << g.bytes() << std::string(pad, '\0');
        };
        Gguf qwen = chat;
        qwen.kv("general.type", "model").kv("general.basename", "Qwen3.8-27B")
            .kv("general.base_model.0.repo_url", "https://huggingface.co/Qwen/Qwen3.8-27B");
        Gguf f16;
        f16.kv("general.architecture", "clip").kv("general.type", "mmproj").kv("clip.projector_type", "qwen3vl_merger")
            .kv("general.basename", "Qwen3.8-27B")
            .kv("general.base_model.0.repo_url", "https://huggingface.co/Qwen/Qwen3.8-27B");
        Gguf other;
        other.kv("general.architecture", "clip").kv("general.type", "mmproj").kv("general.basename", "gemma-4-31B");
        write("Qwen3.8-27B-UD-IQ2_S.gguf", qwen, 0);
        write("mmproj-F16.gguf", f16, 4096);
        write("mmproj-small.gguf", f16, 1024);   // the same base, smaller: loses to F16
        write("mmproj-other.gguf", other, 8192);  // another model's: never pairs
        const ps5lm::ModelInfo m = ps5lm::read_model_info(at + "/Qwen3.8-27B-UD-IQ2_S.gguf");
        assert(m.basename == "Qwen3.8-27B" && m.gguf_type == "model");
        assert(m.mmproj == at + "/mmproj-F16.gguf");
        assert(m.mmproj_bytes > 4096);
        // Its projector is Qwen's, so the plan takes images in pieces: a
        // narrow ubatch and a 1280 batch and image cap.
        assert(m.projector_type == "qwen3vl_merger");
        const ps5lm::Plan vp = ps5lm::plan_model(m, budget, 8192);
        assert(has_args(vp, "-ub", "256") && has_args(vp, "-b", "1280") && has_args(vp, "--image-max-tokens", "1280"));
        assert(has_args(vp, "--mmproj", (at + "/mmproj-F16.gguf").c_str()));
        // Another projector type keeps the wide ubatch.
        assert(!ps5lm::causal_projector("gemma3") && ps5lm::causal_projector("qwen2vl_merger"));
        ps5lm::ModelInfo g = m;
        g.projector_type = "gemma3";
        const ps5lm::Plan gp = ps5lm::plan_model(g, budget, 8192);
        assert(has_args(gp, "-ub", "2048") && has_args(gp, "-b", "2048"));
        assert(std::find(gp.args.begin(), gp.args.end(), "--image-max-tokens") == gp.args.end());
        // It keeps the vision headroom: base, image encoder, 0.5 for photos.
        assert(vp.reserve_gib >= 0.8 + 0.25 + 0.5);
        // The name rule still comes first: a projector named after the model wins.
        write("mmproj-Qwen3.8-27B-Q8_0.gguf", f16, 0);
        assert(ps5lm::read_model_info(at + "/Qwen3.8-27B-UD-IQ2_S.gguf").mmproj == at + "/mmproj-Qwen3.8-27B-Q8_0.gguf");
        // A model with no base-model keys pairs by name only, as before.
        write("Plain-7B.gguf", chat, 0);
        assert(ps5lm::read_model_info(at + "/Plain-7B.gguf").mmproj.empty());
        for (const char * n : { "Qwen3.8-27B-UD-IQ2_S.gguf", "mmproj-F16.gguf", "mmproj-small.gguf", "mmproj-other.gguf",
                                "mmproj-Qwen3.8-27B-Q8_0.gguf", "Plain-7B.gguf" }) {
            unlink((at + "/" + n).c_str());
        }
        rmdir(dir);
    }

    // PS5LM Lean: tensors llama.cpp does not load (the MTP block a file
    // declares with nextn_predict_layers) stay out of the weights; a header cut
    // before the end of its tensor list leaves them counted.
    {
        Gguf mtp;
        mtp.kv("general.architecture", "qwen35").kv("general.name", "Qwen3.8-27B")
            .kv("qwen35.block_count", 3u).kv("qwen35.nextn_predict_layers", 1u)
            .kv("qwen35.attention.head_count", 16u).kv("qwen35.attention.head_count_kv", 4u)
            .kv("qwen35.embedding_length", 4096u).kv("qwen35.context_length", 262144u)
            .tensor("blk.0.ffn_up.weight", 0).tensor("blk.1.ffn_up.weight", 1000)
            .tensor("blk.2.nextn.eh_proj.weight", 2000).tensor("blk.2.ffn_up.weight", 2500)
            .tensor("output.weight", 3000);
        const std::string head = mtp.bytes();
        const uint64_t    file = (head.size() + 31) / 32 * 32 + 4000;
        const ps5lm::ModelInfo q = ps5lm::read_model_header("Qwen3.8-27B-UD-Q2_K_XL.gguf", head, file);
        assert(q.ok && q.n_layer == 2 && q.unloaded_bytes == 1000);
        const ps5lm::ModelInfo cut = ps5lm::read_model_header("Qwen3.8-27B-UD-Q2_K_XL.gguf", head.substr(0, head.size() - 9), file);
        assert(cut.ok && cut.unloaded_bytes == 0);
        const double less = ps5lm::plan_model(cut, budget).weights_gib - ps5lm::plan_model(q, budget).weights_gib;
        assert(less > 999 / 1073741824.0 && less < 1001 / 1073741824.0);
        assert(ps5lm::read_model_header("Qwen3.5-9B-Q4_K_M.gguf", chat.bytes(), 5ull << 30).unloaded_bytes == 0);
    }

    // PS5LM Lean: a model too big with its projector but not without it is
    // planned text only (Qwen 3.8 27B UD-IQ3_XXS, measured text only at 8k with
    // the screen on, was listed as too big); with room for images, images.
    {
        ps5lm::ModelInfo v = c;
        v.mmproj         = "/nowhere/mmproj-F16.gguf";
        v.mmproj_bytes   = 1ull << 30;
        v.projector_type = "qwen3vl_merger";
        const double text_least   = ps5lm::plan_model(c, 64.0, 4096, "q4_0").total_gib;
        const double images_least = ps5lm::plan_model(v, 64.0, 4096, "q4_0").total_gib;
        assert(images_least > text_least + 1.0);
        const double         between = (text_least + images_least) / 2;
        const ps5lm::Plan    t       = ps5lm::plan_model(v, between);
        assert(t.fits && t.text_only && t.images_gib > between);
        assert(t.why.rfind("text only (images need ", 0) == 0 && t.why.find("GiB free") != std::string::npos);
        assert(std::find(t.args.begin(), t.args.end(), "--mmproj") == t.args.end() && has_args(t, "-ub", "64"));
        const ps5lm::Plan room = ps5lm::plan_model(v, images_least + 0.01);
        assert(room.fits && !room.text_only && has_args(room, "--mmproj", "/nowhere/mmproj-F16.gguf"));
        const ps5lm::Plan none = ps5lm::plan_model(v, text_least - 0.2);
        assert(!none.fits && !none.text_only && none.why.find("needs") == 0);
    }

    if (argc < 2) {
        std::puts("model_plan_test: ok (synthetic only)");
        return 0;
    }
    bool saw_q2k = false, saw_iq2xxs = false;
    for (const auto & path : ps5lm::find_models({ argv[1] })) {
        const ps5lm::ModelInfo m = ps5lm::read_model_info(path);
        assert(m.ok);
        const ps5lm::Plan p = ps5lm::plan_model(m, budget);
        const ps5lm::Preset * pre = ps5lm::find_preset(m);
        std::printf("%-34s %s L%u kvL%u kvh%u k%u v%u state %.0f MiB | %s | %s\n", m.file_name.c_str(),
                    m.arch.c_str(), m.n_layer, m.n_kv_layers(), m.n_head_kv, m.key_length, m.value_length,
                    m.state_bytes / 1048576.0, p.why.c_str(), pre ? pre->label : "-");
        if (m.arch == "qwen35") {
            // Qwen 3.8 27B: 16 of 64 layers keep a KV cache, 4 heads of 256.
            assert(m.n_layer == 64 && m.n_kv_layers() == 16 && m.n_head_kv == 4 && m.key_length == 256);
            assert(kv_bytes_per_token(m, "f16") == 65536.0);
            assert(m.state_bytes > 140.0 * 1048576 && m.state_bytes < 170.0 * 1048576);
            assert(pre && std::string(pre->label) == "Qwen 3.8");
        }
        if (m.file_name.find("UD-Q2_K_XL") != std::string::npos) {
            // Ran on the console: 64k with a q4_0 cache.
            assert(p.fits && p.ctx == 65536 && p.kv_type == "q4_0");
            saw_q2k = true;
        }
        if (m.file_name.find("UD-IQ2_XXS") != std::string::npos) {
            // 4k at f16 ran; 64k needs at most q8_0 at this size.
            assert(p.fits && p.ctx == 65536 && p.kv_type == "q8_0");
            saw_iq2xxs = true;
        }
    }
    assert(saw_q2k && saw_iq2xxs);
    // Nothing fits in too little memory.
    ps5lm::ModelInfo big;
    big.ok = true; big.file_bytes = 20ull << 30; big.n_layer = 32; big.n_head_kv = 8;
    big.key_length = big.value_length = 128;
    assert(!ps5lm::plan_model(big, budget).fits);
    std::puts("model_plan_test: ok");
}
