// Clip mode's hole filling on the GPU (hostgpu::Fill, shaders/hostgpu_fill.slang) against
// the CPU code it ports (to_bgra8 + fill_transparent, pixel_convert.cpp): byte for byte,
// on this PC's GPU and on WARP, with the plain (float) and the double-precision kernels.
// Many hole shapes (none, all transparent, single pixels, borders, large holes, noise),
// sizes down to 1 x 1, odd and lopsided, the host formats, straight and premultiplied
// alpha, row orders, sources smaller than the frame / off its edges, crops.
// No CUDA needed: the host source is an ordinary texture of the HAL (as the bridge
// imports it). docs/dev/HOST-GPU.md, "Hole filling on the GPU".
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../gpu/gpu_test_util.h"
#include "gpu/gpu_route.h"
#include "host_gpu.h"
#include "pixel_convert.h"

using namespace ntscrt;

namespace {

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
// D3D11: the kernels must give the CPU's bytes. Metal: the route is off (fast math,
// unverified), so differences are only reported.
bool exact_required() { return gputest::native_backend() == gpu::Backend::D3D11; }

uint32_t lcg(uint32_t &s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}
float frand(uint32_t &s) { return float(lcg(s) & 0xffff) / 65535.f; }

uint16_t f2h(float x) {  // round to nearest (test data)
    uint32_t f;
    std::memcpy(&f, &x, 4);
    const uint32_t s = (f >> 16) & 0x8000u;
    const int e = int((f >> 23) & 0xff) - 127 + 15;
    if ((f & 0x7fffffffu) > 0x7f800000u) return 0x7e00;  // NaN
    if (e <= 0) return uint16_t(s);
    if (e >= 31) return uint16_t(s | 0x7c00u);
    return uint16_t(s | (uint32_t(e) << 10) | (((f & 0x7fffff) + 0x1000) >> 13));
}

enum class Holes { None, All, Single, Border, Large, Noise, Ramp, Stripes, NaN };
const char *hole_name(Holes h) {
    switch (h) {
    case Holes::None: return "none";
    case Holes::All: return "all transparent";
    case Holes::Single: return "single pixels";
    case Holes::Border: return "borders";
    case Holes::Large: return "large hole";
    case Holes::Noise: return "noise";
    case Holes::Ramp: return "soft ramp";
    case Holes::Stripes: return "stripes";
    case Holes::NaN: return "NaN alpha";
    }
    return "?";
}

// Alpha of source pixel (x, y) (top-down, w x h) for a hole shape.
float hole_alpha(Holes h, int x, int y, int w, int hh, uint32_t &s) {
    switch (h) {
    case Holes::None: return 1.f;
    case Holes::All: return (x + y) % 3 == 0 ? 0.f : 0.001f;  // at or below 0.5/255 everywhere
    case Holes::Single: return ((x * 7 + y * 13) % 29 == 0) ? 0.f : 1.f;
    case Holes::Border: return (x < 2 || y < 1 || x >= w - 3 || y >= hh - 2) ? 0.f : 1.f;
    case Holes::Large: {
        const float dx = (x - w * 0.4f) / (w * 0.35f + 1), dy = (y - hh * 0.55f) / (hh * 0.3f + 1);
        return dx * dx + dy * dy < 1.f ? 0.f : 1.f;
    }
    case Holes::Noise: {
        const uint32_t r = lcg(s) % 8;
        return r < 3 ? 0.f : (r == 3 ? 0.5f / 255.f : (r == 4 ? 0.003f : (r == 5 ? 0.37f : 1.f)));
    }
    case Holes::Ramp: {
        const float dx = (x - w * 0.5f) / (w * 0.5f + 1), dy = (y - hh * 0.5f) / (hh * 0.5f + 1);
        return std::fmin(1.f, std::fmax(0.f, 1.6f - 1.4f * std::sqrt(dx * dx + dy * dy)));
    }
    case Holes::Stripes: return (x / 5) % 3 == 0 || (y / 4) % 4 == 1 ? 0.f : 0.8f;
    case Holes::NaN: return (x + 2 * y) % 5 == 0 ? std::numeric_limits<float>::quiet_NaN() : ((x + y) % 7 == 0 ? 0.f : 1.f);
    }
    return 1.f;
}

size_t bpp(ntscrt_pixel_format f) { return f == NTSCRT_RGBA32F ? 16 : f == NTSCRT_RGBA16F ? 8 : 4; }
gpu::Format hal(ntscrt_pixel_format f) {
    return f == NTSCRT_RGBA32F ? gpu::Format::RGBA32F : f == NTSCRT_RGBA16F ? gpu::Format::RGBA16F : gpu::Format::RGBA8;
}
const char *fmt_name(ntscrt_pixel_format f) {
    return f == NTSCRT_RGBA32F ? "float" : f == NTSCRT_RGBA16F ? "half" : "8-bit";
}

struct Case {
    int fw, fh;       // the frame
    int sw, sh;       // the host source
    int sx, sy;       // its place in the frame's memory order
    Holes holes;
    ntscrt_pixel_format f = NTSCRT_RGBA32F;
    bool premult = false, bottom_up = true;
    int crop = 0;  // > 0: cut this many pixels off each side (and one more on the right)
};

std::string describe(const Case &c) {
    char b[200];
    std::snprintf(b, sizeof b, "frame %dx%d, source %dx%d at %d,%d, %s, %s%s%s%s", c.fw, c.fh, c.sw, c.sh, c.sx, c.sy,
                  hole_name(c.holes), fmt_name(c.f), c.premult ? ", premultiplied" : "",
                  c.bottom_up ? "" : ", top row first", c.crop ? ", crop" : "");
    return b;
}

// The source pixel (x, y) of memory row y, in 0..1 floats.
void source_px(const Case &c, int x, int my, uint32_t &s, float v[4]) {
    const int ty = c.bottom_up ? c.sh - 1 - my : my;  // top-down row, for the shapes
    v[0] = float((x * 7 + ty * 3) % 256) / 255.f + 0.0007f * float(lcg(s) % 3);
    v[1] = frand(s);
    v[2] = 0.5f + 0.45f * std::sin(0.05f * float(x + ty));
    v[3] = hole_alpha(c.holes, x, ty, c.sw, c.sh, s);
    if (!(v[3] > 0.f)) {  // transparent pixels with colour in them (the CPU keeps it when nothing fills them)
        v[0] *= 0.6f;
    }
    for (int k = 0; k < 3; ++k) v[k] = std::fmin(1.f, v[k]);
    if (c.premult && v[3] == v[3])
        for (int k = 0; k < 3; ++k) v[k] *= std::fmin(1.f, std::fmax(0.f, v[3]));
}

// The host source in its memory row order, tightly packed.
std::vector<uint8_t> make_source(const Case &c) {
    std::vector<uint8_t> m(size_t(c.sw) * c.sh * bpp(c.f));
    uint32_t s = uint32_t(c.fw * 31 + c.fh * 17 + int(c.holes) * 101 + c.sw);
    for (int y = 0; y < c.sh; ++y)
        for (int x = 0; x < c.sw; ++x) {
            float v[4];
            source_px(c, x, y, s, v);
            uint8_t *p = m.data() + (size_t(y) * c.sw + x) * bpp(c.f);
            for (int k = 0; k < 4; ++k) {
                if (c.f == NTSCRT_RGBA32F) std::memcpy(p + 4 * k, &v[k], 4);
                else if (c.f == NTSCRT_RGBA16F) {
                    const uint16_t h = f2h(v[k]);
                    std::memcpy(p + 2 * k, &h, 2);
                } else {
                    p[k] = v[k] == v[k] ? uint8_t(std::lround(std::fmin(1.f, std::fmax(0.f, v[k])) * 255.f)) : 0;
                }
            }
        }
    return m;
}

// What the CPU path does with it: the frame (top-down, transparent black around the
// source, as the host GPU path's gpu-cpuconvert route builds it), to_bgra8 in Clip mode,
// then the crop.
struct CpuFrame {
    std::vector<uint8_t> frame;  // the host frame, top-down
    ImageBGRA8 pic;              // to_bgra8's picture (filled)
    std::vector<float> alpha;    // to_bgra8's alpha (empty: opaque)
};
CpuFrame cpu_frame(const Case &c, const std::vector<uint8_t> &src) {
    const size_t pb = bpp(c.f);
    std::vector<uint8_t> frame(size_t(c.fw) * c.fh * pb, 0);
    for (int my = 0; my < c.sh; ++my) {
        const int fmy = c.sy + my;
        if (fmy < 0 || fmy >= c.fh) continue;
        const int fy = c.bottom_up ? c.fh - 1 - fmy : fmy;
        for (int x = 0; x < c.sw; ++x) {
            const int fx = c.sx + x;
            if (fx < 0 || fx >= c.fw) continue;
            std::memcpy(frame.data() + (size_t(fy) * c.fw + fx) * pb, src.data() + (size_t(my) * c.sw + x) * pb, pb);
        }
    }
    CpuFrame r;
    ntscrt_image im{c.fw, c.fh, ptrdiff_t(size_t(c.fw) * pb), c.f, frame.data()};
    to_bgra8(im, r.pic, &r.alpha, AlphaMode::Clip, c.premult);
    r.frame = std::move(frame);
    return r;
}
// The case's crop rect of the frame.
void crop_rect(const Case &c, int &cx, int &cy, int &cw, int &ch) {
    cx = std::min(c.crop, c.fw - 1);
    cy = std::min(c.crop, c.fh - 1);
    cw = std::max(1, c.fw - cx - (c.crop ? c.crop + 1 : 0));
    ch = std::max(1, c.fh - cy - c.crop);
}
std::vector<uint8_t> cut(const ImageBGRA8 &im, int cx, int cy, int cw, int ch) {
    std::vector<uint8_t> r(size_t(cw) * ch * 4);
    for (int y = 0; y < ch; ++y)
        std::memcpy(r.data() + size_t(y) * cw * 4, im.px.data() + (size_t(cy + y) * im.width + cx) * 4, size_t(cw) * 4);
    return r;
}

std::vector<Case> cases() {
    std::vector<Case> v;
    const Holes all[] = {Holes::None,  Holes::All,  Holes::Single,  Holes::Border, Holes::Large,
                         Holes::Noise, Holes::Ramp, Holes::Stripes, Holes::NaN};
    // Every hole shape at a few sizes (odd, lopsided, tiny), float, the source the frame.
    const int sizes[][2] = {{1, 1}, {2, 1}, {1, 2}, {2, 2}, {3, 3}, {5, 1}, {1, 7}, {17, 9},
                            {64, 64}, {65, 33}, {333, 197}, {1001, 3}, {3, 1001}};
    for (Holes h : all)
        for (auto &s : sizes) v.push_back({s[0], s[1], s[0], s[1], 0, 0, h});
    // Formats, premultiplication, row order, crops.
    for (Holes h : {Holes::Large, Holes::Noise, Holes::Ramp, Holes::Single})
        for (ntscrt_pixel_format f : {NTSCRT_RGBA32F, NTSCRT_RGBA16F, NTSCRT_RGBA8})
            for (int pm = 0; pm < 2; ++pm) {
                v.push_back({241, 135, 241, 135, 0, 0, h, f, pm != 0, true});
                v.push_back({241, 135, 241, 135, 0, 0, h, f, pm != 0, false, 7});
            }
    // Sources smaller than the frame, offset, partly off its edges (transformed clips).
    v.push_back({333, 197, 315, 179, 9, 9, Holes::None});
    v.push_back({333, 197, 315, 179, 9, 9, Holes::Ramp, NTSCRT_RGBA16F, true});
    v.push_back({333, 197, 200, 120, 100, 3, Holes::None, NTSCRT_RGBA8});
    v.push_back({333, 197, 200, 120, -50, 90, Holes::Single});
    v.push_back({333, 197, 400, 260, -30, -40, Holes::Large, NTSCRT_RGBA32F, true, false});
    v.push_back({333, 197, 1, 1, 160, 90, Holes::None});
    v.push_back({333, 197, 50, 40, 400, 10, Holes::None});  // entirely outside: all transparent
    v.push_back({640, 360, 320, 180, 320, 180, Holes::Noise, NTSCRT_RGBA8, true, true, 11});
    // A large frame: the full pyramid of a 1080p picture (and UHD once, on hardware).
    v.push_back({1920, 1080, 1720, 980, 100, 50, Holes::Large, NTSCRT_RGBA16F});
    v.push_back({3840, 2160, 3840, 2160, 0, 0, Holes::Noise});
    return v;
}

}  // namespace

