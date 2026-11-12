#include "param_model.h"

#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ntscrt_fx {

uint32_t fallback_id(const std::string &key, const std::set<uint32_t> &used) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (unsigned char c : key) { h ^= c; h *= 16777619u; }
    const uint32_t span = kLastFallbackID - kFirstFallbackID + 1;
    uint32_t id = kFirstFallbackID + h % span;
    for (uint32_t n = 0; n < span && used.count(id); ++n) id = id == kLastFallbackID ? kFirstFallbackID : id + 1;
    return id;
}

Model Model::from_schema(const json &schema, const json &id_map, std::vector<std::string> *warnings) {
    auto warn = [&](std::string s) { if (warnings) warnings->push_back(std::move(s)); };
    Model m;
    m.schema = host::Schema::parse(schema);
    json ids = id_map.is_object() && id_map.contains("ids") ? id_map["ids"] : json::object();
    std::set<uint32_t> used;
    for (auto &[k, v] : ids.items()) {
        if (v.is_number_unsigned() || v.is_number_integer()) used.insert(v.get<uint32_t>());
    }
    auto assign = [&](const std::string &key) -> uint32_t {
        auto it = ids.find(key);
        if (it != ids.end() && it->is_number()) {
            uint32_t id = it->get<uint32_t>();
            if (id >= kFirstMappedID && id <= kLastMappedID) return id;
            warn("param_ids.json: id for '" + key + "' out of range");
        }
        uint32_t id = fallback_id(key, used);
        used.insert(id);
        warn("param_ids.json has no id for '" + key + "' (using unstable fallback " + std::to_string(id) +
             "; run fxplug/tools/update_param_ids.py)");
        return id;
    };

    for (const auto &g : m.schema.groups) m.groups.push_back({g.id, g.label, g.parent, assign("group:" + g.id)});
    for (size_t i = 0; i < m.schema.params.size(); ++i) {
        Param P;
        static_cast<host::ParamDef &>(P) = m.schema.params[i];
        P.index = i;
        P.host_id = assign(P.id);
        if (P.type == Kind::Int || P.type == Kind::Float) {
            // FxPlug sliders need a finite range: the schema's, else one around the default.
            const double d = P.def.is_number() ? P.def.get<double>() : 0.0;
            P.ui_min = P.has_min ? P.min : std::min(0.0, d);
            P.ui_max = P.has_max ? P.max : std::max({1.0, d, P.ui_min + 1.0});
            if (P.ui_max < P.ui_min) std::swap(P.ui_min, P.ui_max);
            P.slider_min = std::max(P.ui_min, P.has_dmin ? P.dmin : P.ui_min);
            P.slider_max = std::min(P.ui_max, P.has_dmax ? P.dmax : P.ui_max);
            if (P.slider_max <= P.slider_min) { P.slider_min = P.ui_min; P.slider_max = P.ui_max; }
            P.ui_step = P.has_step ? P.step : (P.type == Kind::Int ? 1.0 : (P.slider_max - P.slider_min) / 100.0);
            if (P.ui_step <= 0) P.ui_step = P.type == Kind::Int ? 1.0 : 0.01;
        }
        m.params.push_back(std::move(P));
    }

    // Layout: groups nested by parent, each group's params in schema order, then child groups.
    std::set<std::string> group_ids;
    for (const auto &g : m.groups) group_ids.insert(g.id);
    std::set<std::string> emitted;
    std::function<void(size_t)> emit = [&](size_t gi) {
        const Group &g = m.groups[gi];
        if (!emitted.insert(g.id).second) return;  // cycle guard
        m.layout.push_back({UiItem::StartGroup, gi});
        for (size_t pi = 0; pi < m.params.size(); ++pi)
            if (m.params[pi].group == g.id) m.layout.push_back({UiItem::Control, pi});
        for (size_t ci = 0; ci < m.groups.size(); ++ci)
            if (m.groups[ci].parent == g.id) emit(ci);
        m.layout.push_back({UiItem::EndGroup, gi});
    };
    for (size_t gi = 0; gi < m.groups.size(); ++gi)
        if (m.groups[gi].parent.empty() || !group_ids.count(m.groups[gi].parent)) emit(gi);
    for (size_t gi = 0; gi < m.groups.size(); ++gi)
        if (!emitted.count(m.groups[gi].id)) { warn("schema: group cycle at '" + m.groups[gi].id + "'"); emit(gi); }
    for (size_t pi = 0; pi < m.params.size(); ++pi)
        if (!group_ids.count(m.params[pi].group)) m.layout.push_back({UiItem::Control, pi});

    std::set<uint32_t> all;
    for (const auto &g : m.groups) if (!all.insert(g.host_id).second) warn("duplicate host id " + std::to_string(g.host_id));
    for (const auto &p : m.params) if (!all.insert(p.host_id).second) warn("duplicate host id " + std::to_string(p.host_id));
    return m;
}

