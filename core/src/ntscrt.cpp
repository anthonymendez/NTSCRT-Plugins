// ntscrt.cpp — C ABI façade. Wave 0: CPU stages wired, GPU stages wired but
// skipped when no gpu::Device is available (stub backend). Wave 2 finishes
// the Metal entry point, caching and Howlaround ordering.
#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "../stages/digital/fields.h"
#include "../stages/digital/parallel.h"
#include "../stages/stages.h"
#include "embedded_schema.h"
#include "pixel_convert.h"
#include "testing.h"

using namespace ntscrt;

namespace {

// Parsed look JSON by content: the host sends the same string for every frame
// of an unanimated look, and the same strings again for history frames.
struct LookStore {
    struct Entry {
        std::string text;
        std::shared_ptr<const json> j;
        uint64_t used = 0;
    };
    static constexpr size_t kMax = 64;
    std::unordered_map<size_t, Entry> m;
    uint64_t tick = 0;
    size_t parses = 0;

    // nullptr if `s` isn't valid JSON.
    std::shared_ptr<const json> get(std::string_view s) {
        const size_t h = std::hash<std::string_view>{}(s);
        auto it = m.find(h);
        if (it != m.end() && it->second.text == s) {
            it->second.used = ++tick;
            return it->second.j;
        }
        ++parses;
        json j = json::parse(s, nullptr, false);
        if (j.is_discarded()) return nullptr;
        auto p = std::make_shared<const json>(std::move(j));
        if (it == m.end() && m.size() >= kMax) {
            auto lru = std::min_element(m.begin(), m.end(),
                                        [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
            m.erase(lru);
        }
        m[h] = Entry{std::string(s), p, ++tick};
        return p;
    }
};

// Converted + cropped source frames, keyed by everything that shapes them (so
// a proxy switch or an alpha / crop edit never reuses a wrong picture), LRU
// within a byte budget.
struct SourceKey {
    int64_t frame;
    double scale;
    int width, height;  // the host picture's size, before the crop
    int mode, premult;
    size_t crop;  // hash of that frame's look["crop"]
    bool operator<(const SourceKey &o) const {
        return std::tie(frame, scale, width, height, mode, premult, crop) <
               std::tie(o.frame, o.scale, o.width, o.height, o.mode, o.premult, o.crop);
    }
};

size_t default_source_budget() {
    if (const char *e = std::getenv("NTSCRT_SOURCE_CACHE_MB"); e && *e) {
        char *end = nullptr;
        double mb = std::strtod(e, &end);
        if (end != e && mb >= 0) return size_t(mb * 1024 * 1024);
    }
    return size_t(512) << 20;  // ~60 frames at 1080p, ~15 at UHD
}

struct SourceCache {
    static constexpr size_t kMaxFrames = 65;  // the furthest any stage looks back, plus this frame
    static constexpr size_t kMinFrames = 8;   // kept even past the budget (huge frames)
    struct Entry {
        std::shared_ptr<const ImageBGRA8> img;
        uint64_t used = 0;
    };
    std::map<SourceKey, Entry> m;
    size_t bytes = 0, budget = default_source_budget();
    uint64_t tick = 0;

    std::shared_ptr<const ImageBGRA8> get(const SourceKey &k) {
        auto it = m.find(k);
        if (it == m.end()) return nullptr;
        it->second.used = ++tick;
        return it->second.img;
    }
    void put(const SourceKey &k, std::shared_ptr<const ImageBGRA8> img) {
        Entry &e = m[k];
        if (e.img) bytes -= e.img->px.size();
        bytes += img->px.size();
        e.img = std::move(img);
        e.used = ++tick;
        evict();
    }
    void evict() {
        while (m.size() > kMaxFrames || (bytes > budget && m.size() > kMinFrames)) {
            auto lru = std::min_element(m.begin(), m.end(),
                                        [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
            bytes -= lru->second.img->px.size();
            m.erase(lru);
        }
    }
    void clear() {
        m.clear();
        bytes = 0;
    }
};

size_t crop_hash(const json &look) {
    auto c = look.find("crop");
    return c == look.end() ? 0 : std::hash<std::string>{}(c->dump());
}

}  // namespace

struct ntscrt_instance {
    std::string resource_dir;
    ntscrt_gpu_backend backend = NTSCRT_GPU_AUTO;
    void *native_device = nullptr, *native_queue = nullptr;  // the host's, when shared
    // Our own device is the process-wide shared one (gpu::Device::shared):
    // kernels compile once per process, and one D3D11 device / Metal queue
    // serves every instance. A host device (native_device) is this instance's.
    std::shared_ptr<gpu::Device> dev;
    DigitalStage digital;
    NtscStage ntsc;
    std::unique_ptr<Downscaler> downscale;
    std::unique_ptr<GlitchStage> glitch;
    std::unique_ptr<CrtStage> crt;
    std::unique_ptr<CrtStage> howl_crt;  // Screen Loop's own CRT chain (upstream HowlaroundFeedback)
    std::unique_ptr<Howlaround> howl;
    std::vector<std::shared_ptr<gpu::Texture>> tex_pool;  // upload / output textures, reused across frames
    std::string last_error;
    std::string gpu_gone;  // set when the GPU was lost and couldn't be replaced: CPU stages from then on
    int device_resets = 0;  // devices recreated since the last good GPU frame
    bool test_lose_device = false, test_recreate_fails = false;
    std::string presets_cache;
    SourceCache sources;
    LookStore looks;
};

namespace {
ntscrt_status fail(ntscrt_instance *i, ntscrt_status s, std::string msg) {
    if (i) i->last_error = std::move(msg);
    return s;
}
gpu::Backend pick_backend(ntscrt_gpu_backend b) {
    if (b == NTSCRT_GPU_D3D11) return gpu::Backend::D3D11;
    if (b == NTSCRT_GPU_METAL) return gpu::Backend::Metal;
#ifdef _WIN32
    return gpu::Backend::D3D11;
#else
    return gpu::Backend::Metal;
#endif
}

void drop_gpu(ntscrt_instance *inst) {
    // The stages and pooled textures belong to the device: they go first.
    inst->tex_pool.clear();
    inst->howl.reset();
    inst->howl_crt.reset();
    inst->crt.reset();
    inst->glitch.reset();
    inst->downscale.reset();
    inst->dev.reset();
}

// Creates the device and the GPU stages; false (and why) when there's no GPU.
bool init_gpu(ntscrt_instance *inst, std::string &err) {
    try {
        const gpu::Backend b = pick_backend(inst->backend);
        if (inst->native_device) inst->dev = gpu::Device::create(b, inst->native_device, inst->native_queue);
        else inst->dev = gpu::Device::shared(b);
    } catch (const std::exception &e) {
        err = e.what();
    }
    if (!inst->dev) {
        if (err.empty()) err = std::string("GPU unavailable: ") + gpu::Device::last_create_error();
        return false;
    }
    inst->downscale = std::make_unique<Downscaler>(*inst->dev);
    inst->glitch = std::make_unique<GlitchStage>(*inst->dev);
    inst->crt = std::make_unique<CrtStage>(*inst->dev, inst->resource_dir);
    inst->howl_crt = std::make_unique<CrtStage>(*inst->dev, inst->resource_dir);
    inst->howl = std::make_unique<Howlaround>(*inst->dev);
    return true;
}

std::atomic<int> &live_instances() {
    static std::atomic<int> n{0};
    return n;
}

bool device_lost(const ntscrt_instance *inst) {
    return inst->dev && (inst->test_lose_device || inst->dev->lost());
}

// After a device loss: one new device of our own per loss, at most 3 in a row
// without a good frame in between. The host's device can't be replaced here.
bool recreate_gpu(ntscrt_instance *inst, std::string &err) {
    if (inst->native_device) {
        err = "the host's GPU device was lost";
        return false;
    }
    if (inst->device_resets >= 3) {
        err = "the GPU device keeps getting lost";
        return false;
    }
    ++inst->device_resets;
    drop_gpu(inst);
    inst->test_lose_device = false;
    if (inst->test_recreate_fails) {
        err = "simulated: no new device";
        return false;
    }
    return init_gpu(inst, err);
}
}  // namespace

namespace ntscrt::testing {
Stats stats(ntscrt_instance *inst) {
    Stats s;
    if (!inst) return s;
    s.source_frames = inst->sources.m.size();
    s.source_bytes = inst->sources.bytes;
    s.source_budget = inst->sources.budget;
    s.look_parses = inst->looks.parses;
    s.device_resets = inst->device_resets;
    s.has_gpu = inst->dev != nullptr;
    s.gpu_device = inst->dev ? inst->dev->native_device() : nullptr;
    return s;
}
void set_source_budget(ntscrt_instance *inst, size_t bytes) {
    inst->sources.budget = bytes;
    inst->sources.evict();
}
void simulate_device_lost(ntscrt_instance *inst, bool recreate_fails) {
    inst->test_lose_device = true;
    inst->test_recreate_fails = recreate_fails;
}
}  // namespace ntscrt::testing

extern "C" {

uint32_t ntscrt_abi_version(void) { return NTSCRT_ABI_VERSION; }

const char *ntscrt_param_schema_json(void) { return ntscrt_embedded_schema(); }

ntscrt_status ntscrt_create(const ntscrt_create_info *info, ntscrt_instance **out) {
    if (!info || !out || info->abi_version != NTSCRT_ABI_VERSION) return NTSCRT_ERR_INVALID_ARG;
    auto inst = std::make_unique<ntscrt_instance>();
    inst->resource_dir = info->resource_dir ? info->resource_dir : "";
    inst->backend = info->backend;
    inst->native_device = info->native_device;
    inst->native_queue = info->native_queue;
    std::string err;
    if (!init_gpu(inst.get(), err)) inst->last_error = err;
    *out = inst.release();
    live_instances().fetch_add(1);
    return NTSCRT_OK;
}

void ntscrt_destroy(ntscrt_instance *inst) {
    if (!inst) return;
    delete inst;  // drops its reference to the shared device (freed with the last one)
    // The last instance: join the Digital stage's worker threads now, from
    // ordinary code, rather than ever in a static destructor (DLL unload).
    if (live_instances().fetch_sub(1) == 1) digital::shutdown_threads();
}

const char *ntscrt_last_error(ntscrt_instance *inst) { return inst ? inst->last_error.c_str() : ""; }

void ntscrt_flush_caches_mask(ntscrt_instance *inst, uint32_t which) {
    if (!inst) return;
    if (which & NTSCRT_CACHE_SOURCE) inst->sources.clear();
    if (which & NTSCRT_CACHE_DIGITAL) inst->digital.flush();
    if ((which & NTSCRT_CACHE_GLITCH) && inst->glitch) inst->glitch->flush();
    if ((which & NTSCRT_CACHE_CRT) && inst->crt) inst->crt->flush();
    if (which & NTSCRT_CACHE_SCREENLOOP) {
        if (inst->howl_crt) inst->howl_crt->flush();
        if (inst->howl) inst->howl->flush();
    }
}

void ntscrt_flush_caches(ntscrt_instance *inst) { ntscrt_flush_caches_mask(inst, NTSCRT_CACHE_ALL); }

const char *ntscrt_bundled_presets_json(ntscrt_instance *inst) {
    if (!inst) return "[]";
    if (!inst->presets_cache.empty()) return inst->presets_cache.c_str();
    json arr = json::array();
    namespace fs = std::filesystem;
    fs::path dir = fs::u8path(inst->resource_dir) / "presets";
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        std::ifstream f(it->path());
        json j = json::parse(f, nullptr, false);
        if (j.is_discarded()) continue;
        auto rel = fs::relative(it->path(), dir);
        rel.replace_extension();
        bool fb = j.is_object() && j.value("kind", std::string()) == "ntscrt-video-feedback";
        auto u8 = rel.generic_u8string();  // std::u8string in C++20; nlohmann would emit it as a number array
        std::string name(reinterpret_cast<const char *>(u8.data()), u8.size());
        arr.push_back({{"name", name}, {"kind", fb ? "feedback" : "look"}, {"json", j}});
    }
    std::sort(arr.begin(), arr.end(), [](const json &a, const json &b) { return a["name"] < b["name"]; });
    inst->presets_cache = arr.dump();
    return inst->presets_cache.c_str();
}

}  // extern "C"

namespace {
// Shared frame renderer behind ntscrt_render_cpu and ntscrt_render_metal.
// get_src: fill `raw` (BGRA8, opaque, prepared for `mode`: to_bgra8) and
//          `alpha` (source alpha per pixel, empty = opaque).
// emit:    deliver the result: `out_gpu` (BGRA8, out_w x out_h) when a GPU
//          device exists, else `cpu_img` (CPU stages only, out_w x out_h).
//          `alpha` is NULL when it doesn't line up with the result (crop on).
//          raw_w x raw_h: the source picture's size.
using SourceFn = std::function<void(ImageBGRA8 &raw, std::vector<float> &alpha, AlphaMode mode)>;
using EmitFn = std::function<void(const std::shared_ptr<gpu::Texture> &out_gpu, const ImageBGRA8 &cpu_img,
                                  int raw_w, int raw_h, const std::vector<float> *alpha, AlphaMode mode)>;

// Looks at other frames during one render call. Entries are dropped oldest
// frame first past kMax, so a reference from look_at stays valid for at least
// kMax further lookups (the stages only read it on the spot).
constexpr size_t kLookHistMax = 256;

ntscrt_status render_once(ntscrt_instance *inst, const ntscrt_render_args *args,
                          const std::shared_ptr<const json> &look_p, const json &feedback, int out_w, int out_h,
                          const SourceFn &get_src, const EmitFn &emit) {
    const json &look = *look_p;
    const std::string_view look_text = args->look_json ? args->look_json : "{}";
    const AlphaMode mode = alpha_mode(look);
    const bool premult = args->source_premultiplied != 0;

    std::map<int64_t, std::shared_ptr<const json>> look_hist;
    FrameContext ctx;
    ctx.frame = args->frame_index;
    ctx.fps = args->fps > 0 ? args->fps : 30.0;
    ctx.render_scale = args->render_scale > 0 ? args->render_scale : 1.0;
    ctx.seed = args->seed;
    ctx.look = &look;
    // NULL from the host, or the current look's own text: this frame's look,
    // nothing parsed. Other strings are parsed once per distinct content.
    auto look_ptr = [&](int64_t f) -> std::shared_ptr<const json> {
        if (f == args->frame_index || !args->look_at) return look_p;
        auto it = look_hist.find(f);
        if (it != look_hist.end()) return it->second;
        const char *s = args->look_at(args->user, f);
        std::shared_ptr<const json> j = look_p;
        if (s && look_text != s) {
            j = inst->looks.get(s);
            if (!j) j = look_p;
        }
        if (look_hist.size() >= kLookHistMax) look_hist.erase(look_hist.begin());
        return look_hist.emplace(f, std::move(j)).first->second;
    };
    ctx.look_at = [&](int64_t f) -> const json & { return *look_ptr(f); };

    int raw_w = 0, raw_h = 0;  // this frame's source size (host frames of a clip share it)
    ctx.source_at = [&](int64_t f) -> std::shared_ptr<const ImageBGRA8> {
        std::shared_ptr<const json> lf = look_ptr(f);
        const SourceKey key{f, ctx.render_scale, raw_w, raw_h, int(mode), int(premult), crop_hash(*lf)};
        if (auto p = inst->sources.get(key)) return p;
        if (!args->fetch_source) return nullptr;
        const ntscrt_image *im = args->fetch_source(args->user, f);
        if (!im) return nullptr;
        ImageBGRA8 raw, cropped;
        to_bgra8(*im, raw, nullptr, mode, premult);
        if (args->release_source) args->release_source(args->user, im);
        Crop::apply(*lf, std::move(raw), cropped);
        auto p = std::make_shared<const ImageBGRA8>(std::move(cropped));
        inst->sources.put(key, p);
        return p;
    };

    std::vector<float> alpha;
    ImageBGRA8 img;
    {
        ImageBGRA8 raw;
        get_src(raw, alpha, mode);
        raw_w = raw.width;
        raw_h = raw.height;
        Crop::apply(look, std::move(raw), img);  // no crop: moved, not copied
    }
    const std::vector<float> *aligned_alpha = img.width == raw_w && img.height == raw_h ? &alpha : nullptr;

    if (!inst->dev) {
        // No GPU: CPU stages only (degraded, but never a black frame).
        if (inst->digital.active(look)) inst->digital.process(img, ctx);
        if (inst->ntsc.active(look)) inst->ntsc.process(img, ctx);
        if (img.width != out_w || img.height != out_h) {
            // Cropped: placed like the CRT bypass would, so all of dst is written.
            ImageBGRA8 boxed;
            letterbox(img, out_w, out_h, boxed);
            emit(nullptr, boxed, raw_w, raw_h, nullptr, mode);
        } else {
            emit(nullptr, img, raw_w, raw_h, aligned_alpha, mode);
        }
        if (!inst->gpu_gone.empty()) return fail(inst, NTSCRT_ERR_GPU, inst->gpu_gone);
        return NTSCRT_OK;
    }
    if (inst->test_lose_device) throw std::runtime_error("simulated device removal");
    gpu::Device &dev = *inst->dev;
    const bool howl_on = inst->howl->active(args->feedback_json ? &feedback : nullptr);

    // Textures reused across frames once nothing holds them (GPU work runs in
    // submission order on both backends, so overwriting a released one is safe).
    auto acquire = [&](const gpu::TextureDesc &d) {
        for (auto &t : inst->tex_pool) {
            const gpu::TextureDesc &e = t->desc();
            if (t.use_count() == 1 && e.width == d.width && e.height == d.height && e.format == d.format &&
                e.shader_write == d.shader_write && e.render_target == d.render_target)
                return t;
        }
        auto t = dev.create_texture(d);
        if (inst->tex_pool.size() >= 8) inst->tex_pool.erase(inst->tex_pool.begin());
        inst->tex_pool.push_back(t);
        return t;
    };

    // Upstream order (Mp4Exporter.exportStill / video path):
    //   no Screen Loop:  crop -> Digital -> NTSC -> upload -> Downscale -> Glitch -> CRT
    //   Screen Loop on:  crop -> upload -> camera -> download -> NTSC -> upload -> Downscale
    //                    -> Glitch -> CRT; Digital is OFF whenever Screen Loop is on.
    auto upload = [&](const ImageBGRA8 &im) {
        auto t = acquire({im.width, im.height, gpu::Format::BGRA8, false, false});  // only ever read
        dev.upload(*t, im.px.data(), ptrdiff_t(im.width) * 4);
        return t;
    };
    auto download = [&](const gpu::Texture &t) {
        ImageBGRA8 im;
        im.resize(t.desc().width, t.desc().height);
        dev.download(t, im.px.data(), ptrdiff_t(im.width) * 4);  // waits for the work before it
        return im;
    };

    std::shared_ptr<gpu::Texture> chain;
    if (howl_on) {
        // Context whose stage counters (internal.h helpers) yield the pass's
        // counters; `pin` keeps the pass's look alive while the context is used.
        auto at = [&](int64_t counter_frame, const std::shared_ptr<const json> &pin) {
            FrameContext c = ctx;
            c.frame = counter_frame;
            c.look = pin.get();
            return c;
        };
        HowlHooks h;
        h.chain_input = [&](const std::shared_ptr<gpu::Texture> &cam, const HowlPass &p) {
            ImageBGRA8 im = download(*cam);
            const auto pin = look_ptr(p.frame);
            FrameContext c = at(p.frame, pin);
            FrameContext cn = at(p.ntsc_frame - 1, pin);  // ntsc_frame(cn) == p.ntsc_frame
            if (inst->ntsc.active(*c.look)) inst->ntsc.process(im, cn);
            auto ch = inst->downscale->run(upload(im), c);
            FrameContext cg = c;
            cg.frame = int64_t(std::llround(p.glitch_time * ctx.fps));  // glitch_time(cg)
            if (inst->glitch->active(*c.look)) ch = inst->glitch->run(ch, cg);
            return ch;
        };
        h.render_tv = [&](const gpu::Texture &ch, gpu::Texture &tv, const HowlPass &p) {
            const auto pin = look_ptr(p.frame);
            inst->howl_crt->render(ch, tv, at(p.shader_frame - 1, pin));  // shader_frame == p.shader_frame
        };
        inst->howl->set_hooks(std::move(h));  // lambdas capture this call's locals: re-set every render

        auto cam = inst->howl->camera(upload(img), feedback, ctx);
        ImageBGRA8 im = download(*cam);
        if (inst->ntsc.active(look)) inst->ntsc.process(im, ctx);
        chain = inst->downscale->run(upload(im), ctx);
        if (inst->glitch->active(look)) chain = inst->glitch->run(chain, ctx);
        inst->howl->after_frame(*chain, feedback, ctx);  // feeds the loop the chain input
    } else {
        if (inst->digital.active(look)) inst->digital.process(img, ctx);
        ctx.line_height = inst->digital.active(look) ? digital::chain_line_height(look) : 1;  // One field
        if (inst->ntsc.active(look)) inst->ntsc.process(img, ctx);
        chain = inst->downscale->run(upload(img), ctx);
        if (inst->glitch->active(look)) chain = inst->glitch->run(chain, ctx);
    }
    auto out = acquire({out_w, out_h, gpu::Format::BGRA8, true, true});
    inst->crt->render(*chain, *out, ctx);
    chain.reset();
    // No finish() here: emit's download (or the Metal path's own finish) waits.
    emit(out, img, raw_w, raw_h, aligned_alpha, mode);
    inst->device_resets = 0;  // a good GPU frame

    // The CRT shader couldn't run: the frame shows the picture without it.
    const CrtStage *bad = inst->crt->fallback_status() ? inst->crt.get()
                          : howl_on && inst->howl_crt->fallback_status() ? inst->howl_crt.get()
                                                                         : nullptr;
    if (bad) return fail(inst, ntscrt_status(bad->fallback_status()), bad->fallback_reason());
    return NTSCRT_OK;
}

ntscrt_status render_impl(ntscrt_instance *inst, const ntscrt_render_args *args, int out_w, int out_h,
                          const SourceFn &get_src, const EmitFn &emit) {
    inst->last_error.clear();
    std::shared_ptr<const json> look = inst->looks.get(args->look_json ? args->look_json : "{}");
    if (!look) return fail(inst, NTSCRT_ERR_BAD_PARAMS, "look_json is not valid JSON");
    json feedback;
    if (args->feedback_json) {
        feedback = json::parse(args->feedback_json, nullptr, false);
        if (feedback.is_discarded()) return fail(inst, NTSCRT_ERR_BAD_PARAMS, "feedback_json is not valid JSON");
    }
    bool recreated = false;
    for (;;) {
        try {
            return render_once(inst, args, look, feedback, out_w, out_h, get_src, emit);
        } catch (const std::exception &e) {
            if (!device_lost(inst)) return fail(inst, NTSCRT_ERR_INTERNAL, e.what());
            // The device is gone (driver reset, GPU removed): try a new one once,
            // else carry on with the CPU stages. Either way, render this frame again.
            const std::string why = e.what();
            std::string err;
            if (!recreated && recreate_gpu(inst, err)) {
                recreated = true;
                continue;
            }
            drop_gpu(inst);
            inst->gpu_gone = "GPU device lost (" + why + (err.empty() ? "" : "; " + err) +
                             "): rendering without the GPU stages (downscale, glitch, CRT)";
        }
    }
}

}  // namespace

extern "C" {

ntscrt_status ntscrt_render_cpu(ntscrt_instance *inst, const ntscrt_render_args *args, const ntscrt_image *src,
                                ntscrt_image *dst) {
    if (!inst || !args || !src || !dst || !src->data || !dst->data) return NTSCRT_ERR_INVALID_ARG;
    const bool premult = args->source_premultiplied != 0;
    return render_impl(
        inst, args, dst->width, dst->height,
        [&](ImageBGRA8 &raw, std::vector<float> &alpha, AlphaMode mode) {
            to_bgra8(*src, raw, &alpha, mode, premult);
        },
        [&](const std::shared_ptr<gpu::Texture> &out, const ImageBGRA8 &cpu_img, int raw_w, int raw_h,
            const std::vector<float> *alpha, AlphaMode mode) {
            if (!out) {
                from_bgra8(cpu_img, alpha, *dst, mode, premult);
                return;
            }
            ImageBGRA8 result;
            result.width = dst->width;
            result.height = dst->height;
            result.px.resize(size_t(dst->width) * dst->height * 4);  // fully overwritten by the download
            inst->dev->download(*out, result.px.data(), ptrdiff_t(dst->width) * 4);
            bool same = result.width == raw_w && result.height == raw_h;
            from_bgra8(result, same ? alpha : nullptr, *dst, mode, premult);
        });
}

// Metal path (FxPlug): host textures in and out, on the device given at create.
// The CPU stages (Digital, ntsc-rs) still need the frame on the CPU, so the
// source is read back once; everything after runs on the GPU, and the result
// is written into the host texture by a kernel (or a blit when the host
// texture isn't shader-writable) encoded on the host's command buffer.
ntscrt_status ntscrt_render_metal(ntscrt_instance *inst, const ntscrt_render_args *args, void *src_texture,
                                  void *dst_texture, void *command_buffer) {
    if (!inst || !args || !src_texture || !dst_texture) return NTSCRT_ERR_INVALID_ARG;
    if (!inst->dev || inst->dev->backend() != gpu::Backend::Metal)
        return fail(inst, NTSCRT_ERR_GPU, "ntscrt_render_metal: instance has no Metal device");
    gpu::Device &dev = *inst->dev;
    std::shared_ptr<gpu::Texture> src, dst;
    try {
        src = dev.wrap_native_texture(src_texture);
        dst = dev.wrap_native_texture(dst_texture);
    } catch (const std::exception &e) {
        return fail(inst, NTSCRT_ERR_INVALID_ARG, e.what());
    }
    const int sw = src->desc().width, sh = src->desc().height;
    const int dw = dst->desc().width, dh = dst->desc().height;

    const bool premult = args->source_premultiplied != 0;
    auto get_src = [&](ImageBGRA8 &raw, std::vector<float> &alpha, AlphaMode mode) {
        // Normalise any host format to RGBA32F on the GPU, read back once.
        auto f32 = dev.create_texture({sw, sh, gpu::Format::RGBA32F, true, false});
        dev.copy(*src, *f32);
        std::vector<float> px(size_t(sw) * sh * 4);
        dev.download(*f32, px.data(), ptrdiff_t(sw) * 16);
        ntscrt_image im{sw, sh, ptrdiff_t(sw) * 16, NTSCRT_RGBA32F, px.data()};
        to_bgra8(im, raw, &alpha, mode, premult);
    };
    auto emit = [&](const std::shared_ptr<gpu::Texture> &out, const ImageBGRA8 &, int raw_w, int,
                    const std::vector<float> *alpha, AlphaMode mode) {
        // render_impl only calls emit with a texture here (the device exists).
        // Layout matches facade.slang MergeParams.
        struct { uint32_t w, h, use_alpha, mode, premult, pad[3]; } p{
            uint32_t(dw), uint32_t(dh), uint32_t(alpha && sw == dw && sh == dh && raw_w == sw),
            uint32_t(mode == AlphaMode::Spill), uint32_t(premult), {0, 0, 0}};
        auto merge = [&](gpu::Texture &target) {
            gpu::Bindings b;
            b.constants = &p;
            b.constants_size = sizeof p;
            b.textures = {out.get(), p.use_alpha ? src.get() : out.get()};
            b.out_textures = {&target};
            dev.dispatch(dev.kernel("facade/alpha_merge"), b, dw, dh, 1);
        };
        std::shared_ptr<gpu::Texture> staged;
        if (!dst->desc().shader_write) {
            // Merge into an internal texture of the destination's format, then blit.
            staged = dev.create_texture({dw, dh, dst->desc().format, true, false});
            merge(*staged);
        }
        dev.finish();  // our work completes before the host's buffer runs (it may be another queue)
        if (command_buffer) dev.set_external_command_buffer(command_buffer);
        try {
            if (staged) dev.copy(*staged, *dst);  // same format: blit
            else merge(*dst);
        } catch (...) {
            if (command_buffer) dev.set_external_command_buffer(nullptr);
            throw;
        }
        if (command_buffer) dev.set_external_command_buffer(nullptr);
        else dev.finish();
    };
    return render_impl(inst, args, dw, dh, get_src, emit);
}

}  // extern "C"
