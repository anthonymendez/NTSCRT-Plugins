#include "../ntscrt_host/params.h"

#include <ntscrt/ntscrt.h>

#include "../../core/stages/digital/migrate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <fstream>
#include <stdexcept>

namespace ntscrt::host {
namespace {
const char kFbPrefix[] = "fb:";
constexpr size_t kFbLen = sizeof(kFbPrefix) - 1;
const char kFormatMenuPrefix[] = "Digital video/";

std::optional<Cond> parse_cond(const json &j) {
    if (j.is_string()) return Cond{j.get<std::string>(), false, json()};
    if (j.is_object() && j.contains("path") && j["path"].is_string()) {
        Cond c{j["path"].get<std::string>(), false, json()};
        if (j.contains("equals")) {
            c.has_equals = true;
            c.equals = j["equals"];
        }
        return c;
    }
    return std::nullopt;
}

bool num(const json &j, const char *k, double &out) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_number()) return false;
    out = it->get<double>();
    return true;
}

bool parse_hex(const std::string &s, std::vector<double> &out) {
    std::string h = s;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() != 6 && h.size() != 8) return false;
    out.clear();
    for (size_t i = 0; i < h.size(); i += 2) {
        unsigned v = 0;
        if (std::sscanf(h.substr(i, 2).c_str(), "%2x", &v) != 1) return false;
        out.push_back(v / 255.0);
    }
    return true;
}

double clamped(const ParamDef &p, double v) {
    if (p.has_min && v < p.min) v = p.min;
    if (p.has_max && v > p.max) v = p.max;
    return v;
}
}  // namespace

bool ParamDef::has_applies() const {
    for (const auto &o : options)
        if (!o.applies.is_null()) return true;
    return false;
}

std::string host_name(const std::string &id) {
    std::string r = id;
    for (char &c : r) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) c = '_';
    }
    if (r.empty() || (r[0] >= '0' && r[0] <= '9')) r = "p_" + r;
    return r;
}

Schema Schema::parse(const char *schema_json) { return parse(json::parse(schema_json ? schema_json : "{}")); }

Schema Schema::parse(const json &j) {
    Schema s;
    if (!j.is_object()) return s;
    for (const auto &g : j.value("groups", json::array())) {
        if (!g.is_object()) continue;
        GroupDef d;
        d.id = g.value("id", std::string());
        if (d.id.empty()) continue;
        d.name = "grp_" + host_name(d.id);
        d.label = g.value("label", d.id);
        if (g.contains("parent") && g["parent"].is_string()) d.parent = g["parent"].get<std::string>();
        s.groups.push_back(std::move(d));
    }
    for (const auto &pj : j.value("params", json::array())) {
        if (!pj.is_object()) continue;
        ParamDef p;
        p.id = pj.value("id", std::string());
        const std::string type = pj.value("type", std::string());
        const std::string path = pj.value("path", std::string());
        if (p.id.empty() || path.empty() || s.by_id.count(p.id)) continue;
        if (type == "bool") p.type = PType::Bool;
        else if (type == "int") p.type = PType::Int;
        else if (type == "float") p.type = PType::Float;
        else if (type == "choice") p.type = PType::Choice;
        else if (type == "color") p.type = PType::Color;
        else continue;  // unknown type: skip rather than break the host UI
        p.name = host_name(p.id);
        p.group = pj.value("group", std::string());
        p.label = pj.value("label", p.id);
        p.hint = pj.value("hint", std::string());
        if (path.rfind(kFbPrefix, 0) == 0) {
            p.feedback = true;
            p.path = path.substr(kFbLen);
        } else {
            p.path = path;
        }
        if (p.path.empty() || p.path[0] != '/') continue;  // not a JSON pointer
        p.has_min = num(pj, "min", p.min);
        p.has_max = num(pj, "max", p.max);
        p.has_dmin = num(pj, "display_min", p.dmin);
        p.has_dmax = num(pj, "display_max", p.dmax);
        p.has_step = num(pj, "step", p.step);
        p.animatable = pj.value("animatable", true);
        p.in_presets = pj.value("preset", true);
        p.encode_number = pj.value("encode", std::string()) == "number";
        if (pj.contains("enabled_if")) p.enabled_if = parse_cond(pj["enabled_if"]);
        if (pj.contains("visible_if")) p.visible_if = parse_cond(pj["visible_if"]);
        if (p.type == PType::Choice) {
            for (const auto &o : pj.value("options", json::array())) {
                if (o.is_object()) {
                    json v = o.value("value", json());
                    std::string label = o.contains("label") && o["label"].is_string() ? o["label"].get<std::string>()
                                        : v.is_string()                             ? v.get<std::string>()
                                                                                    : v.dump();
                    p.options.push_back({v, label, o.contains("applies") && o["applies"].is_object() ? o["applies"] : json()});
                } else {
                    p.options.push_back({o, o.is_string() ? o.get<std::string>() : o.dump(), json()});
                }
            }
            if (p.options.empty()) continue;
        }
        json d = pj.value("default", json());
        if (p.type == PType::Color) {
            if (d.is_string()) p.color_hex = true;
            else if (d.is_array() && d.size() == 4) p.color_n = 4;
        }
        if (auto cd = coerce(p, d)) {
            p.def = *cd;
        } else {
            switch (p.type) {
            case PType::Bool: p.def = false; break;
            case PType::Int: p.def = int64_t(p.has_min ? std::llround(p.min) : 0); break;
            case PType::Float: p.def = p.has_min ? p.min : 0.0; break;
            case PType::Choice: p.def = p.options[0].value; break;
            case PType::Color: p.def = color_value(p, std::vector<double>(size_t(p.color_n), 0.0)); break;
            }
        }
        (p.feedback ? s.by_fb_path : s.by_look_path)[p.path] = s.params.size();
        s.by_id[p.id] = s.params.size();
        s.params.push_back(std::move(p));
    }
    if (j.contains("constants") && j["constants"].is_object()) s.constants = j["constants"];
    if (j.contains("digital_formats") && j["digital_formats"].is_array()) s.digital_formats = j["digital_formats"];
    s.timeline = ntscrt::timeline::Rules::from_schema(j);
    return s;
}

const ParamDef *Schema::owner(const std::string &cond_path) const {
    if (cond_path.rfind(kFbPrefix, 0) == 0) {
        auto it = by_fb_path.find(cond_path.substr(kFbLen));
        return it == by_fb_path.end() ? nullptr : &params[it->second];
    }
    auto it = by_look_path.find(cond_path);
    return it == by_look_path.end() ? nullptr : &params[it->second];
}

