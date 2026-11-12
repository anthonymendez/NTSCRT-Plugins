// NtscStage — ntsc-rs signal degradation (via rust/ntscrs-capi), CPU, in place.
//
// Port of upstream Sources/CrtCore/NtscStage.swift + CrtAppBridge/NtscRsBridge.m:
//  * settings: look["ntsc"]["settings"], ntsc-rs "version": 1 preset JSON. As
//    in upstream AppState, the effective settings are the app's house
//    defaults (ntsc-rs defaults overlaid with AppState.appNtscDefaults)
//    overlaid with whatever the look carries, so a partial settings object
//    (host params) renders the same as upstream. Legacy ntscQT presets
//    (detected by "_composite_preemphasis", as ntsc-rs does) pass unchanged.
//  * frame number: ntsc_frame(ctx) (= frame + 1, upstream's export convention).
//  * BGRA8 in place at the (cropped) source's full resolution; ntsc-rs writes
//    alpha opaque, the façade restores source alpha at the output boundary.
//  * render_scale: upstream has no equivalent (it always renders at full
//    res and passes ntsc-rs scale_factor [1, 1]). We pass the host's proxy
//    scale as ntsc-rs's own scale_factor (ntsc_process_frame_ex) so feature
//    sizes stay proportional to the picture; at render_scale 1 the call is
//    identical to upstream. With "scale_with_video_size" on (every bundled
//    preset), ntsc-rs already scales by picture height and the extra factor
//    is skipped to avoid scaling twice.
//  * seed: ctx.seed is added to ntsc-rs's random_seed (0 = upstream).
#include <ntscrs_capi.h>

#include <cmath>
#include <map>
#include <mutex>
#include <stdexcept>

#include "../stages.h"
#include "gpu/ntsc_gpu.h"
#include "ntsc_internal.h"

namespace ntscrt {
namespace {

struct CString {
    char *p = nullptr;
    explicit CString(char *s) : p(s) {}
    ~CString() { if (p) ntsc_string_free(p); }
    CString(const CString &) = delete;
    CString &operator=(const CString &) = delete;
    std::string str() const { return p ? std::string(p) : std::string(); }
};

// Upstream AppState.appNtscDefaults (2b3013d), values exact.
const json &app_ntsc_defaults() {
    static const json j = json::parse(R"({
        "filter_type": 0,
        "composite_preemphasis": 0.2753769054878048,
        "composite_noise_intensity": 0.1391983173076923,
        "composite_noise_frequency": 0.8576,
        "composite_noise_detail": 2,
        "snow_intensity": 0,
        "video_scanline_phase_shift_offset": 3,
        "luma_smear": 0.6692,
        "head_switching_height": 8,
        "head_switching_offset": 3,
        "head_switching_horizontal_shift": 41.57,
        "head_switching_mid_line_jitter": 0.181,
        "tracking_noise_height": 63,
        "ringing_power": 5.674,
        "ringing_scale": 4.935,
        "luma_noise_intensity": 0.153,
        "chroma_noise_intensity": 0.13054193376068376,
        "chroma_noise_frequency": 0.0777,
        "chroma_phase_error": 0.016,
        "chroma_phase_noise_intensity": 0.029,
        "chroma_delay_horizontal": 1.7279639582876793,
        "chroma_delay_vertical": 2,
        "vhs_chroma_loss": 0.003639155982905983,
        "vhs_edge_wave": 0.34226190476190477,
        "vhs_tape_speed": 3,
        "scale_with_video_size": true,
        "bandwidth_scale": 0.3,
        "vertical_scale": 0.3
    })");
    return j;
}

enum class Kind { Bool, Int, Float };

void collect_kinds(const json &arr, std::map<std::string, Kind> &out) {
    for (const auto &d : arr) {
        const std::string name = d.value("name", "");
        const std::string kind = d.value("kind", "");
        if (kind == "boolean" || kind == "group") out[name] = Kind::Bool;  // a group's name is its on/off key
        else if (kind == "int" || kind == "enum") out[name] = Kind::Int;
        else if (kind == "float" || kind == "percentage") out[name] = Kind::Float;
        if (d.contains("children")) collect_kinds(d["children"], out);
    }
}

struct Statics {
    std::string descriptors;           // ntsc_settings_descriptors_json()
    std::map<std::string, Kind> kinds; // setting name -> JSON type ntsc-rs expects
    json house;                        // ntsc-rs defaults + app defaults (+ "version": 1)
};

const Statics &statics() {
    static Statics s;
    static std::once_flag once;
    std::call_once(once, [] {
        CString d(ntsc_settings_descriptors_json());
        s.descriptors = d.str();
        json dj = json::parse(s.descriptors, nullptr, false);
        if (dj.is_array()) collect_kinds(dj, s.kinds);
        NtscInstance *tmp = ntsc_new();
        if (tmp) {
            CString js(ntsc_settings_to_json(tmp));
            s.house = json::parse(js.str(), nullptr, false);
            ntsc_free(tmp);
        }
        if (!s.house.is_object()) s.house = json::object();
        for (auto it = app_ntsc_defaults().begin(); it != app_ntsc_defaults().end(); ++it) s.house[it.key()] = it.value();
        s.house["version"] = 1;
    });
    return s;
}

// Coerce host-written values to the JSON types ntsc-rs's parser expects
// (hosts may write 3.0 for an int/enum, 1/0 for a bool).
json coerce(const std::string &key, const json &v) {
    const auto &k = statics().kinds;
    auto it = k.find(key);
    if (it == k.end()) return v;
    switch (it->second) {
    case Kind::Bool:
        if (v.is_number()) return v.get<double>() >= 0.5;
        return v;
    case Kind::Int:
        if (v.is_number_float()) return static_cast<int64_t>(std::llround(v.get<double>()));
        if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
        return v;
    case Kind::Float:
        if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
        return v;
    }
    return v;
}

}  // namespace