TEST_CASE("hole filling on the GPU: the probe") {
    for (auto &d : devices()) {
        hostgpu::Fill f(*d.dev);
        const std::string mode = f.arith_mode();
        std::string why;
        const bool on = hostgpu::fill_gpu_route(*d.dev, &why, &f);
        MESSAGE(d.name << ": arith " << mode << ", route " << std::string(on ? "on" : "off") << " (" << why << ")");
        gputest::set_env("NTSCRT_FILL_GPU_ARITH", "double");
        hostgpu::Fill x(*d.dev);
        if (exact_required()) CHECK(std::string(x.arith_mode()) == "double");
        gputest::set_env("NTSCRT_FILL_GPU_ARITH", nullptr);
        if (gputest::native_backend() == gpu::Backend::D3D11) CHECK(mode != "inexact");
        // The rule: on on a hardware D3D11 GPU, off on WARP (unless forced), off with 0.
        if (gputest::native_backend() == gpu::Backend::D3D11) CHECK(on == !gpu_is_software(*d.dev));
        gputest::set_env("NTSCRT_FILL_GPU", "0");
        CHECK(!hostgpu::fill_gpu_route(*d.dev, &why, &f));
        CHECK(why == "NTSCRT_FILL_GPU=0");
        gputest::set_env("NTSCRT_FILL_GPU", "force");
        CHECK(hostgpu::fill_gpu_route(*d.dev, &why, &f));
        gputest::set_env("NTSCRT_FILL_GPU", nullptr);
    }
}

