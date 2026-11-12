// GPU port of the NTSC stage (core/stages/ntsc/gpu, docs/dev/NTSC-GPU.md):
// parity with the Rust path (ntsc-rs via the shim), pass by pass, and the
// pipeline's fallback rules.
//
// Parity rule (docs/dev/HOST-GPU.md's): each channel within 1/255 of the Rust
// path's and at most 0.2% of the values different at all, per case.
//
// Environment:
//   NTSCRT_REQUIRE_GPU=1         a missing GPU fails instead of skipping
//   NTSCRT_D3D11_WARP=1          run on WARP (CI's software device)
//   NTSCRT_NTSC_GPU_DUMP=<dir>   write the YIQ planes of the stage-by-stage comparison
//                                (<case>.<n>.<stage>.{rust,gpu}.f32: Y, I, Q planes, width x plane_rows each)
//   NTSCRT_NTSC_GPU_VERBOSE=1    per-case lines on stdout ("PARITY ...", "STAGE ...", "COVERAGE ...")
#include <doctest/doctest.h>
#include <ntscrs_capi.h>
#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../gpu/gpu_test_util.h"
#include "stages/ntsc/gpu/ntsc_gpu.h"
#include "stages/ntsc/ntsc_internal.h"
#include "stages/stages.h"
#include "testing.h"

using namespace ntscrt;
namespace fs = std::filesystem;

namespace {

bool verbose() {
    const char *v = std::getenv("NTSCRT_NTSC_GPU_VERBOSE");
    return v && *v && *v != '0';
}

// ---- test pictures --------------------------------------------------------------------
ImageBGRA8 picture(const std::string &kind, int w, int h) {
    ImageBGRA8 im;
    im.resize(w, h);
    uint32_t rng = 0x2545F491u;
    static const uint8_t bars[8][3] = {{192, 192, 192}, {0, 192, 192}, {192, 192, 0}, {0, 192, 0},
                                       {192, 0, 192},   {0, 0, 192},   {192, 0, 0},   {16, 16, 16}};  // BGR
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = im.row(y) + x * 4;
            if (kind == "gradient") {
                p[0] = uint8_t(x * 255 / std::max(1, w - 1));
                p[1] = uint8_t(y * 255 / std::max(1, h - 1));
                p[2] = uint8_t(((x + y) * 255) / std::max(1, w + h - 2));
            } else if (kind == "bars") {
                const uint8_t *b = bars[std::min(7, x * 8 / w)];
                const bool low = y > h * 3 / 4;
                p[0] = low ? b[2] : b[0];
                p[1] = b[1];
                p[2] = low ? b[0] : b[2];
            } else if (kind == "noise") {
                for (int c = 0; c < 3; ++c) {
                    rng = rng * 1664525u + 1013904223u;
                    p[c] = uint8_t(rng >> 24);
                }
            } else {  // "edges": 1-px lines, a checkerboard, hard colour steps
                const bool line = (x % 7) == 0 || (y % 5) == 0;
                const bool check = ((x / 3) + (y / 3)) % 2 == 0;
                p[0] = line ? 255 : (check ? 0 : 128);
                p[1] = x < w / 2 ? (check ? 255 : 0) : 30;
                p[2] = y < h / 2 ? 255 : (line ? 0 : 200);
            }
            p[3] = 255;
        }
    return im;
}

// ---- looks ----------------------------------------------------------------------------
// Every optional stage off: what's left is ntsc-rs's mandatory modulation.
json minimal_settings() {
    return json::parse(R"({
        "input_luma_filter": 0, "chroma_lowpass_in": 0, "composite_preemphasis": 0,
        "composite_noise": false, "snow_intensity": 0, "head_switching": false, "tracking_noise": false,
        "luma_smear": 0, "ringing": false, "luma_noise": false, "chroma_noise": false,
        "chroma_phase_error": 0, "chroma_phase_noise_intensity": 0,
        "chroma_delay_horizontal": 0, "chroma_delay_vertical": 0,
        "vhs_settings": false, "vhs_chroma_vert_blend": false, "chroma_lowpass_out": 0
    })");
}
// The house defaults with the stages later phases implement turned off: every
// pass the GPU has now, as the house look sets them.
json gpu_full_settings() {
    return json::parse(R"({
        "composite_noise": false, "snow_intensity": 0, "head_switching": false, "tracking_noise": false,
        "luma_noise": false, "chroma_noise": false, "chroma_delay_horizontal": 0, "chroma_delay_vertical": 0,
        "vhs_edge_wave_enabled": false
    })");
}
json with(json base, const json &over) {
    for (auto it = over.begin(); it != over.end(); ++it) base[it.key()] = it.value();
    return base;
}
json look_of(const json &settings) { return json{{"ntsc", {{"enabled", true}, {"settings", settings}}}}; }

