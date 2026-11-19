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
    // One loop per scene size: a host renders the same effect at another size in between
    // (Resolve's viewer and thumbnails at a reduced render scale while the render queue
    // renders full size). Each keeps its own history, so such a render doesn't throw away
    // the other's, which costs a re-run of up to 479 full-size source frames.
    struct Slot {
        int w = 0, h = 0, format = 0;  // the scene this loop is for
        std::unique_ptr<howl::Engine<GpuOps>> engine;
        std::string key;
        // Loops of the settings shown before (parked: Engine::park), newest last. Trying
        // Screen Loop presets in turn and going back to one continues its history instead of
        // re-running up to 479 source frames. Each is keyed by everything its pictures depend
        // on that the key holds (the feedback JSON, fps, sizes, window); the look is not in the
        // key: whoever changes the look flushes (ntscrt_flush_caches_mask), as before.
        struct Parked {
            std::unique_ptr<howl::Engine<GpuOps>> engine;
            std::string key;
            uint64_t used = 0;
        };
        std::vector<Parked> parked;
        // Chain input size learned for this scene size (from the main path, or one probe).
        int chain_w = 0, chain_h = 0;
        uint64_t used = 0;
    };
    static constexpr size_t kSlots = 3;  // Resolve: full size and two or three viewer / thumbnail scales
    static constexpr size_t kParkedPerSlot = 4;
    std::vector<std::unique_ptr<Slot>> slots;
    Slot *cur = nullptr;  // the loop of the last camera() call
    uint64_t tick = 0;
    TexPtr last_camera;
    int64_t last_frame = -1;
    std::string last_feedback;  // the feedback JSON of the last camera() call
    size_t parked_budget = default_parked_budget();

    // NTSCRT_SCREENLOOP_KEEP_MB: memory the parked loops may hold in all (0 = none kept), default 256 MB.
    static size_t default_parked_budget() {
        if (const char *e = std::getenv("NTSCRT_SCREENLOOP_KEEP_MB"); e && *e) {
            char *end = nullptr;
            double mb = std::strtod(e, &end);
            if (end != e && mb >= 0) return size_t(mb * 1024 * 1024);
        }
        return size_t(256) << 20;
    }

    // The slot's loop for `key`: the current one, a parked one taken back, or none (made by the caller).
    void select(Slot &sl, const std::string &key) {
        if (sl.engine && sl.key == key) return;
        if (sl.engine) {
            sl.engine->park();
            sl.parked.push_back({std::move(sl.engine), sl.key, ++tick});
        }
        sl.engine.reset();
        sl.key.clear();
        for (auto it = sl.parked.begin(); it != sl.parked.end(); ++it)
            if (it->key == key) {
                sl.engine = std::move(it->engine);
                sl.key = key;
                sl.parked.erase(it);
                break;
            }
        trim_parked();
    }
    // At most kParkedPerSlot per slot and parked_budget bytes in all: the least recently parked go.
    void trim_parked() {
        for (;;) {
            size_t bytes = 0;
            Slot *worst = nullptr;
            size_t worst_i = 0;
            bool over_count = false;
            for (auto &s : slots) {
                for (size_t i = 0; i < s->parked.size(); ++i) {
                    bytes += s->parked[i].engine->held_bytes();
                    if (!worst || s->parked[i].used < worst->parked[worst_i].used) worst = s.get(), worst_i = i;
                }
                if (s->parked.size() > kParkedPerSlot) over_count = true;
            }
            if (!worst) return;
            if (over_count) {
                for (auto &s : slots)
                    if (s->parked.size() > kParkedPerSlot) {
                        s->parked.erase(s->parked.begin());  // the oldest of that slot
                        break;
                    }
                continue;
            }
            if (bytes <= parked_budget) return;
            worst->parked.erase(worst->parked.begin() + ptrdiff_t(worst_i));
        }
    }

    explicit Impl(gpu::Device &d) : dev(d), ops(d) {}

    Slot &slot_for(const gpu::TextureDesc &sd) {
        for (auto &s : slots)
            if (s->w == sd.width && s->h == sd.height && s->format == int(sd.format)) return *s;
        if (slots.size() >= kSlots) {
            // The least recently used goes, but never the biggest for a smaller one: the
            // full-size history is the expensive one (a re-run fetches full-size frames).
            auto big = slots.begin();
            for (auto i = slots.begin(); i != slots.end(); ++i)
                if (int64_t((*i)->w) * (*i)->h > int64_t((*big)->w) * (*big)->h) big = i;
            const bool keep_big = int64_t(sd.width) * sd.height < int64_t((*big)->w) * (*big)->h;
            auto lru = slots.end();
            for (auto i = slots.begin(); i != slots.end(); ++i)
                if (!(keep_big && i == big) && (lru == slots.end() || (*i)->used < (*lru)->used)) lru = i;
            if (cur == lru->get()) cur = nullptr;
            slots.erase(lru);
        }
        auto s = std::make_unique<Slot>();
        s->w = sd.width;
        s->h = sd.height;
        s->format = int(sd.format);
        slots.push_back(std::move(s));
        return *slots.back();
    }

    // Checkpoints only while the TV pass keeps no state of its own.
    bool tv_keeps_state() const { return hooks.tv_keeps_state && hooks.tv_keeps_state(); }
    void sync_checkpoints() {
        const bool on = !tv_keeps_state();
        for (auto &s : slots)
            if (s->engine) s->engine->set_checkpoints(on);
    }
    void clear() {
        slots.clear();
        cur = nullptr;
        last_camera.reset();
        last_frame = -1;
        ops.pool.clear();
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
        impl_->slots.clear();
        impl_->cur = nullptr;
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

    // A change of the Screen Loop settings while the loop's CRT keeps state of its own (a temporal
    // shader preset): no loop can be kept or taken back (each depends on what that CRT drew before),
    // so the loop and its CRT start over, as a flush does (hooks.tv_reset).
    std::string fb_text = feedback.dump();
    if (fb_text != m.last_feedback) {
        if (!m.last_feedback.empty() && m.tv_keeps_state()) {
            if (m.hooks.tv_reset) m.hooks.tv_reset();
            m.clear();
        }
        m.last_feedback = std::move(fb_text);
    }

    const auto &sd = scene->desc();
    m.ops.ctx = &ctx;
    m.ops.hooks = &m.hooks;
    m.ops.scene_w = sd.width;
    m.ops.scene_h = sd.height;
    m.ops.scene_format = sd.format;

    Impl::Slot &sl = m.slot_for(sd);
    sl.used = ++m.tick;
    m.cur = &sl;
    if (sl.chain_w <= 0) {
        sl.chain_w = sd.width;
        sl.chain_h = sd.height;
        if (m.ops.has_hooks()) {
            // One probe pass for the chain input size (the run-up plan needs it).
            HowlPass p = howl::Engine<GpuOps>::normal_pass(ctx.frame, ctx.fps);
            if (auto c = m.hooks.chain_input(scene, p)) {
                sl.chain_w = c->desc().width;
                sl.chain_h = c->desc().height;
            }
        }
    }

    int64_t window = howl::kDefaultWindow;
    if (auto it = feedback.find("window"); it != feedback.end() && it->is_number_integer())
        window = it->get<int64_t>();
    std::string key = m.last_feedback + "|" + std::to_string(ctx.fps) + "|" + std::to_string(sd.width) + "x" +
                      std::to_string(sd.height) + "|" + std::to_string(sl.chain_w) + "x" + std::to_string(sl.chain_h) +
                      "|" + std::to_string(int(sd.format)) + "|" + std::to_string(window);
    m.select(sl, key);
    if (!sl.engine) {
        howl::EngineConfig cfg;
        cfg.settings = s;
        cfg.fps = ctx.fps > 0 ? ctx.fps : 30.0;
        cfg.length = fb.length;
        cfg.aspect = double(sd.width) / double(std::max(1, sd.height));
        cfg.chain_w = sl.chain_w;
        cfg.chain_h = sl.chain_h;
        cfg.window = window;
        sl.engine = std::make_unique<howl::Engine<GpuOps>>(cfg);
        sl.key = key;
    }
    m.sync_checkpoints();
    TexPtr cam = sl.engine->camera(m.ops, ctx.frame, scene);
    m.sync_checkpoints();  // a temporal preset may first have run just now
    m.ops.ctx = nullptr;
    m.last_camera = cam;
    m.last_frame = ctx.frame;
    return cam;
}

