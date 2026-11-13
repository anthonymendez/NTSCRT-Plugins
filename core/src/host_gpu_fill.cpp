// host_gpu_fill.cpp — Clip mode's hole filling for host GPU frames on the GPU
// (shaders/hostgpu_fill.slang; docs/dev/HOST-GPU.md, "Hole filling on the GPU").
//
// fill_transparent (pixel_convert.cpp) is the reference and the fallback: with
// NTSCRT_FILL_GPU=0, where the route's rule says no, and for a frame whose kernels
// fail, the frame takes the gpu-cpuconvert route (the float frame read to the CPU,
// to_bgra8 there). Per frame here: the pull levels (fill_down0 from the host source,
// then fill_down to 1 x 1), the push (fill_up, coarse to fine), and convert_fill (the
// picture, convert_in's but for the transparent pixels). No CPU round trip.
#include <cstring>
#include <stdexcept>

#include "../gpu/gpu_route.h"
#include "host_gpu.h"

namespace ntscrt::hostgpu {

std::string fill_gpu_env_text() { return gpu_route_env_text("NTSCRT_FILL_GPU"); }

bool fill_gpu_route(gpu::Device &dev, std::string *why, Fill *f) {
    auto no = [&](const std::string &w) {
        if (why) *why = w;
        return false;
    };
    const GpuRouteEnv env = gpu_route_env("NTSCRT_FILL_GPU");
    if (env == GpuRouteEnv::Off) return no("NTSCRT_FILL_GPU=0");
    const bool force = env == GpuRouteEnv::Force;
    // Metal: the kernels compile for it (CI builds the library), but the route is
    // unverified there and Metal compiles with fast math (Slang drops `precise`).
    if (!force && dev.backend() != gpu::Backend::D3D11) return no("not on Metal yet");
    // Host GPU frames never come with WARP (the bridge needs the host's GPU); for the
    // core's own tests on WARP: as the other routes, only when forced.
    if (!force && gpu_is_software(dev)) return no("software device (WARP)");
    const char *arith = f ? f->arith_mode() : "?";
    if (!force && std::strcmp(arith, "inexact") == 0)
        return no("this GPU rounds neither float nor double arithmetic like the CPU: not exact");
    if (why) {
        std::string w = gpu_route_env_label("NTSCRT_FILL_GPU", env);
        const std::string ad = gpu_adapter_description(dev);
        if (!ad.empty()) w += ": " + ad;
        w += std::string(", arith ") + arith;
        *why = w;
    }
    return true;
}

namespace {
// Mirrors FillParams in hostgpu_fill.slang (ConstantBuffer b0).
struct FillParams {
    InParams in;
    uint32_t in_w, in_h, out_w, out_h;
    float probe[4];
    float probe2[4];
};
static_assert(sizeof(InParams) == 64 && sizeof(FillParams) == 112, "FillParams must match the shader's");
}  // namespace

struct Fill::Impl {
    gpu::Device &dev;
    explicit Impl(gpu::Device &d) : dev(d) {}

    int arith = -1;  // -1 not probed, 0 float, 1 double, 2 inexact
    std::shared_ptr<gpu::Buffer> unorm8;  // k / 255.f for every byte k: the CPU's values
    int lw = 0, lh = 0;                   // the frame size the levels are for
    std::vector<std::pair<int, int>> sizes;
    std::vector<std::shared_ptr<gpu::Texture>> down, up;  // per level: pull result, finished

    gpu::Kernel *k(const std::string &name) {
        gpu::Kernel *kk = dev.kernel("hostgpu_fill/" + name);
        if (!kk) throw std::runtime_error("hole filling: kernel " + name + " missing");
        return kk;
    }
    gpu::Kernel *ka(const char *name) {
        probe();
        return k(std::string(name) + (arith == 1 ? "_x" : ""));
    }

    // fill_probe (float, then double): see the shader; as DigitalGpu's.
    void probe() {
        if (arith >= 0) return;
        const std::string forced = gpu_route_env_text("NTSCRT_FILL_GPU_ARITH");
        if (forced == "float") {
            arith = 0;
            return;
        }
        auto run = [&](const char *name) {
            auto t = dev.create_texture({1, 1, gpu::Format::RGBA32F, true, false});
            FillParams u{};
            u.probe[0] = 1.0f + 0x1p-12f;             // x
            u.probe[1] = 1.0f + 0x1p-12f + 0x1p-23f;  // y
            u.probe[2] = 1.0f + 0x1p-11f;             // z
            u.probe[3] = 1.0f + 0x1p-11f + 0x1p-22f;  // w
            u.probe2[0] = 1.0f;
            u.probe2[1] = 0x1p-24f + 0x1p-30f;
            gpu::Bindings b;
            b.constants = &u;
            b.constants_size = sizeof u;
            b.out_textures = {t.get()};
            dev.dispatch(k(name), b, 1, 1);
            float r[4] = {};
            dev.download(*t, r, sizeof r);
            return r[0] == 0.0f && r[1] == 0.0f && r[2] == 0x1p-23f && r[3] == 1.0f;
        };
        if (forced != "double" && run("fill_probe")) {
            arith = 0;
            return;
        }
        try {
            arith = run("fill_probe_x") ? 1 : 2;
        } catch (const std::exception &) {
            arith = 2;  // no doubles
        }
    }

