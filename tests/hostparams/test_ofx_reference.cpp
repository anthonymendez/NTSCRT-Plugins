// The OpenFX plug-in renders exactly what it rendered before the param models were
// merged: for every bundled preset (looks, animated looks at several frames and two
// frame rates, Screen Loop presets, Digital video formats), the default instance and a
// few preset-file cases, the look and feedback JSON built through the shared library
// equal ofx_reference.json, which the 0.5.0-dev OpenFX param model wrote
// (tests/hostparams/gen_ofx_reference.cpp, removed with that model; see git history).
#include <doctest/doctest.h>

#include "ofx_host_sim.h"

using namespace hostsim;

namespace {
const json &reference() {
    static const json r = load_file(std::string(NTSCRT_SOURCE_DIR) + "/tests/hostparams/ofx_reference.json");
    return r;
}

const json &find(const std::vector<Preset> &v, const std::string &n) {
    for (const auto &p : v)
        if (p.name == n) return p.data;
    throw std::runtime_error("no preset " + n);
}

// name -> the look / feedback the shared library gives (same cases as the generator).
std::map<std::string, Built> cases(std::vector<Preset> &looks, std::vector<Preset> &loops) {
    const Schema &s = schema();
    for (auto &p : parse_presets(bundled(), s.digital_formats)) (p.feedback ? loops : looks).push_back(std::move(p));
    std::map<std::string, Built> out;
    auto record = [&](const std::string &name, const OfxHost &h, int64_t n) { out[name] = h.look(n); };
    const int64_t kAvailable = 300;
    for (const auto &p : looks) {
        if (p.format) {
            OfxHost h(s);
            h.apply_values(applies_values(s, p.data));
            record("format/" + p.name, h, 0);
            continue;
        }
        for (double fps : {30.0, 24.0}) {
            OfxHost k(s);
            k.apply_preset(p.data, fps, kAvailable);
            if (!preset_keys(s, p.data, fps, kAvailable).animated()) {
                if (fps == 30.0) record("look/" + p.name, k, 0);
                continue;
            }
            const std::vector<int64_t> frames = fps == 30.0 ? std::vector<int64_t>{0, 1, 7, 29, 30, 59, 60, 61, 149, 299, 400}
                                                            : std::vector<int64_t>{0, 5, 47, 100};
            for (int64_t n : frames) record("look/" + p.name + "@" + std::to_string(int(fps)) + "fps/" + std::to_string(n), k, n);
        }
    }
    for (const auto &p : loops) {
        OfxHost h(s);
        h.apply_preset(p.data, 30, kAvailable);
        record("loop/" + p.name, h, 0);
    }
    {
        json j = find(looks, "Clean VHS");
        j["screen_loop"] = find(loops, "Screen Loop/Bottom to top");
        OfxHost h(s);
        h.apply_preset(j, 30, kAvailable);
        record("file/look with screen_loop", h, 0);
    }
    {
        json j = find(loops, "Screen Loop/12 copies luma key");
        j["enabled"] = false;
        OfxHost h(s);
        h.apply_preset(j, 30, kAvailable);
        record("file/loop enabled false", h, 0);
    }
    {
        json j = {{"version", 1},
                  {"digital", {{"enabled", true}, {"values", {{"quality", 0.22}, {"exposure", 0.3}, {"lens_blur", 0.2}}}}}};
        OfxHost h(s);
        h.apply_preset(j, 30, kAvailable);
        record("file/digital version 1", h, 0);
    }
    {
        json j = {{"downscale", {{"method", "Chunky"}, {"enabled", 1}, {"width", 300.4}}},
                  {"shader", {{"preset", "CRT Easymode"}}},
                  {"glitch", {{"enabled", 0}, {"values", {{"closed_captions", 0}}}}},
                  {"digital", {{"values", {{"codec", 1.0}}}}},
                  {"unknown", {{"x", 1}}}};
        OfxHost h(s);
        h.apply_preset(j, 30, kAvailable);
        record("file/labels and spellings", h, 0);
    }
    {
        OfxHost h(s);
        const size_t pre = index_of(s, "downscale.preset"), w = index_of(s, "downscale.width");
        h.user_edit(pre, "Arcade (384px)");
        record("edit/horizontal resolution arcade", h, 0);
        h.user_edit(w, int64_t(400));
        record("edit/width 400 demotes", h, 0);
    }
    {
        OfxHost h(s);
        h.apply_preset(find(looks, "Clean VHS"), 30, kAvailable);
        h.apply_values(applies_values(s, find(looks, "Digital video/MiniDV")));
        record("format over look/Clean VHS + MiniDV", h, 0);
    }
    for (const auto &o : s.params[index_of(s, "crt.preset")].options) {
        OfxHost h(s);
        h.user_edit(index_of(s, "crt.preset"), o.value);
        record("edit/crt preset " + o.value.get<std::string>(), h, 0);
    }
    return out;
}
}  // namespace

TEST_CASE("OpenFX: menus keep the 0.5.0-dev order (saved projects store indices)") {
    std::vector<Preset> looks, loops;
    cases(looks, loops);
    std::vector<std::string> l, f;
    for (const auto &p : looks) l.push_back(p.name);
    for (const auto &p : loops) f.push_back(p.name);
    CHECK(json(l) == reference()["look_menu"]);
    CHECK(json(f) == reference()["loop_menu"]);
}

TEST_CASE("OpenFX: every bundled preset builds the look and feedback it built before") {
    std::vector<Preset> looks, loops;
    const auto got = cases(looks, loops);
    const json &ref = reference();
    const json &def = ref["default"];
    std::string where;
    {
        const Built b = OfxHost(schema()).look(0);
        CHECK_MESSAGE(close(b.look, def["look"], where), "default look " << where);
        CHECK_MESSAGE(close(b.feedback, def["feedback"], where), "default feedback " << where);
    }
    CHECK(got.size() == ref["cases"].size());
    for (auto &[name, patch] : ref["cases"].items()) {
        CAPTURE(name);
        auto it = got.find(name);
        REQUIRE_MESSAGE(it != got.end(), "case not built: " << name);
        json look = def["look"], fb = def["feedback"];
        look.merge_patch(patch["look"]);
        fb.merge_patch(patch["feedback"]);
        // 1e-6: the reference was written on Windows; CRT shader params that timeline keys lerp in
        // float (core/params/timeline.h) differ in the last float bit on macOS (1.1000000238 vs 1.0999999046).
        // Animated looks (name@fps/frame): keyed sparsely since 0.5.0-rc.3, so within the keys'
        // tolerance of the per-frame keys the reference was written with.
        if (name.find('@') != std::string::npos)
            CHECK_MESSAGE(within_key_tolerance(schema(), it->second.look, look, &where), "look " << where);
        else
            CHECK_MESSAGE(close(it->second.look, look, where, "", 1e-6), "look " << where);
        CHECK_MESSAGE(close(it->second.feedback, fb, where, "", 1e-6), "feedback " << where);
    }
}
