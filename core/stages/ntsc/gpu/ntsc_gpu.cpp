// ntsc_gpu.cpp — driver of the GPU port of ntsc-rs (shaders/ntsc_gpu.slang).
// docs/dev/NTSC-GPU.md has the design; the plan comes from the shim
// (rust/ntscrs-capi/src/gpu_plan.rs) and this file only turns it into dispatches.
//
// Per frame: four R32F planes (Y, I, Q, scratch) of width x plane_rows from a
// small pool, the row tables uploaded as structured buffers, then
//   from_bgra8 -> one dispatch (or two) per pass -> to_bgra8.
#include "ntsc_gpu.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
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

// NoiseRow in the shader: the Fbm seed and start offset of the row
// (NtscGpuRowSeed), its intensity (* 0.25), whether the pass touches it.
struct NoiseRowGpu {
    int32_t seed;
    float offset;
    float intensity;
    uint32_t active;
};
static_assert(sizeof(NoiseRowGpu) == 16, "shader NoiseRow");

// Runs [first, end) of the rows for which `on(row)` holds, gaps under 32 rows
// merged: the row-table passes are dispatched over these only (tracking noise
// and head switching touch the bottom rows of each field).
template <class F>
std::vector<std::pair<int, int>> row_runs(int rows, F on) {
    std::vector<std::pair<int, int>> runs;
    for (int r = 0; r < rows; ++r) {
        if (!on(r)) continue;
        if (!runs.empty() && r - runs.back().second < 32) runs.back().second = r + 1;
        else runs.emplace_back(r, r + 1);
    }
    return runs;
}

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
    // The phase 2-3 tables, one buffer per use (a buffer keeps the stride it was first bound with).
    struct Slot {
        std::shared_ptr<gpu::Buffer> buf;
        size_t cap = 0;
    };
    std::map<std::string, Slot> slots;
    gpu::Buffer *table(const std::string &key, const void *data, size_t bytes) {
        Slot &s = slots[key];
        ensure_room(s.buf, s.cap, std::max<size_t>(bytes, 16), data, bytes);
        return s.buf.get();
    }
    std::vector<NoiseRowGpu> noise_rows;
    explicit Impl(gpu::Device &d) : dev(d) {}

    // NTSCRT_PROFILE=1: time per pass (finish() after each).
    std::vector<std::pair<std::string, double>> times;
    const bool profile = [] {
        const char *e = std::getenv("NTSCRT_PROFILE");
        return e && *e && *e != '0';
    }();
    std::chrono::steady_clock::time_point mark;
    void tick(const std::string &what) {
        if (!profile) return;
        dev.finish();
        const auto now = std::chrono::steady_clock::now();
        times.emplace_back(what, std::chrono::duration<double, std::milli>(now - mark).count());
        mark = now;
    }

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
            for (const char *x : {"row_filter_x", "from_bgra8_x", "to_bgra8_x", "noise_add_x", "shift_rows_x",
                                  "chroma_delay_x"})
                dev.kernel(std::string("ntsc_gpu/") + x);
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
    // As ensure(), for `bytes` of data in a buffer of at least `room` bytes.
    void ensure_room(std::shared_ptr<gpu::Buffer> &buf, size_t &cap, size_t room, const void *data, size_t bytes) {
        if (!buf || cap < room) {
            std::vector<uint8_t> init(room, 0);
            if (bytes) std::memcpy(init.data(), data, bytes);
            buf = dev.create_buffer(room, init.data());
            cap = room;
            return;
        }
        if (bytes) dev.upload(*buf, data, bytes, 0);
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
        for (const NtscGpuPass &ps : plan.passes)
            if ((ps.kind == NTSC_GPU_K_NOISE || ps.kind == NTSC_GPU_K_SPECKLES || ps.kind == NTSC_GPU_K_SHIFT_ROWS) &&
                !plan.tables_ready)
                throw std::runtime_error("ntsc gpu: the plan has no tables (NtscStage::gpu_tables)");
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
        base.ix[1] = int32_t(plan.simd_lanes ? plan.simd_lanes : 8);
        const std::vector<gpu::Texture *> planes = {p.y.get(), p.i.get(), p.q.get(), p.s.get()};

        auto run = [&](const char *name, const NtscGpuU &u, int wx, int wy, bool use_rows,
                       const gpu::Texture *src = nullptr, bool fused = false,
                       std::vector<const gpu::Buffer *> tables = {}) {
            gpu::Bindings b;
            b.constants = &u;
            b.constants_size = sizeof u;
            if (src) b.textures = {src};
            if (use_rows || !tables.empty()) b.buffers = {rows.get()};
            if (!tables.empty()) {
                b.buffers.push_back(nullptr);  // t9: outRows (to_bgra8 only)
                for (const gpu::Buffer *t : tables) b.buffers.push_back(t);  // t10..
            }
            b.out_textures = planes;
            dev.dispatch(fused ? kf(name) : k(name), b, wx, wy);
        };
        gpu::Texture *const plane_tex[3] = {p.y.get(), p.i.get(), p.q.get()};

        times.clear();
        if (profile) {
            dev.finish();
            mark = std::chrono::steady_clock::now();
        }
        run("ntsc_gpu/from_bgra8", base, W, R, true, &in, true);
        tick("from_bgra8");
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
                if (ps.mode == 1 || ps.pad[0] == 1) {
                    // the notch; with pad[0] = 1 (a comb, some 1-row fields) only on those fields' rows
                    NtscGpuU un = u;
                    un.planes = kPlaneY;
                    un.ix[2] = ps.pad[0];
                    set_filter(un, 0, ps.filter[0]);
                    run("ntsc_gpu/row_filter", un, R, 1, true, nullptr, true);
                }
                if (ps.mode != 1) {
                    u.mode = uint32_t(ps.mode);
                    u.ix[2] = ps.pad[0];
                    run("ntsc_gpu/demod_luma", u, W, R, true);
                }
                run("ntsc_gpu/demodulate", base, W, R, true);
                break;
            case NTSC_GPU_K_NOISE: {
                // video_noise_line per row: the row seeds of the pass, the intensity of
                // the pass or (tracking noise, pad[1] = 1) of the row
                const auto &seeds = plan.seeds[ps.mode];
                const auto *sh = ps.pad[1] == 1 ? &plan.shifts[ps.stage] : nullptr;
                if (seeds.size() < size_t(R) || (sh && sh->size() < size_t(R)))
                    throw std::runtime_error("ntsc gpu: noise tables missing");
                noise_rows.resize(size_t(R));
                for (int r = 0; r < R; ++r) {
                    NoiseRowGpu &n = noise_rows[size_t(r)];
                    n.seed = seeds[size_t(r)].noise_seed;
                    n.offset = seeds[size_t(r)].offset;
                    n.intensity = sh ? (*sh)[size_t(r)].intensity : ps.b;
                    n.active = sh ? ((*sh)[size_t(r)].boundary >= 0) : 1u;
                }
                const gpu::Buffer *nb =
                    table("noise" + std::to_string(idx), noise_rows.data(), noise_rows.size() * sizeof(NoiseRowGpu));
                u.planes = ps.planes;
                u.fx[0] = ps.a;
                u.fx[2] = 1.0f;  // gain (video_noise_line's Fbm)
                u.ix[0] = ps.pad[0];
                for (auto [r0, r1] : row_runs(R, [&](int r) { return noise_rows[size_t(r)].active != 0; })) {
                    u.ix[3] = r0;
                    run("ntsc_gpu/noise_add", u, W, r1 - r0, true, nullptr, true, {nb});
                }
                break;
            }
            case NTSC_GPU_K_SPECKLES: {
                const NtscGpuSpeckleList &l = plan.speckles[ps.stage];
                if (l.events.empty()) break;
                if (l.row_ranges.size() < size_t(R) * 2) throw std::runtime_error("ntsc gpu: speckle tables missing");
                const std::string key = std::to_string(ps.stage);
                const gpu::Buffer *rr = table("spr" + key, l.row_ranges.data(), size_t(R) * 2 * sizeof(uint32_t));
                const gpu::Buffer *ev =
                    table("spe" + key, l.events.data(), l.events.size() * sizeof(NtscGpuSpeckle));
                const gpu::Buffer *va = table("spv" + key, l.values.data(), l.values.size() * sizeof(float));
                uint32_t longest = 0;
                for (const NtscGpuSpeckle &e : l.events) longest = std::max(longest, e.count);
                u.ix[0] = int32_t(longest);
                for (auto [r0, r1] : row_runs(R, [&](int r) { return l.row_ranges[size_t(r) * 2 + 1] != 0; })) {
                    u.ix[3] = r0;
                    run("ntsc_gpu/add_speckles", u, W, r1 - r0, true, nullptr, false, {nullptr, nullptr, rr, ev, va});
                }
                break;
            }
            case NTSC_GPU_K_SHIFT_ROWS: {
                const auto &sh = plan.shifts[ps.stage];
                if (sh.size() < size_t(R)) throw std::runtime_error("ntsc gpu: shift table missing");
                const gpu::Buffer *sb =
                    table("shift" + std::to_string(ps.stage), sh.data(), size_t(R) * sizeof(NtscGpuRowShift));
                const auto runs = row_runs(R, [&](int r) { return sh[size_t(r)].boundary >= 0; });
                for (int pl = 0; pl < 3; ++pl) {
                    if (!(ps.planes & (1u << pl)) || runs.empty()) continue;
                    dev.copy(*plane_tex[pl], *p.s);  // shift_row reads the row as it was
                    NtscGpuU us = u;
                    us.planes = 1u << pl;
                    for (auto [r0, r1] : runs) {
                        us.ix[3] = r0;
                        run("ntsc_gpu/shift_rows", us, W, r1 - r0, true, nullptr, true, {nullptr, sb});
                    }
                }
                break;
            }
            case NTSC_GPU_K_CHROMA_DELAY:
                for (int pl = 1; pl < 3; ++pl) {
                    dev.copy(*plane_tex[pl], *p.s);
                    NtscGpuU uc = u;
                    uc.planes = 1u << pl;
                    uc.fx[0] = ps.a;
                    uc.ix[0] = ps.mode;
                    uc.ix[2] = ps.pad[0];
                    run("ntsc_gpu/chroma_delay", uc, W, R, true, nullptr, true);
                }
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
                if (plan.height < 2) break;  // ntsc-rs: yiq.dimensions.1 < 2 skips it
                dev.copy(*p.i, *p.s);
                u.mode = 0;
                run("ntsc_gpu/vert_blend", u, W, R, true);
                dev.copy(*p.q, *p.s);
                u.mode = 1;
                run("ntsc_gpu/vert_blend", u, W, R, true);
                break;
            default:
                throw std::runtime_error("ntsc gpu: unknown pass kind " + std::to_string(ps.kind));
            }
            tick(ntsc_stage_name(ps.stage));
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
    m.tick("to_bgra8");
    return out;
}

const std::vector<std::pair<std::string, double>> &NtscGpu::pass_times() const { return impl_->times; }

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