    // The levels of a W x H frame: (W + 1) / 2 x (H + 1) / 2, ..., 1 x 1 (as fill_transparent).
    void levels(int W, int H) {
        if (W == lw && H == lh) return;
        sizes.clear();
        down.clear();
        up.clear();
        int w = W, h = H;
        while (w > 1 || h > 1) {
            w = (w + 1) / 2;
            h = (h + 1) / 2;
            sizes.push_back({w, h});
            down.push_back(dev.create_texture({w, h, gpu::Format::RGBA32F, true, false}));
            up.push_back(dev.create_texture({w, h, gpu::Format::RGBA32F, true, false}));
        }
        lw = W;
        lh = H;
    }
};

Fill::Fill(gpu::Device &dev) : impl_(std::make_unique<Impl>(dev)) {}
Fill::~Fill() = default;

const char *Fill::arith_mode() {
    impl_->probe();
    static const char *names[] = {"float", "double", "inexact"};
    return names[impl_->arith];
}

void Fill::convert(const gpu::Texture &src, const InParams &p, gpu::Texture &pic) {
    Impl &m = *impl_;
    const int W = int(p.frame_w), H = int(p.frame_h);
    if (W <= 0 || H <= 0 || p.cw == 0 || p.ch == 0) throw std::runtime_error("hole filling: empty frame");
    if (!m.unorm8) {
        float t[256];
        for (int i = 0; i < 256; ++i) t[i] = i / 255.f;  // fill_transparent's p / 255.f
        m.unorm8 = m.dev.create_buffer(sizeof t, t);
    }
    m.levels(W, H);
    FillParams q{};
    q.in = p;
    gpu::Bindings b;
    b.constants = &q;
    b.constants_size = sizeof q;
    if (m.sizes.empty()) {
        // 1 x 1: transparent or not, nothing changes (fill_transparent: all or none empty).
        b.textures = {&src};
        b.out_textures = {&pic};
        m.dev.dispatch(m.dev.kernel("hostgpu_in/convert_in"), b, int(p.cw), int(p.ch), 1);
        return;
    }
    const size_t n = m.sizes.size();
    // pull
    for (size_t i = 0; i < n; ++i) {
        q.in_w = uint32_t(i ? m.sizes[i - 1].first : W);
        q.in_h = uint32_t(i ? m.sizes[i - 1].second : H);
        q.out_w = uint32_t(m.sizes[i].first);
        q.out_h = uint32_t(m.sizes[i].second);
        b.textures = {&src, i ? m.down[i - 1].get() : nullptr};
        b.buffers = {m.unorm8.get()};
        b.out_textures = {m.down[i].get()};
        m.dev.dispatch(m.ka(i ? "fill_down" : "fill_down0"), b, int(q.out_w), int(q.out_h), 1);
    }
    // push: level i finished from level i + 1 (the top one is finished as pulled)
    auto finished = [&](size_t i) { return i + 1 == n ? m.down[i].get() : m.up[i].get(); };
    for (size_t i = n - 1; i-- > 0;) {
        q.in_w = uint32_t(m.sizes[i].first);
        q.in_h = uint32_t(m.sizes[i].second);
        q.out_w = uint32_t(m.sizes[i + 1].first);
        q.out_h = uint32_t(m.sizes[i + 1].second);
        b.textures = {&src, m.down[i].get(), finished(i + 1)};
        b.buffers = {};
        b.out_textures = {m.up[i].get()};
        m.dev.dispatch(m.ka("fill_up"), b, int(q.in_w), int(q.in_h), 1);
    }
    // the picture
    q.in_w = uint32_t(W);
    q.in_h = uint32_t(H);
    q.out_w = uint32_t(m.sizes[0].first);
    q.out_h = uint32_t(m.sizes[0].second);
    b.textures = {&src, nullptr, finished(0), m.down[n - 1].get()};
    b.buffers = {};
    b.out_textures = {&pic};
    m.dev.dispatch(m.ka("convert_fill"), b, int(p.cw), int(p.ch), 1);
}

}  // namespace ntscrt::hostgpu
