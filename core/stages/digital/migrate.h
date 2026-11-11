// migrate.h — Digital video values saved by older versions, mapped onto
// today's controls. Header-only (nlohmann::json + <cmath>) so the hosts'
// param model (hostparams/, shared by the OpenFX and FxPlug plug-ins) can apply
// the same rules as the core when they load a preset.
//
// Port of upstream DigitalSettings.migrated (DigitalVideoParams.swift, 0.19):
// looks saved before 0.19 ("version" 1) had Exposure and Black level (now
// CCD response: the stronger of the two decides), Lens softness (gone) and
// Quality on JPEG's own curve (converted to the value giving the same
// quantizer). Looks now save as version 2.
//
// Plug-in additions:
// - Version detection: a look without "version" (the plug-ins don't write the
//   schema constants) is version 1 if its Digital values still carry
//   exposure / black_level / lens_blur without ccd_response (what the 0.4.0
//   plug-ins built and saved), else current.
// - /digital/legacy: plug-in instances made with 0.4.0 keep their old
//   controls as hidden params (schema ids digital.quality, digital.exposure,
//   digital.black_level, digital.lens_blur), written under /digital/legacy
//   only while non-zero. legacy.quality (old curve) stands in for Quality
//   while Quality is at its default; legacy exposure / black_level become CCD
//   response while CCD response is 0. Lens softness is dropped, as upstream.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

namespace ntscrt::digital::migrate {
using json = nlohmann::json;

constexpr int kLookVersion = 2;        // upstream lookDictionary "version" since 0.19
constexpr double kQualityDefault = 0.749;  // upstream DigitalParam "quality" default since 0.19

// Upstream DigitalCodec.qualityFromJPEGScale: a pre-0.19 Quality as today's
// slider value giving the same quantizer (scale roots 5 and sqrt(0.1)).
inline double quality_from_jpeg_scale(double old) {
    const double worst = 5, best = std::sqrt(0.1);
    double q = 2 + 93 * std::min(1.0, std::max(0.0, old));
    double scale = (q < 50 ? 5000 / q : 200 - 2 * q) / 100;
    return std::min(1.0, std::max(0.0, (worst - std::sqrt(scale)) / (worst - best)));
}

inline const json *digital_values(const json &look) {
    if (!look.is_object()) return nullptr;
    auto d = look.find("digital");
    if (d == look.end() || !d->is_object()) return nullptr;
    auto v = d->find("values");
    return v != d->end() && v->is_object() ? &*v : nullptr;
}

inline int look_version(const json &look) {
    if (look.is_object()) {
        auto v = look.find("version");
        if (v != look.end() && v->is_number()) return int(v->get<double>());
    }
    const json *v = digital_values(look);
    if (v && !v->contains("ccd_response") &&
        (v->contains("exposure") || v->contains("black_level") || v->contains("lens_blur")))
        return 1;
    return kLookVersion;
}

inline double number(const json &o, const char *key, double fallback) {
    auto it = o.find(key);
    if (it == o.end()) return fallback;
    if (it->is_number()) return it->get<double>();
    if (it->is_boolean()) return it->get<bool>() ? 1.0 : 0.0;
    return fallback;
}

// Upstream DigitalSettings.migrated on one values object (the look's
// digital.values or a keyframe's digital).
inline json migrated(const json &values, int lookVersion) {
    if (!values.is_object()) return values;
    json v = values;
    if (lookVersion < 2 && v.contains("quality") && v["quality"].is_number())
        v["quality"] = quality_from_jpeg_scale(v["quality"].get<double>());
    const bool hasExposure = v.contains("exposure"), hasBlack = v.contains("black_level");
    const double exposure = number(v, "exposure", 0), black = number(v, "black_level", 0);
    v.erase("exposure");
    v.erase("black_level");
    v.erase("lens_blur");
    if (!v.contains("ccd_response") && (hasExposure || hasBlack))
        v["ccd_response"] = std::min(1.0, std::max({0.0, exposure / 0.6, black / 0.08}));
    return v;
}

// The 0.4.0 plug-in controls kept as hidden params (see the top).
inline void apply_legacy(json &values, const json &legacy) {
    if (!legacy.is_object() || !values.is_object()) return;
    const double q = number(legacy, "quality", 0);
    if (q > 0 && std::fabs(number(values, "quality", kQualityDefault) - kQualityDefault) < 1e-9)
        values["quality"] = quality_from_jpeg_scale(q);
    const double exposure = number(legacy, "exposure", 0), black = number(legacy, "black_level", 0);
    if ((exposure != 0 || black != 0) && number(values, "ccd_response", 0) == 0)
        values["ccd_response"] = std::min(1.0, std::max({0.0, exposure / 0.6, black / 0.08}));
}

// The Digital values a look renders with: migrated for its version, then the
// legacy controls applied. An object (empty when the look has none).
inline json values_for(const json &look) {
    const json *v = digital_values(look);
    json out = v ? migrated(*v, look_version(look)) : json::object();
    if (look.is_object()) {
        auto d = look.find("digital");
        if (d != look.end() && d->is_object()) {
            auto l = d->find("legacy");
            if (l != d->end()) apply_legacy(out, *l);
        }
    }
    return out;
}

// The look upgraded to the current version: Digital values and every
// timeline key's Digital values migrated, "version" set (when the look was
// older). For hosts applying a preset onto their params.
inline json upgraded(const json &look) {
    if (!look.is_object()) return look;
    const int version = look_version(look);
    json out = look;
    if (auto it = out.find("digital"); it != out.end() && it->is_object()) {
        auto v = it->find("values");
        if (v != it->end() && v->is_object()) *v = values_for(look);
        it->erase("legacy");
    }
    if (auto tl = out.find("timeline"); tl != out.end() && tl->is_object()) {
        if (auto keys = tl->find("keys"); keys != tl->end() && keys->is_array())
            for (auto &k : *keys)
                if (k.is_object() && k.contains("digital")) k["digital"] = migrated(k["digital"], version);
    }
    if (version < kLookVersion) out["version"] = kLookVersion;
    return out;
}

}  // namespace ntscrt::digital::migrate
