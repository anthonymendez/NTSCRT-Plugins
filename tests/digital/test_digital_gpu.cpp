// The Digital video stage's GPU route (core/stages/digital/gpu, docs/dev/DIGITAL-GPU.md)
// against the CPU code, which stays the reference: byte for byte.
//   - the kernels alone: Renderer::scale / upscale vs DigitalGpu::scale / upscale, many
//     sizes (odd ones, upscaling, the same size, tiny), aliasing and fields, on this
//     PC's GPU and on WARP (the double-precision kernels, if the probe picks them);
//   - the pipeline: ntscrt_render_cpu with the GPU route against NTSCRT_DIGITAL_GPU=0,
//     every Digital video format, the bundled Digital looks and each Digital setting
//     alone, sequences of frames with a seek back and a jump (Datamosh and the
//     inter-frame codec need them), odd sizes, crop, render scale, 8-bit / half / float,
//     NTSC on the GPU and on ntsc-rs, GPU errors.
//
// Environment:
//   NTSCRT_REQUIRE_GPU=1     a missing GPU fails instead of skipping
//   NTSCRT_D3D11_WARP=1      the shared device is WARP (CI): the route is forced on there
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../gpu/gpu_test_util.h"
#include "gpu/gpu_route.h"
#include "ntscrt_host/params.h"
#include "stages/digital/gpu/digital_gpu.h"
#include "stages/digital/renderer.h"
#include "testing.h"

using namespace ntscrt;
using json = nlohmann::json;

namespace {

// ---- devices: this PC's GPU, and WARP ------------------------------------------------------
struct TestDevice {
    std::string name;
    std::unique_ptr<gpu::Device> dev;
};
std::vector<TestDevice> &devices() {
    static std::vector<TestDevice> all = [] {
        std::vector<TestDevice> v;
        if (auto d = gputest::device()) {
            const bool sw = gpu_is_software(*d);
            v.push_back({sw ? "WARP" : gpu_adapter_description(*d), std::move(d)});
            if (!sw && gputest::native_backend() == gpu::Backend::D3D11) {
                const char *was = std::getenv("NTSCRT_D3D11_WARP");
                const std::string keep = was ? was : "";
                gputest::set_env("NTSCRT_D3D11_WARP", "1");
                if (auto w = gpu::Device::create(gpu::Backend::D3D11, nullptr, nullptr)) v.push_back({"WARP", std::move(w)});
                gputest::set_env("NTSCRT_D3D11_WARP", was ? keep.c_str() : nullptr);
            }
        }
        return v;
    }();
    return all;
}

// The shared device the pipeline uses is a software one (CI): the route is off by
// default there, so the pipeline tests force it.
bool shared_is_software() {
    static const bool sw = [] {
        auto d = gputest::device();
        return d && gpu_is_software(*d);
    }();
    return sw;
}

// ---- pictures --------------------------------------------------------------------------------
uint32_t lcg(uint32_t &s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}
// Busy BGRA: gradients, hard edges and noise in every channel; alpha varies when `alpha`.
std::vector<uint8_t> busy(int w, int h, uint32_t seed, bool alpha) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    uint32_t s = seed;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            const int edge = ((x / 7 + y / 5) & 1) ? 200 : 30;
            p[0] = uint8_t((x * 255 / std::max(1, w - 1) + int(lcg(s) % 40)) % 256);
            p[1] = uint8_t(std::clamp(edge + int(lcg(s) % 50) - 25, 0, 255));
            p[2] = uint8_t((y * 255 / std::max(1, h - 1) + (x % 3) * 60) % 256);
            p[3] = alpha ? uint8_t(lcg(s) % 256) : 255;
        }
    return px;
}

struct Mismatch {
    size_t n = 0, first = 0;
    int max = 0;
};
Mismatch diff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    Mismatch m;
    if (a.size() != b.size()) return {std::max(a.size(), b.size()), 0, 255};
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) {
            if (!m.n) m.first = i;
            ++m.n;
            m.max = std::max(m.max, std::abs(int(a[i]) - int(b[i])));
        }
    return m;
}

// D3D11: the kernels must give the CPU's bytes. Metal: the route is off (compiled with
// fast math, unverified), so the differences are only reported.
bool exact_required() { return gputest::native_backend() == gpu::Backend::D3D11; }