const Param *Model::find_host(uint32_t host_id) const {
    for (const auto &p : params) if (p.host_id == host_id) return &p;
    return nullptr;
}

const Param *Model::find(const std::string &id) const {
    auto i = schema.index_of(id);
    return i ? &params[*i] : nullptr;
}

json Model::default_host_value(const Param &p) const {
    if (auto h = to_host_value(p, p.def)) return *h;
    switch (p.type) {
    case Kind::Bool: return false;
    case Kind::Int: return int64_t(std::llround(p.ui_min));
    case Kind::Float: return p.ui_min;
    case Kind::Choice: return 0;
    case Kind::Color: return json::array({1.0, 1.0, 1.0});
    }
    return nullptr;
}

json Model::to_look_value(const Param &p, const json &h) const {
    switch (p.type) {
    case Kind::Choice: {
        // An index outside the menu reads as the default option (as the OpenFX plug-in does).
        const int64_t i = h.is_number() ? h.get<int64_t>() : -1;
        if (i >= 0 && size_t(i) < p.options.size()) return p.options[size_t(i)].value;
        return p.def;
    }
    case Kind::Color: {
        std::vector<double> c = host::color_components(p, p.def);  // alpha from the default
        if (h.is_array())
            for (size_t i = 0; i < 3 && i < h.size(); ++i) c[i] = h[i].is_number() ? h[i].get<double>() : 0.0;
        return host::color_value(p, c);
    }
    default: {
        auto v = host::coerce(p, h);
        return v ? *v : p.def;
    }
    }
}

std::optional<json> Model::to_host_value(const Param &p, const json &v) const {
    auto c = host::coerce(p, v);
    if (!c) return std::nullopt;
    switch (p.type) {
    case Kind::Choice: return json(int64_t(host::choice_index(p, *c)));
    case Kind::Color: {
        const auto rgb = host::color_components(p, *c);
        return json::array({rgb[0], rgb[1], rgb[2]});
    }
    default: return c;
    }
}

HostValues Model::to_host_values(const std::vector<std::pair<size_t, json>> &vals) const {
    HostValues out;
    for (const auto &[i, v] : vals) {
        if (i >= params.size()) continue;
        auto h = to_host_value(params[i], v);
        out[params[i].host_id] = h ? *h : default_host_value(params[i]);
    }
    return out;
}

host::ValueFn Model::look_values(const HostGetter &get) const {
    return [this, &get](size_t i) { return to_look_value(params[i], get(params[i])); };
}

host::Built Model::build(const HostGetter &get) const { return host::build_look(schema, look_values(get)); }

bool Model::is_enabled(const Param &p, const HostGetter &get) const {
    return host::is_enabled(schema, p, look_values(get));
}

bool Model::is_visible(const Param &p, const HostGetter &get) const {
    return host::is_visible(schema, p, look_values(get));
}

std::vector<host::Preset> Model::look_menu(const json &bundled) const {
    std::vector<host::Preset> out;
    for (auto &p : host::parse_presets(bundled, schema.digital_formats))
        if (!p.feedback) out.push_back(std::move(p));
    return out;
}