std::optional<size_t> Schema::index_of(const std::string &id) const {
    auto it = by_id.find(id);
    if (it == by_id.end()) return std::nullopt;
    return it->second;
}

// ---- values -------------------------------------------------------------------
bool truthy(const json &v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) return !v.get<std::string>().empty();
    return !v.is_null();
}

bool same_value(const json &a, const json &b) {
    if (a.is_number() && b.is_number()) return std::fabs(a.get<double>() - b.get<double>()) < 1e-9;
    if (a.is_array() && b.is_array() && a.size() == b.size()) {
        for (size_t i = 0; i < a.size(); ++i)
            if (!same_value(a[i], b[i])) return false;
        return true;
    }
    return a == b;
}

std::optional<json> value_at(const json &doc, const std::string &pointer) {
    if (!doc.is_object() && !doc.is_array()) return std::nullopt;
    try {
        json::json_pointer ptr(pointer);
        if (!doc.contains(ptr)) return std::nullopt;
        return doc.at(ptr);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

bool set_at(json &doc, const std::string &pointer, const json &v) {
    try {
        if (doc.is_null()) doc = json::object();
        doc[json::json_pointer(pointer)] = v;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

std::optional<json> coerce(const ParamDef &p, const json &v) {
    switch (p.type) {
    case PType::Bool:
        if (v.is_boolean()) return v;
        if (v.is_number()) return json(v.get<double>() != 0.0);
        return std::nullopt;
    case PType::Int:
        if (v.is_number()) return json(int64_t(std::llround(clamped(p, v.get<double>()))));
        if (v.is_boolean()) return json(int64_t(std::llround(clamped(p, v.get<bool>() ? 1.0 : 0.0))));
        return std::nullopt;
    case PType::Float:
        if (v.is_number()) return json(clamped(p, v.get<double>()));
        if (v.is_boolean()) return json(clamped(p, v.get<bool>() ? 1.0 : 0.0));
        return std::nullopt;
    case PType::Choice: {
        int i = choice_index(p, v);
        if (i < 0 && v.is_string()) {  // tolerate a label
            for (size_t k = 0; k < p.options.size() && i < 0; ++k)
                if (p.options[k].label == v.get<std::string>()) i = int(k);
        }
        if (i < 0) return std::nullopt;
        return p.options[size_t(i)].value;
    }
    case PType::Color: {
        std::vector<double> c;
        if (v.is_string() && parse_hex(v.get<std::string>(), c)) return color_value(p, c);
        if (v.is_array() && v.size() >= 3) {
            for (const auto &e : v)
                if (e.is_number()) c.push_back(e.get<double>());
            if (c.size() >= 3) return color_value(p, c);
        }
        return std::nullopt;
    }
    }
    return std::nullopt;
}

int choice_index(const ParamDef &p, const json &value) {
    for (size_t k = 0; k < p.options.size(); ++k)
        if (same_value(p.options[k].value, value)) return int(k);
    return -1;
}

std::vector<double> color_components(const ParamDef &p, const json &value) {
    std::vector<double> c(size_t(p.color_n), p.color_n == 4 ? 1.0 : 0.0);
    std::fill(c.begin(), c.begin() + 3, 0.0);
    std::vector<double> in;
    if (value.is_string()) parse_hex(value.get<std::string>(), in);
    else if (value.is_array())
        for (const auto &e : value)
            if (e.is_number()) in.push_back(e.get<double>());
    for (size_t i = 0; i < c.size() && i < in.size(); ++i) c[i] = in[i];
    return c;
}

json color_value(const ParamDef &p, const std::vector<double> &c) {
    auto at = [&](size_t i) { return i < c.size() ? c[i] : (i == 3 ? 1.0 : 0.0); };
    if (p.color_hex) {
        char buf[16];
        auto b = [&](size_t i) { return unsigned(std::lround(std::clamp(at(i), 0.0, 1.0) * 255.0)); };
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x", b(0), b(1), b(2));
        return std::string(buf);
    }
    json a = json::array();
    for (size_t i = 0; i < size_t(p.color_n); ++i) a.push_back(at(i));
    return a;
}

// ---- the look --------------------------------------------------------------------
void build_look(const Schema &s, const ValueFn &value, json &look, json &feedback) {
    look = json::object();
    feedback = s.has_feedback() ? json{{"kind", "ntscrt-video-feedback"}, {"version", 2}} : json();
    // Schema constants (/version, /ntsc/settings/version, fb:/kind, fb:/version): the
    // look is then upstream's file format as it is.
    for (auto &[path, v] : s.constants.items()) {
        if (path.rfind(kFbPrefix, 0) == 0) {
            if (!feedback.is_null()) set_at(feedback, path.substr(kFbLen), v);
        } else {
            set_at(look, path, v);
        }
    }
    for (size_t i = 0; i < s.params.size(); ++i) {
        const ParamDef &p = s.params[i];
        // Hidden params aren't written: CRT presets share /shader/params/<NAME> keys
        // (e.g. GAMMA_INPUT in aperture and easymode), so only the selected preset's
        // values may go into the look (docs/params.md).
        if (p.visible_if && !cond_true(s, *p.visible_if, value)) continue;
        json v = value(i);
        if (p.encode_number && v.is_boolean()) v = v.get<bool>() ? 1 : 0;
        set_at(p.feedback ? feedback : look, p.path, v);  // a malformed pointer: ignored
    }
}

Built build_look(const Schema &s, const ValueFn &value) {
    Built b;
    build_look(s, value, b.look, b.feedback);
    return b;
}

bool cond_true(const Schema &s, const Cond &c, const ValueFn &value) {
    const ParamDef *o = s.owner(c.path);
    if (!o) return true;
    json v = value(size_t(o - s.params.data()));
    return c.has_equals ? same_value(v, c.equals) : truthy(v);
}

bool is_enabled(const Schema &s, const ParamDef &p, const ValueFn &value) {
    return !p.enabled_if || cond_true(s, *p.enabled_if, value);
}

bool is_visible(const Schema &s, const ParamDef &p, const ValueFn &value) {
    return !p.visible_if || cond_true(s, *p.visible_if, value);
}

bool screen_loop_on(const json &feedback) {
    if (!feedback.is_object()) return false;
    auto it = feedback.find("enabled");
    return it == feedback.end() || truthy(*it);
}

bool temporal_stages_on(const json &look, const json &feedback) {
    if (screen_loop_on(feedback)) return true;
    for (const char *p : {"/digital/enabled", "/glitch/enabled"})
        if (auto v = value_at(look, p); v && truthy(*v)) return true;
    return false;
}

// ---- presets ----------------------------------------------------------------------
bool is_feedback_preset(const json &j) {
    return j.is_object() && j.value("kind", std::string()) == "ntscrt-video-feedback";
}

namespace {
// preset_value on a preset that is already upgraded (or a Screen Loop file).
std::optional<json> preset_value_raw(const ParamDef &p, const json &preset, bool feedback_preset) {
    if (p.feedback != feedback_preset || !p.in_presets) return std::nullopt;
    if (auto v = value_at(preset, p.path)) {
        if (auto c = coerce(p, *v)) return c;
    }
    // Upstream saves only loops that are on, so its files have no "enabled": the core's
    // "missing means on" rule. Choosing / loading one turns Screen Loop on.
    if (feedback_preset && p.type == PType::Bool && p.path == "/enabled") return json(true);
    return p.def;
}
}  // namespace

std::optional<json> preset_value(const ParamDef &p, const json &preset, bool feedback_preset) {
    if (p.feedback != feedback_preset || !p.in_presets) return std::nullopt;
    // Older looks' Digital video values mapped onto today's controls, as the
    // core does when it renders such a look (core/stages/digital/migrate.h).
    if (!feedback_preset && p.path.rfind("/digital/", 0) == 0)
        return preset_value_raw(p, ntscrt::digital::migrate::upgraded(preset), false);
    return preset_value_raw(p, preset, feedback_preset);
}

json preset_look_at_frame(const Schema &s, const json &preset, int64_t n, double fps) {
    const json up = ntscrt::digital::migrate::upgraded(preset);
    return ntscrt::timeline::look_at_frame(up, ntscrt::timeline::parse(up), s.timeline, n, fps);
}

namespace {
// A numeric value's components (a colour's channels; one for the others).
std::vector<double> components(const ParamDef &p, const json &v) {
    if (p.type == PType::Color) return color_components(p, v);
    return {v.is_number() ? v.get<double>() : v.is_boolean() ? double(v.get<bool>()) : 0.0};
}

// Frames of one pass (0..n-1) where a piecewise-linear curve through the per-frame values
// bends: between two consecutive ones, linear interpolation of the end values is within
// tol[c] of every frame's value (greedy, longest segment first). Always holds 0 and n - 1.
// Sparse keys also stay within this fraction of the value itself: small values (a chance per
// line, a noise level near 0) are followed closely, as ntsc-rs's random events depend on them.
constexpr double kKeyRelTolerance = 0.02;

// Values whose per-frame curve is keyed exactly (a key wherever it isn't a straight line): the
// Glitch receiver's timing knobs (core/stages/glitch/receiver.cpp kParams, timing = true), which
// it integrates over every earlier frame, and ntsc-rs's scale settings, whose filters change in
// steps (a 0.2% change moved pixels by 10/255 on average in test_ofx).
bool keyed_exactly(const ParamDef &p) {
    static const char *const glitch_timing[] = {"signal_strength", "vertical_hold", "horizontal_hold", "afc_speed",
                                                "hum", "head_switch", "timebase_jitter", "crinkle", "head_clog",
                                                "tracking", "search_speed"};
    const std::string g = "/glitch/values/", n = "/ntsc/settings/";
    if (p.path.rfind(g, 0) == 0) {
        for (const char *id : glitch_timing)
            if (p.path.compare(g.size(), std::string::npos, id) == 0) return true;
        return false;
    }
    return p.path.rfind(n, 0) == 0 && p.path.size() > 6 && p.path.compare(p.path.size() - 6, 6, "_scale") == 0;
}

// flat: only constant segments (integers: hosts round or truncate an interpolated int).
std::vector<int64_t> bends(const std::vector<std::vector<double>> &v, const std::vector<double> &tol, bool flat) {
    const int64_t n = int64_t(v.size());
    // Frames where the value holds (equals a neighbour's): kept exactly, so a value resting at
    // a default or at 0 between moves rests there (ntsc-rs reacts to tiny changes of some).
    std::vector<char> held(size_t(n), 0);
    for (int64_t k = 0; k < n; ++k)
        held[size_t(k)] = (k > 0 && v[size_t(k)] == v[size_t(k - 1)]) || (k + 1 < n && v[size_t(k)] == v[size_t(k + 1)]);
    std::vector<int64_t> out{0};
    int64_t a = 0;
    while (a < n - 1) {
        int64_t best = a + 1;
        for (int64_t b = a + 2; b < n; ++b) {
            bool ok = true;
            for (int64_t k = a + 1; k < b && ok; ++k) {
                const double u = flat ? 0.0 : double(k - a) / double(b - a);
                for (size_t c = 0; c < tol.size() && ok; ++c)
                    ok = std::fabs(v[size_t(a)][c] + (v[size_t(b)][c] - v[size_t(a)][c]) * u - v[size_t(k)][c]) <=
                         (held[size_t(k)] ? 1e-12 * std::max(1.0, std::fabs(v[size_t(k)][c]))
                                          : std::min(tol[c], std::max(1e-12, kKeyRelTolerance * std::fabs(v[size_t(k)][c]))));
            }
            for (size_t c = 0; c < tol.size() && ok && flat; ++c) ok = v[size_t(b)][c] == v[size_t(a)][c];
            if (!ok) break;
            best = b;
        }
        out.push_back(best);
        a = best;
    }
    return out;
}
}  // namespace

json interpolated_key_value(const ParamDef &p, const std::vector<std::pair<int64_t, json>> &keys, double n) {
    if (keys.empty()) return p.def;
    auto hi = std::lower_bound(keys.begin(), keys.end(), n, [](const auto &k, double x) { return double(k.first) < x; });
    if (hi == keys.begin()) return hi->second;
    if (hi == keys.end()) return keys.back().second;
    if (double(hi->first) == n) return hi->second;
    const auto &lo = *std::prev(hi);
    if (p.type == PType::Bool || p.type == PType::Choice) return lo.second;
    const double u = (n - double(lo.first)) / double(hi->first - lo.first);
    const auto a = components(p, lo.second), b = components(p, hi->second);
    std::vector<double> c(a.size());
    for (size_t i = 0; i < a.size(); ++i) c[i] = a[i] + (b[i] - a[i]) * u;
    if (p.type == PType::Color) return color_value(p, c);
    if (p.type == PType::Int) return json(int64_t(std::llround(c[0])));
    return json(c[0]);
}

namespace {
// The range a value's tolerance is measured against: its slider's (display) range.
double display_span(const ParamDef &p) {
    if (p.type == PType::Float) {
        if (p.has_dmin && p.has_dmax && p.dmax > p.dmin) return p.dmax - p.dmin;
        if (p.has_min && p.has_max && p.max > p.min) return p.max - p.min;
    }
    return 1;
}
}  // namespace

double key_tolerance(const ParamDef &p, double factor) {
    if (p.type != PType::Float && p.type != PType::Color) return 0;
    double tol = kKeyTolerance * display_span(p);
    if (p.path.rfind("/shader/params/", 0) == 0 && p.has_step && p.step > 0) tol = std::max(tol, 0.5 * p.step);
    return factor * tol + 1e-9;
}

bool within_key_tolerance(const Schema &s, const json &a, const json &b, std::string *where, double factor) {
    // Compare with every param's value replaced by the other's when within its tolerance.
    json x = a, y = b;
    for (const ParamDef &p : s.params) {
        auto va = value_at(x, p.path), vb = value_at(y, p.path);
        if (!va || !vb) continue;
        const double tol = key_tolerance(p, factor);
        bool ok = false;
        if (va->is_number() && vb->is_number()) {
            ok = std::fabs(va->get<double>() - vb->get<double>()) <= tol;
        } else if (va->is_array() && vb->is_array() && va->size() == vb->size()) {
            ok = true;
            for (size_t i = 0; i < va->size() && ok; ++i)
                ok = (*va)[i].is_number() && (*vb)[i].is_number() &&
                     std::fabs((*va)[i].get<double>() - (*vb)[i].get<double>()) <= tol;
        } else if (p.type == PType::Color && va->is_string() && vb->is_string()) {
            const auto ca = color_components(p, *va), cb = color_components(p, *vb);
            ok = ca.size() == cb.size();
            for (size_t i = 0; i < ca.size() && ok; ++i) ok = std::fabs(ca[i] - cb[i]) <= std::max(tol, 0.5 / 255);
        }
        if (ok) set_at(y, p.path, *va);
    }
    if (x == y) return true;
    if (where) *where = json::diff(x, y).dump();
    return false;
}

namespace {
// One pass of an animated look's per-frame curves at `fps` (shared by the host keyframes,
// preset_keys, and the procedural animation, Animation, so both give the same values).
struct Moving {
    size_t index;
    std::vector<json> v;    // the value on each frame of a pass
    std::vector<char> want; // numbers: frames of a pass with a key
    bool stepped;
};
struct PassCurves {
    int64_t pass = 0;
    std::map<size_t, json> still;  // params that hold still (or can't be keyed): the timeline's first value
    std::vector<Moving> moving;
    int64_t per_pass = 0;  // keys one pass takes as host keyframes
};
// up: an upgraded look (ntscrt::digital::migrate::upgraded) with timeline keys `t`.
PassCurves pass_curves(const Schema &s, const json &up, const ntscrt::timeline::Timeline &t, double fps,
                       bool guards) {
    namespace tl = ntscrt::timeline;
    PassCurves out;
    const int64_t P = tl::pass_frames(t, fps);
    out.pass = P;
    // One pass of looks: later passes repeat it. Per param, the frames of a pass that get a key.
    // (Built from the look without its timeline: look_at copies the look for every frame.)
    json base = up;
    if (base.is_object()) base.erase("timeline");
    std::vector<json> looks;
    looks.reserve(size_t(P));
    for (int64_t i = 0; i < P; ++i) looks.push_back(tl::look_at(base, t, s.timeline, tl::time_of_frame(i, P)));
    for (size_t pi = 0; pi < s.params.size(); ++pi) {
        const ParamDef &p = s.params[pi];
        if (p.feedback || !p.in_presets) continue;
        std::vector<json> v;
        v.reserve(looks.size());
        bool still = true;
        for (const auto &l : looks) {
            v.push_back(*preset_value_raw(p, l, false));
            if (still && !same_value(v.back(), v.front())) still = false;
        }
        if (still || !p.animatable) {  // a param the host can't key: the timeline's first value
            out.still[pi] = v.front();
            continue;
        }
        Moving m{pi, std::move(v), {}, p.type == PType::Bool || p.type == PType::Choice};
        if (m.stepped) {
            for (int64_t n = 0; n < P; ++n)  // two keys per change, the wrap back to frame 0 included
                if (!same_value(m.v[size_t(n)], m.v[size_t((n + 1) % P)])) out.per_pass += 2;
        } else {
            // Numbers: the frames of a pass where the curve bends.
            std::vector<std::vector<double>> c;
            c.reserve(m.v.size());
            for (const auto &x : m.v) c.push_back(components(p, x));
            const size_t nc = c.front().size();
            std::vector<double> tol(nc, 0.0);  // integers and keyed_exactly: exact
            const bool crt = p.path.rfind("/shader/params/", 0) == 0;
            if (p.type != PType::Int && !keyed_exactly(p)) {
                for (size_t k = 0; k < nc; ++k) {
                    double lo = c.front()[k], hi = lo;
                    for (const auto &x : c) lo = std::min(lo, x[k]), hi = std::max(hi, x[k]);
                    const double swing = hi - lo;
                    tol[k] = std::max(1e-9, kKeyTolerance * std::min(swing, display_span(p)));
                    // CRT values snap to their step: a fine grid (many steps over the swing) is
                    // followed as a line, a coarse one exactly.
                    if (crt && p.has_step && p.step > 0 && swing >= 20 * p.step) tol[k] = std::max(tol[k], 0.5 * p.step);
                }
            }
            m.want.assign(size_t(P), 0);
            for (int64_t b : bends(c, tol, p.type == PType::Int))
                for (int64_t d = guards ? -1 : 0; d <= (guards ? 1 : 0); ++d)
                    if (b + d >= 0 && b + d < P) m.want[size_t(b + d)] = 1;
            for (char w : m.want) out.per_pass += w;
        }
        out.moving.push_back(std::move(m));
    }
    return out;
}
}  // namespace

namespace {
// The keyed stretch: whole passes from the origin, as many as kMaxPresetKeys allows (at
// least one, at most kMaxKeyedFrames), covering `at` with a quarter of it before `at` when
// the effect is longer, inside the effect when its length is known. (first, frames).
std::pair<int64_t, int64_t> keyed_stretch(int64_t P, int64_t per_pass, int64_t available, int64_t at) {
    const int64_t passes = std::max<int64_t>(1, kMaxPresetKeys / std::max<int64_t>(1, per_pass));
    const int64_t cap = std::max(P, std::min(kMaxKeyedFrames, passes * P));
    const int64_t hi = available > 0 ? available - 1 : INT64_MAX / 4;
    at = std::clamp<int64_t>(at, 0, hi);
    auto down = [&](int64_t f) { return f <= 0 ? 0 : f / P * P; };
    auto ceil_pass = [&](int64_t f) { return f <= 0 ? 0 : (f + P - 1) / P * P; };
    int64_t first = down(at - cap / 4);
    if (first + cap <= at) first = down(at);
    if (hi < INT64_MAX / 4 && first + cap - 1 > hi) first = std::min(first, ceil_pass(hi - cap + 1));
    return {first, std::min(hi, first + cap - 1) - first + 1};
}
}  // namespace

PresetKeys preset_keys(const Schema &s, const json &preset, double fps, int64_t available, int64_t at,
                       bool guards) {
    namespace tl = ntscrt::timeline;
    PresetKeys out;
    if (is_feedback_preset(preset)) return out;
    const json up = ntscrt::digital::migrate::upgraded(preset);
    const tl::Timeline t = tl::parse(up);
    if (!t.animated()) return out;
    PassCurves pc = pass_curves(s, up, t, fps, guards);
    const int64_t P = pc.pass;
    out.pass = P;
    for (auto &[pi, v] : pc.still) out.keys[pi].emplace_back(0, v);
    const std::vector<Moving> &moving = pc.moving;
    const int64_t per_pass = pc.per_pass;

    const auto [first, frames] = keyed_stretch(P, per_pass, available, at);
    out.first = first;
    out.frames = frames;
    const int64_t last = first + out.frames - 1;

    for (const Moving &m : moving) {
        auto &keys = out.keys[m.index];
        if (m.stepped) {
            for (int64_t n = first; n <= last; ++n) {
                const json &x = m.v[size_t(n % P)];
                if (!keys.empty()) {
                    if (same_value(keys.back().second, x)) continue;
                    // The old value again just before the change: hosts that interpolate a
                    // checkbox / menu then still switch exactly at frame n.
                    if (keys.back().first < n - 1) keys.emplace_back(n - 1, keys.back().second);
                }
                keys.emplace_back(n, x);
            }
            continue;
        }
        for (int64_t n = first; n <= last; ++n)
            if (m.want[size_t(n % P)] || n == last) keys.emplace_back(n, m.v[size_t(n % P)]);
    }
    return out;
}

PresetPlan plan_preset(const Schema &s, const json &preset, double fps, int64_t available, int64_t at,
                       bool guards) {
    PresetPlan plan;
    auto loop_values = [&](const json &fb) {
        for (size_t i = 0; i < s.params.size(); ++i)
            if (auto v = preset_value(s.params[i], fb, true)) plan.feedback.emplace_back(i, *v);
    };
    if (is_feedback_preset(preset)) {
        loop_values(preset);
        return plan;
    }
    plan.replaces_look = true;
    plan.look = preset_keys(s, preset, fps, available, at, guards);
    if (!plan.look.animated()) {
        const json up = ntscrt::digital::migrate::upgraded(preset);
        for (size_t i = 0; i < s.params.size(); ++i)
            if (auto v = preset_value_raw(s.params[i], up, false)) plan.look.keys[i] = {{0, *v}};
    }
    // Our preset files carry the Screen Loop settings too (saved_preset).
    if (preset.is_object() && preset.contains("screen_loop") && is_feedback_preset(preset["screen_loop"]))
        loop_values(preset["screen_loop"]);
    return plan;
}

// ---- procedural animation ----------------------------------------------------------------
namespace {
const char kAnimationKind[] = "ntscrt_animation";

// The value as a host control stores and returns it (a choice as its option's value, a colour
// through its components), so a computed value builds the same look a keyframe would.
json host_round_trip(const ParamDef &p, const json &v) {
    switch (p.type) {
    case PType::Bool: return json(truthy(v));
    case PType::Int: return v.is_number() ? json(int64_t(int(v.get<int64_t>()))) : v;
    case PType::Float: return v.is_number() ? json(v.get<double>()) : v;
    case PType::Choice: {
        const int k = choice_index(p, v);
        return k >= 0 ? p.options[size_t(k)].value : v;
    }
    case PType::Color: return color_value(p, color_components(p, v));
    }
    return v;
}

// Values every key of an object section gives alike (numbers only when `numbers`).
std::map<std::string, json> alike(const json &keys, const char *section, bool numbers) {
    std::map<std::string, json> same;
    bool first = true;
    for (const auto &k : keys) {
        std::map<std::string, json> here;
        if (k.is_object()) {
            auto o = k.find(section);
            if (o != k.end() && o->is_object())
                for (auto &[name, v] : o->items())
                    if (!numbers || v.is_number()) here[name] = v;
        }
        if (first) {
            same = std::move(here);
            first = false;
            continue;
        }
        for (auto it = same.begin(); it != same.end();) {
            auto h = here.find(it->first);
            if (h == here.end() || h->second != it->second) it = same.erase(it);
            else ++it;
        }
    }
    return same;
}

// The upgraded look with the timeline values that can't change any frame left out of its keys
// (stored with every effect that plays it, so kept small): values every key gives alike.
// Glitch and Digital video numbers then live in the look itself (a key without a value takes
// the look's: the same number on every frame). ntsc-rs settings, which a key replaces as a
// whole (a setting it leaves out takes its default), are only dropped when every key has
// settings, and one stays in each key so that no key's settings become empty: the dropped ones
// then read as defaults, which never matters, as only params the full look moves are animated
// ("moving") and the others are written as still values. CRT values only go when the value
// between keys (snapped to the step grid) is the keys' value.
json reduced_look(const Schema &s, const json &up) {
    json out = up;
    auto tl = out.find("timeline");
    if (tl == out.end() || !tl->is_object()) return out;
    auto keys = tl->find("keys");
    if (keys == tl->end() || !keys->is_array() || keys->empty()) return out;
    // Digital video's structure controls never key (the look's value always counts): out of the keys.
    for (auto &k : *keys)
        if (k.is_object() && k.contains("digital") && k["digital"].is_object())
            for (const auto &id : s.timeline.digital_structure) k["digital"].erase(id);
    for (const char *section : {"glitch", "digital"}) {
        const auto same = alike(*keys, section, true);
        if (same.empty()) continue;
        json &sec = out[section];
        if (!sec.is_object()) sec = json::object();
        json &vals = sec["values"];
        if (!vals.is_object()) vals = json::object();
        for (auto &[name, v] : same) vals[name] = v;
        for (auto &k : *keys)
            if (k.is_object() && k.contains(section) && k[section].is_object())
                for (auto &[name, v] : same) k[section].erase(name);
    }
    // CRT values every key gives alike, where the value between two keys (Float arithmetic,
    // snapped to the step grid, timeline.h values_at) is that value exactly: into the look.
    {
        std::string preset;
        if (out.contains("shader") && out["shader"].is_object() && out["shader"].contains("preset") &&
            out["shader"]["preset"].is_string())
            preset = out["shader"]["preset"].get<std::string>();
        auto pm = s.timeline.shader.find(preset);
        std::map<std::string, json> same = alike(*keys, "shader", true);
        for (auto it = same.begin(); it != same.end();) {
            const double c = it->second.get<double>();
            float v = float(c);
            if (pm != s.timeline.shader.end()) {
                auto m = pm->second.find(it->first);
                if (m != pm->second.end()) {
                    if (m->second.step > 0)
                        v = m->second.min + std::round((v - m->second.min) / m->second.step) * m->second.step;
                    v = std::min(m->second.max, std::max(m->second.min, v));
                }
            }
            if (double(v) == c) ++it;
            else it = same.erase(it);
        }
        if (!same.empty()) {
            json &sec = out["shader"];
            if (!sec.is_object()) sec = json::object();
            json &vals = sec["params"];
            if (!vals.is_object()) vals = json::object();
            for (auto &[name, v] : same) vals[name] = v;
            for (auto &k : *keys)
                if (k.is_object() && k.contains("shader") && k["shader"].is_object())
                    for (auto &[name, v] : same) k["shader"].erase(name);
        }
    }
    bool all_settings = true;
    for (const auto &k : *keys) {
        auto o = k.is_object() ? k.find("ntsc") : k.end();
        if (!k.is_object() || o == k.end() || !o->is_object() || o->empty()) all_settings = false;
    }
    if (all_settings) {
        // Every frame's settings come from a key: the look's own are never read.
        if (out.contains("ntsc") && out["ntsc"].is_object() && out["ntsc"].contains("settings"))
            out["ntsc"]["settings"] = json::object();
        const auto same = alike(*keys, "ntsc", false);
        for (auto &k : *keys) {
            json &o = k["ntsc"];
            std::string keep;
            for (auto &[name, v] : o.items())
                if (!same.count(name)) {
                    keep = name;
                    break;
                }
            if (keep.empty()) keep = o.begin().key();
            for (auto &[name, v] : same)
                if (name != keep) o.erase(name);
        }
    }
    return out;
}
}  // namespace

std::string animation_text(const Schema &s, const json &preset) {
    namespace tl = ntscrt::timeline;
    if (is_feedback_preset(preset) || !preset.is_object()) return std::string();
    const json up = ntscrt::digital::migrate::upgraded(preset);
    const tl::Timeline t = tl::parse(up);
    if (!t.animated()) return std::string();
    // The params that can move: a value differs between frames only if the keys' own values
    // differ (a lerp of equal values is that value), except CRT values, which snap to their
    // step grid between keys. Which of them do move at a frame rate: Animation (pass_curves).
    json base = up;
    base.erase("timeline");
    std::vector<json> looks;
    for (const auto &k : t.keys) {
        tl::Timeline one = t;
        one.keys = {k};
        looks.push_back(tl::look_at(base, one, s.timeline, 0));
    }
    std::vector<std::string> moving;
    for (const ParamDef &p : s.params) {
        if (p.feedback || !p.in_presets || !p.animatable) continue;
        bool moves = p.path.rfind("/shader/params/", 0) == 0;
        const json first = *preset_value_raw(p, looks.front(), false);
        for (size_t k = 1; k < looks.size() && !moves; ++k)
            moves = !same_value(*preset_value_raw(p, looks[k], false), first);
        if (moves) moving.push_back(p.id);
    }
    if (moving.empty()) return std::string();
    const json out = {{"kind", kAnimationKind}, {"version", 1},      {"look", reduced_look(s, up)},
                      {"moving", moving},       {"off", json::array()}};
    return out.dump();
}

std::string animation_without(const std::string &text, const std::vector<std::string> &ids) {
    json j = json::parse(text, nullptr, false);
    if (!j.is_object() || j.value("kind", std::string()) != kAnimationKind) return text;
    json &off = j["off"];
    if (!off.is_array()) off = json::array();
    bool changed = false;
    for (const auto &id : ids) {
        if (std::find(off.begin(), off.end(), json(id)) != off.end()) continue;
        off.push_back(id);
        changed = true;
    }
    return changed ? j.dump() : text;
}

Animation::Animation(const Schema &s, const std::string &text, double fps) {
    namespace tl = ntscrt::timeline;
    if (text.empty()) return;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || j.value("kind", std::string()) != kAnimationKind || !j.contains("look")) return;
    const json &look = j["look"];
    const tl::Timeline t = tl::parse(look);
    if (!t.animated()) return;
    std::set<std::string> moving, off;
    for (const auto &x : j.value("moving", json::array()))
        if (x.is_string()) moving.insert(x.get<std::string>());
    for (const auto &x : j.value("off", json::array()))
        if (x.is_string()) off.insert(x.get<std::string>());
    PassCurves pc = pass_curves(s, look, t, fps, false);
    // The stretch host keyframes covered (frames at the rate the preset was applied at; at
    // another rate, the same time span in whole passes).
    if (auto k = j.find("keyed"); k != j.end() && k->is_object()) {
        const double kf = k->value("fps", 0.0);
        int64_t first = k->value("first", int64_t(0)), frames = k->value("frames", int64_t(0));
        if (kf > 0 && frames > 0 && pc.pass > 0) {
            if (std::fabs(kf - fps) > 1e-6) {
                const double r = fps / kf;
                first = std::llround(double(first) * r / double(pc.pass)) * pc.pass;
                frames = std::max<int64_t>(pc.pass, std::llround(double(frames) * r));
            }
            held_ = true;
            first_ = std::max<int64_t>(0, first);
            last_ = first_ + frames - 1;
        }
    }
    slot_.assign(s.params.size(), -1);
    for (Moving &m : pc.moving) {
        const ParamDef &p = s.params[m.index];
        if (!moving.count(p.id)) continue;
        if (off.count(p.id)) {
            ++taken_over_;
            continue;
        }
        Curve c;
        c.index = m.index;
        c.def = &p;
        c.stepped = m.stepped;
        for (auto &x : m.v) x = host_round_trip(p, x);
        if (!m.stepped) {
            // The keys host keyframes held within one pass, then the next pass's first frame.
            for (int64_t n = 0; n < pc.pass; ++n)
                if (m.want[size_t(n)]) c.keys.emplace_back(n, m.v[size_t(n)]);
            c.keys.emplace_back(pc.pass, m.v.front());
        }
        c.v = std::move(m.v);
        slot_[m.index] = int(curves_.size());
        curves_.push_back(std::move(c));
    }
    pass_ = pc.pass;
}

std::vector<size_t> Animation::moving() const {
    std::vector<size_t> out;
    out.reserve(curves_.size());
    for (const Curve &c : curves_) out.push_back(c.index);
    return out;
}

bool Animation::moves_under(const std::string &path_prefix) const {
    for (const Curve &c : curves_)
        if (c.def->path.rfind(path_prefix, 0) == 0) return true;
    return false;
}

json Animation::value(size_t i, double n) const {
    const Curve &c = curves_[size_t(slot_[i])];
    if (!(n >= 0)) n = 0;  // before the effect's first frame (or NaN): its first frame's value
    // The keyframes' stretch: their first value before it, their last one after it.
    if (held_) n = std::clamp(n, double(first_), double(last_));
    const double P = double(pass_);
    const double base = std::floor(n / P) * P;
    const double x = n - base;  // within the pass, [0, P)
    if (c.stepped) {
        const int64_t k = std::clamp<int64_t>(int64_t(std::floor(x)), 0, pass_ - 1);
        return c.v[size_t(k)];
    }
    if (held_) {
        // The stretch's last frame had a key of its own (the value on that frame): the segment
        // up to it runs from the key before it.
        const double last_x = double(last_) - base;
        auto hi = std::lower_bound(c.keys.begin(), c.keys.end(), x,
                                   [](const auto &k, double v) { return double(k.first) < v; });
        if (hi != c.keys.end() && double(hi->first) > last_x && hi != c.keys.begin() && double(hi->first) != x) {
            const int64_t li = int64_t(last_x);
            const std::vector<std::pair<int64_t, json>> seg = {*std::prev(hi), {li, c.v[size_t(li % pass_)]}};
            return interpolated_key_value(*c.def, seg, x);
        }
    }
    return interpolated_key_value(*c.def, c.keys, x);
}

size_t Animation::keys_per_pass() const {
    size_t n = 0;
    for (const Curve &c : curves_) {
        if (!c.stepped) {
            n += c.keys.size() - 1;
            continue;
        }
        for (size_t k = 0; k < c.v.size(); ++k)
            if (!same_value(c.v[k], c.v[(k + 1) % c.v.size()])) n += 2;
    }
    return n;
}

PresetPlan plan_preset_procedural(const Schema &s, const json &preset, double fps, int64_t available, int64_t at) {
    namespace tl = ntscrt::timeline;
    if (is_feedback_preset(preset)) return plan_preset(s, preset, 30, 0);
    PresetPlan plan;
    plan.replaces_look = true;
    // Every look param presets set: the timeline's first frame (plan_preset's keys at frame 0).
    const json up = ntscrt::digital::migrate::upgraded(preset);
    const tl::Timeline t = tl::parse(up);
    json first = up;
    if (first.is_object()) first.erase("timeline");
    if (t.animated()) first = tl::look_at(first, t, s.timeline, 0.0);
    for (size_t i = 0; i < s.params.size(); ++i)
        if (auto v = preset_value_raw(s.params[i], first, false)) plan.look.keys[i] = {{0, *v}};
    plan.animation = animation_text(s, preset);
    if (!plan.animation.empty() && fps > 0) {
        // The stretch the host keyframes would have covered (preset_keys): the animation holds
        // outside it as they did, so the pictures are the keyframed version's.
        const PassCurves pc = pass_curves(s, up, t, fps, false);
        const auto [kfirst, kframes] = keyed_stretch(pc.pass, pc.per_pass, available, at);
        json j = json::parse(plan.animation);
        j["keyed"] = {{"fps", fps}, {"first", kfirst}, {"frames", kframes}};
        plan.animation = j.dump();
        plan.look.pass = pc.pass;
        plan.look.first = kfirst;
        plan.look.frames = kframes;
    }
    if (preset.is_object() && preset.contains("screen_loop") && is_feedback_preset(preset["screen_loop"])) {
        for (size_t i = 0; i < s.params.size(); ++i)
            if (auto v = preset_value(s.params[i], preset["screen_loop"], true)) plan.feedback.emplace_back(i, *v);
    }
    return plan;
}

// ---- applies ----------------------------------------------------------------------------
std::vector<std::pair<size_t, json>> applies_values(const Schema &s, const json &applies) {
    std::vector<std::pair<size_t, json>> out;
    if (!applies.is_object()) return out;
    auto one = [&](const std::string &path, const json &v) {
        const ParamDef *o = s.owner(path);
        if (!o) return;
        if (auto c = coerce(*o, v)) out.emplace_back(size_t(o - s.params.data()), *c);
    };
    for (auto &[path, v] : applies.items()) {
        if (v.is_object()) {
            for (auto &[k, x] : v.items()) one(path + "/" + k, x);
        } else {
            one(path, v);
        }
    }
    return out;
}

std::vector<std::pair<size_t, json>> option_applies(const Schema &s, size_t pi, const json &value) {
    if (pi >= s.params.size()) return {};
    const ParamDef &p = s.params[pi];
    const int k = choice_index(p, value);
    if (k < 0) return {};
    return applies_values(s, p.options[size_t(k)].applies);
}

std::optional<json> demoted_choice(const Schema &s, size_t ci, const json &choice_value, size_t edited,
                                   const json &edited_value) {
    if (ci >= s.params.size() || edited >= s.params.size()) return std::nullopt;
    const ParamDef &c = s.params[ci];
    const int k = choice_index(c, choice_value);
    if (k < 0) return std::nullopt;
    const json &a = c.options[size_t(k)].applies;
    const ParamDef &e = s.params[edited];
    if (!a.is_object() || e.feedback != c.feedback || !a.contains(e.path)) return std::nullopt;
    auto want = coerce(e, a[e.path]);
    if (want && same_value(*want, edited_value)) return std::nullopt;
    for (const auto &o : c.options)  // the option that sets nothing ("Custom")
        if (o.applies.is_null()) return o.value;
    return std::nullopt;
}

std::vector<std::pair<size_t, json>> edit_effects(const Schema &s, size_t edited, const ValueFn &value) {
    std::vector<std::pair<size_t, json>> out;
    if (edited >= s.params.size()) return out;
    if (s.params[edited].type == PType::Choice) out = option_applies(s, edited, value(edited));
    // The params as they are once the applies are written.
    auto now = [&](size_t i) {
        for (auto it = out.rbegin(); it != out.rend(); ++it)
            if (it->first == i) return it->second;
        return value(i);
    };
    const json ev = now(edited);
    for (size_t c = 0; c < s.params.size(); ++c) {
        if (c == edited || s.params[c].type != PType::Choice || !s.params[c].has_applies()) continue;
        if (auto d = demoted_choice(s, c, now(c), edited, ev)) out.emplace_back(c, *d);
    }
    return out;
}

// ---- bundled presets ----------------------------------------------------------------------
std::string bundled_presets_json(const std::string &resource_dir) {
    // Same listing as ntscrt_bundled_presets_json (core/src/ntscrt.cpp).
    json arr = json::array();
    namespace fs = std::filesystem;
    const fs::path dir = fs::u8path(resource_dir) / "presets";
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        std::ifstream f(it->path());
        json j = json::parse(f, nullptr, false);
        if (j.is_discarded()) continue;
        auto rel = fs::relative(it->path(), dir, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        rel.replace_extension();
        const auto u8 = rel.generic_u8string();
        std::string name(reinterpret_cast<const char *>(u8.data()), u8.size());
        arr.push_back({{"name", name}, {"kind", is_feedback_preset(j) ? "feedback" : "look"}, {"json", j}});
    }
    std::sort(arr.begin(), arr.end(), [](const json &a, const json &b) { return a["name"] < b["name"]; });
    return arr.dump();
}

std::vector<Preset> parse_presets(const char *bundled_json, const json &digital_formats) {
    json j = json::parse(bundled_json ? bundled_json : "[]", nullptr, false);
    return parse_presets(j, digital_formats);
}

std::vector<Preset> parse_presets(const json &j, const json &digital_formats) {
    std::vector<Preset> out;
    if (j.is_array())
        for (const auto &e : j) {
            if (!e.is_object()) continue;
            Preset p;
            const json &n = e.contains("name") ? e["name"] : json();
            if (n.is_string()) {
                p.name = n.get<std::string>();
            } else if (n.is_array()) {
                // A core built as C++20 once serialised std::u8string as an array of code units.
                for (const auto &c : n)
                    if (c.is_number_integer()) p.name.push_back(char(c.get<int>()));
            }
            p.data = e.value("json", json::object());
            p.feedback = e.value("kind", std::string()) == "feedback" || is_feedback_preset(p.data);
            if (!p.name.empty()) out.push_back(std::move(p));
        }
    // Upstream 0.20 offers the Digital video formats in its Preset menu ("Digital video" section).
    if (digital_formats.is_array())
        for (const auto &f : digital_formats) {
            if (!f.is_object() || !f.contains("name") || !f["name"].is_string() || !f.contains("applies")) continue;
            Preset p;
            p.name = kFormatMenuPrefix + f["name"].get<std::string>();
            p.format = true;
            p.data = f["applies"];
            out.push_back(std::move(p));
        }
    // Hosts save the Preset menus by index: released presets keep their place,
    // newer ones follow (alphabetically, as the core lists them).
    const auto &released = released_preset_order();
    auto rank = [&](const Preset &p) {
        auto it = std::find(released.begin(), released.end(), p.name);
        return size_t(it - released.begin());
    };
    std::stable_sort(out.begin(), out.end(), [&](const Preset &a, const Preset &b) { return rank(a) < rank(b); });
    return out;
}

const std::vector<std::string> &released_preset_order() {
    // Every bundled preset of a released version, in that version's menu order
    // (0.4.0: the core's alphabetical list). Append, never reorder: a sync that
    // bundles new upstream presets adds them here at the next release.
    static const std::vector<std::string> names = {
        // v0.4.0
        "15 second glitches", "5 second blips", "5 second glitch", "5 seconds mild glitch", "Clean CRT",
        "Clean VHS", "Gentle waves loop", "Glitch 1", "Glitch 2", "Glitch loop 1", "Glitch pop", "Gltch 3",
        "High Chroma static", "Low Res", "Medium VHS", "Mild VHS", "Obliterated", "Readable Text",
        "Screen Loop/12 copies luma key", "Screen Loop/15 copies color drift", "Screen Loop/22 copies bottom left",
        "Screen Loop/7 copies slow luma", "Screen Loop/Bottom to top", "Static loop 1", "Tracking Noise up",
        "Two blips loop", "V Hold glitch", "Vertical Freak Out", "Vertical spike loop", "Very wavy", "Wavy loop",
        "Webcam (low quality)",
        // v0.5.0 (upstream 0.20 sync): two new looks, then upstream's Digital video formats
        "CD-ROM (low quality)", "Chunky webcam", "Digital video/Webcam (2005)", "Digital video/Video call",
        "Digital video/MiniDV", "Digital video/Early web video", "Digital video/CD-ROM (Video 1)",
        "Digital video/Datamosh",
        // v0.5.0 (upstream 0.21 sync, after rc.3): one new look
        "Cheap camcorder",
    };
    return names;
}

// ---- Save preset ----------------------------------------------------------------------------
json saved_preset(const json &look, const json &feedback) {
    json out = look;
    if (feedback.is_object()) out["screen_loop"] = feedback;
    return out;
}

std::optional<json> saved_screen_loop(const json &feedback) {
    if (!screen_loop_on(feedback)) return std::nullopt;
    return feedback;
}

// ---- host extras ------------------------------------------------------------------------------
uint64_t render_seed(int64_t v) { return uint64_t(std::clamp<int64_t>(v, kSeedMin, kSeedMax)); }

uint32_t cache_mask(const ParamDef &p, bool keyed) {
    if (p.feedback) return NTSCRT_CACHE_SCREENLOOP;
    // Screen Loop recomputes earlier frames through every stage: always.
    uint32_t m = NTSCRT_CACHE_SCREENLOOP;
    const size_t slash = p.path.find('/', 1);
    const std::string top = p.path.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
    if (top == "crop" || top == "alpha" || top == "downscale") {
        m |= NTSCRT_CACHE_DIGITAL | NTSCRT_CACHE_CRT;  // Digital encodes the cropped source; CRT history
    } else if (top == "digital") {
        m |= NTSCRT_CACHE_DIGITAL;
    } else if (top == "shader") {
        m |= NTSCRT_CACHE_CRT;
    } else if (top == "glitch") {
        // The receiver re-checks its history, which catches edits that hold
        // for every frame; a keyframed value can change between its probes.
        if (keyed) m |= NTSCRT_CACHE_GLITCH;
    } else if (top != "ntsc") {  // ntsc-rs keeps no history
        m = NTSCRT_CACHE_ALL & ~NTSCRT_CACHE_SOURCE;
    }
    return m;
}

}  // namespace ntscrt::host