digital::Settings optics(double aliasing, double fields) {
    digital::Settings s;
    s[digital::kAliasing] = aliasing;
    s[digital::kFields] = fields;
    return s;
}

}  // namespace

TEST_CASE("digital gpu: the arithmetic probe") {
    for (auto &d : devices()) {
        DigitalGpu g(*d.dev);
        const std::string mode = g.arith_mode();
        MESSAGE(d.name << ": arith " << mode);
        // The double-precision kernels, as the probe would pick them, give the same probe results.
        gputest::set_env("NTSCRT_DIGITAL_GPU_ARITH", "double");
        DigitalGpu x(*d.dev);
        if (exact_required()) CHECK(std::string(x.arith_mode()) == "double");
        gputest::set_env("NTSCRT_DIGITAL_GPU_ARITH", nullptr);
        // Hardware D3D11 GPUs and WARP round float arithmetic like the CPU, or do it in double.
        if (gputest::native_backend() == gpu::Backend::D3D11) CHECK(mode != "inexact");
    }
}

TEST_CASE("digital gpu: the capture scale-down is Renderer::scale's, byte for byte") {
    struct Case {
        int w, h, cw, ch;
    };
    const Case cases[] = {
        {3840, 2160, 320, 180},  // UHD to a webcam
        {1920, 1080, 720, 406},  // MiniDV
        {1920, 1080, 1280, 720},  // Downscale off: capped at 1280 wide
        {1920, 1080, 240, 136},  // CD-ROM
        {1280, 720, 480, 270},   // Datamosh
        {333, 197, 320, 190},    // a little smaller
        {161, 97, 320, 194},     // smaller than the capture: Lanczos upsamples
        {17, 9, 16, 16},         // tiny; upsampled rows
        {1, 1, 16, 16},          // one pixel
        {64, 48, 64, 48},        // the same size: copied
        {1001, 3, 320, 16},      // a strip
    };
    const double aliasing[] = {0, 0.2, 0.5, 0.74, 1.0};
    for (auto &d : devices())
      for (const char *mode : {"", "double"}) {
        // The plain kernels (or what the probe picks), then the double-precision ones.
        gputest::set_env("NTSCRT_DIGITAL_GPU_ARITH", *mode ? mode : nullptr);
        DigitalGpu g(*d.dev);
        const std::string arith = g.arith_mode();
        gputest::set_env("NTSCRT_DIGITAL_GPU_ARITH", nullptr);
        const bool warp = gpu_is_software(*d.dev);
        int n = 0, bad = 0;
        for (const Case &c : cases) {
            // WARP: the UHD case once (it emulates the GPU on the CPU, in double precision)
            for (bool alpha : {false, true}) {
                const std::vector<uint8_t> src = busy(c.w, c.h, uint32_t(c.w * 31 + c.h + alpha), alpha);
                auto tex = gputest::bgra(*d.dev, c.w, c.h, src);
                for (double a : aliasing)
                    for (double f : {0.0, 1.0}) {
                        if ((warp || *mode) && c.w >= 3840 && (alpha || a != 0.74)) continue;
                        const digital::Settings s = optics(a, f);
                        const auto cpu = digital::Renderer::scale(src.data(), c.w, c.h, c.w * 4, {c.cw, c.ch}, s);
                        const auto gpu = g.scale(*tex, {c.cw, c.ch}, s);
                        const Mismatch m = diff(cpu, gpu);
                        ++n;
                        if (m.n) {
                            ++bad;
                            MESSAGE(d.name << " arith " << arith << " " << c.w << "x" << c.h << " -> " << c.cw << "x" << c.ch << " aliasing " << a
                                           << " fields " << f << " alpha " << alpha << ": " << m.n << " bytes differ (max "
                                           << m.max << ", first at byte " << m.first << ")");
                        }
                        if (exact_required()) CHECK(m.n == 0);
                    }
            }
        }
        MESSAGE(d.name << " (arith " << arith << "): " << n << " scale cases, " << bad << " different");
      }
}

