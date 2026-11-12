// ntscrt_render_host_gpu against ntscrt_render_cpu on the same frames, with this
// test as a fake CUDA host (Windows, NVIDIA GPU): frames in CUDA device memory in
// OpenFX layout (bottom row first, padded rows), a non-blocking stream of the
// primary context. Skipped without nvcuda.dll / a CUDA device (CI runners) or when
// the core's D3D11 device is on another GPU.
//
// Tolerance (docs/dev/HOST-GPU.md): every channel within 1/255 (+1e-5) of the CPU
// path's value and at most 0.2% of the values different at all. In practice the
// GPU path is exact except half-float output (GPU rounds to nearest even).
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

#include "fake_cuda_host.h"
#include "ntscrt_host/params.h"

namespace {

uint16_t f2h(float x) {  // round to nearest; fine for test data in 0..1
    uint32_t f;
    std::memcpy(&f, &x, 4);
    const uint32_t s = (f >> 16) & 0x8000u;
    int e = int((f >> 23) & 0xff) - 127 + 15;
    if (e <= 0) return uint16_t(s);
    return uint16_t(s | (uint32_t(e) << 10) | (((f & 0x7fffff) + 0x1000) >> 13));
}
float h2f(uint16_t h) {
    const int e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m + 1024), e - 25);
    return (h & 0x8000) ? -v : v;
}

size_t bpp(ntscrt_pixel_format f) { return f == NTSCRT_RGBA32F ? 16 : f == NTSCRT_RGBA16F ? 8 : 4; }

// One RGBA host image in OpenFX layout (row 0 in memory = bottom row).
struct HostImage {
    int w = 0, h = 0;
    ntscrt_pixel_format f = NTSCRT_RGBA32F;
    size_t pitch = 0;
    std::vector<uint8_t> mem;
    void init(int W, int H, ntscrt_pixel_format F) {
        w = W, h = H, f = F;
        pitch = size_t(w) * bpp(f) + 48;  // padded rows, as hosts may have
        mem.assign(pitch * h, 0);
    }
    void set(int x, int ytop, const float v[4]) {
        uint8_t *p = mem.data() + pitch * size_t(h - 1 - ytop) + size_t(x) * bpp(f);
        for (int c = 0; c < 4; ++c) {
            if (f == NTSCRT_RGBA32F) std::memcpy(p + c * 4, &v[c], 4);
            else if (f == NTSCRT_RGBA16F) { uint16_t q = f2h(v[c]); std::memcpy(p + c * 2, &q, 2); }
            else p[c] = uint8_t(std::lround(std::fmin(1.f, std::fmax(0.f, v[c])) * 255.f));
        }
    }
    float get(int x, int ytop, int c) const {
        const uint8_t *p = mem.data() + pitch * size_t(h - 1 - ytop) + size_t(x) * bpp(f);
        if (f == NTSCRT_RGBA32F) { float v; std::memcpy(&v, p + c * 4, 4); return v; }
        if (f == NTSCRT_RGBA16F) { uint16_t q; std::memcpy(&q, p + c * 2, 2); return h2f(q); }
        return p[c] / 255.f;
    }
    ntscrt_image cpu() {  // top row first, negative stride (what the OpenFX plug-in passes)
        return ntscrt_image{w, h, -ptrdiff_t(pitch), f, mem.data() + pitch * size_t(h - 1)};
    }
};

enum class Alpha { Opaque, Straight, Premult };

void fill(HostImage &im, int frame, Alpha alpha) {
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            float v[4];
            v[0] = float((x * 7 + frame * 13) % im.w) / float(im.w);
            v[1] = float((y * 5 + x) % im.h) / float(im.h);
            v[2] = 0.5f + 0.45f * std::sin(0.05f * float(x + y) + 0.3f * float(frame));
            v[3] = 1.f;
            if (alpha != Alpha::Opaque) {
                // A soft blob: opaque middle, a ramp, fully transparent corners.
                const float dx = (x - im.w * 0.5f) / (im.w * 0.5f), dy = (y - im.h * 0.5f) / (im.h * 0.5f);
                v[3] = std::fmin(1.f, std::fmax(0.f, 1.6f - 1.4f * std::sqrt(dx * dx + dy * dy)));
                if (alpha == Alpha::Premult)
                    for (int c = 0; c < 3; ++c) v[c] *= v[3];
            }
            im.set(x, y, v);
        }
}


