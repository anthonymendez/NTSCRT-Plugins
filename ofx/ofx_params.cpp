#include "ofx_params.h"

#include "../core/stages/digital/migrate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace ntscrt_ofx {
namespace {
const char kFbPrefix[] = "fb:";
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

bool json_equal(const json &a, const json &b) {
    if (a.is_number() && b.is_number()) return std::fabs(a.get<double>() - b.get<double>()) < 1e-9;
    return a == b;
}

bool truthy(const json &v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) return !v.get<std::string>().empty();
    return !v.is_null();
}

std::optional<json> lookup(const json &root, const std::string &path) {
    try {
        json::json_pointer ptr(path);
        if (!root.contains(ptr)) return std::nullopt;
        return root.at(ptr);
    } catch (const std::exception &) {
        return std::nullopt;
    }
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
}  // namespace

std::string host_name(const std::string &id) {
    std::string r = id;
    for (char &c : r) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) c = '_';
    }
    if (r.empty() || (r[0] >= '0' && r[0] <= '9')) r = "p_" + r;
    return r;
}

Schema Schema::parse(const char *schema_json) {
    json j = json::parse(schema_json ? schema_json : "{}");
    Schema s;
    for (const auto &g : j.value("groups", json::array())) {
        GroupDef d;
        d.id = g.value("id", std::string());
        if (d.id.empty()) continue;
        d.name = "grp_" + host_name(d.id);
        d.label = g.value("label", d.id);
        if (g.contains("parent") && g["parent"].is_string()) d.parent = g["parent"].get<std::string>();
        s.groups.push_back(std::move(d));
    }
    for (const auto &pj : j.value("params", json::array())) {
        ParamDef p;
        p.id = pj.value("id", std::string());
        std::string type = pj.value("type", std::string());
        std::string path = pj.value("path", std::string());
        if (p.id.empty() || path.empty()) continue;
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
            p.path = path.substr(sizeof(kFbPrefix) - 1);
        } else {
            p.path = path;
        }
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
                    p.options.emplace_back(v, label);
                    p.option_applies.push_back(o.contains("applies") && o["applies"].is_object() ? o["applies"] : json());
                } else {
                    p.options.emplace_back(o, o.is_string() ? o.get<std::string>() : o.dump());
                    p.option_applies.push_back(json());
                }
            }
            if (p.options.empty()) continue;
        }
        json d = pj.value("default", json());
        if (p.type == PType::Color) {
            if (d.is_string()) p.color_hex = true;
            else if (d.is_array() && d.size() == 4) p.color_n = 4;
        }
        auto cd = coerce(p, d);
        if (cd) {
            p.def = *cd;
        } else {
            switch (p.type) {
            case PType::Bool: p.def = false; break;
            case PType::Int: p.def = int64_t(p.has_min ? std::llround(p.min) : 0); break;
            case PType::Float: p.def = p.has_min ? p.min : 0.0; break;
            case PType::Choice: p.def = p.options[0].first; break;
            case PType::Color: p.def = color_value(p, std::vector<double>(size_t(p.color_n), 0.0)); break;
            }
        }
        (p.feedback ? s.by_fb_path : s.by_look_path)[p.path] = s.params.size();
        s.params.push_back(std::move(p));
    }
    if (j.contains("constants") && j["constants"].is_object()) s.constants = j["constants"];
    if (j.contains("digital_formats") && j["digital_formats"].is_array()) s.digital_formats = j["digital_formats"];
    s.timeline = ntscrt::timeline::Rules::from_schema(j);
    return s;
}

const ParamDef *Schema::owner(const std::string &cond_path) const {
    if (cond_path.rfind(kFbPrefix, 0) == 0) {
        auto it = by_fb_path.find(cond_path.substr(sizeof(kFbPrefix) - 1));
        return it == by_fb_path.end() ? nullptr : &params[it->second];
    }
    auto it = by_look_path.find(cond_path);
    return it == by_look_path.end() ? nullptr : &params[it->second];
}

bool is_feedback_preset(const json &j) {
    return j.is_object() && j.value("kind", std::string()) == "ntscrt-video-feedback";
}