TEST_CASE("digital gpu: the upscale is Renderer::upscale's, byte for byte") {
    struct Case {
        int cw, ch, w, h;
    };
    const Case cases[] = {{320, 180, 3840, 2160}, {720, 406, 1920, 1080}, {240, 136, 333, 197},
                          {16, 16, 17, 9},        {320, 194, 161, 97},    {64, 48, 64, 48}};
    for (auto &d : devices()) {
        DigitalGpu g(*d.dev);
        for (const Case &c : cases) {
            const std::vector<uint8_t> pic = busy(c.cw, c.ch, uint32_t(c.cw + 7 * c.ch), true);
            std::vector<uint8_t> cpu(size_t(c.w) * c.h * 4, 0);
            digital::Renderer::upscale(pic, {c.cw, c.ch}, cpu.data(), c.w, c.h, c.w * 4);
            auto tex = g.upscale(pic, {c.cw, c.ch}, c.w, c.h);
            const Mismatch m = diff(cpu, gputest::read(*d.dev, *tex));
            if (m.n) MESSAGE(d.name << " " << c.cw << "x" << c.ch << " -> " << c.w << "x" << c.h << ": " << m.n << " differ");
            CHECK(m.n == 0);  // (a copy: exact on any backend)
        }
    }
}

// ---- the pipeline ------------------------------------------------------------------------------
namespace {

size_t bytes_per_pixel(ntscrt_pixel_format f) { return f == NTSCRT_RGBA32F ? 16 : f == NTSCRT_RGBA16F ? 8 : 4; }
uint16_t f2h(float x) {
    uint32_t f;
    std::memcpy(&f, &x, 4);
    const uint32_t s = (f >> 16) & 0x8000u;
    const int e = int((f >> 23) & 0xff) - 127 + 15;
    if (e <= 0) return uint16_t(s);
    return uint16_t(s | (uint32_t(e) << 10) | (((f & 0x7fffff) + 0x1000) >> 13));
}

// A host picture (top row first) of frame `frame`: a scene moving right and down,
// with noise, so the codec's motion search, its residuals and Datamosh all work.
struct HostPic {
    int w = 0, h = 0;
    ntscrt_pixel_format f = NTSCRT_RGBA8;
    std::vector<uint8_t> mem;
    ntscrt_image image() { return ntscrt_image{w, h, ptrdiff_t(size_t(w) * bytes_per_pixel(f)), f, mem.data()}; }
};
HostPic host_frame(int w, int h, ntscrt_pixel_format f, int64_t frame) {
    HostPic p{w, h, f, std::vector<uint8_t>(size_t(w) * h * bytes_per_pixel(f))};
    uint32_t s = uint32_t(frame * 7919 + 17);
    const int dx = int(frame * 3), dy = int(frame);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int X = x - dx, Y = y - dy;
            float v[4];
            v[0] = float(((X * 255 / std::max(1, w)) % 256 + 256) % 256) / 255.f;
            v[1] = (((X / 9 + Y / 7) & 1) ? 0.75f : 0.2f) + float(lcg(s) % 16) / 255.f - 0.03f;
            v[2] = 0.5f + 0.45f * std::sin(0.07f * float(X + 2 * Y));
            const bool box = x > w / 3 + dx && x < w / 3 + dx + w / 5 && y > h / 4 + dy && y < h / 4 + dy + h / 4;
            if (box) v[0] = 0.95f, v[1] = 0.9f, v[2] = 0.1f;
            v[3] = 1.f;
            uint8_t *q = &p.mem[(size_t(y) * w + x) * bytes_per_pixel(f)];
            for (int c = 0; c < 4; ++c) {
                const float c01 = std::clamp(v[c], 0.f, 1.f);
                if (f == NTSCRT_RGBA32F) std::memcpy(q + c * 4, &c01, 4);
                else if (f == NTSCRT_RGBA16F) {
                    const uint16_t hv = f2h(c01);
                    std::memcpy(q + c * 2, &hv, 2);
                } else q[c] = uint8_t(std::lround(c01 * 255.f));
            }
        }
    return p;
}

struct Source {
    int w, h;
    ntscrt_pixel_format f;
    std::map<int64_t, HostPic> frames;
    ntscrt_image im;
    const ntscrt_image *get(int64_t n) {
        auto it = frames.find(n);
        if (it == frames.end()) it = frames.emplace(n, host_frame(w, h, f, n)).first;
        im = it->second.image();
        return &im;
    }
};
const ntscrt_image *fetch(void *u, int64_t n) { return static_cast<Source *>(u)->get(n); }
void release(void *, const ntscrt_image *) {}

