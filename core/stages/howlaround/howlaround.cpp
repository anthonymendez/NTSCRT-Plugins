// howlaround.cpp — Screen Loop (video feedback) stage on the GPU HAL.
// Port of upstream Sources/CrtCore/Howlaround.swift (HowlaroundLoop). The
// sequencing lives in howl_engine.h, the math in howl_math.cpp; this file is
// the pixel work: kernels shaders/howl_*.slang, textures, readback.
#include <cstring>
#include <stdexcept>

#include "../stages.h"
#include "howl_engine.h"

namespace ntscrt {

namespace {

using howl::CameraUniforms;
using howl::CounterBitmap;
using TexPtr = std::shared_ptr<gpu::Texture>;

struct GpuOps {
    using Pic = TexPtr;

    gpu::Device &dev;
    const FrameContext *ctx = nullptr;
    const HowlHooks *hooks = nullptr;
    int scene_w = 0, scene_h = 0;
    gpu::Format scene_format = gpu::Format::BGRA8;

    TexPtr black, counter_tex;
    std::string counter_uploaded;
    std::vector<TexPtr> pool;

    explicit GpuOps(gpu::Device &d) : dev(d) {}

    bool has_hooks() const { return hooks && hooks->chain_input && hooks->render_tv; }

    gpu::Kernel *kernel(const char *name) {
        gpu::Kernel *k = dev.kernel(name);
        if (!k) throw std::runtime_error(std::string("howlaround: missing kernel ") + name);
        return k;
    }

    // A texture nobody else holds, or a new one. Pictures handed to the engine
    // stay referenced there and are never recycled while kept.
    TexPtr acquire(int w, int h, gpu::Format f, bool render_target = false) {
        for (auto &t : pool) {
            const auto &d = t->desc();
            if (t.use_count() == 1 && d.width == w && d.height == h && d.format == f &&
                d.render_target == render_target)
                return t;
        }
        TexPtr t = dev.create_texture({w, h, f, true, render_target});
        if (pool.size() < 64) pool.push_back(t);
        return t;
    }

    TexPtr upload_bgra8(const ImageBGRA8 &im) {
        TexPtr t = acquire(im.width, im.height, scene_format);
        dev.upload(*t, im.px.data(), ptrdiff_t(im.width) * 4);
        return t;
    }

    Pic scene(int64_t f) {
        if (!ctx || !ctx->source_at) return {};
        auto im = ctx->source_at(f);
        if (!im || im->width != scene_w || im->height != scene_h) return {};
        if (scene_format != gpu::Format::BGRA8) return {};
        return upload_bgra8(*im);
    }

    void blur(const gpu::Texture &src, gpu::Texture &dst, double sigma, float dx, float dy) {
        howl::BlurUniforms u{{dx, dy}, float(sigma), howl::blur_radius(sigma)};
        gpu::Bindings b;
        b.constants = &u;
        b.constants_size = sizeof u;
        b.textures = {&src};
        b.out_textures = {&dst};
        dev.dispatch(kernel("howl_blur/blur"), b, dst.desc().width, dst.desc().height);
    }

    Pic camera(const Pic &scene_tex, const Pic &tv, const CameraUniforms &u, double sigma,
               const CounterBitmap *counter, bool measure, double &scene_luma, double &camera_luma) {
        if (!black) {
            black = dev.create_texture({1, 1, gpu::Format::RGBA8, false, false});
            const uint8_t px[4] = {0, 0, 0, 255};
            dev.upload(*black, px, 4);
        }
        TexPtr tvsrc = tv ? tv : black;
        TexPtr a, b;
        if (tv && sigma > 0) {
            const auto &d = tv->desc();
            a = acquire(d.width, d.height, gpu::Format::RGBA8);
            b = acquire(d.width, d.height, gpu::Format::RGBA8);
            blur(*tv, *a, sigma, 1, 0);
            blur(*a, *b, sigma, 0, 1);
            tvsrc = b;
        }
        if (counter && counter->text != counter_uploaded) {
            counter_tex = dev.create_texture({counter->width, counter->height, gpu::Format::BGRA8, false, false});
            dev.upload(*counter_tex, counter->bgra.data(), ptrdiff_t(counter->width) * 4);
            counter_uploaded = counter->text;
        }
        const int W = scene_tex->desc().width, H = scene_tex->desc().height;
        TexPtr out = acquire(W, H, scene_tex->desc().format);
        gpu::Bindings bind;
        bind.constants = &u;
        bind.constants_size = sizeof u;
        bind.textures = {scene_tex.get(), tvsrc.get(), counter ? counter_tex.get() : tvsrc.get()};
        bind.out_textures = {out.get()};
        dev.dispatch(kernel("howl_camera/camera"), bind, W, H);

        if (measure) {
            // The camcorder's exposure meter: its own picture against the room's.
            const int tw = (W + 15) / 16, th = (H + 15) / 16;
            TexPtr sums = acquire(tw, th, gpu::Format::RG32F);
            gpu::Bindings mb;
            mb.textures = {out.get(), scene_tex.get()};
            mb.out_textures = {sums.get()};
            dev.dispatch(kernel("howl_mean/tiles"), mb, tw, th);
            std::vector<float> px(size_t(tw) * th * 2);  // download waits for the dispatch
            dev.download(*sums, px.data(), ptrdiff_t(tw) * 8);
            double cs = 0, ss = 0;
            for (size_t i = 0; i < px.size(); i += 2) { cs += px[i]; ss += px[i + 1]; }
            const double n = double(W) * double(H);
            camera_luma = cs / n;
            scene_luma = ss / n;
        }
        return out;
    }