struct Diff {
    double max = 0;
    size_t off = 0, n = 0;
};
Diff compare(const HostImage &a, const HostImage &b) {
    Diff d;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x)
            for (int c = 0; c < 4; ++c) {
                const double e = std::fabs(double(a.get(x, y, c)) - double(b.get(x, y, c)));
                d.max = std::fmax(d.max, e);
                d.off += e > 1e-5;
                ++d.n;
            }
    return d;
}

struct Case {
    const char *name;
    std::string look;
    ntscrt_pixel_format f;
    Alpha alpha;
    double scale = 1.0;
    int pad = 0;  // source smaller than the frame by this much on each side
};

}  // namespace

TEST_CASE("host GPU frames (fake CUDA host) match the CPU path") {
#ifndef _WIN32
    MESSAGE("CUDA host frames are Windows-only: skipped");
#else
    fakecuda::FakeCudaHost cuda;
    if (!cuda.start()) {
        MESSAGE("no CUDA (" << cuda.why << "): skipped");
        return;
    }
    const std::string res = NTSCRT_BUILT_RESOURCES_DIR;
    // Looks as the plug-in builds them: every param at its default (ntsc-rs, Glitch, the default CRT
    // shader on), with the changes each case names; and the bundled Clean CRT look.
    const auto schema = ntscrt::host::Schema::parse(ntscrt_param_schema_json());
    const nlohmann::json plugin_default =
        ntscrt::host::build_look(schema, [&](size_t i) { return schema.params[i].def; }).look;
    auto look = [&](const char *patch) {
        nlohmann::json l = plugin_default;
        l.merge_patch(nlohmann::json::parse(patch));
        return l.dump();
    };
    std::string clean_crt;
    {
        std::ifstream f(std::string(NTSCRT_SOURCE_DIR) + "/presets/Clean CRT.json");
        std::stringstream ss;
        ss << f.rdbuf();
        clean_crt = ss.str();
        REQUIRE(!clean_crt.empty());
    }
    const char *off = R"({"ntsc":{"enabled":false}})";
    const Case cases[] = {
        {"default look (NTSC on: download route), float", look("{}"), NTSCRT_RGBA32F, Alpha::Opaque},
        {"Clean CRT (direct route), float", clean_crt, NTSCRT_RGBA32F, Alpha::Opaque},
        {"NTSC off (direct route), float", look(off), NTSCRT_RGBA32F, Alpha::Opaque},
        {"NTSC off, half float", look(off), NTSCRT_RGBA16F, Alpha::Opaque},
        {"NTSC off, 8-bit", look(off), NTSCRT_RGBA8, Alpha::Opaque},
        {"default look, 8-bit", look("{}"), NTSCRT_RGBA8, Alpha::Opaque},
        {"NTSC off, premultiplied, Clip (holes: CPU conversion)", look(off), NTSCRT_RGBA32F, Alpha::Premult},
        {"NTSC off, straight, Spill", look(R"({"ntsc":{"enabled":false},"alpha":{"mode":"spill"}})"), NTSCRT_RGBA32F,
         Alpha::Straight},
        {"NTSC off, premultiplied, Spill", look(R"({"ntsc":{"enabled":false},"alpha":{"mode":"spill"}})"),
         NTSCRT_RGBA32F, Alpha::Premult},
        {"default look, premultiplied, Spill, half", look(R"({"alpha":{"mode":"spill"}})"), NTSCRT_RGBA16F,
         Alpha::Premult},
        {"NTSC off, render scale 0.5", look(off), NTSCRT_RGBA32F, Alpha::Opaque, 0.5},
        {"NTSC off, crop 4:3", look(R"({"ntsc":{"enabled":false},"crop":{"enabled":true,"ratio":"4:3"}})"),
         NTSCRT_RGBA32F, Alpha::Opaque},
        {"NTSC off, source smaller than the frame, Spill",
         look(R"({"ntsc":{"enabled":false},"alpha":{"mode":"spill"}})"), NTSCRT_RGBA32F, Alpha::Opaque, 1.0, 9},
        {"NTSC off, source smaller than the frame, Clip", look(off), NTSCRT_RGBA32F, Alpha::Opaque, 1.0, 9},
        {"Digital video on (download route)", look(R"({"ntsc":{"enabled":false},"digital":{"enabled":true}})"),
         NTSCRT_RGBA32F, Alpha::Opaque},
        {"core defaults, NTSC off (no Glitch)", off, NTSCRT_RGBA32F, Alpha::Opaque},
    };
    for (const Case &k : cases) {
        const std::string case_name = k.name;
        CAPTURE(case_name);
        const int W = 333, H = 197;
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, res.c_str()};
        ntscrt_instance *cpu = nullptr, *gpu = nullptr;
        REQUIRE(ntscrt_create(&ci, &cpu) == NTSCRT_OK);
        REQUIRE(ntscrt_create(&ci, &gpu) == NTSCRT_OK);
        const int premult = k.alpha == Alpha::Premult ? 1 : 0;
        bool skipped = false;
        for (int frame = 0; frame < 3 && !skipped; ++frame) {
            HostImage src, frame_src, out_cpu, out_gpu;
            src.init(W - 2 * k.pad, H - 2 * k.pad, k.f);
            fill(src, frame, k.alpha);
            // The CPU path gets the frame padded with transparent black, as the plug-in does.
            frame_src.init(W, H, k.f);
            for (int y = 0; y < src.h; ++y)
                std::memcpy(frame_src.mem.data() + frame_src.pitch * size_t(y + k.pad) + size_t(k.pad) * bpp(k.f),
                            src.mem.data() + src.pitch * size_t(y), size_t(src.w) * bpp(k.f));
            out_cpu.init(W, H, k.f);
            out_gpu.init(W, H, k.f);

            ntscrt_render_args ra{};
            ra.frame_index = frame;
            ra.fps = 30;
            ra.look_json = k.look.c_str();
            ra.render_scale = k.scale;
            ra.source_premultiplied = premult;
            ntscrt_image s = frame_src.cpu(), d = out_cpu.cpu();
            const ntscrt_status cs = ntscrt_render_cpu(cpu, &ra, &s, &d);
            REQUIRE_MESSAGE(cs == NTSCRT_OK, ntscrt_last_error(cpu));

            auto dsrc = cuda.upload(src.mem.data(), src.mem.size());
            REQUIRE(dsrc != 0);
            auto ddst = cuda.upload(out_gpu.mem.data(), out_gpu.mem.size());
            REQUIRE(ddst != 0);
            ntscrt_host_gpu host{NTSCRT_HOST_GPU_CUDA, cuda.stream};
            ntscrt_gpu_image gs{src.w, src.h, ptrdiff_t(src.pitch), k.f, 1, reinterpret_cast<void *>(dsrc), 0};
            ntscrt_gpu_image gd{W, H, ptrdiff_t(out_gpu.pitch), k.f, 1, reinterpret_cast<void *>(ddst), 0};
            const ntscrt_status gs_st = ntscrt_render_host_gpu(gpu, &ra, &host, &gs, k.pad, k.pad, &gd);
            if (gs_st == NTSCRT_ERR_HOST_GPU && frame == 0 &&
                std::string(ntscrt_last_error(gpu)).find("another GPU") != std::string::npos) {
                MESSAGE("core device on another GPU than CUDA device 0: skipped (" << ntscrt_last_error(gpu) << ")");
                skipped = true;
            } else {
                REQUIRE_MESSAGE(gs_st == NTSCRT_OK, ntscrt_last_error(gpu));
                REQUIRE(cuda.download(ddst, out_gpu.mem.data(), out_gpu.mem.size()));
                const Diff df = compare(out_cpu, out_gpu);
                MESSAGE(std::string(k.name) << " frame " << frame << ": " << std::string(ntscrt_last_timings(gpu)) << "; max diff "
                               << df.max * 255 << "/255, " << df.off << " of " << df.n << " values differ");
                CHECK(df.max <= 1.0 / 255 + 1e-5);
                CHECK(double(df.off) <= 0.002 * double(df.n));
            }
            cuda.MemFree(dsrc);
            cuda.MemFree(ddst);
        }
        ntscrt_destroy(cpu);
        ntscrt_destroy(gpu);
    }