struct Rendered {
    std::vector<uint8_t> px;
    std::string timings;
};
Rendered render(ntscrt_instance *inst, Source &src, const std::string &look, int64_t frame, double scale,
                const char *digital_env) {
    gputest::set_env("NTSCRT_DIGITAL_GPU", digital_env);
    Rendered r;
    ntscrt_image s = *src.get(frame);
    r.px.assign(size_t(src.w) * src.h * bytes_per_pixel(src.f), 0);
    ntscrt_image d{src.w, src.h, s.row_bytes, src.f, r.px.data()};
    ntscrt_render_args a{};
    a.fps = 30;
    a.frame_index = frame;
    a.render_scale = scale;
    a.look_json = look.c_str();
    a.seed = 7;
    a.fetch_source = fetch;
    a.release_source = release;
    a.user = &src;
    const ntscrt_status st = ntscrt_render_cpu(inst, &a, &s, &d);
    gputest::set_env("NTSCRT_DIGITAL_GPU", nullptr);
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

// The plug-in's look (every param at its schema default), as hosts build it.
json plugin_default() {
    static const json l = [] {
        const auto schema = host::Schema::parse(ntscrt_param_schema_json());
        return host::build_look(schema, [&](size_t i) { return schema.params[i].def; }).look;
    }();
    return l;
}
const json &schema_json() {
    static const json s = json::parse(ntscrt_param_schema_json());
    return s;
}
// A Digital video format applied to the look, as the Preset menu does.
json with_format(json look, const std::string &id) {
    for (const json &f : schema_json()["digital_formats"])
        if (f.value("id", std::string()) == id)
            for (auto it = f["applies"].begin(); it != f["applies"].end(); ++it) {
                if (it.value().is_object())
                    for (auto v = it.value().begin(); v != it.value().end(); ++v)
                        host::set_at(look, it.key() + "/" + v.key(), v.value());
                else host::set_at(look, it.key(), it.value());
            }
    return look;
}
json preset(const std::string &name) {
    std::ifstream f(std::string(NTSCRT_SOURCE_DIR) + "/presets/" + name + ".json", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    json p = json::parse(ss.str(), nullptr, false);
    REQUIRE(!p.is_discarded());
    return p;
}

const char *gpu_env() { return shared_is_software() ? "force" : nullptr; }

// Frames in order, the same frame again, a seek back (a re-run from the last
// keyframe, earlier frames fetched), a jump forward and on: the GPU route's
// pictures are the CPU code's, frame by frame.
struct Sequence {
    int w = 241, h = 135;
    ntscrt_pixel_format f = NTSCRT_RGBA8;
    double scale = 1.0;
    std::vector<int64_t> frames = {0, 1, 2, 3, 4, 5, 5, 2, 30, 31, 32};
};
void check_sequence(const std::string &name, const json &look_j, const Sequence &q = {}, const char *ntsc_env = nullptr) {
    CAPTURE(name);
    const std::string look = look_j.dump();
    ntscrt_instance *cpu = make_instance(), *gpu = make_instance();
    Source src{q.w, q.h, q.f, {}, {}};
    // On a software device NTSC takes ntsc-rs by default, which keeps Digital video on
    // the CPU: forced onto the GPU there too, so the Digital kernels run.
    gputest::set_env("NTSCRT_NTSC_GPU", ntsc_env ? ntsc_env : shared_is_software() ? "force" : nullptr);
    size_t frames = 0, same = 0;
    std::string last;
    for (int64_t n : q.frames) {
        const Rendered a = render(cpu, src, look, n, q.scale, "0");
        const Rendered b = render(gpu, src, look, n, q.scale, gpu_env());
        CHECK(a.timings.find("digital_route=cpu (NTSCRT_DIGITAL_GPU=0)") != std::string::npos);
        if (ntscrt::testing::stats(gpu).has_gpu) {
            if (ntsc_env && std::string(ntsc_env) == "0")
                CHECK(b.timings.find("digital_route=cpu (ntsc-rs needs the picture on the CPU)") != std::string::npos);
            else if (gputest::native_backend() == gpu::Backend::D3D11)
                CHECK(b.timings.find("digital_route=gpu") != std::string::npos);
        }
        const Mismatch m = diff(a.px, b.px);
        if (m.n) MESSAGE(name << " frame " << n << ": " << m.n << " bytes differ (max " << m.max << ")\n" << b.timings);
        CHECK(m.n == 0);
        ++frames;
        same += m.n == 0;
        last = b.timings;
    }
    gputest::set_env("NTSCRT_NTSC_GPU", nullptr);
    MESSAGE(name << ": " << same << " of " << frames << " frames identical; " << last);
    ntscrt_destroy(cpu);
    ntscrt_destroy(gpu);
}

json quick(json l) {  // the CRT shader off: the Digital stage's picture is what's compared
    l["shader"]["enabled"] = false;
    return l;
}

}  // namespace

TEST_CASE("digital gpu pipeline: every Digital video format, sizes and pixel formats") {
    for (const char *id : {"webcam", "video_call", "minidv", "early_web", "cd_rom", "datamosh"}) {
        const json l = with_format(plugin_default(), id);
        check_sequence(std::string(id) + " 241x135 8-bit", quick(l));
        Sequence fhd;
        fhd.w = 1920, fhd.h = 1080, fhd.f = NTSCRT_RGBA32F;
        fhd.frames = {0, 1, 2, 3, 1, 9};
        check_sequence(std::string(id) + " 1920x1080 float, CRT shader on", l, fhd);
        Sequence odd;
        odd.w = 161, odd.h = 97, odd.f = NTSCRT_RGBA16F;
        check_sequence(std::string(id) + " 161x97 half", quick(l), odd);
    }
}

TEST_CASE("digital gpu pipeline: the bundled Digital looks") {
    const auto schema = host::Schema::parse(ntscrt_param_schema_json());
    for (const char *name : {"Webcam (low quality)", "Cheap camcorder", "Chunky webcam", "CD-ROM (low quality)"}) {
        const json p = preset(name);
        const json l = host::build_look(schema, [&](size_t i) -> json {
                           if (auto v = host::preset_value(schema.params[i], p, false)) return *v;
                           return schema.params[i].def;
                       }).look;
        REQUIRE(l["digital"]["enabled"] == true);
        Sequence q;
        q.w = 640, q.h = 360, q.f = NTSCRT_RGBA32F;
        check_sequence(name, l, q);
    }
}

TEST_CASE("digital gpu pipeline: each Digital setting alone") {
    json base = plugin_default();
    base["digital"]["enabled"] = true;
    const std::vector<std::pair<const char *, double>> values = {
        {"frame_rate", 30},       {"frame_rate", 7},          {"fields", 0},           {"aliasing", 0.3},
        {"aliasing", 0.9},        {"sensor_noise", 0.95},     {"low_light_noise", 1},  {"noise_reduction", 1},
        {"ccd_response", 1},      {"white_balance", 0.7},     {"tint", -0.6},          {"saturation", 1.7},
        {"sharpening", 0.8},      {"sharpening_width", 2.5},  {"codec", 1},            {"chroma", 0},
        {"chroma", 1},            {"chroma", 3},              {"quality", 0.2},        {"keyframe_interval", 1},
        {"fixed_bitrate", 0.8},   {"deblocking", 0.8},        {"data_loss", 0.5},      {"datamosh", 1}};
    for (auto [id, v] : values) {
        json l = base;
        l["digital"]["values"][id] = v;
        if (std::string(id) == "datamosh") l["digital"]["values"]["keyframe_interval"] = 1;
        check_sequence(std::string(id) + " = " + std::to_string(v), quick(l));
    }
    // Downscale off: the capture is the picture's own size, capped at 1280 wide.
    json off = base;
    off["downscale"]["enabled"] = false;
    check_sequence("Downscale off (capture at the picture's size)", quick(off));
    Sequence big;
    big.w = 1600, big.h = 900;
    big.frames = {0, 1, 2};
    check_sequence("Downscale off, 1600x900 (capture 1280 wide)", quick(off), big);
}

TEST_CASE("digital gpu pipeline: crop, render scale, NTSC on ntsc-rs, Datamosh seeks") {
    json mini = with_format(plugin_default(), "minidv");
    json crop = mini;
    crop["crop"] = {{"enabled", true}, {"ratio", "4:3"}};
    check_sequence("MiniDV, crop 4:3", quick(crop), Sequence{333, 197});
    Sequence half;
    half.scale = 0.5;
    check_sequence("MiniDV, render scale 0.5", quick(mini), half);
    // NTSC on ntsc-rs needs the picture on the CPU: Digital video stays on the CPU there.
    check_sequence("MiniDV, NTSC on ntsc-rs", quick(mini), Sequence{}, "0");
    // Datamosh: dropped keyframes, the history reaching back across a seek.
    Sequence mosh;
    mosh.frames = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 4, 12, 60, 61, 3};
    check_sequence("Datamosh, long sequence with seeks", quick(with_format(plugin_default(), "datamosh")), mosh);
    json moshi = with_format(plugin_default(), "datamosh");
    moshi["digital"]["values"]["keyframe_interval"] = 0.5;
    check_sequence("Datamosh, keyframe every 0.5 s", quick(moshi), mosh);
}