struct Case {
    std::string name;
    json settings;
    uint32_t expect;  // stages that must be enabled (checks the settings took)
};

uint32_t bit(int s) { return 1u << s; }
constexpr uint32_t kModem = (1u << NTSC_ST_CHROMA_INTO_LUMA) | (1u << NTSC_ST_LUMA_INTO_CHROMA);

std::vector<Case> single_cases() {
    const json m = minimal_settings();
    const json vhs = {{"vhs_settings", true}, {"vhs_tape_speed", 0}, {"vhs_chroma_loss", 0},
                      {"vhs_sharpen_enabled", false}, {"vhs_edge_wave_enabled", false}};
    std::vector<Case> c;
    for (int d = 0; d < 4; ++d)
        c.push_back({"demod" + std::to_string(d), with(m, {{"chroma_demodulation", d}}), kModem});
    for (int ps = 0; ps < 4; ++ps)
        c.push_back({"phase_shift" + std::to_string(ps),
                     with(m, {{"video_scanline_phase_shift", ps}, {"video_scanline_phase_shift_offset", ps + 1}}),
                     kModem});
    c.push_back({"luma_box", with(m, {{"input_luma_filter", 1}}), kModem | bit(NTSC_ST_LUMA_FILTER)});
    c.push_back({"luma_notch", with(m, {{"input_luma_filter", 2}}), kModem | bit(NTSC_ST_LUMA_FILTER)});
    for (int ft = 0; ft < 2; ++ft)
        for (int lp = 1; lp <= 2; ++lp) {
            const std::string sfx = std::string(lp == 2 ? "full" : "light") + (ft ? "_bw" : "_ck");
            c.push_back({"lowpass_in_" + sfx, with(m, {{"chroma_lowpass_in", lp}, {"filter_type", ft}}),
                         kModem | bit(NTSC_ST_CHROMA_LOWPASS_IN)});
            c.push_back({"lowpass_out_" + sfx, with(m, {{"chroma_lowpass_out", lp}, {"filter_type", ft}}),
                         kModem | bit(NTSC_ST_CHROMA_LOWPASS_OUT)});
        }
    c.push_back({"sharpening", with(m, {{"composite_preemphasis", 1.106}}), kModem | bit(NTSC_ST_COMPOSITE_SHARPENING)});
    c.push_back({"luma_smear", with(m, {{"luma_smear", 0.6692}}), kModem | bit(NTSC_ST_LUMA_SMEAR)});
    c.push_back({"ringing", with(m, {{"ringing", true}, {"ringing_frequency", 0.45}, {"ringing_power", 5.674},
                                     {"ringing_scale", 4.935}}),
                 kModem | bit(NTSC_ST_RINGING)});
    c.push_back({"phase_error", with(m, {{"chroma_phase_error", 0.2}}), kModem | bit(NTSC_ST_CHROMA_PHASE_ERROR)});
    c.push_back({"phase_noise", with(m, {{"chroma_phase_noise_intensity", 0.2}}),
                 kModem | bit(NTSC_ST_CHROMA_PHASE_NOISE)});
    for (int t = 1; t <= 3; ++t)
        for (int ft = 0; ft < 2; ++ft)
            c.push_back({"tape" + std::to_string(t) + (ft ? "_bw" : "_ck"),
                         with(with(m, vhs), {{"vhs_tape_speed", t}, {"filter_type", ft}}),
                         kModem | bit(NTSC_ST_TAPE_SPEED)});
    c.push_back({"chroma_loss", with(with(m, vhs), {{"vhs_chroma_loss", 0.124}}), kModem | bit(NTSC_ST_CHROMA_LOSS)});
    for (int ft = 0; ft < 2; ++ft)
        c.push_back({std::string("sharpen") + (ft ? "_bw" : "_ck"),
                     with(with(m, vhs), {{"vhs_tape_speed", 2}, {"vhs_sharpen_enabled", true}, {"vhs_sharpen", 0.5},
                                         {"vhs_sharpen_frequency", 1.0}, {"filter_type", ft}}),
                     kModem | bit(NTSC_ST_TAPE_SPEED) | bit(NTSC_ST_SHARPEN)});
    c.push_back({"vert_blend", with(m, {{"vhs_chroma_vert_blend", true}}), kModem | bit(NTSC_ST_VERT_BLEND)});
    return c;
}

