// ntscrt.cpp — C ABI façade. Wave 0: CPU stages wired, GPU stages wired but
// skipped when no gpu::Device is available (stub backend). Wave 2 finishes
// the Metal entry point, caching and Howlaround ordering.
#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
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
        inst->howl = std::make_unique<Howlaround>(*inst->dev);
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
        arr.push_back({{"name", rel.generic_u8string()}, {"kind", fb ? "feedback" : "look"}, {"json", j}});
    }
    std::sort(arr.begin(), arr.end(), [](const json &a, const json &b) { return a["name"] < b["name"]; });
    inst->presets_cache = arr.dump();
    return inst->presets_cache.c_str();
}

ntscrt_status ntscrt_render_cpu(ntscrt_instance *inst, const ntscrt_render_args *args, const ntscrt_image *src,
                                ntscrt_image *dst) {
    if (!inst || !args || !src || !dst || !src->data || !dst->data) return NTSCRT_ERR_INVALID_ARG;
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
        to_bgra8(*src, raw, &alpha);
        Crop::apply(look, raw, img);

        if (inst->digital.active(look)) inst->digital.process(img, ctx);
        if (inst->ntsc.active(look)) inst->ntsc.process(img, ctx);

        if (!inst->dev) {
            // Stub / no-GPU path: emit the CPU result (same size when crop is off).
            from_bgra8(img, &alpha, *dst);
            return NTSCRT_OK;
        }
        gpu::Device &dev = *inst->dev;
        auto tex = dev.create_texture({img.width, img.height, gpu::Format::BGRA8, true, false});
        dev.upload(*tex, img.px.data(), ptrdiff_t(img.width) * 4);
        bool howl_on = inst->howl->active(args->feedback_json ? &feedback : nullptr);
        if (howl_on) tex = inst->howl->camera(tex, feedback, ctx);
        auto chain = inst->downscale->run(tex, ctx);
        if (inst->glitch->active(look)) chain = inst->glitch->run(chain, ctx);
        auto out = dev.create_texture({dst->width, dst->height, gpu::Format::BGRA8, true, true});
        inst->crt->render(*chain, *out, ctx);
        if (howl_on) inst->howl->after_frame(*out, feedback, ctx);
        dev.finish();
        ImageBGRA8 result;
        result.resize(dst->width, dst->height);
        dev.download(*out, result.px.data(), ptrdiff_t(dst->width) * 4);
        bool same = result.width == raw.width && result.height == raw.height;
        from_bgra8(result, same ? &alpha : nullptr, *dst);
        return NTSCRT_OK;
    } catch (const std::exception &e) {
        return fail(inst, NTSCRT_ERR_INTERNAL, e.what());
    }
}

ntscrt_status ntscrt_render_metal(ntscrt_instance *inst, const ntscrt_render_args *, void *, void *, void *) {
    return fail(inst, NTSCRT_ERR_INTERNAL, "ntscrt_render_metal: not implemented yet (Wave 2)");
}

}  // extern "C"
