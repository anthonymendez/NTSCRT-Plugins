// gen_ofx_reference.cpp — ONE-OFF generator, built against the 0.5.0-dev OpenFX
// param model (ofx/ofx_params.cpp) before it was replaced by the shared host
// param library. It replays what ofx/ntscrt_ofx.cpp did for every bundled
// preset (apply, then build the look at several frames) on a simulated host and
// writes the looks / feedback JSON to tests/hostparams/ofx_reference.json, which
// test_ofx_reference.cpp compares the shared library's OFX path against.
//
//   gen_ofx_reference <out.json>
#include <ntscrt/ntscrt.h>

#include "ofx_params.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>

using namespace ntscrt_ofx;

namespace {
// Merge patch from a to b (objects recurse; removed keys -> null). Looks hold no nulls.
json diff(const json &a, const json &b) {
    if (!a.is_object() || !b.is_object()) return b;
    json d = json::object();
    for (auto &[k, v] : b.items()) {
        auto it = a.find(k);
        if (it == a.end()) d[k] = v;
        else if (*it != v) d[k] = diff(*it, v);
    }
    for (auto &[k, v] : a.items())
        if (!b.contains(k)) d[k] = nullptr;
    return d;
}

// The OpenFX host as ntscrt_ofx.cpp drives it: one value per param, keyframes.
struct Host {
    const Schema &s;
    std::vector<json> val;
    std::vector<std::map<int64_t, json>> keys;
    explicit Host(const Schema &sc) : s(sc), val(sc.params.size()), keys(sc.params.size()) {
        for (size_t i = 0; i < s.params.size(); ++i) val[i] = s.params[i].def;
    }
    json at(size_t i, int64_t n) const {
        if (keys[i].empty()) return val[i];
        auto it = keys[i].upper_bound(n);
        if (it == keys[i].begin()) return it->second;
        return std::prev(it)->second;
    }
    void set_value(size_t i, const json &v, int64_t t) {
        if (!keys[i].empty()) keys[i][t] = v;
        else val[i] = v;
    }
    // ntscrt_ofx.cpp apply_look_preset (t = 0, the clip starts at 0)
    void apply_look_preset(const json &preset, double fps, int64_t available) {
        const PresetKeys pk = preset_keys(s, preset, fps, available);
        for (size_t i = 0; i < s.params.size(); ++i) {
            const ParamDef &p = s.params[i];
            if (p.feedback || !p.in_presets) continue;
            keys[i].clear();
            if (pk.animated()) {
                auto it = pk.keys.find(i);
                if (it == pk.keys.end() || it->second.empty()) continue;
                if (it->second.size() == 1) set_value(i, it->second.front().second, 0);
                else
                    for (const auto &[n, v] : it->second) keys[i][n] = v;
            } else if (auto v = preset_value(p, preset, false)) {
                set_value(i, *v, 0);
            }
        }
    }
    // ntscrt_ofx.cpp apply_preset
    void apply_preset(const json &preset, bool feedback, double fps, int64_t available) {
        if (feedback) {
            for (size_t i = 0; i < s.params.size(); ++i)
                if (auto v = preset_value(s.params[i], preset, true)) set_value(i, *v, 0);
        } else {
            apply_look_preset(preset, fps, available);
            if (preset.is_object() && preset.contains("screen_loop") && is_feedback_preset(preset["screen_loop"]))
                for (size_t i = 0; i < s.params.size(); ++i)
                    if (auto v = preset_value(s.params[i], preset["screen_loop"], true)) set_value(i, *v, 0);
        }
    }
    void apply_values(const std::vector<std::pair<size_t, json>> &vals) {
        for (const auto &[i, v] : vals) set_value(i, v, 0);
    }
    // ntscrt_ofx.cpp apply_choice_effects after the user set param i to v
    void user_edit(size_t i, const json &v) {
        set_value(i, v, 0);
        if (s.params[i].type == PType::Choice) apply_values(option_applies(s, i, at(i, 0)));
        const json edited = at(i, 0);
        for (size_t c = 0; c < s.params.size(); ++c) {
            if (c == i || s.params[c].type != PType::Choice || s.params[c].option_applies.empty()) continue;
            if (auto d = demoted_choice(s, c, at(c, 0), i, edited)) apply_values({{c, *d}});
        }
    }
    void look(int64_t n, json &l, json &f) const {
        build_look(s, [&](size_t i) { return at(i, n); }, l, f);
    }
};

size_t index_of(const Schema &s, const std::string &id) {
    for (size_t i = 0; i < s.params.size(); ++i)
        if (s.params[i].id == id) return i;
    throw std::runtime_error("no param " + id);
}
}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: gen_ofx_reference <out.json>\n";
        return 2;
    }
    const Schema s = Schema::parse(ntscrt_param_schema_json());
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_BUILT_RESOURCES_DIR};
    ntscrt_instance *inst = nullptr;
    if (ntscrt_create(&ci, &inst) != NTSCRT_OK || !inst) {
        std::cerr << "ntscrt_create failed\n";
        return 1;
    }
    std::vector<Preset> looks, loops;
    for (auto &p : parse_presets(ntscrt_bundled_presets_json(inst), s.digital_formats))
        (p.feedback ? loops : looks).push_back(std::move(p));
    ntscrt_destroy(inst);

    json out;
    out["generated_by"] = "tests/hostparams/gen_ofx_reference.cpp (OpenFX param model of 0.5.0-dev, before the shared library)";
    json menu = json::array(), loop_menu = json::array();
    for (const auto &p : looks) menu.push_back(p.name);
    for (const auto &p : loops) loop_menu.push_back(p.name);
    out["look_menu"] = menu;
    out["loop_menu"] = loop_menu;

    json def_look, def_fb;
    Host(s).look(0, def_look, def_fb);
    out["default"] = {{"look", def_look}, {"feedback", def_fb}};
    json cases = json::object();
    auto record = [&](const std::string &name, const Host &h, int64_t n) {
        json l, f;
        h.look(n, l, f);
        cases[name] = {{"look", diff(def_look, l)}, {"feedback", diff(def_fb, f)}};
    };

    const int64_t kAvailable = 300;
    for (const auto &p : looks) {
        Host h(s);
        if (p.format) {
            h.apply_values(applies_values(s, p.data));
            record("format/" + p.name, h, 0);
            continue;
        }
        for (double fps : {30.0, 24.0}) {
            Host k(s);
            k.apply_preset(p.data, false, fps, kAvailable);
            const bool animated = preset_keys(s, p.data, fps, kAvailable).animated();
            if (!animated) {
                if (fps == 30.0) record("look/" + p.name, k, 0);
                continue;
            }
            const std::vector<int64_t> frames = fps == 30.0 ? std::vector<int64_t>{0, 1, 7, 29, 30, 59, 60, 61, 149, 299, 400}
                                                            : std::vector<int64_t>{0, 5, 47, 100};
            for (int64_t n : frames) record("look/" + p.name + "@" + std::to_string(int(fps)) + "fps/" + std::to_string(n), k, n);
        }
    }
    for (const auto &p : loops) {
        Host h(s);
        h.apply_preset(p.data, true, 30, kAvailable);
        record("loop/" + p.name, h, 0);
    }

    // Behaviours the shared library has to keep for the OpenFX plug-in.
    auto find = [&](const std::vector<Preset> &v, const std::string &n) -> const json & {
        for (const auto &p : v)
            if (p.name == n) return p.data;
        throw std::runtime_error("no preset " + n);
    };
    {  // a look file with the Screen Loop settings riding along (Save preset writes them)
        json j = find(looks, "Clean VHS");
        j["screen_loop"] = find(loops, "Screen Loop/Bottom to top");
        Host h(s);
        h.apply_preset(j, false, 30, kAvailable);
        record("file/look with screen_loop", h, 0);
    }
    {  // a Screen Loop file that says it is off
        json j = find(loops, "Screen Loop/12 copies luma key");
        j["enabled"] = false;
        Host h(s);
        h.apply_preset(j, true, 30, kAvailable);
        record("file/loop enabled false", h, 0);
    }
    {  // a Digital video look saved before upstream 0.19 (version 1): migrated
        json j = {{"version", 1},
                  {"digital", {{"enabled", true}, {"values", {{"quality", 0.22}, {"exposure", 0.3}, {"lens_blur", 0.2}}}}}};
        Host h(s);
        h.apply_preset(j, false, 30, kAvailable);
        record("file/digital version 1", h, 0);
    }
    {  // choice labels, numbers for bools, number spellings, unknown keys
        json j = {{"downscale", {{"method", "Chunky"}, {"enabled", 1}, {"width", 300.4}}},
                  {"shader", {{"preset", "CRT Easymode"}}},
                  {"glitch", {{"enabled", 0}, {"values", {{"closed_captions", 0}}}}},
                  {"digital", {{"values", {{"codec", 1.0}}}}},
                  {"unknown", {{"x", 1}}}};
        Host h(s);
        h.apply_preset(j, false, 30, kAvailable);
        record("file/labels and spellings", h, 0);
    }
    {  // Horizontal resolution sets the width; editing the width demotes it to Custom
        Host h(s);
        const size_t pre = index_of(s, "downscale.preset"), w = index_of(s, "downscale.width");
        h.user_edit(pre, "Arcade (384px)");
        record("edit/horizontal resolution arcade", h, 0);
        h.user_edit(w, int64_t(400));
        record("edit/width 400 demotes", h, 0);
    }
    {  // a Digital video format over a look
        Host h(s);
        h.apply_preset(find(looks, "Clean VHS"), false, 30, kAvailable);
        h.apply_values(applies_values(s, find(looks, "Digital video/MiniDV")));
        record("format over look/Clean VHS + MiniDV", h, 0);
    }
    {  // every CRT preset with its defaults
        for (const auto &o : s.params[index_of(s, "crt.preset")].options) {
            Host h(s);
            h.user_edit(index_of(s, "crt.preset"), o.first);
            record("edit/crt preset " + o.first.get<std::string>(), h, 0);
        }
    }
    out["cases"] = cases;

    std::ofstream f(argv[1], std::ios::binary | std::ios::trunc);
    // One case per line: readable diffs when the reference changes.
    f << "{\n";
    bool first = true;
    for (auto &[k, v] : out.items()) {
        if (k == "cases") continue;
        f << (first ? "" : ",\n") << json(k).dump() << ": " << v.dump();
        first = false;
    }
    f << ",\n\"cases\": {\n";
    first = true;
    for (auto &[k, v] : cases.items()) {
        f << (first ? "" : ",\n") << json(k).dump() << ": " << v.dump();
        first = false;
    }
    f << "\n}\n}\n";
    std::cout << cases.size() << " cases written to " << argv[1] << "\n";
    return f ? 0 : 1;
}