// ---- comparison -----------------------------------------------------------------------
struct Diff {
    int max = 0;
    double sum = 0;
    size_t n = 0, differ = 0, over1 = 0;
    void add(const Diff &o) {
        max = std::max(max, o.max);
        sum += o.sum;
        n += o.n;
        differ += o.differ;
        over1 += o.over1;
    }
    double mean() const { return n ? sum / double(n) : 0; }
    double frac() const { return n ? double(differ) / double(n) : 0; }
    double frac1() const { return n ? double(over1) / double(n) : 0; }
};

Diff compare(const ImageBGRA8 &ref, const std::vector<uint8_t> &gpu) {
    Diff d;
    for (int y = 0; y < ref.height; ++y)
        for (int x = 0; x < ref.width; ++x)
            for (int c = 0; c < 4; ++c) {
                const int a = ref.row(y)[x * 4 + c], b = gpu[(size_t(y) * ref.width + x) * 4 + c];
                const int e = std::abs(a - b);
                d.max = std::max(d.max, e);
                d.sum += e;
                d.n += c < 3;  // alpha must match too, but the statistics are over colour
                d.differ += e != 0;
                d.over1 += e > 1;
            }
    return d;
}

// Tolerance per case: the HOST-GPU.md rule.
struct Tol {
    int max = 1;
    double frac = 0.002;
};

struct Gpu {
    std::unique_ptr<gpu::Device> dev;
    std::unique_ptr<NtscGpu> ntsc;
    bool software = false;
};
Gpu &gpu_env() {
    static Gpu g = [] {
        Gpu x;
        x.dev = gputest::device();
        if (x.dev) {
            x.ntsc = std::make_unique<NtscGpu>(*x.dev);
            x.software = gpu_is_software(*x.dev);
            std::printf("ntsc gpu: %s device, multiply-adds: %s\n", x.software ? "software" : "hardware",
                        x.ntsc->fma_mode());
        }
        return x;
    }();
    return g;
}

FrameContext ctx_for(const json &look, int64_t frame, uint64_t seed, double scale) {
    FrameContext c;
    c.frame = frame;
    c.look = &look;
    c.seed = seed;
    c.render_scale = scale;
    return c;
}

// One case: the Rust path and the GPU path on the same picture and frame.
Diff run_case(NtscStage &st, const json &look, const ImageBGRA8 &src, int64_t frame, uint64_t seed, double scale,
              uint32_t expect, double *rust_ms = nullptr, double *gpu_ms = nullptr) {
    Gpu &g = gpu_env();
    const FrameContext ctx = ctx_for(look, frame, seed, scale);
    ImageBGRA8 ref = src;
    auto t0 = std::chrono::steady_clock::now();
    st.process(ref, ctx);
    auto t1 = std::chrono::steady_clock::now();
    NtscGpuPlan plan;
    st.gpu_plan(ctx, src.width, src.height, plan);
    REQUIRE_MESSAGE(plan.supported(), "unsupported: " << plan.unsupported_names());
    CHECK_MESSAGE((plan.info.enabled & expect) == expect, "settings didn't take: enabled 0x" << std::hex
                                                                                             << plan.info.enabled);
    auto tex = gputest::bgra(*g.dev, src.width, src.height, src.px);
    auto t2 = std::chrono::steady_clock::now();
    auto out = g.ntsc->run(*tex, plan);
    auto px = gputest::read(*g.dev, *out);
    auto t3 = std::chrono::steady_clock::now();
    if (rust_ms) *rust_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (gpu_ms) *gpu_ms += std::chrono::duration<double, std::milli>(t3 - t2).count();
    return compare(ref, px);
}

struct Report {
    std::map<std::string, Diff> by_case;
    void line(const std::string &name, const Diff &d, const Tol &t) {
        by_case[name].add(d);
        if (verbose())
            std::printf("PARITY %-24s max=%d mean=%.5f differ=%.4f%% over1=%.4f%% (tol max %d, %.2f%%)\n",
                        name.c_str(), d.max, d.mean(), d.frac() * 100, d.frac1() * 100, t.max, t.frac * 100);
    }
};

// The parity rule is enforced on D3D11 (fused mad, or the double kernels on
// WARP). On Metal, where the route is off in this phase (unverified, fast
// math), a reduced set of cases runs and only reports its numbers.
bool strict() {
    Gpu &g = gpu_env();
    return g.dev && g.dev->backend() == gpu::Backend::D3D11;
}

