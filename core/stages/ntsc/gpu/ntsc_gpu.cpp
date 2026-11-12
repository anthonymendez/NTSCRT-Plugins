// ntsc_gpu.cpp — driver of the GPU port of ntsc-rs (shaders/ntsc_gpu.slang).
// docs/dev/NTSC-GPU.md has the design; the plan comes from the shim
// (rust/ntscrs-capi/src/gpu_plan.rs) and this file only turns it into dispatches.
//
// Per frame: four R32F planes (Y, I, Q, scratch) of width x plane_rows from a
// small pool, the row tables uploaded as structured buffers, then
//   from_bgra8 -> one dispatch (or two) per pass -> to_bgra8.
#include "ntsc_gpu.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace ntscrt {

const char *ntsc_stage_name(int stage) {
    static const char *names[NTSC_ST_COUNT] = {
        "luma_filter",    "chroma_lowpass_in",  "chroma_into_luma",  "composite_sharpening", "composite_noise",
        "snow",           "head_switching",     "tracking_noise",    "luma_into_chroma",     "luma_smear",
        "ringing",        "luma_noise",         "chroma_noise",      "chroma_phase_error",   "chroma_phase_noise",
        "chroma_delay",   "vhs_edge_wave",      "vhs_tape_speed",    "vhs_chroma_loss",      "vhs_sharpen",
        "chroma_vert_blend", "chroma_lowpass_out"};
    return stage >= 0 && stage < NTSC_ST_COUNT ? names[stage] : "?";
}

std::string NtscGpuPlan::unsupported_names() const {
    std::string s;
    for (int i = 0; i < NTSC_ST_COUNT; ++i)
        if (info.unsupported & (1u << i)) {
            if (!s.empty()) s += ',';
            s += ntsc_stage_name(i);
        }
    return s;
}

bool ntsc_gpu_requested() {
    const char *e = std::getenv("NTSCRT_NTSC_GPU");
    return e && *e && std::strcmp(e, "0") != 0;
}

bool gpu_is_software(gpu::Device &dev) {
#ifdef _WIN32
    if (dev.backend() != gpu::Backend::D3D11) return false;
    auto *d = static_cast<ID3D11Device *>(dev.native_device());
    if (!d) return false;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(d->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&ad)) || FAILED(ad->GetDesc(&desc)))
        return false;
    // Microsoft Basic Render Driver (WARP): vendor 0x1414, device 0x8c.
    return desc.VendorId == 0x1414 && desc.DeviceId == 0x8c;
#else
    (void)dev;
    return false;
#endif
}

bool ntsc_gpu_route(gpu::Device &dev, std::string *why) {
    auto no = [&](const char *w) {
        if (why) *why = w;
        return false;
    };
    const char *e = std::getenv("NTSCRT_NTSC_GPU");
    if (!e || !*e || std::strcmp(e, "0") == 0) return no("off (NTSCRT_NTSC_GPU unset)");
    // Metal: the kernels compile for it (CI builds the library) but the route is
    // unverified there, and Metal compiles with fast math (no `precise`), which
    // the ntsc-rs parity of the IIR filters relies on.
    if (dev.backend() != gpu::Backend::D3D11) return no("not on Metal yet");
    // On WARP (a CPU rasterizer) the shaders are slower than ntsc-rs itself.
    const bool software_ok = std::strcmp(e, "force") == 0;
    if (!software_ok && gpu_is_software(dev)) return no("software device (WARP): ntsc-rs is faster");
    return true;
}

namespace {

constexpr uint32_t kPlaneY = 1, kPlaneI = 2, kPlaneQ = 4;

void set_filter(NtscGpuU &u, int p, const NtscGpuFilter &f) {
    std::memcpy(u.num[p], f.num, sizeof f.num);
    std::memcpy(u.den[p], f.den, sizeof f.den);
    std::memcpy(u.zi[p], f.zi, sizeof f.zi);
    u.fp[p][0] = f.len;
    u.fp[p][1] = f.delay;
    u.fp[p][2] = f.initial;
    u.fp[p][3] = 0;
    u.fiv[p] = f.initial_value;
}

}  // namespace

struct NtscGpu::Impl {
    gpu::Device &dev;
    std::vector<std::shared_ptr<gpu::Texture>> pool;
    std::shared_ptr<gpu::Buffer> rows, out;
    size_t rows_cap = 0, out_cap = 0;
    explicit Impl(gpu::Device &d) : dev(d) {}