void Howlaround::after_frame(const gpu::Texture &chain_input, const json &, const FrameContext &ctx) {
    Impl &m = *impl_;
    if (!m.cur || !m.cur->engine || m.last_frame != ctx.frame) return;
    Impl::Slot &sl = *m.cur;
    auto &engine = *sl.engine;
    if (!engine.wants_tv(ctx.frame)) return;
    if (!engine.config().settings.tv_in_view()) {
        engine.store_tv(ctx.frame, nullptr);
        return;
    }
    m.ops.hooks = &m.hooks;
    if (m.ops.has_hooks()) {
        const auto &d = chain_input.desc();
        if (d.width != sl.chain_w || d.height != sl.chain_h) {
            // The chain size changed (look edited): rebuild on the next frame.
            sl.chain_w = d.width;
            sl.chain_h = d.height;
            sl.engine.reset();
            return;
        }
        TexPtr tv = m.ops.render_tv(chain_input, engine.normal_pass(ctx.frame));
        m.sync_checkpoints();
        engine.store_tv(ctx.frame, tv);
    } else {
        // No chain hooks: the TV shows the camera's picture directly, the same
        // as every recomputed history frame (so results don't depend on order).
        engine.store_tv(ctx.frame, m.ops.tv_pass(m.last_camera, engine.normal_pass(ctx.frame)));
    }
}

void Howlaround::flush() {
    impl_->clear();
    impl_->last_feedback.clear();
}

}  // namespace ntscrt