void check_tol(const std::string &name, const Diff &d, const Tol &t) {
    if (!strict()) {
        MESSAGE("report only (" << name << "): max " << d.max << ", " << d.frac() * 100 << "% differ");
        return;
    }
    CAPTURE(name);
    CAPTURE(d.max);
    CAPTURE(d.frac());
    CHECK(d.max <= t.max);
    CHECK(d.frac() <= t.frac);
}

// Stage-by-stage comparison of the YIQ planes (before the deinterlace): ntsc-rs
// with the stages up to each one (ntsc_debug_process_yiq) against the GPU
// with the same stages. The first stage whose planes differ is the culprit.
struct StageDiff {
    int stage;
    float max[3];
};
std::vector<StageDiff> stage_by_stage(NtscStage &st, const std::string &name, const json &look,
                                      const ImageBGRA8 &src, int64_t frame, uint64_t seed, double scale) {
    Gpu &g = gpu_env();
    const FrameContext ctx = ctx_for(look, frame, seed, scale);
    NtscGpuPlan full;
    st.gpu_plan(ctx, src.width, src.height, full);
    const char *dump = std::getenv("NTSCRT_NTSC_GPU_DUMP");
    auto tex = gputest::bgra(*g.dev, src.width, src.height, src.px);
    std::vector<StageDiff> out;
    uint32_t mask = 0;
    int n = 0;
    for (int s = 0; s < NTSC_ST_COUNT; ++s) {
        if (!(full.info.enabled & (1u << s))) continue;
        mask |= 1u << s;
        NtscGpuPlan plan;
        st.gpu_plan(ctx, src.width, src.height, plan, mask);
        int rows = 0;
        std::vector<float> rust = st.debug_planes(src, ctx, mask, &rows);
        std::vector<float> gp = g.ntsc->planes(*tex, plan);
        REQUIRE(rust.size() == gp.size());
        StageDiff d{s, {0, 0, 0}};
        const size_t pn = rust.size() / 3;
        for (size_t i = 0; i < rust.size(); ++i) {
            const float e = std::abs(rust[i] - gp[i]);
            float &m = d.max[i / pn];
            if (!(e <= m)) m = e;  // NaN shows up as NaN
        }
        out.push_back(d);
        if (verbose())
            std::printf("STAGE %-22s %2d %-22s maxabs Y=%.3g I=%.3g Q=%.3g\n", name.c_str(), n, ntsc_stage_name(s),
                        d.max[0], d.max[1], d.max[2]);
        if (dump && *dump) {
            fs::create_directories(dump);
            const std::string base = std::string(dump) + "/" + name + "." + std::to_string(n) + "." + ntsc_stage_name(s);
            std::ofstream(base + ".rust.f32", std::ios::binary)
                .write(reinterpret_cast<const char *>(rust.data()), std::streamsize(rust.size() * 4));
            std::ofstream(base + ".gpu.f32", std::ios::binary)
                .write(reinterpret_cast<const char *>(gp.data()), std::streamsize(gp.size() * 4));
        }
        ++n;
    }
    return out;
}

}  // namespace

TEST_CASE("ntsc gpu: plan of the house look names what keeps it on the CPU") {
    NtscStage st;
    const json look = json::object();
    FrameContext ctx = ctx_for(look, 0, 0, 1.0);
    NtscGpuPlan plan;
    st.gpu_plan(ctx, 64, 48, plan);
    CHECK_FALSE(plan.supported());
    const std::string u = plan.unsupported_names();
    CHECK(u.find("head_switching") != std::string::npos);
    CHECK(u.find("composite_noise") != std::string::npos);
    CHECK(plan.info.plane_rows == 48);  // InterleavedUpper: both fields' rows
    CHECK(plan.info.num_fields == 2);
    CHECK((ntsc_gpu_implemented_stages() & kModem) == kModem);
}