    // ntsc-rs's mul_adds: mad() where the device fuses it, else the double-precision kernels.
    int fma = -1;  // -1 not probed, 0 mad, 1 double, 2 unfused mad (no doubles)
    void probe() {
        if (fma >= 0) return;
        auto t = dev.create_texture({2, 1, gpu::Format::R32F, true, false});
        NtscGpuU u{};
        u.rotSin = 1.0f + 0x1p-13f;
        u.rotCos = -(1.0f + 0x1p-12f);
        gpu::Bindings b;
        b.constants = &u;
        b.constants_size = sizeof u;
        b.out_textures = {t.get()};
        dev.dispatch(k("ntsc_gpu/probe_fma"), b, 1, 1);
        float r[2] = {};
        dev.download(*t, r, sizeof r);
        if (r[0] == 0x1p-26f) {
            fma = 0;
            return;
        }
        try {
            dev.kernel("ntsc_gpu/row_filter_x");
            dev.kernel("ntsc_gpu/from_bgra8_x");
            dev.kernel("ntsc_gpu/to_bgra8_x");
            fma = 1;
        } catch (const std::exception &) {
            fma = 2;  // no double support: mad as it is
        }
    }
    // The kernel `name`, or its exact-FMA variant on devices that need it.
    gpu::Kernel *kf(const char *name) {
        probe();
        return k(fma == 1 ? (std::string(name) + "_x").c_str() : name);
    }

    std::shared_ptr<gpu::Texture> acquire(const gpu::TextureDesc &d) {
        for (auto &t : pool) {
            const gpu::TextureDesc &e = t->desc();
            if (t.use_count() == 1 && e.width == d.width && e.height == d.height && e.format == d.format &&
                e.shader_write == d.shader_write && e.render_target == d.render_target)
                return t;
        }
        auto t = dev.create_texture(d);
        if (pool.size() >= 10) pool.erase(pool.begin());
        pool.push_back(t);
        return t;
    }
    void ensure(std::shared_ptr<gpu::Buffer> &buf, size_t &cap, size_t bytes, const void *data) {
        if (!buf || cap < bytes) {
            buf = dev.create_buffer(bytes, data);
            cap = bytes;
            return;
        }
        dev.upload(*buf, data, bytes, 0);
    }
    gpu::Kernel *k(const char *name) {
        gpu::Kernel *kk = dev.kernel(name);
        if (!kk) throw std::runtime_error(std::string("ntsc gpu: kernel ") + name + " missing");
        return kk;
    }

    struct Planes {
        std::shared_ptr<gpu::Texture> y, i, q, s;
    };

