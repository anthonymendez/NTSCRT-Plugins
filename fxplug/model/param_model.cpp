#include "param_model.h"

#include "../../core/stages/digital/migrate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace ntscrt_fx {

namespace {

double num(const json &j, const char *key, double fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : fallback;
}

Kind parse_kind(const std::string &t) {
    if (t == "bool") return Kind::Bool;
    if (t == "int") return Kind::Int;
    if (t == "choice") return Kind::Choice;
    if (t == "color") return Kind::Color;
    return Kind::Float;
}

bool parse_hex_color(const std::string &s, double rgb[3]) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (int i = 0; i < 3; ++i) {
        char buf[3] = {s[1 + 2 * i], s[2 + 2 * i], 0};
        char *end = nullptr;
        long v = std::strtol(buf, &end, 16);
        if (end != buf + 2) return false;
        rgb[i] = double(v) / 255.0;
    }
    return true;
}

std::string hex_color(const double rgb[3]) {
    char buf[8];
    auto c = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c(rgb[0]), c(rgb[1]), c(rgb[2]));
    return buf;
}

// Splits "fb:/a/b" -> (true, "/a/b").
std::pair<bool, std::string> split_fb(const std::string &path) {
    if (path.rfind("fb:", 0) == 0) return {true, path.substr(3)};
    return {false, path};
}

}  // namespace