TEST_CASE("ntsc gpu: row seeds are ntsc-rs's (phase noise rows match through the effect)") {
    // The chroma phase noise rows come from the same Seeder copy as
    // ntsc_gpu_row_seeds; the parity cases below check them against ntsc-rs.
    const json look = look_of(gpu_full_settings());
    std::vector<NtscGpuRowSeed> seeds(20);
    NtscInstance *inst = ntsc_new();
    REQUIRE(ntsc_settings_from_json(inst, ntsc_effective_settings_json(look).c_str(), nullptr) == 0);
    REQUIRE(ntsc_gpu_row_seeds(inst, 32, 20, 4, 1, 1, 77, 0, seeds.data()) == 0);
    // Deterministic, distinct per row.
    std::vector<NtscGpuRowSeed> again(20);
    REQUIRE(ntsc_gpu_row_seeds(inst, 32, 20, 4, 1, 1, 77, 0, again.data()) == 0);
    for (int i = 0; i < 20; ++i) {
        CHECK(seeds[size_t(i)].seed_lo == again[size_t(i)].seed_lo);
        CHECK(seeds[size_t(i)].offset >= 0.0f);
        CHECK(seeds[size_t(i)].offset < 32.0f);
    }
    CHECK(seeds[0].seed_lo != seeds[1].seed_lo);
    CHECK(ntsc_gpu_row_shifts(inst, 32, 20, 4, 1, 1, 77, 0, nullptr) == -1);  // phase 3
    ntsc_free(inst);
}

TEST_CASE("ntsc gpu: every kernel compiles on this device") {
    Gpu &g = gpu_env();
    if (!g.dev) return;
    // On Metal this compiles the MSL library at runtime: the build only translates the Slang.
    for (const char *k : {"from_bgra8", "to_bgra8", "row_filter", "luma_box", "chroma_into_luma", "demod_luma",
                          "demodulate", "rotate_chroma", "chroma_loss", "vert_blend", "probe_fma"}) {
        CAPTURE(k);
        gpu::Kernel *kk = nullptr;
        CHECK_NOTHROW(kk = g.dev->kernel(std::string("ntsc_gpu/") + k));
        CHECK(kk != nullptr);
    }
    // The exact-FMA variants use doubles in HLSL (fma() on Metal): needed only where mad() isn't fused.
    for (const char *k : {"from_bgra8_x", "to_bgra8_x", "row_filter_x"}) {
        try {
            g.dev->kernel(std::string("ntsc_gpu/") + k);
        } catch (const std::exception &e) {
            CHECK_MESSAGE(std::string(g.ntsc->fma_mode()) != "double", k << ": " << e.what());
        }
    }
}

TEST_CASE("ntsc gpu: parity with ntsc-rs, each pass alone") {
    Gpu &g = gpu_env();
    if (!g.dev) return;
    if (!strict()) {
        MESSAGE("not on D3D11: the per-pass matrix runs on D3D11 only in this phase");
        return;
    }
    NtscStage st;
    Report rep;
    const std::vector<std::string> pics = {"gradient", "bars", "noise", "edges"};
    for (const Case &c : single_cases()) {
        const json look = look_of(c.settings);
        Diff total;
        for (const std::string &pk : pics) {
            const ImageBGRA8 src = picture(pk, 720, 480);
            for (int64_t frame : {0, 1, 7})
                for (uint64_t seed : {0ull, 12345ull}) total.add(run_case(st, look, src, frame, seed, 1.0, c.expect));
        }
        const Tol t;
        rep.line(c.name, total, t);
        check_tol(c.name, total, t);
    }
}

TEST_CASE("ntsc gpu: parity with ntsc-rs, all GPU passes together, every field mode, sizes, render scale") {
    Gpu &g = gpu_env();
    if (!g.dev) return;
    NtscStage st;
    Report rep;
    const std::vector<std::string> pics = {"gradient", "bars", "noise", "edges"};
    struct Size {
        int w, h;
        double scale;
        bool video_size;
    };
    const Size sizes[] = {{720, 480, 1.0, true}, {333, 247, 1.0, true}, {720, 480, 0.5, false}, {5, 6, 1.0, true}};
    for (int field = 0; field <= 5; ++field)
        for (const Size &sz : sizes) {
            if (!strict() && (field != 4 || sz.w != 333)) continue;  // Metal: one report-only case
            json s = with(gpu_full_settings(), {{"use_field", field}, {"scale_with_video_size", sz.video_size}});
            const json look = look_of(s);
            Diff total;
            double rust_ms = 0, gpu_ms = 0;
            for (const std::string &pk : pics) {
                const ImageBGRA8 src = picture(pk, sz.w, sz.h);
                for (int64_t frame : {0, 1, 2, 7})
                    for (uint64_t seed : {0ull, 99ull})
                        total.add(run_case(st, look, src, frame, seed, sz.scale, kModem, &rust_ms, &gpu_ms));
            }
            const std::string name = "full_f" + std::to_string(field) + "_" + std::to_string(sz.w) + "x" +
                                     std::to_string(sz.h) + (sz.scale != 1.0 ? "_rs0.5" : "");
            const Tol t;
            rep.line(name, total, t);
            if (verbose() && sz.w == 720 && sz.scale == 1.0)
                std::printf("TIME %-24s rust %.2f ms/frame, gpu (upload+run+readback) %.2f ms/frame%s\n", name.c_str(),
                            rust_ms / 32, gpu_ms / 32, g.software ? " [software device]" : "");
            check_tol(name, total, t);
        }
}

