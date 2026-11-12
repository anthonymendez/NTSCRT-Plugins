// digital_gpu.cpp — driver of the Digital video stage's kernels
// (shaders/digital_gpu.slang; design: docs/dev/DIGITAL-GPU.md).
//
// Per frame: scale_h (W x H -> cw x H, float) and scale_v (-> cw x ch, BGRA8),
// the capture picture read back (cw x ch x 4 bytes); after the codec, its
// picture uploaded (the same size) and upscale (-> W x H, BGRA8). The tables
// (Renderer::scalePlan, ninths, the upscale maps) are made on the CPU and
// uploaded when the sizes or the optics change.
#include "digital_gpu.h"

#include <chrono>
#include <cstring>
#include <stdexcept>

#include "../../../src/phase_times.h"

namespace ntscrt {

GpuRouteEnv digital_gpu_env() { return gpu_route_env("NTSCRT_DIGITAL_GPU"); }
std::string digital_gpu_env_text() { return gpu_route_env_text("NTSCRT_DIGITAL_GPU"); }

bool digital_gpu_route(gpu::Device &dev, std::string *why, DigitalGpu *g) {
    auto no = [&](const std::string &w) {
        if (why) *why = w;
        return false;
    };
    const GpuRouteEnv env = digital_gpu_env();
    if (env == GpuRouteEnv::Off) return no("NTSCRT_DIGITAL_GPU=0");
    const bool force = env == GpuRouteEnv::Force;
    // Metal: the kernels compile for it (CI builds the library), but the route is
    // unverified there and Metal compiles with fast math (Slang drops `precise`).
    if (!force && dev.backend() != gpu::Backend::D3D11) return no("not on Metal yet");
    // On WARP (a CPU rasterizer) the kernels are slower than the CPU code.
    if (!force && gpu_is_software(dev)) return no("software device (WARP): the CPU code is faster");
    const char *arith = g ? g->arith_mode() : "?";
    if (!force && std::strcmp(arith, "inexact") == 0)
        return no("this GPU rounds neither float nor double arithmetic like the CPU: not exact");
    if (why) {
        std::string w = gpu_route_env_label("NTSCRT_DIGITAL_GPU", env);
        const std::string ad = gpu_adapter_description(dev);
        if (!ad.empty()) w += ": " + ad;
        w += std::string(", arith ") + arith;
        *why = w;
    }
    return true;
}

namespace {

// Mirrors DigitalU in the shader (ConstantBuffer b0).
struct DigitalU {
    uint32_t W, H, cw, ch;
    uint32_t tapsX, tapsY, flags, pad0;
    float mix, boost, pad1, pad2;
    float probe[4];
};
static_assert(sizeof(DigitalU) == 64, "DigitalU must match the shader's constant buffer");
constexpr uint32_t kCopy = 1, kAlias = 2, kBoost = 4, kFields = 8;

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point &t) {
    const auto now = Clock::now();
    const double d = std::chrono::duration<double, std::milli>(now - t).count();
    t = now;
    return d;
}

}  // namespace

struct DigitalGpu::Impl {
    gpu::Device &dev;
    explicit Impl(gpu::Device &d) : dev(d) {}

    int arith = -1;  // -1 not probed, 0 float, 1 double, 2 inexact
    std::vector<std::shared_ptr<gpu::Texture>> pool;

    // The tables of the current plan, uploaded once per plan.
    digital::ScalePlan plan;
    bool have_plan = false;
    struct Slot {
        std::shared_ptr<gpu::Buffer> buf;
        size_t cap = 0;
    };
    Slot spanX, weightX, spanY, weightY, aliasRow, aliasCol, ninths, mapX, mapY;
    // The upscale maps' sizes.
    int map_cw = 0, map_ch = 0, map_w = 0, map_h = 0;

    gpu::Kernel *k(const std::string &name) {
        gpu::Kernel *kk = dev.kernel(name);
        if (!kk) throw std::runtime_error("digital gpu: kernel " + name + " missing");
        return kk;
    }
    // The kernel `name`, or its double-precision variant where the device needs it.
    gpu::Kernel *ka(const char *name) {
        probe();
        return k(std::string("digital_gpu/") + name + (arith == 1 ? "_x" : ""));
    }