    // Conversion in and the first `n` passes; the planes hold the result.
    Planes effect(const gpu::Texture &in, const NtscGpuPlan &plan, int n) {
        const int W = plan.width, R = int(plan.info.plane_rows);
        if (W <= 0 || R <= 0 || in.desc().width != W || in.desc().height != plan.height)
            throw std::runtime_error("ntsc gpu: plan / picture size mismatch");
        Planes p;
        const gpu::TextureDesc pd{W, R, gpu::Format::R32F, true, false};
        p.y = acquire(pd);
        p.i = acquire(pd);
        p.q = acquire(pd);
        p.s = acquire(pd);
        ensure(rows, rows_cap, size_t(R) * sizeof(NtscGpuRow), plan.rows.data());

        NtscGpuU base{};
        base.width = uint32_t(W);
        base.planeRows = uint32_t(R);
        base.outHeight = uint32_t(plan.height);
        base.c16 = 16.0f / 255.0f;  // ntsc-rs's f32 constants
        base.inv255 = 1.0f / 255.0f;
        const std::vector<gpu::Texture *> planes = {p.y.get(), p.i.get(), p.q.get(), p.s.get()};

        auto run = [&](const char *name, const NtscGpuU &u, int wx, int wy, bool use_rows,
                       const gpu::Texture *src = nullptr, bool fused = false) {
            gpu::Bindings b;
            b.constants = &u;
            b.constants_size = sizeof u;
            if (src) b.textures = {src};
            if (use_rows) b.buffers = {rows.get()};
            b.out_textures = planes;
            dev.dispatch(fused ? kf(name) : k(name), b, wx, wy);
        };

        run("ntsc_gpu/from_bgra8", base, W, R, true, &in, true);
        const int count = n < 0 ? int(plan.passes.size()) : std::min<int>(n, int(plan.passes.size()));
        for (int idx = 0; idx < count; ++idx) {
            const NtscGpuPass &ps = plan.passes[size_t(idx)];
            NtscGpuU u = base;
            switch (ps.kind) {
            case NTSC_GPU_K_FILTER:
                u.planes = ps.planes;
                for (int pl = 0; pl < 3; ++pl)
                    if (ps.planes & (1u << pl)) set_filter(u, pl, ps.filter[pl]);
                run("ntsc_gpu/row_filter", u, R, 3, false, nullptr, true);
                break;
            case NTSC_GPU_K_LUMA_BOX:
                if (W >= 2) run("ntsc_gpu/luma_box", u, R, 1, false);
                break;
            case NTSC_GPU_K_CHROMA_INTO_LUMA:
                run("ntsc_gpu/chroma_into_luma", u, W, R, true);
                break;
            case NTSC_GPU_K_DEMOD:
                dev.copy(*p.y, *p.s);  // the modulated signal (ntsc-rs's scratch)
                if (ps.mode == 1) {
                    u.planes = kPlaneY;
                    set_filter(u, 0, ps.filter[0]);
                    run("ntsc_gpu/row_filter", u, R, 1, false, nullptr, true);
                } else {
                    u.mode = uint32_t(ps.mode);
                    run("ntsc_gpu/demod_luma", u, W, R, true);
                }
                run("ntsc_gpu/demodulate", base, W, R, true);
                break;
            case NTSC_GPU_K_ROTATE_CONST:
                u.mode = 0;
                u.rotSin = ps.a;
                u.rotCos = ps.b;
                run("ntsc_gpu/rotate_chroma", u, W, R, true);
                break;
            case NTSC_GPU_K_ROTATE_ROWS:
                u.mode = 1;
                run("ntsc_gpu/rotate_chroma", u, W, R, true);
                break;
            case NTSC_GPU_K_CHROMA_LOSS:
                run("ntsc_gpu/chroma_loss", u, W, R, true);
                break;
            case NTSC_GPU_K_VERT_BLEND:
                if (plan.height >= 2) run("ntsc_gpu/vert_blend", u, W, 2, true);  // yiq.dimensions.1 < 2: skipped
                break;
            default:
                throw std::runtime_error("ntsc gpu: unknown pass kind " + std::to_string(ps.kind));
            }
        }
        (void)kPlaneI;
        (void)kPlaneQ;
        return p;
    }
};

NtscGpu::NtscGpu(gpu::Device &dev) : impl_(std::make_unique<Impl>(dev)) {}
NtscGpu::~NtscGpu() = default;

std::shared_ptr<gpu::Texture> NtscGpu::run(const gpu::Texture &in, const NtscGpuPlan &plan) {
    Impl &m = *impl_;
    Impl::Planes p = m.effect(in, plan, -1);
    auto out = m.acquire({plan.width, plan.height, gpu::Format::BGRA8, true, false});
    m.ensure(m.out, m.out_cap, plan.out.size() * sizeof(NtscGpuOutRow), plan.out.data());
    NtscGpuU u{};
    u.width = uint32_t(plan.width);
    u.planeRows = plan.info.plane_rows;
    u.outHeight = uint32_t(plan.height);
    gpu::Bindings b;
    b.constants = &u;
    b.constants_size = sizeof u;
    b.buffers = {m.rows.get(), m.out.get()};
    b.out_textures = {p.y.get(), p.i.get(), p.q.get(), p.s.get(), out.get()};
    m.dev.dispatch(m.kf("ntsc_gpu/to_bgra8"), b, plan.width, plan.height);
    return out;
}

const char *NtscGpu::fma_mode() {
    impl_->probe();
    static const char *names[] = {"mad", "double", "mad-unfused"};
    return names[impl_->fma];
}

std::vector<float> NtscGpu::planes(const gpu::Texture &in, const NtscGpuPlan &plan, int num_passes) {
    Impl &m = *impl_;
    Impl::Planes p = m.effect(in, plan, num_passes);
    const size_t n = size_t(plan.width) * plan.info.plane_rows;
    std::vector<float> out(n * 3);
    m.dev.download(*p.y, out.data(), ptrdiff_t(plan.width) * 4);
    m.dev.download(*p.i, out.data() + n, ptrdiff_t(plan.width) * 4);
    m.dev.download(*p.q, out.data() + 2 * n, ptrdiff_t(plan.width) * 4);
    return out;
}

}  // namespace ntscrt