std::string ntsc_effective_settings_json(const json &look) {
    const json *settings = nullptr;
    if (look.is_object()) {
        auto n = look.find("ntsc");
        if (n != look.end() && n->is_object()) {
            auto s = n->find("settings");
            if (s != n->end() && s->is_object()) settings = &*s;
        }
    }
    if (settings && settings->contains("_composite_preemphasis")) return settings->dump();  // ntscQT preset
    json merged = statics().house;
    if (settings)
        for (auto it = settings->begin(); it != settings->end(); ++it) merged[it.key()] = coerce(it.key(), it.value());
    if (!merged.contains("version")) merged["version"] = 1;
    return merged.dump();
}

struct NtscStage::Impl {
    NtscInstance *inst = nullptr;
    std::string applied;  // settings JSON currently loaded into `inst`
    bool scale_with_video_size = false;
    ~Impl() { if (inst) ntsc_free(inst); }

    // Loads the look's effective settings into `inst` (when they changed) and
    // returns the scale factor ntsc_process_frame_ex gets for this frame.
    float apply(const FrameContext &ctx) {
        if (!inst) {
            inst = ntsc_new();
            if (!inst) throw std::runtime_error("ntsc: ntsc_new failed");
        }
        static const json empty = json::object();
        const json &look = ctx.look ? *ctx.look : empty;
        std::string s = ntsc_effective_settings_json(look);
        if (s != applied) {
            char *err = nullptr;
            int rc = ntsc_settings_from_json(inst, s.c_str(), &err);
            if (rc != 0) {
                CString e(err);
                applied.clear();
                throw std::runtime_error("ntsc: settings rejected: " + (e.p ? e.str() : "error " + std::to_string(rc)));
            }
            applied = std::move(s);
            json j = json::parse(applied, nullptr, false);
            scale_with_video_size = j.is_object() && j.value("scale_settings", true) &&
                                    j.value("scale_with_video_size", false);
        }
        float scale = 1.0f;
        if (!scale_with_video_size && ctx.render_scale > 0 && std::abs(ctx.render_scale - 1.0) > 1e-9)
            scale = float(ctx.render_scale);
        return scale;
    }
    static int32_t seed_offset(const FrameContext &ctx) { return int32_t(uint32_t(ctx.seed & 0xffffffffu)); }
};