    // Runs probe_round (float, then double): see the shader.
    void probe() {
        if (arith >= 0) return;
        // NTSCRT_DIGITAL_GPU_ARITH=double / float: those kernels whatever the probe says
        // (debugging, and the test of the double-precision kernels on GPUs that don't need them).
        const std::string forced = gpu_route_env_text("NTSCRT_DIGITAL_GPU_ARITH");
        if (forced == "float") {
            arith = 0;
            return;
        }
        auto run = [&](const char *name) {
            auto t = dev.create_texture({1, 1, gpu::Format::RGBA32F, true, false});
            DigitalU u{};
            u.probe[0] = 1.0f + 0x1p-12f;                  // x
            u.probe[1] = 1.0f + 0x1p-12f + 0x1p-23f;       // y
            u.probe[2] = 1.0f + 0x1p-11f;                  // z
            u.probe[3] = 1.0f + 0x1p-11f + 0x1p-22f;       // w
            u.mix = 1.0f;
            u.boost = 0x1p-24f + 0x1p-30f;
            gpu::Bindings b;
            b.constants = &u;
            b.constants_size = sizeof u;
            b.out_textures = {t.get()};
            dev.dispatch(k(std::string("digital_gpu/") + name), b, 1, 1);
            float r[4] = {};
            dev.download(*t, r, sizeof r);
            return r[0] == 0.0f && r[1] == 0.0f && r[2] == 0x1p-23f && r[3] == 1.0f;
        };
        if (forced != "double" && run("probe_round")) {
            arith = 0;
            return;
        }
        try {
            arith = run("probe_round_x") ? 1 : 2;
        } catch (const std::exception &) {
            arith = 2;  // no doubles
        }
    }

    std::shared_ptr<gpu::Texture> acquire(const gpu::TextureDesc &d) {
        for (auto &t : pool) {
            const gpu::TextureDesc &e = t->desc();
            if (t.use_count() == 1 && e.width == d.width && e.height == d.height && e.format == d.format &&
                e.shader_write == d.shader_write && e.render_target == d.render_target)
                return t;
        }
        auto t = dev.create_texture(d);
        if (pool.size() >= 8) pool.erase(pool.begin());
        pool.push_back(t);
        return t;
    }
    gpu::Buffer *table(Slot &s, const void *data, size_t bytes) {
        const size_t room = std::max<size_t>(bytes, 16);
        if (!s.buf || s.cap < room) {
            std::vector<uint8_t> init(room, 0);
            if (bytes) std::memcpy(init.data(), data, bytes);
            s.buf = dev.create_buffer(room, init.data());
            s.cap = room;
        } else if (bytes) {
            dev.upload(*s.buf, data, bytes, 0);
        }
        return s.buf.get();
    }
    template <class T>
    gpu::Buffer *table(Slot &s, const std::vector<T> &v) {
        return table(s, v.data(), v.size() * sizeof(T));
    }

    void use_plan(digital::ScalePlan p) {
        if (have_plan && p == plan) return;
        plan = std::move(p);
        have_plan = true;
        table(spanX, plan.spanX);
        table(weightX, plan.weightX);
        table(spanY, plan.spanY);
        table(weightY, plan.weightY);
        table(aliasRow, plan.aliasRow);
        table(aliasCol, plan.aliasCol);
        if (!ninths.buf) table(ninths, digital::Renderer::ninths());
    }
};

DigitalGpu::DigitalGpu(gpu::Device &dev) : impl_(std::make_unique<Impl>(dev)) {}
DigitalGpu::~DigitalGpu() = default;

const char *DigitalGpu::arith_mode() {
    impl_->probe();
    static const char *names[] = {"float", "double", "inexact"};
    return names[impl_->arith];
}