#endif
}

TEST_CASE("host GPU entry points refuse what they can't do, without writing") {
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_BUILT_RESOURCES_DIR};
    ntscrt_instance *inst = nullptr;
    REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
    CHECK(ntscrt_host_gpu_supported(NTSCRT_HOST_GPU_OPENCL) == 0);
    CHECK(ntscrt_host_gpu_supported(NTSCRT_HOST_GPU_NONE) == 0);
    int dummy = 0;
    ntscrt_render_args ra{};
    ra.look_json = "{}";
    ntscrt_host_gpu host{NTSCRT_HOST_GPU_OPENCL, nullptr};
    ntscrt_gpu_image im{8, 8, 8 * 16, NTSCRT_RGBA32F, 1, &dummy, 0};
    CHECK(ntscrt_render_host_gpu(inst, &ra, &host, &im, 0, 0, &im) == NTSCRT_ERR_HOST_GPU);
    CHECK(std::string(ntscrt_last_error(inst)).find("OpenCL") != std::string::npos);
    ntscrt_gpu_image bad = im;
    bad.format = NTSCRT_RGBA16;  // no GPU format for 16-bit unorm
    host.api = NTSCRT_HOST_GPU_CUDA;
    CHECK(ntscrt_render_host_gpu(inst, &ra, &host, &bad, 0, 0, &bad) == NTSCRT_ERR_INVALID_ARG);
    ntscrt_destroy(inst);
}

