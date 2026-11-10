// ntscrt.cpp — C ABI façade. Wave 0: CPU stages wired, GPU stages wired but
// skipped when no gpu::Device is available (stub backend). Wave 2 finishes
// the Metal entry point, caching and Howlaround ordering.
#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

#include "../stages/stages.h"
#include "embedded_schema.h"
#include "pixel_convert.h"

using namespace ntscrt;

struct ntscrt_instance {
    std::string resource_dir;
    std::unique_ptr<gpu::Device> dev;
    DigitalStage digital;
    NtscStage ntsc;
    std::unique_ptr<Downscaler> downscale;
    std::unique_ptr<GlitchStage> glitch;
    std::unique_ptr<CrtStage> crt;
    std::unique_ptr<CrtStage> howl_crt;  // Screen Loop's own CRT chain (upstream HowlaroundFeedback)
    std::unique_ptr<Howlaround> howl;
    std::string last_error;
    std::string presets_cache;
    std::map<int64_t, std::shared_ptr<const ImageBGRA8>> source_cache;  // cropped history
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
}  // namespace

extern "C" {

uint32_t ntscrt_abi_version(void) { return NTSCRT_ABI_VERSION; }

const char *ntscrt_param_schema_json(void) { return ntscrt_embedded_schema(); }

ntscrt_status ntscrt_create(const ntscrt_create_info *info, ntscrt_instance **out) {
    if (!info || !out || info->abi_version != NTSCRT_ABI_VERSION) return NTSCRT_ERR_INVALID_ARG;
    auto inst = std::make_unique<ntscrt_instance>();
    inst->resource_dir = info->resource_dir ? info->resource_dir : "";
    try {
        inst->dev = gpu::Device::create(pick_backend(info->backend), info->native_device, info->native_queue);
    } catch (const std::exception &e) {
        inst->last_error = e.what();
    }
    if (inst->dev) {
        inst->downscale = std::make_unique<Downscaler>(*inst->dev);
        inst->glitch = std::make_unique<GlitchStage>(*inst->dev);
        inst->crt = std::make_unique<CrtStage>(*inst->dev, inst->resource_dir);
        inst->howl_crt = std::make_unique<CrtStage>(*inst->dev, inst->resource_dir);
        inst->howl = std::make_unique<Howlaround>(*inst->dev);
    } else if (inst->last_error.empty()) {
        inst->last_error = std::string("GPU unavailable: ") + gpu::Device::last_create_error();
    }
    *out = inst.release();
    return NTSCRT_OK;
}

void ntscrt_destroy(ntscrt_instance *inst) { delete inst; }

const char *ntscrt_last_error(ntscrt_instance *inst) { return inst ? inst->last_error.c_str() : ""; }

void ntscrt_flush_caches(ntscrt_instance *inst) {
    if (!inst) return;
    inst->source_cache.clear();
    inst->digital.flush();
    if (inst->glitch) inst->glitch->flush();
    if (inst->crt) inst->crt->flush();
    if (inst->howl_crt) inst->howl_crt->flush();
    if (inst->howl) inst->howl->flush();
}

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
// get_src: fill `raw` (BGRA8, opaque) and `alpha` (source alpha per pixel).
// emit:    deliver the result: `out_gpu` (BGRA8, out_w x out_h) when a GPU
//          device exists, else `cpu_img` (CPU stages only, crop-sized).
using SourceFn = std::function<void(ImageBGRA8 &raw, std::vector<float> &alpha)>;
using EmitFn = std::function<void(const std::shared_ptr<gpu::Texture> &out_gpu, const ImageBGRA8 &cpu_img,
                                  const ImageBGRA8 &raw, const std::vector<float> &alpha)>;

ntscrt_status render_impl(ntscrt_instance *inst, const ntscrt_render_args *args, int out_w, int out_h,
                          const SourceFn &get_src, const EmitFn &emit) {
    json look = json::parse(args->look_json ? args->look_json : "{}", nullptr, false);
    if (look.is_discarded()) return fail(inst, NTSCRT_ERR_BAD_PARAMS, "look_json is not valid JSON");
    json feedback;
    if (args->feedback_json) {
        feedback = json::parse(args->feedback_json, nullptr, false);
        if (feedback.is_discarded()) return fail(inst, NTSCRT_ERR_BAD_PARAMS, "feedback_json is not valid JSON");
    }
    try {
        std::map<int64_t, json> look_hist;
        FrameContext ctx;
        ctx.frame = args->frame_index;
        ctx.fps = args->fps > 0 ? args->fps : 30.0;
        ctx.render_scale = args->render_scale > 0 ? args->render_scale : 1.0;
        ctx.seed = args->seed;
        ctx.look = &look;
        ctx.look_at = [&](int64_t f) -> const json & {
            if (f == args->frame_index || !args->look_at) return look;
            auto it = look_hist.find(f);
            if (it != look_hist.end()) return it->second;
            const char *s = args->look_at(args->user, f);
            json j = s ? json::parse(s, nullptr, false) : look;
            if (j.is_discarded()) j = look;
            return look_hist.emplace(f, std::move(j)).first->second;
        };
        ctx.source_at = [&](int64_t f) -> std::shared_ptr<const ImageBGRA8> {
            auto it = inst->source_cache.find(f);
            if (it != inst->source_cache.end()) return it->second;
            if (!args->fetch_source) return nullptr;
            const ntscrt_image *im = args->fetch_source(args->user, f);
            if (!im) return nullptr;
            ImageBGRA8 raw, cropped;
            to_bgra8(*im, raw, nullptr);
            if (args->release_source) args->release_source(args->user, im);
            Crop::apply(ctx.look_at(f), raw, cropped);
            auto p = std::make_shared<const ImageBGRA8>(std::move(cropped));
            if (inst->source_cache.size() > 64) inst->source_cache.erase(inst->source_cache.begin());
            inst->source_cache[f] = p;
            return p;
        };

        std::vector<float> alpha;
        ImageBGRA8 raw, img;
        get_src(raw, alpha);
        Crop::apply(look, raw, img);

        if (!inst->dev) {
            // No GPU: CPU stages only (degraded, but never a black frame).
            if (inst->digital.active(look)) inst->digital.process(img, ctx);
            if (inst->ntsc.active(look)) inst->ntsc.process(img, ctx);
            emit(nullptr, img, raw, alpha);
            return NTSCRT_OK;
        }
        gpu::Device &dev = *inst->dev;
        const bool howl_on = inst->howl->active(args->feedback_json ? &feedback : nullptr);

        // Upstream order (Mp4Exporter.exportStill / video path):
        //   no Screen Loop:  crop -> Digital -> NTSC -> upload -> Downscale -> Glitch -> CRT
        //   Screen Loop on:  crop -> upload -> camera -> download -> NTSC -> upload -> Downscale
        //                    -> Glitch -> CRT; Digital is OFF whenever Screen Loop is on.
        auto upload = [&](const ImageBGRA8 &im) {
            auto t = dev.create_texture({im.width, im.height, gpu::Format::BGRA8, true, false});
            dev.upload(*t, im.px.data(), ptrdiff_t(im.width) * 4);
            return t;
        };
        auto download = [&](const gpu::Texture &t) {
            ImageBGRA8 im;
            im.resize(t.desc().width, t.desc().height);
            dev.finish();
            dev.download(t, im.px.data(), ptrdiff_t(im.width) * 4);
            return im;
        };

        std::shared_ptr<gpu::Texture> chain;
        if (howl_on) {
            // Context whose stage counters (internal.h helpers) yield the pass's counters.
            auto at = [&](int64_t counter_frame, const HowlPass &p) {
                FrameContext c = ctx;
                c.frame = counter_frame;
                c.look = &ctx.look_at(p.frame);
                return c;
            };
            HowlHooks h;
            h.chain_input = [&](const std::shared_ptr<gpu::Texture> &cam, const HowlPass &p) {
                ImageBGRA8 im = download(*cam);
                FrameContext c = at(p.frame, p);
                FrameContext cn = at(p.ntsc_frame - 1, p);  // ntsc_frame(cn) == p.ntsc_frame
                if (inst->ntsc.active(*c.look)) inst->ntsc.process(im, cn);
                auto ch = inst->downscale->run(upload(im), c);
                FrameContext cg = c;
                cg.frame = int64_t(std::llround(p.glitch_time * ctx.fps));  // glitch_time(cg)
                if (inst->glitch->active(*c.look)) ch = inst->glitch->run(ch, cg);
                return ch;
            };
            h.render_tv = [&](const gpu::Texture &ch, gpu::Texture &tv, const HowlPass &p) {
                inst->howl_crt->render(ch, tv, at(p.shader_frame - 1, p));  // shader_frame == p.shader_frame
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
            if (inst->ntsc.active(look)) inst->ntsc.process(img, ctx);
            chain = inst->downscale->run(upload(img), ctx);
            if (inst->glitch->active(look)) chain = inst->glitch->run(chain, ctx);
        }
        auto out = dev.create_texture({out_w, out_h, gpu::Format::BGRA8, true, true});
        inst->crt->render(*chain, *out, ctx);
        dev.finish();
        emit(out, img, raw, alpha);
        return NTSCRT_OK;
    } catch (const std::exception &e) {
        return fail(inst, NTSCRT_ERR_INTERNAL, e.what());
    }
}

}  // namespace

extern "C" {

ntscrt_status ntscrt_render_cpu(ntscrt_instance *inst, const ntscrt_render_args *args, const ntscrt_image *src,
                                ntscrt_image *dst) {
    if (!inst || !args || !src || !dst || !src->data || !dst->data) return NTSCRT_ERR_INVALID_ARG;
    return render_impl(
        inst, args, dst->width, dst->height,
        [&](ImageBGRA8 &raw, std::vector<float> &alpha) { to_bgra8(*src, raw, &alpha); },
        [&](const std::shared_ptr<gpu::Texture> &out, const ImageBGRA8 &cpu_img, const ImageBGRA8 &raw,
            const std::vector<float> &alpha) {
            if (!out) {
                from_bgra8(cpu_img, &alpha, *dst);
                return;
            }
            ImageBGRA8 result;
            result.resize(dst->width, dst->height);
            inst->dev->download(*out, result.px.data(), ptrdiff_t(dst->width) * 4);
            bool same = result.width == raw.width && result.height == raw.height;
            from_bgra8(result, same ? &alpha : nullptr, *dst);
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

    auto get_src = [&](ImageBGRA8 &raw, std::vector<float> &alpha) {
        // Normalise any host format to RGBA32F on the GPU, read back once.
        auto f32 = dev.create_texture({sw, sh, gpu::Format::RGBA32F, true, false});
        dev.copy(*src, *f32);
        std::vector<float> px(size_t(sw) * sh * 4);
        dev.finish();
        dev.download(*f32, px.data(), ptrdiff_t(sw) * 16);
        ntscrt_image im{sw, sh, ptrdiff_t(sw) * 16, NTSCRT_RGBA32F, px.data()};
        to_bgra8(im, raw, &alpha);
    };
    auto emit = [&](const std::shared_ptr<gpu::Texture> &out, const ImageBGRA8 &, const ImageBGRA8 &raw,
                    const std::vector<float> &) {
        // render_impl only calls emit with a texture here (the device exists).
        struct { uint32_t w, h, use_alpha, pad; } p{uint32_t(dw), uint32_t(dh),
                                                     uint32_t(sw == dw && sh == dh && raw.width == sw), 0};
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