    TexPtr resample(const gpu::Texture &src, int w, int h) {
        TexPtr out = acquire(w, h, gpu::Format::RGBA8, true);
        gpu::Bindings b;
        b.textures = {&src};
        b.out_textures = {out.get()};
        dev.dispatch(kernel("howl_resample/resample"), b, w, h);
        return out;
    }

    // The main path's chain input through the loop's own CRT copy.
    TexPtr render_tv(const gpu::Texture &chain, const HowlPass &p) {
        howl::Size fb = howl::feedback_size(chain.desc().width, chain.desc().height);
        TexPtr tv = acquire(fb.width, fb.height, gpu::Format::RGBA8, true);
        hooks->render_tv(chain, *tv, p);
        return tv;
    }

    Pic tv_pass(const Pic &cam, const HowlPass &p) {
        if (has_hooks()) {
            TexPtr chain = hooks->chain_input(cam, p);
            if (chain) return render_tv(*chain, p);
        }
        return direct_tv(cam);
    }
    // No chain: the TV shows the camera's picture resized.
    TexPtr direct_tv(const Pic &cam) {
        howl::Size fb = howl::feedback_size(cam->desc().width, cam->desc().height);
        return resample(*cam, fb.width, fb.height);
    }

    // Pipelined TV passes (howl_engine.h): the chain's CPU half of one pass
    // runs while the next camera is exposed.
    struct Pending {
        std::function<TexPtr()> finish;
        HowlPass pass;
        TexPtr cam;
    };
    int max_inflight() const { return has_hooks() && hooks->chain_input_begin ? 2 : 0; }
    Pending tv_begin(const Pic &cam, const HowlPass &p) { return {hooks->chain_input_begin(cam, p), p, cam}; }
    Pic tv_end(Pending &&q) {
        TexPtr chain = q.finish ? q.finish() : nullptr;
        return chain ? render_tv(*chain, q.pass) : direct_tv(q.cam);
    }

    static size_t bytes(const TexPtr &t) {
        const auto &d = t->desc();
        size_t px = 4;
        switch (d.format) {
            case gpu::Format::RGBA16F:
            case gpu::Format::RG32F: px = 8; break;
            case gpu::Format::RGBA32F: px = 16; break;
            default: break;
        }
        return size_t(d.width) * size_t(d.height) * px;
    }
};

}  // namespace

struct Howlaround::Impl {
    gpu::Device &dev;
    GpuOps ops;
    HowlHooks hooks;
    std::unique_ptr<howl::Engine<GpuOps>> engine;
    std::string key;
    // Chain input size learned for a scene size (from the main path, or one probe).
    int chain_for_w = 0, chain_for_h = 0, chain_w = 0, chain_h = 0;
    TexPtr last_camera;
    int64_t last_frame = -1;

    explicit Impl(gpu::Device &d) : dev(d), ops(d) {}