std::vector<host::Preset> Model::loop_menu(const json &bundled) const {
    std::vector<host::Preset> out;
    for (auto &p : host::parse_presets(bundled))
        if (p.feedback) out.push_back(std::move(p));
    return out;
}

HostPlan Model::plan_preset(const json &preset, double fps, int64_t available, int64_t at) const {
    const host::PresetPlan p = host::plan_preset(schema, preset, fps, available, at);
    HostPlan out;
    out.replaces_look = p.replaces_look;
    out.pass = p.look.pass;
    out.first = p.look.first;
    out.frames = p.look.frames;
    for (const auto &[i, entries] : p.look.keys) {
        auto &keys = out.look[params[i].host_id];
        for (const auto &[n, v] : entries) {
            auto h = to_host_value(params[i], v);
            keys.emplace_back(n, h ? *h : default_host_value(params[i]));
        }
    }
    out.feedback = to_host_values(p.feedback);
    return out;
}

HostValues Model::applies_host_values(const json &applies) const {
    return to_host_values(host::applies_values(schema, applies));
}

HostValues Model::edit_effects(const Param &edited, const HostGetter &get) const {
    return to_host_values(host::edit_effects(schema, edited.index, look_values(get)));
}

HostValues Model::screen_loop_switch(bool on) const {
    auto it = schema.by_fb_path.find("/enabled");
    if (it == schema.by_fb_path.end()) return {};
    return {{params[it->second].host_id, json(on)}};
}

uint32_t Model::cache_mask(uint32_t pid, bool keyed) const {
    switch (pid) {
    case kPresetMenu:
    case kLoadPreset:
    case kSavePreset:
    case kScreenLoopMenu:  // menus write their values as edits of the params below
    case kHistoryFrames:
    case kBaseLook:        // unused since 0.5.0
    case kBaseFeedback:
    case kDebugLogging:
    case kOpenLogs:
    case kDebugAll:
    case kTopGroup:
    case kExtrasGroup: return 0;
    default: break;
    }
    const Param *p = find_host(pid);
    if (!p) return NTSCRT_CACHE_ALL;  // the seed, anything unknown
    return host::cache_mask(*p, keyed);
}

std::string RenderState::serialize() const {
    json j;
    j["v"] = 1;
    j["frame"] = frame;
    j["fps"] = fps;
    j["fdv"] = frame_duration_value;
    j["fdt"] = frame_duration_timescale;
    j["history"] = history;
    j["look"] = look;
    j["feedback"] = feedback;
    j["seed"] = std::to_string(seed);  // keep all 64 bits
    json past = json::object();
    for (const auto &[f, s] : past_looks) past[std::to_string(f)] = s;
    j["past"] = std::move(past);
    if (debug) j["debug"] = true;
    return j.dump();
}

std::optional<RenderState> RenderState::parse(const std::string &bytes) {
    json j = json::parse(bytes, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("v", 0) != 1) return std::nullopt;
    RenderState s;
    try {
        s.frame = j.value("frame", int64_t(0));
        s.fps = j.value("fps", 30.0);
        s.frame_duration_value = j.value("fdv", int64_t(1001));
        s.frame_duration_timescale = j.value("fdt", int32_t(30000));
        s.history = j.value("history", 0);
        s.look = j.value("look", std::string("{}"));
        s.feedback = j.value("feedback", std::string());
        s.seed = std::strtoull(j.value("seed", std::string("0")).c_str(), nullptr, 10);
        if (j.contains("past"))
            for (auto &[k, v] : j["past"].items()) s.past_looks[std::strtoll(k.c_str(), nullptr, 10)] = v.get<std::string>();
        s.debug = j.value("debug", false);
    } catch (const std::exception &) {
        return std::nullopt;
    }
    return s;
}

int64_t frame_from_offset(int64_t ov, int32_t ot, int64_t fv, int32_t ft) {
    if (ot == 0 || fv == 0 || ft == 0) return 0;
    long double frames = (static_cast<long double>(ov) * ft) / (static_cast<long double>(ot) * fv);
    return static_cast<int64_t>(std::llround(static_cast<double>(frames)));
}

}  // namespace ntscrt_fx
