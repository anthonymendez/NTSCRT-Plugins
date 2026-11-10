// glitch.cpp — GlitchStage (GPU HAL path) and the CPU stage. Port of upstream
// GlitchRenderer.swift / GlitchStage.swift (NTSCRT 2b3013d). Owner: agent B.
//
// GPU passes (shaders/glitch.slang, kernels "glitch/<entry>"):
//   glitch_gate    b0 U, t0 src, t8 rows            -> u8 gate (float2/row)
//   glitch_chroma  b0 U, u8 gate (read)             -> u9 color (float4/row)
//   glitch_compose b0 U, t0 src, t8 rows, t9 color, t10 drops -> u0 dst
#include <cstring>

#include "../stages.h"
#include "glitch_cpu.h"

namespace ntscrt {

namespace glitch {

bool glitch_active(const json &look) {
    if (!look.is_object()) return false;
    auto g = look.find("glitch");
    if (g == look.end() || !g->is_object()) return false;
    auto e = g->find("enabled");
    return e != g->end() && e->is_boolean() && e->get<bool>();
}

void GlitchCpuStage::process(ImageBGRA8 &inout, const FrameContext &ctx) {
    if (inout.width <= 0 || inout.height <= 0) return;
    const GlitchFieldPlan &plan = timeline_.plan_for(ctx, inout.width, inout.height);
    ImageBGRA8 out;
    cpu_render_plan(plan, inout, out, noise_seed(ctx.seed));
    inout = std::move(out);
}

}  // namespace glitch

using namespace glitch;

struct GlitchStage::Impl {
    gpu::Device &dev;
    GlitchTimeline timeline;
    std::shared_ptr<gpu::Buffer> rows, gate, color, drops;
    size_t rows_cap = 0, gate_cap = 0, color_cap = 0, drops_cap = 0;
    explicit Impl(gpu::Device &d) : dev(d) {}

    // Reuses `buf` when it already holds `bytes`; uploads `data` if given.
    void ensure(std::shared_ptr<gpu::Buffer> &buf, size_t &cap, size_t bytes, const void *data) {
        if (!buf || cap < bytes) {
            buf = dev.create_buffer(bytes, data);
            cap = bytes;
            return;
        }
        if (data) dev.upload(*buf, data, bytes, 0);
    }
};

GlitchStage::GlitchStage(gpu::Device &dev) : impl_(std::make_unique<Impl>(dev)) {}
GlitchStage::~GlitchStage() = default;

bool GlitchStage::active(const json &look) const { return glitch_active(look); }

std::shared_ptr<gpu::Texture> GlitchStage::run(const std::shared_ptr<gpu::Texture> &in, const FrameContext &ctx) {
    if (!in) return in;
    const gpu::TextureDesc &d = in->desc();
    if (d.width <= 0 || d.height <= 0) return in;
    gpu::Device &dev = impl_->dev;

    const GlitchFieldPlan &plan = impl_->timeline.plan_for(ctx, d.width, d.height);
    if (int(plan.rows.size()) != d.height) return in;
    GlitchU U = make_uniforms(plan, d.width, noise_seed(ctx.seed));
    std::vector<GlitchDropout> drops = dropout_buffer(plan);

    const size_t n = plan.rows.size();
    impl_->ensure(impl_->rows, impl_->rows_cap, n * sizeof(GlitchRow), plan.rows.data());
    impl_->ensure(impl_->gate, impl_->gate_cap, n * 2 * sizeof(float), nullptr);
    impl_->ensure(impl_->color, impl_->color_cap, n * 4 * sizeof(float), nullptr);
    impl_->ensure(impl_->drops, impl_->drops_cap, drops.size() * sizeof(GlitchDropout), drops.data());

    gpu::TextureDesc od = d;
    od.shader_write = true;
    od.render_target = false;
    auto out = dev.create_texture(od);

    gpu::Kernel *kGate = dev.kernel("glitch/glitch_gate");
    gpu::Kernel *kChroma = dev.kernel("glitch/glitch_chroma");
    gpu::Kernel *kCompose = dev.kernel("glitch/glitch_compose");
    if (!kGate || !kChroma || !kCompose) return in;

    {
        gpu::Bindings b;
        b.constants = &U;
        b.constants_size = sizeof U;
        b.textures = {in.get()};
        b.buffers = {impl_->rows.get()};
        b.out_buffers = {impl_->gate.get()};
        dev.dispatch(kGate, b, int(n), 1);
    }
    {
        gpu::Bindings b;
        b.constants = &U;
        b.constants_size = sizeof U;
        b.out_buffers = {impl_->gate.get(), impl_->color.get()};
        dev.dispatch(kChroma, b, 1, 1);
    }
    {
        gpu::Bindings b;
        b.constants = &U;
        b.constants_size = sizeof U;
        b.textures = {in.get()};
        b.buffers = {impl_->rows.get(), impl_->color.get(), impl_->drops.get()};
        b.out_textures = {out.get()};
        dev.dispatch(kCompose, b, d.width, d.height);
    }
    return out;
}

void GlitchStage::flush() { impl_->timeline.flush(); }

}  // namespace ntscrt