NtscStage::NtscStage() : impl_(std::make_unique<Impl>()) {}
NtscStage::~NtscStage() = default;

bool NtscStage::active(const json &look) const {
    // Upstream AppState.ntscEnabled defaults to true and a look without the
    // key leaves it alone.
    if (!look.is_object()) return true;
    auto n = look.find("ntsc");
    if (n == look.end() || !n->is_object()) return true;
    auto e = n->find("enabled");
    if (e == n->end()) return true;
    if (e->is_boolean()) return e->get<bool>();
    if (e->is_number()) return e->get<double>() >= 0.5;
    return true;
}

void NtscStage::process(ImageBGRA8 &inout, const FrameContext &ctx) {
    if (inout.width <= 0 || inout.height <= 0) return;
    const float scale = impl_->apply(ctx);
    int rc = ntsc_process_frame_ex(impl_->inst, NTSC_BGRA8, inout.px.data(), uint32_t(inout.width),
                                   uint32_t(inout.height), uint32_t(inout.width) * 4, ntsc_frame(ctx), scale, scale,
                                   Impl::seed_offset(ctx));
    if (rc != 0) throw std::runtime_error("ntsc: frame processing failed (" + std::to_string(rc) + ")");
}

void NtscStage::gpu_plan(const FrameContext &ctx, int width, int height, NtscGpuPlan &out, uint32_t stage_mask) {
    if (width <= 0 || height <= 0) throw std::runtime_error("ntsc gpu: empty picture");
    const float scale = impl_->apply(ctx);
    out.width = width;
    out.height = height;
    out.rows.resize(size_t(height));
    out.out.resize(size_t(height));
    if (out.passes.size() < 32) out.passes.resize(32);
    for (int attempt = 0; attempt < 2; ++attempt) {
        int rc = ntsc_gpu_plan(impl_->inst, uint32_t(width), uint32_t(height), ntsc_frame(ctx), scale, scale,
                               Impl::seed_offset(ctx), stage_mask, &out.info, out.passes.data(),
                               uint32_t(out.passes.size()), out.rows.data(), out.out.data());
        if (rc == 3) {
            out.passes.resize(out.info.num_passes);
            continue;
        }
        if (rc != 0) throw std::runtime_error("ntsc gpu: plan failed (" + std::to_string(rc) + ")");
        out.passes.resize(out.info.num_passes);
        out.rows.resize(out.info.plane_rows);
        return;
    }
    throw std::runtime_error("ntsc gpu: plan failed");
}

std::vector<float> NtscStage::debug_planes(const ImageBGRA8 &in, const FrameContext &ctx, uint32_t stage_mask,
                                           int *plane_rows) {
    NtscGpuPlan plan;
    gpu_plan(ctx, in.width, in.height, plan, stage_mask);
    const size_t n = size_t(in.width) * plan.info.plane_rows;
    std::vector<float> planes(n * 3);
    const float scale = impl_->apply(ctx);
    int rc = ntsc_debug_process_yiq(impl_->inst, NTSC_BGRA8, in.px.data(), uint32_t(in.width), uint32_t(in.height),
                                    uint32_t(in.width) * 4, ntsc_frame(ctx), scale, scale, Impl::seed_offset(ctx),
                                    stage_mask, planes.data(), planes.size());
    if (rc != 0) throw std::runtime_error("ntsc: debug planes failed (" + std::to_string(rc) + ")");
    if (plane_rows) *plane_rows = int(plan.info.plane_rows);
    return planes;
}

std::string NtscStage::descriptors_json() { return statics().descriptors; }

}  // namespace ntscrt