TEST_CASE("digital gpu pipeline: a GPU error takes the CPU code for the frame, three in a row for the instance") {
    ntscrt_instance *inst = make_instance(), *ref = make_instance();
    if (!ntscrt::testing::stats(inst).has_gpu || gputest::native_backend() != gpu::Backend::D3D11) {
        ntscrt_destroy(inst);
        ntscrt_destroy(ref);
        return;
    }
    json lj = quick(with_format(plugin_default(), "webcam"));
    lj["ntsc"]["enabled"] = false;  // (on WARP ntsc-rs would keep Digital video on the CPU)
    const std::string look = lj.dump();
    Source src{241, 135, NTSCRT_RGBA8, {}, {}};
    const char *env = shared_is_software() ? "force" : "1";
    int64_t f = 0;
    auto step = [&](const char *expect) {
        const Rendered a = render(ref, src, look, f, 1, "0");
        const Rendered b = render(inst, src, look, f, 1, env);
        INFO(b.timings);
        CHECK(b.timings.find(expect) != std::string::npos);
        CHECK(a.px == b.px);
        ++f;
    };
    ntscrt::testing::simulate_digital_gpu_errors(inst, 2);
    step("digital_route=cpu (GPU error: simulated");
    step("digital_route=cpu (GPU error: simulated");
    step("digital_route=gpu");
    ntscrt::testing::simulate_digital_gpu_errors(inst, 3);
    step("digital_route=cpu (GPU error: simulated");
    step("digital_route=cpu (GPU error: simulated");
    step("3 frames in a row");
    step("digital_route=cpu (GPU error 3 frames in a row");
    ntscrt::testing::simulate_digital_gpu_errors(inst, 0);
    // Another value of the switch decides again.
    if (!shared_is_software()) {
        env = "force";
        step("digital_route=gpu");
    }
    ntscrt_destroy(inst);
    ntscrt_destroy(ref);
}

