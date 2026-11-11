// timeline.h — upstream NTSCRT's keyframe timeline ("timeline" in a look /
// preset file), evaluated the way the app does. Header-only (nlohmann::json)
// so both host param models (ofx/ofx_params.cpp, fxplug/model/param_model.cpp),
// ntscrt-cli and the tests share one evaluation.
//
// Port of upstream Sources/CrtApp/Timeline.swift (KeyEasing, TimelineEvaluator)
// and the parts of AppState that load and play a timeline:
// - keys: {"t" 0…1, "easing", "shader", "ntsc", "glitch", "digital"}; a look with
//   keys plays them whatever "enabled" says (AppState.loadLook opens the timeline).
// - frame -> time: the timeline spans round(duration × fps) frames and frame i
//   sits at t = i / (frames - 1) (AppState.timelineTotalFrames, normalizedTime).
//   Playback loops: frame i plays i mod frames (AppState.digitalHistory wraps
//   the same way; the preview loops back to t = 0).
// - shader params lerp in Float and snap to their step grid, ntsc-rs settings
//   lerp (ints rounded, bools / enums hold), Glitch toggles and Digital choices
//   hold, Digital structure controls never key. Keyed values are merged over the
//   look's own (applyTimeline); ntsc settings are replaced as a whole.
//
// NTSCRT-Plugins: hosts have no timeline of their own, so they turn the keys
// into host keyframes at the host's frame rate: the timeline's duration in
// seconds from the effect's first frame, repeated until the effect ends.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ntscrt::timeline {
using json = nlohmann::json;

enum class Easing { Linear, EaseIn, EaseOut, EaseInOut, Hold };

inline Easing parse_easing(const json &j) {
    if (!j.is_string()) return Easing::Linear;
    const std::string s = j.get<std::string>();
    if (s == "Ease in") return Easing::EaseIn;
    if (s == "Ease out") return Easing::EaseOut;
    if (s == "Ease in-out") return Easing::EaseInOut;
    if (s == "Hold") return Easing::Hold;
    return Easing::Linear;  // "Linear" and anything unknown (upstream: ?? .linear)
}

// KeyEasing.apply: normalized segment progress 0…1, shaped.
inline double ease(Easing e, double u) {
    switch (e) {
    case Easing::Linear: return u;
    case Easing::EaseIn: return u * u;
    case Easing::EaseOut: return 1 - (1 - u) * (1 - u);
    case Easing::EaseInOut: return u < 0.5 ? 2 * u * u : 1 - std::pow(-2 * u + 2, 2) / 2;
    case Easing::Hold: return 0;
    }
    return u;
}

// How each keyed value interpolates, from the param schema (docs/params.md).
struct Rules {
    enum class Interp { Lerp, LerpInt, Hold };
    struct ShaderMeta {
        float min = 0, max = 0, step = 0;
    };
    std::map<std::string, Interp> ntsc;                                   // ntsc-rs setting -> interpolation
    std::map<std::string, std::map<std::string, ShaderMeta>> shader;      // CRT preset id -> NAME -> range
    std::set<std::string> glitch_toggles, digital_choices, digital_structure;

    static Rules from_schema(const json &schema) {
        Rules r;
        if (!schema.is_object()) return r;
        auto tail = [](const std::string &path, const char *prefix) -> std::optional<std::string> {
            const std::string p(prefix);
            if (path.rfind(p, 0) != 0 || path.size() == p.size()) return std::nullopt;
            std::string rest = path.substr(p.size());
            if (rest.find('/') != std::string::npos) return std::nullopt;
            return rest;
        };
        for (const auto &q : schema.value("params", json::array())) {
            if (!q.is_object()) continue;
            const std::string path = q.value("path", std::string()), type = q.value("type", std::string());
            if (auto n = tail(path, "/ntsc/settings/")) {
                r.ntsc[*n] = type == "float" ? Interp::Lerp : type == "int" ? Interp::LerpInt : Interp::Hold;
            } else if (auto n = tail(path, "/shader/params/")) {
                const json vi = q.value("visible_if", json());
                if (!vi.is_object() || vi.value("path", std::string()) != "/shader/preset" || !vi.contains("equals") ||
                    !vi["equals"].is_string())
                    continue;
                ShaderMeta m;
                m.min = float(q.value("min", 0.0));
                m.max = float(q.value("max", 0.0));
                m.step = float(q.value("step", 0.0));
                r.shader[vi["equals"].get<std::string>()][*n] = m;
            } else if (auto n = tail(path, "/glitch/values/")) {
                if (type == "bool") r.glitch_toggles.insert(*n);
            } else if (auto n = tail(path, "/digital/values/")) {
                if (type == "choice" || type == "bool") r.digital_choices.insert(*n);
                if (q.value("structure", false)) r.digital_structure.insert(*n);
            }
        }
        return r;
    }
};