TEST_CASE("ntsc gpu: parity on the bundled presets that run entirely on the GPU") {
    Gpu &g = gpu_env();
    if (!g.dev) return;
    NtscStage st;
    Report rep;
    int ran = 0;
    for (auto &e : fs::directory_iterator(fs::path(NTSCRT_SOURCE_DIR) / "presets")) {
        if (e.path().extension() != ".json") continue;
        std::ifstream f(e.path(), std::ios::binary);
        const json look = json::parse(f);
        if (!st.active(look)) continue;
        NtscGpuPlan plan;
        st.gpu_plan(ctx_for(look, 0, 0, 1.0), 1920, 1080, plan);
        if (!plan.supported()) continue;
        ++ran;
        if (!strict()) continue;
        Diff total;
        for (const std::string &pk : {"gradient", "edges"})
            for (int64_t frame : {0, 1, 30}) {
                total.add(run_case(st, look, picture(pk, 1920, 1080), frame, 0, 1.0, kModem));
                total.add(run_case(st, look, picture(pk, 1280, 720), frame, 5, 0.5, kModem));
            }
        const std::string name = "preset " + e.path().stem().string();
        rep.line(name, total, Tol{});
        check_tol(name, total, Tol{});
    }
    CHECK(ran >= 1);  // "Mild VHS" in this phase
}

TEST_CASE("ntsc gpu: a field of one row with the vertical blend stays on ntsc-rs (which fails the frame)") {
    // ntsc-rs's chroma_vert_blend slices two rows of a field's scratch buffer and
    // panics on a 1-row field (pictures 1-3 rows tall, depending on the field
    // mode); the GPU plan leaves those frames to it so both paths behave alike.
    NtscStage st;
    const json look = look_of(with(gpu_full_settings(), {{"use_field", 4}}));
    NtscGpuPlan plan;
    st.gpu_plan(ctx_for(look, 0, 0, 1.0), 2, 3, plan);
    CHECK(plan.unsupported_names() == "chroma_vert_blend");
    ImageBGRA8 im = picture("bars", 2, 3);
    FrameContext c = ctx_for(look, 0, 0, 1.0);
    CHECK_THROWS(st.process(im, c));  // (ntsc-rs prints its panic message on stderr)
    const json no_blend = look_of(with(gpu_full_settings(), {{"use_field", 4}, {"vhs_chroma_vert_blend", false}}));
    st.gpu_plan(ctx_for(no_blend, 0, 0, 1.0), 2, 3, plan);
    CHECK(plan.supported());
}

TEST_CASE("ntsc gpu: stage-by-stage YIQ planes match ntsc-rs") {
    Gpu &g = gpu_env();
    if (!g.dev) return;
    NtscStage st;
    for (int field : {4, 0, 3}) {
        if (!strict() && field != 4) continue;
        json s = with(gpu_full_settings(), {{"use_field", field}});
        const json look = look_of(s);
        const ImageBGRA8 src = picture("edges", 333, 247);
        const std::string name = "full_f" + std::to_string(field);
        auto diffs = stage_by_stage(st, name, look, src, 5, 3, 1.0);
        CHECK(diffs.size() >= 10);
        {
            // The harness sees a difference when there is one: the GPU planes
            // without the last pass against ntsc-rs's with it.
            const FrameContext c = ctx_for(look, 5, 3, 1.0);
            NtscGpuPlan plan;
            st.gpu_plan(c, src.width, src.height, plan);
            auto tex = gputest::bgra(*g.dev, src.width, src.height, src.px);
            const std::vector<float> rust = st.debug_planes(src, c, 0xffffffffu);
            const std::vector<float> short_gpu = g.ntsc->planes(*tex, plan, int(plan.passes.size()) - 1);
            float m = 0;
            for (size_t i = 0; i < rust.size(); ++i) m = std::max(m, std::abs(rust[i] - short_gpu[i]));
            if (strict()) CHECK(m > 1e-3f);
        }
        for (const StageDiff &d : diffs) {
            CAPTURE(ntsc_stage_name(d.stage));
            // Float differences of a few ulps of the planes' range (|Y| < ~2) at most:
            // the same operations, fused where ntsc-rs fuses.
            if (strict())
                for (int p = 0; p < 3; ++p) CHECK(d.max[p] <= 1e-4f);
            else
                MESSAGE("report only: " << ntsc_stage_name(d.stage) << " Y " << d.max[0] << " I " << d.max[1]
                                        << " Q " << d.max[2]);
        }
    }
}