TEST_CASE("hole filling on the GPU: fill_transparent's bytes, every case") {
    const std::vector<Case> all = cases();
    for (auto &d : devices()) {
        const bool warp = gpu_is_software(*d.dev);
        for (const char *mode : {"", "double"}) {
            gputest::set_env("NTSCRT_FILL_GPU_ARITH", *mode ? mode : nullptr);
            hostgpu::Fill fill(*d.dev);
            const std::string arith = fill.arith_mode();
            gputest::set_env("NTSCRT_FILL_GPU_ARITH", nullptr);
            size_t n = 0, bad = 0, fill_bad = 0, bytes = 0, holes = 0;
            for (const Case &c : all) {
                if ((warp || *mode) && c.fw >= 3840) continue;  // WARP and the double kernels: up to 1080p
                const std::string what = describe(c);
                CAPTURE(what);
                const std::vector<uint8_t> src = make_source(c);
                int cx, cy, cw, ch;
                crop_rect(c, cx, cy, cw, ch);
                const CpuFrame cpu = cpu_frame(c, src);
                const std::vector<uint8_t> want = cut(cpu.pic, cx, cy, cw, ch);
                auto tex = d.dev->create_texture({c.sw, c.sh, hal(c.f), false, false});
                d.dev->upload(*tex, src.data(), ptrdiff_t(size_t(c.sw) * bpp(c.f)));
                hostgpu::InParams p{};
                p.frame_w = uint32_t(c.fw);
                p.frame_h = uint32_t(c.fh);
                p.src_w = uint32_t(c.sw);
                p.src_h = uint32_t(c.sh);
                p.src_x = c.sx;
                p.src_y = c.sy;
                p.bottom_up = c.bottom_up ? 1 : 0;
                p.mode = 0;
                p.cx = uint32_t(cx);
                p.cy = uint32_t(cy);
                p.cw = uint32_t(cw);
                p.ch = uint32_t(ch);
                p.premult = c.premult ? 1 : 0;
                auto pic = d.dev->create_texture({cw, ch, gpu::Format::BGRA8, true, false});
                fill.convert(*tex, p, *pic);
                const std::vector<uint8_t> got = gputest::read(*d.dev, *pic);
                auto count = [](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, int &maxd) {
                    size_t k = 0;
                    maxd = 0;
                    for (size_t i = 0; i < a.size(); ++i)
                        if (a[i] != b[i]) {
                            ++k;
                            maxd = std::max(maxd, std::abs(int(a[i]) - int(b[i])));
                        }
                    return k;
                };
                int maxd = 0;
                const size_t diff = count(got, want, maxd);
                // The fill alone: fill_transparent on the CPU from this device's own
                // convert_in picture (the whole frame) must give the kernels' bytes.
                hostgpu::InParams q = p;
                q.cx = q.cy = 0;
                q.cw = uint32_t(c.fw);
                q.ch = uint32_t(c.fh);
                auto conv = d.dev->create_texture({c.fw, c.fh, gpu::Format::BGRA8, true, false});
                gpu::Bindings b;
                b.constants = &q;
                b.constants_size = sizeof q;
                b.textures = {tex.get()};
                b.out_textures = {conv.get()};
                d.dev->dispatch(d.dev->kernel("hostgpu_in/convert_in"), b, c.fw, c.fh, 1);
                ImageBGRA8 own;
                own.width = c.fw;
                own.height = c.fh;
                own.px = gputest::read(*d.dev, *conv);
                fill_transparent(own, cpu.alpha);
                int fill_max = 0;
                const size_t fill_diff = count(got, cut(own, cx, cy, cw, ch), fill_max);
                ++n;
                bytes += got.size();
                bad += diff != 0;
                fill_bad += fill_diff != 0;
                holes += c.holes != Holes::None;
                if (diff || fill_diff)
                    MESSAGE(d.name << " arith " << arith << ": " << what << ": " << diff << " bytes differ from the CPU path (max "
                                   << maxd << "), " << fill_diff << " from fill_transparent on this device's convert_in picture");
                if (exact_required()) CHECK(fill_diff == 0);  // the fill itself: exact on D3D11 (WARP included)
                if (exact_required() && !warp) CHECK(diff == 0);
            }
            MESSAGE(d.name << ", arith " << arith << ": " << n << " cases (" << holes << " with holes), " << bytes
                           << " bytes, " << bad << " cases differing from the CPU path, " << fill_bad
                           << " from the CPU fill of the device's own conversion");
        }
    }
}