json value_at(const json &doc, const std::string &pointer) {
    if (!doc.is_object() && !doc.is_array()) return nullptr;
    try {
        json::json_pointer p(pointer);
        if (!doc.contains(p)) return nullptr;
        return doc.at(p);
    } catch (const std::exception &) {
        return nullptr;
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

    const json groups = schema.value("groups", json::array());
    for (const auto &g : groups) {
        Group G;
        G.id = g.value("id", "");
        G.label = g.value("label", G.id);
        if (g.contains("parent") && g["parent"].is_string()) G.parent = g["parent"];
        if (G.id.empty()) continue;
        G.host_id = assign("group:" + G.id);
        m.groups.push_back(std::move(G));
    }
    std::set<std::string> seen;
    for (const auto &q : schema.value("params", json::array())) {
        Param P;
        P.id = q.value("id", "");
        if (P.id.empty() || !seen.insert(P.id).second) {
            warn("schema: missing or duplicate param id '" + P.id + "'");
            continue;
        }
        P.group = q.value("group", "");
        P.label = q.value("label", P.id);
        P.hint = q.value("hint", "");
        P.kind = parse_kind(q.value("type", "float"));
        P.def = q.contains("default") ? q["default"] : json();
        auto [fb, ptr] = split_fb(q.value("path", ""));
        P.feedback = fb;
        P.pointer = ptr;
        P.animatable = q.value("animatable", P.kind == Kind::Float || P.kind == Kind::Int || P.kind == Kind::Color);
        P.in_presets = q.value("preset", true);
        if (q.contains("enabled_if") && q["enabled_if"].is_string()) P.enabled_if = q["enabled_if"];
        if (q.contains("visible_if") && q["visible_if"].is_object()) {
            P.visible_path = q["visible_if"].value("path", "");
            P.visible_equals = q["visible_if"].value("equals", json());
            P.visible_truthy = !q["visible_if"].contains("equals");
        }
        switch (P.kind) {
        case Kind::Bool:
            if (!P.def.is_boolean()) P.def = truthy(P.def);
            break;
        case Kind::Int:
        case Kind::Float: {
            const double d = P.def.is_number() ? P.def.get<double>() : 0.0;
            if (!P.def.is_number()) P.def = d;
            P.min = num(q, "min", std::min(0.0, d));
            P.max = num(q, "max", std::max({1.0, d, P.min + 1.0}));
            if (P.max < P.min) std::swap(P.min, P.max);
            P.slider_min = std::max(P.min, num(q, "display_min", P.min));
            P.slider_max = std::min(P.max, num(q, "display_max", P.max));
            if (P.slider_max <= P.slider_min) { P.slider_min = P.min; P.slider_max = P.max; }
            P.step = num(q, "step", P.kind == Kind::Int ? 1.0 : (P.slider_max - P.slider_min) / 100.0);
            if (P.step <= 0) P.step = P.kind == Kind::Int ? 1.0 : 0.01;
            break;
        }
        case Kind::Choice:
            for (const auto &o : q.value("options", json::array())) {
                if (o.is_object()) P.options.push_back({o.value("value", json()), o.value("label", o.value("value", json()).dump())});
                else P.options.push_back({o, o.is_string() ? o.get<std::string>() : o.dump()});
            }
            if (P.options.empty()) { warn("schema: choice '" + P.id + "' has no options"); continue; }
            break;
        case Kind::Color:
            break;
        }
        if (P.pointer.empty() || P.pointer[0] != '/') {
            warn("schema: param '" + P.id + "' has no JSON-Pointer path");
            continue;
        }
        P.host_id = assign(P.id);
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
    for (const auto &p : params) if (p.id == id) return &p;
    return nullptr;
}

json Model::default_host_value(const Param &p) const {
    auto h = to_host_value(p, p.def);
    if (h) return *h;
    switch (p.kind) {
    case Kind::Bool: return false;
    case Kind::Int: return int64_t(std::llround(p.min));
    case Kind::Float: return p.min;
    case Kind::Choice: return 0;
    case Kind::Color: return json::array({1.0, 1.0, 1.0});
    }
    return nullptr;
}

json Model::to_look_value(const Param &p, const json &host) const {
    switch (p.kind) {
    case Kind::Bool: return truthy(host);
    case Kind::Int: {
        double v = host.is_number() ? host.get<double>() : 0.0;
        return int64_t(std::llround(std::clamp(v, p.min, p.max)));
    }
    case Kind::Float: {
        double v = host.is_number() ? host.get<double>() : 0.0;
        return std::clamp(v, p.min, p.max);
    }
    case Kind::Choice: {
        int64_t i = host.is_number() ? host.get<int64_t>() : 0;
        i = std::clamp<int64_t>(i, 0, int64_t(p.options.size()) - 1);
        return p.options[size_t(i)].value;
    }
    case Kind::Color: {
        double rgb[3] = {1, 1, 1};
        if (host.is_array())
            for (size_t i = 0; i < 3 && i < host.size(); ++i) rgb[i] = host[i].is_number() ? host[i].get<double>() : 0.0;
        if (p.def.is_string()) return hex_color(rgb);
        json a = json::array({rgb[0], rgb[1], rgb[2]});
        if (p.def.is_array() && p.def.size() >= 4) a.push_back(p.def[3]);
        return a;
    }
    }
    return nullptr;
}

std::optional<json> Model::to_host_value(const Param &p, const json &v) const {
    switch (p.kind) {
    case Kind::Bool:
        if (v.is_boolean()) return v;
        if (v.is_number()) return v.get<double>() != 0.0;
        return std::nullopt;
    case Kind::Int:
        if (v.is_boolean()) return int64_t(v.get<bool>());
        if (!v.is_number()) return std::nullopt;
        return int64_t(std::llround(std::clamp(v.get<double>(), p.min, p.max)));
    case Kind::Float:
        if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
        if (!v.is_number()) return std::nullopt;
        return std::clamp(v.get<double>(), p.min, p.max);
    case Kind::Choice:
        for (size_t i = 0; i < p.options.size(); ++i)
            if (p.options[i].value == v) return int64_t(i);
        return std::nullopt;
    case Kind::Color: {
        double rgb[3];
        if (v.is_string() && parse_hex_color(v.get<std::string>(), rgb)) return json::array({rgb[0], rgb[1], rgb[2]});
        if (v.is_array() && v.size() >= 3 && v[0].is_number() && v[1].is_number() && v[2].is_number())
            return json::array({v[0].get<double>(), v[1].get<double>(), v[2].get<double>()});
        return std::nullopt;
    }
    }
    return std::nullopt;
}

json Model::build_look(const json &base_look, const std::function<json(const Param &)> &get) const {
    json look = base_look.is_object() ? base_look : json::object();
    for (const auto &p : params)
        if (!p.feedback && p.visible_path.empty()) set_at(look, p.pointer, to_look_value(p, get(p)));
    // Hidden params aren't written: CRT presets share /shader/params/<NAME> keys
    // (e.g. GAMMA_INPUT in aperture and easymode), so only the selected preset's
    // values may go into the look (docs/params.md). Their condition reads the look
    // built above (e.g. /shader/preset).
    for (const auto &p : params) {
        if (p.feedback || p.visible_path.empty()) continue;
        if (p.visible_truthy && p.visible_path == p.pointer) {
            // Shown (and written) only while its own value is truthy: the
            // Digital video controls kept from 0.4.0 (schema "legacy").
            json v = to_look_value(p, get(p));
            if (truthy(v)) set_at(look, p.pointer, v);
        } else if (is_visible(p, look, json())) {
            set_at(look, p.pointer, to_look_value(p, get(p)));
        }
    }
    return look;
}

std::optional<json> Model::build_feedback(bool on, const json &base, const std::function<json(const Param &)> &get) const {
    if (!on) return std::nullopt;
    json fb = base.is_object() ? base : json::object();
    if (!fb.contains("kind")) fb["kind"] = "ntscrt-video-feedback";
    for (const auto &p : params)
        if (p.feedback) set_at(fb, p.pointer, to_look_value(p, get(p)));
    return fb;
}

std::map<uint32_t, json> Model::host_values_from_preset(const json &preset_in, bool feedback) const {
    std::map<uint32_t, json> out;
    // Older looks' Digital video values mapped onto today's controls (as the core does).
    const json preset = feedback ? preset_in : ntscrt::digital::migrate::upgraded(preset_in);
    for (const auto &p : params) {
        if (p.feedback != feedback || !p.in_presets) continue;
        json v = value_at(preset, p.pointer);
        std::optional<json> h = v.is_null() ? std::nullopt : to_host_value(p, v);
        out[p.host_id] = h ? *h : default_host_value(p);
    }
    return out;
}

bool Model::truthy(const json &v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) return !v.get<std::string>().empty();
    return !v.is_null();
}

bool Model::is_enabled(const Param &p, const json &look, const json &feedback) const {
    if (p.enabled_if.empty()) return true;
    auto [fb, ptr] = split_fb(p.enabled_if);
    json v = value_at(fb ? feedback : look, ptr);
    return v.is_null() ? true : truthy(v);
}

bool Model::is_visible(const Param &p, const json &look, const json &feedback) const {
    if (p.visible_path.empty()) return true;
    auto [fb, ptr] = split_fb(p.visible_path);
    json v = value_at(fb ? feedback : look, ptr);
    return p.visible_truthy ? truthy(v) : v == p.visible_equals;
}

bool needs_history(const json &look, const std::optional<json> &feedback) {
    if (feedback) return true;
    return Model::truthy(value_at(look, "/digital/enabled")) || Model::truthy(value_at(look, "/glitch/enabled"));
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

// ---- FileLog ------------------------------------------------------------------
FileLog::FileLog(std::string path, uint64_t max_bytes) : path_(std::move(path)), max_bytes_(max_bytes) {}

std::string FileLog::folder() const {
    if (path_.empty()) return std::string();
    auto s = std::filesystem::u8path(path_).parent_path().u8string();
    return std::string(s.begin(), s.end());
}

void FileLog::write(const std::string &msg) const {
    if (path_.empty()) return;
    try {
        namespace fs = std::filesystem;
        static std::mutex mu;
        std::lock_guard<std::mutex> lk(mu);
        const fs::path p = fs::u8path(path_);
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (fs::exists(p, ec) && fs::file_size(p, ec) > max_bytes_) {
            fs::path old = p;
            old.replace_extension();
            old += ".old";
            old += p.extension();
            fs::remove(old, ec);
            fs::rename(p, old, ec);
        }
        std::ofstream f(p, std::ios::app);
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char ts[32];
        std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
        char msbuf[8];
        std::snprintf(msbuf, sizeof msbuf, ".%03d", ms);
        std::ostringstream tid;
        tid << std::this_thread::get_id();
        f << ts << msbuf << " [" << tid.str() << "] " << msg << '\n';
    } catch (...) {
    }
}

}  // namespace ntscrt_fx