    // Checkpoints only while the TV pass keeps no state of its own.
    void sync_checkpoints() {
        if (engine) engine->set_checkpoints(!(hooks.tv_keeps_state && hooks.tv_keeps_state()));
    }
};

Howlaround::Howlaround(gpu::Device &dev) : impl_(std::make_unique<Impl>(dev)) {}
Howlaround::~Howlaround() = default;

void Howlaround::set_hooks(HowlHooks hooks) {
    // Re-setting the same hooks each render keeps the history; switching between
    // hooked and unhooked changes what the TV shows, so the loop starts over.
    auto complete = [](const HowlHooks &h) { return h.chain_input && h.render_tv; };
    const bool changed = complete(impl_->hooks) != complete(hooks);
    impl_->hooks = std::move(hooks);
    if (changed) {
        impl_->engine.reset();
        impl_->chain_w = impl_->chain_h = 0;
    }
}

bool Howlaround::active(const json *fb) const {
    if (!fb || !fb->is_object()) return false;
    auto it = fb->find("enabled");
    return it == fb->end() || !it->is_boolean() || it->get<bool>();
}

std::shared_ptr<gpu::Texture> Howlaround::camera(const std::shared_ptr<gpu::Texture> &scene, const json &feedback,
                                                 const FrameContext &ctx) {
    Impl &m = *impl_;
    howl::Feedback fb = howl::parse_feedback(feedback);
    const howl::Settings &s = fb.settings;
    // Zero zoom without the counter: the camera sees the room only — exactly the scene.
    if (!fb.enabled || (!s.tv_in_view() && !s.counter())) return scene;

    const auto &sd = scene->desc();
    m.ops.ctx = &ctx;
    m.ops.hooks = &m.hooks;
    m.ops.scene_w = sd.width;
    m.ops.scene_h = sd.height;
    m.ops.scene_format = sd.format;

    if (m.chain_for_w != sd.width || m.chain_for_h != sd.height || m.chain_w <= 0) {
        m.chain_for_w = sd.width;
        m.chain_for_h = sd.height;
        m.chain_w = sd.width;
        m.chain_h = sd.height;
        if (m.ops.has_hooks()) {
            // One probe pass for the chain input size (the run-up plan needs it).
            HowlPass p = howl::Engine<GpuOps>::normal_pass(ctx.frame, ctx.fps);
            if (auto c = m.hooks.chain_input(scene, p)) {
                m.chain_w = c->desc().width;
                m.chain_h = c->desc().height;
            }
        }
    }

    int64_t window = howl::kDefaultWindow;
    if (auto it = feedback.find("window"); it != feedback.end() && it->is_number_integer())
        window = it->get<int64_t>();
    std::string key = feedback.dump() + "|" + std::to_string(ctx.fps) + "|" + std::to_string(sd.width) + "x" +
                      std::to_string(sd.height) + "|" + std::to_string(m.chain_w) + "x" + std::to_string(m.chain_h) +
                      "|" + std::to_string(int(sd.format)) + "|" + std::to_string(window);
    if (!m.engine || key != m.key) {
        howl::EngineConfig cfg;
        cfg.settings = s;
        cfg.fps = ctx.fps > 0 ? ctx.fps : 30.0;
        cfg.length = fb.length;
        cfg.aspect = double(sd.width) / double(std::max(1, sd.height));
        cfg.chain_w = m.chain_w;
        cfg.chain_h = m.chain_h;
        cfg.window = window;
        m.engine = std::make_unique<howl::Engine<GpuOps>>(cfg);
        m.key = key;
    }
    m.sync_checkpoints();
    TexPtr cam = m.engine->camera(m.ops, ctx.frame, scene);
    m.sync_checkpoints();  // a temporal preset may first have run just now
    m.ops.ctx = nullptr;
    m.last_camera = cam;
    m.last_frame = ctx.frame;
    return cam;
}

void Howlaround::after_frame(const gpu::Texture &chain_input, const json &, const FrameContext &ctx) {
    Impl &m = *impl_;
    if (!m.engine || m.last_frame != ctx.frame || !m.engine->wants_tv(ctx.frame)) return;
    if (!m.engine->config().settings.tv_in_view()) {
        m.engine->store_tv(ctx.frame, nullptr);
        return;
    }
    m.ops.hooks = &m.hooks;
    if (m.ops.has_hooks()) {
        const auto &d = chain_input.desc();
        if (d.width != m.chain_w || d.height != m.chain_h) {
            // The chain size changed (look edited): rebuild on the next frame.
            m.chain_w = d.width;
            m.chain_h = d.height;
            m.engine.reset();
            return;
        }
        TexPtr tv = m.ops.render_tv(chain_input, m.engine->normal_pass(ctx.frame));
        m.sync_checkpoints();
        m.engine->store_tv(ctx.frame, tv);
    } else {
        // No chain hooks: the TV shows the camera's picture directly, the same
        // as every recomputed history frame (so results don't depend on order).
        m.engine->store_tv(ctx.frame, m.ops.tv_pass(m.last_camera, m.engine->normal_pass(ctx.frame)));
    }
}

void Howlaround::flush() {
    impl_->engine.reset();
    impl_->last_camera.reset();
    impl_->last_frame = -1;
    impl_->chain_w = impl_->chain_h = 0;
    impl_->ops.pool.clear();
}

}  // namespace ntscrt