TEST_CASE("ntsc gpu: GPU time per stage (NTSCRT_NTSC_GPU_VERBOSE=1)") {
    // Throughput, not latency: 20 frames queued back to back, one wait. A
    // stage's cost is the full plan's time minus the time without it.
    Gpu &g = gpu_env();
    if (!g.dev || !verbose()) return;
    NtscStage st;
    const json look = look_of(gpu_full_settings());
    for (int w : {3840, 1920}) {
        const int h = w * 9 / 16;
        const ImageBGRA8 src = picture("gradient", w, h);
        const FrameContext c = ctx_for(look, 1, 0, 1.0);
        auto tex = gputest::bgra(*g.dev, w, h, src.px);
        auto time_mask = [&](uint32_t mask) {
            NtscGpuPlan plan;
            st.gpu_plan(c, w, h, plan, mask);
            for (int i = 0; i < 20; ++i) g.ntsc->run(*tex, plan);  // warm-up (clocks up)
            g.dev->finish();
            double best = 1e9;
            for (int trial = 0; trial < 3; ++trial) {
                const int n = 30;
                const auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < n; ++i) g.ntsc->run(*tex, plan);
                g.dev->finish();
                const auto t1 = std::chrono::steady_clock::now();
                best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count() / n);
            }
            return best;
        };
        NtscGpuPlan full;
        st.gpu_plan(c, w, h, full);
        const double all = time_mask(0xffffffffu), modem = time_mask(0);
        std::printf("STAGETIME %dx%d %-22s %.3f ms/frame (%s)\n", w, h, "all", all, g.ntsc->fma_mode());
        std::printf("STAGETIME %dx%d %-22s %.3f ms/frame (conversions + modulation)\n", w, h, "modem only", modem);
        for (int s2 = 0; s2 < NTSC_ST_COUNT; ++s2) {
            if (!(full.info.enabled & (1u << s2)) || (kModem & (1u << s2))) continue;
            std::printf("STAGETIME %dx%d %-22s +%.3f ms\n", w, h, ntsc_stage_name(s2), time_mask(1u << s2) - modem);
        }
    }
}

TEST_CASE("ntsc gpu: coverage of the bundled presets") {
    // Which presets would run entirely on the GPU after each phase. Phase 2:
    // composite / snow / luma / chroma noise; phase 3: the VHS passes (head
    // switching, tracking noise, chroma delay, edge wave).
    const uint32_t phase2 = bit(NTSC_ST_COMPOSITE_NOISE) | bit(NTSC_ST_SNOW) | bit(NTSC_ST_LUMA_NOISE) |
                            bit(NTSC_ST_CHROMA_NOISE);
    const uint32_t phase3 = bit(NTSC_ST_HEAD_SWITCHING) | bit(NTSC_ST_TRACKING_NOISE) | bit(NTSC_ST_CHROMA_DELAY) |
                            bit(NTSC_ST_EDGE_WAVE);
    NtscStage st;
    int n = 0, ntsc_on = 0, p1 = 0, p2 = 0, p3 = 0;
    for (auto &e : fs::directory_iterator(fs::path(NTSCRT_SOURCE_DIR) / "presets")) {
        if (e.path().extension() != ".json") continue;
        std::ifstream f(e.path(), std::ios::binary);
        const json look = json::parse(f);
        ++n;
        if (!st.active(look)) {
            if (verbose()) std::printf("COVERAGE %-28s ntsc off\n", e.path().stem().string().c_str());
            continue;
        }
        ++ntsc_on;
        NtscGpuPlan plan;
        st.gpu_plan(ctx_for(look, 10, 0, 1.0), 1920, 1080, plan);
        const uint32_t u = plan.info.unsupported;
        CHECK((u & ~(phase2 | phase3)) == 0);  // nothing outside the plan for phases 2-3
        const int ph = u == 0 ? 1 : (u & ~phase2) == 0 ? 2 : 3;
        p1 += ph <= 1;
        p2 += ph <= 2;
        p3 += ph <= 3;
        if (verbose())
            std::printf("COVERAGE %-28s gpu after phase %d (needs: %s)\n", e.path().stem().string().c_str(), ph,
                        u ? plan.unsupported_names().c_str() : "-");
    }
    if (verbose())
        std::printf("COVERAGE presets=%d ntsc_on=%d full_gpu_after_phase1=%d phase2=%d phase3=%d\n", n, ntsc_on, p1,
                    p2, p3);
    CHECK(ntsc_on > 0);
}

