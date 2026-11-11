#include "../ntscrt_host/params.h"

#include <ntscrt/ntscrt.h>

#include "../../core/stages/digital/migrate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
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

PresetKeys preset_keys(const Schema &s, const json &preset, double fps, int64_t available) {
    namespace tl = ntscrt::timeline;
    PresetKeys out;
    if (is_feedback_preset(preset)) return out;
    const json up = ntscrt::digital::migrate::upgraded(preset);
    const tl::Timeline t = tl::parse(up);
    if (!t.animated()) return out;
    out.pass = tl::pass_frames(t, fps);
    out.frames = available > 0 ? std::min(available, kMaxKeyedFrames) : out.pass;
    // One pass of looks: later passes repeat it.
    std::vector<json> looks;
    looks.reserve(size_t(out.pass));
    for (int64_t i = 0; i < out.pass; ++i) looks.push_back(tl::look_at(up, t, s.timeline, tl::time_of_frame(i, out.pass)));
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
        auto &keys = out.keys[pi];
        if (still || !p.animatable) {  // a param the host can't key: the timeline's first value
            keys.emplace_back(0, v.front());
            continue;
        }
        const bool stepped = p.type == PType::Bool || p.type == PType::Choice;
        for (int64_t n = 0; n < out.frames; ++n) {
            const json &x = v[size_t(n % out.pass)];
            if (stepped && !keys.empty()) {
                if (same_value(keys.back().second, x)) continue;
                // The old value again just before the change: hosts that interpolate a
                // checkbox / menu (Fusion) then still switch exactly at frame n.
                if (keys.back().first < n - 1) keys.emplace_back(n - 1, keys.back().second);
            }
            keys.emplace_back(n, x);
        }
    }
    return out;
}

PresetPlan plan_preset(const Schema &s, const json &preset, double fps, int64_t available) {
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
    plan.look = preset_keys(s, preset, fps, available);
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