std::vector<uint8_t> DigitalGpu::scale(const gpu::Texture &in, digital::Size capture, const digital::Settings &optics,
                                       digital::Renderer::Profile *prof) {
    Impl &m = *impl_;
    const int W = in.desc().width, H = in.desc().height, cw = capture.width, ch = capture.height;
    if (W <= 0 || H <= 0 || cw <= 0 || ch <= 0) throw std::runtime_error("digital gpu: empty picture");
    const bool profiling = PhaseTimes::profiling();
    auto t = Clock::now();
    m.use_plan(digital::Renderer::scalePlan(W, H, capture, optics));
    const digital::ScalePlan &p = m.plan;

    DigitalU u{};
    u.W = uint32_t(W);
    u.H = uint32_t(H);
    u.cw = uint32_t(cw);
    u.ch = uint32_t(ch);
    u.tapsX = uint32_t(p.tapsX);
    u.tapsY = uint32_t(p.tapsY);
    u.flags = (p.copy ? kCopy : 0) | (p.mix > 0 ? kAlias : 0) | (p.boost > 0 ? kBoost : 0) | (p.fields ? kFields : 0);
    u.mix = p.mix;
    u.boost = p.boost;

    std::shared_ptr<gpu::Texture> mid;
    if (!p.copy) {
        mid = m.acquire({cw, H, gpu::Format::RGBA32F, true, false});
        gpu::Bindings b;
        b.constants = &u;
        b.constants_size = sizeof u;
        b.textures = {&in};
        b.buffers = {m.spanX.buf.get(), m.weightX.buf.get()};
        b.out_textures = {mid.get()};
        m.dev.dispatch(m.ka("scale_h"), b, cw, H);
    }
    auto cap = m.acquire({cw, ch, gpu::Format::BGRA8, true, false});
    {
        gpu::Bindings b;
        b.constants = &u;
        b.constants_size = sizeof u;
        b.textures = {&in, mid ? mid.get() : &in};
        b.buffers = {m.spanY.buf.get(), m.weightY.buf.get(), m.aliasRow.buf.get(), m.aliasCol.buf.get(),
                     m.ninths.buf.get()};
        b.out_textures = {cap.get()};
        m.dev.dispatch(m.ka("scale_v"), b, cw, ch);
    }
    if (profiling) m.dev.finish();
    if (prof) prof->scale += ms_since(t);
    std::vector<uint8_t> out(size_t(cw) * ch * 4);
    m.dev.download(*cap, out.data(), ptrdiff_t(cw) * 4);  // waits for the kernels
    if (prof) prof->download += ms_since(t);
    return out;
}

std::shared_ptr<gpu::Texture> DigitalGpu::upscale(const std::vector<uint8_t> &picture, digital::Size capture,
                                                  int width, int height, digital::Renderer::Profile *prof) {
    Impl &m = *impl_;
    const int cw = capture.width, ch = capture.height;
    if (width <= 0 || height <= 0 || cw <= 0 || ch <= 0 || picture.size() != size_t(cw) * ch * 4)
        throw std::runtime_error("digital gpu: upscale of a picture of the wrong size");
    auto t = Clock::now();
    auto shown = m.acquire({cw, ch, gpu::Format::BGRA8, false, false});
    m.dev.upload(*shown, picture.data(), ptrdiff_t(cw) * 4);
    if (prof) prof->upload += ms_since(t);
    if (m.map_cw != cw || m.map_ch != ch || m.map_w != width || m.map_h != height) {
        std::vector<int32_t> xs, ys;
        digital::Renderer::upscaleMaps(capture, width, height, xs, ys);
        m.table(m.mapX, xs);
        m.table(m.mapY, ys);
        m.map_cw = cw, m.map_ch = ch, m.map_w = width, m.map_h = height;
    }
    auto out = m.acquire({width, height, gpu::Format::BGRA8, true, false});
    DigitalU u{};
    u.W = uint32_t(width);
    u.H = uint32_t(height);
    u.cw = uint32_t(cw);
    u.ch = uint32_t(ch);
    gpu::Bindings b;
    b.constants = &u;
    b.constants_size = sizeof u;
    b.textures = {shown.get()};
    b.buffers = {nullptr, nullptr, nullptr, nullptr, nullptr, m.mapX.buf.get(), m.mapY.buf.get()};
    b.out_textures = {out.get()};
    m.dev.dispatch(m.k("digital_gpu/upscale"), b, width, height);
    if (PhaseTimes::profiling()) m.dev.finish();
    if (prof) prof->upscale += ms_since(t);
    return out;
}

}  // namespace ntscrt