struct Key {
    double t = 0;
    Easing easing = Easing::Linear;
    json shader = json::object(), ntsc = json::object(), glitch = json::object(), digital = json::object();
};

struct Timeline {
    double duration = 5.0;  // seconds (upstream timelineDuration default)
    int fps = 30;           // upstream timelineFPS (stills only; hosts use their own rate)
    std::vector<Key> keys;  // sorted by t; empty = nothing animates
    bool animated() const { return !keys.empty(); }
};

// The look's timeline (keys sorted). Digital key values should already be
// migrated (ntscrt::digital::migrate::upgraded does that for the whole look).
inline Timeline parse(const json &look) {
    Timeline tl;
    if (!look.is_object()) return tl;
    auto it = look.find("timeline");
    if (it == look.end() || !it->is_object()) return tl;
    const json &t = *it;
    if (t.contains("duration") && t["duration"].is_number()) tl.duration = t["duration"].get<double>();
    if (t.contains("fps") && t["fps"].is_number()) tl.fps = int(t["fps"].get<double>());
    auto obj = [](const json &k, const char *name) {
        auto f = k.find(name);
        return f != k.end() && f->is_object() ? *f : json::object();
    };
    for (const auto &k : t.value("keys", json::array())) {
        if (!k.is_object() || !k.contains("t") || !k["t"].is_number()) continue;  // upstream: compactMap drops it
        Key key;
        key.t = k["t"].get<double>();
        key.easing = parse_easing(k.value("easing", json()));
        key.shader = obj(k, "shader");
        key.ntsc = obj(k, "ntsc");
        key.glitch = obj(k, "glitch");
        key.digital = obj(k, "digital");
        tl.keys.push_back(std::move(key));
    }
    std::stable_sort(tl.keys.begin(), tl.keys.end(), [](const Key &a, const Key &b) { return a.t < b.t; });
    return tl;
}

// Frames one pass of the timeline spans at `fps` (upstream timelineTotalFrames).
inline int64_t pass_frames(const Timeline &tl, double fps) {
    const double f = fps > 0 ? fps : double(std::max(1, tl.fps));
    return std::max<int64_t>(1, int64_t(std::llround(std::max(0.0, tl.duration) * f)));
}

// Normalized time of frame n (0 = the effect's first frame), looping.
inline double time_of_frame(int64_t n, int64_t pass) {
    if (pass <= 1) return 0;
    const int64_t i = ((n % pass) + pass) % pass;
    return double(i) / double(pass - 1);
}

namespace detail {
struct Segment {
    const Key *a, *b;
    double u;
};
// TimelineEvaluator.segment: clamps outside the first / last key.
inline Segment segment(const Timeline &tl, double t) {
    const Key &first = tl.keys.front(), &last = tl.keys.back();
    if (t <= first.t || tl.keys.size() == 1) return {&first, &first, 0};
    if (t >= last.t) return {&last, &last, 0};
    const Key *a = &first, *b = &last;
    for (const auto &k : tl.keys) {
        if (k.t <= t) a = &k;
        else {
            b = &k;
            break;
        }
    }
    const double span = b->t - a->t;
    const double raw = span > 0 ? (t - a->t) / span : 0;
    return {a, b, ease(a->easing, std::min(1.0, std::max(0.0, raw)))};
}
inline bool is_num(const json &v) { return v.is_number(); }  // json bools are not numbers
inline double num(const json &v) { return v.get<double>(); }
}  // namespace detail

// Keyed values at normalized time t, one object per section (only the keys the
// segment's first key carries, as upstream).
struct Values {
    json shader = json::object(), ntsc = json::object(), glitch = json::object(), digital = json::object();
};