TEST_CASE("host GPU conversion rounds like the CPU path at every 8-bit boundary") {
#ifdef _WIN32
    fakecuda::FakeCudaHost cuda;
    if (!cuda.start()) {
        MESSAGE("no CUDA (" << cuda.why << "): skipped");
        return;
    }
    // Every stage off: the output is the 8-bit picture itself (opaque), so any rounding
    // difference of the conversion shows. Values a few float steps either side of each
    // (k + 0.5) / 255, straight and premultiplied (Clip divides by alpha).
    const char *look = R"({"downscale":{"enabled":false},"ntsc":{"enabled":false},"glitch":{"enabled":false},)"
                       R"("digital":{"enabled":false},"shader":{"enabled":false}})";
    const int W = 255 * 17, H = 3;
    for (int premult = 0; premult < 2; ++premult) {
        HostImage src, out_cpu, out_gpu;
        src.init(W, H, NTSCRT_RGBA32F);
        for (int k = 0; k < 255; ++k)
            for (int n = -8; n <= 8; ++n) {
                float v = (float(k) + 0.5f) / 255.f;
                for (int s = 0; s < std::abs(n); ++s) v = std::nextafter(v, n < 0 ? 0.f : 1.f);
                for (int y = 0; y < H; ++y) {
                    const float a = y == 0 ? 1.f : (y == 1 ? 0.5f : 0.37f);
                    float px[4] = {v, v * 0.999f, std::fmin(1.f, v * 1.001f), premult ? a : 1.f};
                    if (premult)
                        for (int c = 0; c < 3; ++c) px[c] *= a;
                    src.set(k * 17 + n + 8, y, px);
                }
            }
        out_cpu.init(W, H, NTSCRT_RGBA32F);
        out_gpu.init(W, H, NTSCRT_RGBA32F);
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_BUILT_RESOURCES_DIR};
        ntscrt_instance *a = nullptr, *b = nullptr;
        REQUIRE(ntscrt_create(&ci, &a) == NTSCRT_OK);
        REQUIRE(ntscrt_create(&ci, &b) == NTSCRT_OK);
        ntscrt_render_args ra{};
        ra.fps = 30;
        ra.look_json = look;
        ra.render_scale = 1;
        ra.source_premultiplied = premult;
        ntscrt_image s = src.cpu(), d = out_cpu.cpu();
        REQUIRE(ntscrt_render_cpu(a, &ra, &s, &d) == NTSCRT_OK);
        auto ds = cuda.upload(src.mem.data(), src.mem.size()), dd = cuda.upload(out_gpu.mem.data(), out_gpu.mem.size());
        ntscrt_host_gpu host{NTSCRT_HOST_GPU_CUDA, cuda.stream};
        ntscrt_gpu_image gs{W, H, ptrdiff_t(src.pitch), NTSCRT_RGBA32F, 1, reinterpret_cast<void *>(ds), 0};
        ntscrt_gpu_image gd{W, H, ptrdiff_t(out_gpu.pitch), NTSCRT_RGBA32F, 1, reinterpret_cast<void *>(dd), 0};
        const ntscrt_status st = ntscrt_render_host_gpu(b, &ra, &host, &gs, 0, 0, &gd);
        if (st == NTSCRT_ERR_HOST_GPU) {
            MESSAGE("GPU path unavailable: " << std::string(ntscrt_last_error(b)));
        } else {
            REQUIRE(st == NTSCRT_OK);
            REQUIRE(cuda.download(dd, out_gpu.mem.data(), out_gpu.mem.size()));
            const Diff df = compare(out_cpu, out_gpu);
            MESSAGE("premult " << premult << ": " << std::string(ntscrt_last_timings(b)) << "; max diff "
                               << df.max * 255 << "/255, " << df.off << " of " << df.n << " values differ");
            CHECK(df.off == 0);  // exact, divisions included
        }
        cuda.MemFree(ds);
        cuda.MemFree(dd);
        ntscrt_destroy(a);
        ntscrt_destroy(b);
    }
#endif
}