TEST_CASE("digital gpu pipeline: the switch") {
    ntscrt_instance *inst = make_instance();
    const std::string look = quick(with_format(plugin_default(), "webcam")).dump();
    Source src{64, 48, NTSCRT_RGBA8, {}, {}};
    Rendered r = render(inst, src, look, 0, 1, "0");
    CHECK(r.timings.find("digital_route=cpu (NTSCRT_DIGITAL_GPU=0)") != std::string::npos);
    r = render(inst, src, look, 1, 1, nullptr);
    if (ntscrt::testing::stats(inst).has_gpu) {
        if (gputest::native_backend() != gpu::Backend::D3D11)
            CHECK(r.timings.find("digital_route=cpu (not on Metal yet)") != std::string::npos);
        else if (shared_is_software())
            CHECK(r.timings.find("digital_route=cpu (software device (WARP)") != std::string::npos);
        else
            CHECK(r.timings.find("digital_route=gpu (auto: ") != std::string::npos);
    } else {
        CHECK(r.timings.find("digital_route=cpu (no GPU device)") != std::string::npos);
    }
    r = render(inst, src, look, 2, 1, "1");
    if (ntscrt::testing::stats(inst).has_gpu && !shared_is_software() &&
        gputest::native_backend() == gpu::Backend::D3D11)
        CHECK(r.timings.find("digital_route=gpu (NTSCRT_DIGITAL_GPU=1: ") != std::string::npos);
    ntscrt_destroy(inst);
}