inline Values values_at(const Timeline &tl, const Rules &rules, const std::string &shader_preset, double t) {
    Values out;
    if (!tl.animated()) return out;
    const auto seg = detail::segment(tl, t);
    const Key &a = *seg.a, &b = *seg.b;
    const double u = seg.u;
    // shaderParams(at:): Float arithmetic, snapped to the step grid, clamped.
    if (u == 0) {
        out.shader = a.shader;
    } else {
        auto pm = rules.shader.find(shader_preset);
        for (auto &[name, va_j] : a.shader.items()) {
            if (!detail::is_num(va_j)) continue;
            const float va = float(detail::num(va_j));
            auto bj = b.shader.find(name);
            const float vb = bj != b.shader.end() && detail::is_num(*bj) ? float(detail::num(*bj)) : va;
            float v = va + (vb - va) * float(u);
            if (pm != rules.shader.end()) {
                auto m = pm->second.find(name);
                if (m != pm->second.end()) {
                    if (m->second.step > 0)
                        v = m->second.min + std::round((v - m->second.min) / m->second.step) * m->second.step;
                    v = std::min(m->second.max, std::max(m->second.min, v));
                }
            }
            out.shader[name] = double(v);
        }
    }
    // ntscValues(at:)
    if (u == 0) {
        out.ntsc = a.ntsc;
    } else {
        for (auto &[name, ra] : a.ntsc.items()) {
            auto it = rules.ntsc.find(name);
            const Rules::Interp in = it == rules.ntsc.end() ? Rules::Interp::Hold : it->second;
            auto rb = b.ntsc.find(name);
            if (in == Rules::Interp::Hold || !detail::is_num(ra) || rb == b.ntsc.end() || !detail::is_num(*rb)) {
                out.ntsc[name] = ra;
                continue;
            }
            const double v = detail::num(ra) + (detail::num(*rb) - detail::num(ra)) * u;
            if (in == Rules::Interp::LerpInt) out.ntsc[name] = int64_t(std::llround(v));
            else out.ntsc[name] = v;
        }
    }
    // glitchValues(at:): sliders interpolate, toggles hold.
    if (u == 0) {
        out.glitch = a.glitch;
    } else {
        for (auto &[id, va_j] : a.glitch.items()) {
            if (!detail::is_num(va_j)) continue;
            const double va = detail::num(va_j);
            auto bj = b.glitch.find(id);
            const double vb = bj != b.glitch.end() && detail::is_num(*bj) ? detail::num(*bj) : va;
            out.glitch[id] = rules.glitch_toggles.count(id) ? va : va + (vb - va) * u;
        }
    }
    // digitalValues(at:): choices hold, structure controls are left out.
    for (auto &[id, va_j] : a.digital.items()) {
        if (rules.digital_structure.count(id) || !detail::is_num(va_j)) continue;
        const double va = detail::num(va_j);
        auto bj = b.digital.find(id);
        const double vb = bj != b.digital.end() && detail::is_num(*bj) ? detail::num(*bj) : va;
        out.digital[id] = (u == 0 || rules.digital_choices.count(id)) ? va : va + (vb - va) * u;
    }
    return out;
}

// The look at normalized time t: the look's own values with the keyed ones
// applied (upstream applyTimeline). Without keys, the look unchanged.
inline json look_at(const json &look, const Timeline &tl, const Rules &rules, double t) {
    if (!tl.animated() || !look.is_object()) return look;
    json out = look;
    std::string preset;
    if (out.contains("shader") && out["shader"].is_object() && out["shader"].contains("preset") &&
        out["shader"]["preset"].is_string())
        preset = out["shader"]["preset"].get<std::string>();
    Values v = values_at(tl, rules, preset, t);
    auto merge = [&](const char *section, const char *field, const json &vals) {
        if (vals.empty()) return;
        json &s = out[section];
        if (!s.is_object()) s = json::object();
        json &f = s[field];
        if (!f.is_object()) f = json::object();
        for (auto &[k, x] : vals.items()) f[k] = x;
    };
    merge("shader", "params", v.shader);
    if (!v.ntsc.empty()) {  // applyNtscValues: the whole settings object
        json &n = out["ntsc"];
        if (!n.is_object()) n = json::object();
        n["settings"] = v.ntsc;
    }
    merge("glitch", "values", v.glitch);
    merge("digital", "values", v.digital);
    return out;
}

// The look host frame n shows (0 = the effect's first frame, at `fps`).
inline json look_at_frame(const json &look, const Timeline &tl, const Rules &rules, int64_t n, double fps) {
    return look_at(look, tl, rules, time_of_frame(n, pass_frames(tl, fps)));
}

}  // namespace ntscrt::timeline