// ---- the pipeline: default untouched, fallback exact --------------------------------------
namespace {

std::string pipeline_look(const json &settings) {
    json l = look_of(settings);
    l["shader"] = {{"enabled", false}};
    l["downscale"] = {{"enabled", false}};
    l["glitch"] = {{"enabled", false}};
    l["digital"] = {{"enabled", false}};
    return l.dump();
}

struct Rendered {
    std::vector<uint8_t> px;
    std::string timings;
};
Rendered render(ntscrt_instance *inst, const std::string &look, const ImageBGRA8 &src, int64_t frame) {
    Rendered r;
    r.px.assign(src.px.size(), 0);
    ntscrt_image si{src.width, src.height, ptrdiff_t(src.width) * 4, NTSCRT_BGRA8,
                    const_cast<uint8_t *>(src.px.data())};
    ntscrt_image di{src.width, src.height, ptrdiff_t(src.width) * 4, NTSCRT_BGRA8, r.px.data()};
    ntscrt_render_args a{};
    a.fps = 30;
    a.frame_index = frame;
    a.render_scale = 1;
    a.look_json = look.c_str();
    const ntscrt_status st = ntscrt_render_cpu(inst, &a, &si, &di);
    REQUIRE_MESSAGE(st == NTSCRT_OK, ntscrt_last_error(inst));
    r.timings = ntscrt_last_timings(inst);
    return r;
}

ntscrt_instance *make_instance() {
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_BUILT_RESOURCES_DIR};
    ntscrt_instance *inst = nullptr;
    REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
    return inst;
}

struct EnvGuard {
    ~EnvGuard() { gputest::set_env("NTSCRT_NTSC_GPU", nullptr); }
};

}  // namespace

TEST_CASE("ntsc gpu pipeline: unset or 0 changes nothing; an unsupported setting falls back exactly") {
    EnvGuard guard;
    ntscrt_instance *inst = make_instance();
    const ImageBGRA8 src = picture("bars", 160, 120);
    const std::string house = pipeline_look(json::object());  // head switching etc.: not on the GPU yet
    const std::string gpu_ok = pipeline_look(gpu_full_settings());

    gputest::set_env("NTSCRT_NTSC_GPU", nullptr);
    const Rendered a = render(inst, house, src, 4);
    const Rendered a2 = render(inst, gpu_ok, src, 4);
    CHECK(a.timings.find("ntsc_route") == std::string::npos);
    CHECK(a2.timings.find("ntsc_route") == std::string::npos);

    // The Rust stage itself, for reference: the pipeline's result is it (all GPU stages off).
    {
        NtscStage st;
        const json lj = json::parse(gpu_ok);
        FrameContext c = ctx_for(lj, 4, 0, 1.0);
        ImageBGRA8 ref = src;
        st.process(ref, c);
        CHECK(ref.px == a2.px);
    }

    gputest::set_env("NTSCRT_NTSC_GPU", "0");
    CHECK(render(inst, house, src, 4).px == a.px);
    CHECK(render(inst, gpu_ok, src, 4).px == a2.px);

    gputest::set_env("NTSCRT_NTSC_GPU", "1");
    const Rendered b = render(inst, house, src, 4);
    CHECK(b.px == a.px);  // fallback: byte-identical to the Rust path
    if (ntscrt::testing::stats(inst).has_gpu) {
        CHECK(b.timings.find("ntsc_route=cpu") != std::string::npos);
        // Why: the unsupported stage, or (WARP without "force") the software device.
        CHECK((b.timings.find("head_switching") != std::string::npos ||
               b.timings.find("software device") != std::string::npos));
    }

    if (ntscrt::testing::stats(inst).has_gpu && gputest::native_backend() == gpu::Backend::D3D11) {
        gputest::set_env("NTSCRT_NTSC_GPU", "force");  // also on WARP
        const Rendered c = render(inst, gpu_ok, src, 4);
        CHECK(c.timings.find("ntsc_route=gpu") != std::string::npos);
        ImageBGRA8 ref;
        ref.resize(src.width, src.height);
        ref.px = a2.px;
        const Diff d = compare(ref, c.px);
        check_tol("pipeline", d, Tol{});
    }
    ntscrt_destroy(inst);
}