std::vector<Preset> parse_presets(const char *presets_json, const json &digital_formats) {
    std::vector<Preset> out;
    json j = json::parse(presets_json ? presets_json : "[]", nullptr, false);
    if (!j.is_array()) return out;
    for (const auto &e : j) {
        if (!e.is_object()) continue;
        Preset p;
        const json &n = e.contains("name") ? e["name"] : json();
        if (n.is_string()) {
            p.name = n.get<std::string>();
        } else if (n.is_array()) {
            // Wave-0 core serialises std::u8string (C++20) as an array of code units.
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

std::optional<json> coerce(const ParamDef &p, const json &v) {
    switch (p.type) {
    case PType::Bool:
        if (v.is_boolean()) return v;
        if (v.is_number()) return json(v.get<double>() != 0.0);
        return std::nullopt;
    case PType::Int:
        if (v.is_number()) return json(int64_t(std::llround(v.get<double>())));
        if (v.is_boolean()) return json(int64_t(v.get<bool>() ? 1 : 0));
        return std::nullopt;
    case PType::Float:
        if (v.is_number()) return json(v.get<double>());
        if (v.is_boolean()) return json(v.get<bool>() ? 1.0 : 0.0);
        return std::nullopt;
    case PType::Choice: {
        int i = choice_index(p, v);
        if (i < 0 && v.is_string()) {  // tolerate a label
            for (size_t k = 0; k < p.options.size(); ++k)
                if (p.options[k].second == v.get<std::string>()) i = int(k);
        }
        if (i < 0) return std::nullopt;
        return p.options[size_t(i)].first;
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
        if (json_equal(p.options[k].first, value)) return int(k);
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

void build_look(const Schema &s, const ValueFn &value, json &look, json &feedback) {
    look = json::object();
    feedback = s.has_feedback() ? json{{"kind", "ntscrt-video-feedback"}, {"version", 2}} : json();
    // Schema constants (/version, /ntsc/settings/version, fb:/kind, fb:/version): the
    // look is then upstream's file format as it is.
    for (auto &[path, v] : s.constants.items()) {
        try {
            if (path.rfind(kFbPrefix, 0) == 0) {
                if (!feedback.is_null()) feedback[json::json_pointer(path.substr(sizeof(kFbPrefix) - 1))] = v;
            } else {
                look[json::json_pointer(path)] = v;
            }
        } catch (const std::exception &) {
        }
    }
    for (size_t i = 0; i < s.params.size(); ++i) {
        const ParamDef &p = s.params[i];
        // Hidden params aren't written: CRT presets share /shader/params/<NAME> keys
        // (e.g. GAMMA_INPUT in aperture and easymode), so only the selected preset's
        // values may go into the look (docs/params.md).
        if (p.visible_if && !cond_true(s, *p.visible_if, value)) continue;
        try {
            json::json_pointer ptr(p.path);
            json v = value(i);
            if (p.encode_number && v.is_boolean()) v = v.get<bool>() ? 1 : 0;
            (p.feedback ? feedback : look)[ptr] = std::move(v);
        } catch (const std::exception &) {
            // malformed pointer in schema: ignore this param
        }
    }
}

namespace {
// preset_value on a preset that is already upgraded (or a feedback preset).
std::optional<json> preset_value_raw(const ParamDef &p, const json &preset, bool feedback_preset) {
    if (p.feedback != feedback_preset || !p.in_presets) return std::nullopt;
    if (auto v = lookup(preset, p.path)) {
        if (auto c = coerce(p, *v)) return c;
    }
    if (feedback_preset && p.type == PType::Bool && p.path == "/enabled") return json(true);
    return p.def;
}
}  // namespace

std::optional<json> preset_value(const ParamDef &p, const json &preset_in, bool feedback_preset) {
    if (p.feedback != feedback_preset || !p.in_presets) return std::nullopt;
    // Older looks' Digital video values mapped onto today's controls, as the
    // core does when it renders such a look (core/stages/digital/migrate.h).
    if (!feedback_preset && p.path.rfind("/digital/", 0) == 0)
        return preset_value_raw(p, ntscrt::digital::migrate::upgraded(preset_in), false);
    return preset_value_raw(p, preset_in, feedback_preset);
}

std::vector<std::pair<size_t, json>> option_applies(const Schema &s, size_t pi, const json &value) {
    if (pi >= s.params.size()) return {};
    const ParamDef &p = s.params[pi];
    const int k = choice_index(p, value);
    if (k < 0 || size_t(k) >= p.option_applies.size()) return {};
    return applies_values(s, p.option_applies[size_t(k)]);
}

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

std::optional<json> demoted_choice(const Schema &s, size_t ci, const json &choice_value, size_t edited,
                                   const json &edited_value) {
    if (ci >= s.params.size() || edited >= s.params.size()) return std::nullopt;
    const ParamDef &c = s.params[ci];
    const int k = choice_index(c, choice_value);
    if (k < 0 || size_t(k) >= c.option_applies.size()) return std::nullopt;
    const json &a = c.option_applies[size_t(k)];
    const ParamDef &e = s.params[edited];
    if (!a.is_object() || e.feedback != c.feedback || !a.contains(e.path)) return std::nullopt;
    auto want = coerce(e, a[e.path]);
    if (want && json_equal(*want, edited_value)) return std::nullopt;
    for (size_t i = 0; i < c.options.size(); ++i)  // the option that sets nothing ("Custom")
        if (i < c.option_applies.size() && c.option_applies[i].is_null()) return c.options[i].first;
    return std::nullopt;
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
            if (still && !json_equal(v.back(), v.front())) still = false;
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
                if (json_equal(keys.back().second, x)) continue;
                // The old value again just before the change: hosts that interpolate a
                // checkbox / menu (Fusion) then still switch exactly at frame n.
                if (keys.back().first < n - 1) keys.emplace_back(n - 1, keys.back().second);
            }
            keys.emplace_back(n, x);
        }
    }
    return out;
}

bool cond_true(const Schema &s, const Cond &c, const ValueFn &value) {
    const ParamDef *o = s.owner(c.path);
    if (!o) return true;
    json v = value(size_t(o - s.params.data()));
    return c.has_equals ? json_equal(v, c.equals) : truthy(v);
}
}  // namespace ntscrt_ofx
