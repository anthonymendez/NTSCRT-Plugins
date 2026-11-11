// ofx_params: look building from the real schema.
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include "ofx_params.h"

using namespace ntscrt_ofx;

namespace {
// The look the plug-in sends with every param at its default, except `crt.preset`.
json look_with_crt_preset(const Schema &s, const std::string &preset) {
    json look, fb;
    build_look(s, [&](size_t i) {
        return s.params[i].id == "crt.preset" ? json(preset) : s.params[i].def;
    }, look, fb);
    return look;
}
}  // namespace

TEST_CASE("build_look writes only the selected CRT preset's shader params") {
    Schema s = Schema::parse(ntscrt_param_schema_json());
    // aperture and easymode share GAMMA_INPUT / GAMMA_OUTPUT, easymode and hyllian share
    // MASK_STRENGTH, all with different defaults: a later preset must not overwrite them.
    json ap = look_with_crt_preset(s, "aperture");
    REQUIRE(ap["shader"]["preset"] == "aperture");
    CHECK(ap["shader"]["params"]["GAMMA_INPUT"].get<double>() == doctest::Approx(2.4));
    CHECK(ap["shader"]["params"]["GAMMA_OUTPUT"].get<double>() == doctest::Approx(2.4));

    json em = look_with_crt_preset(s, "easymode");
    CHECK(em["shader"]["params"]["GAMMA_INPUT"].get<double>() == doctest::Approx(2.0));
    CHECK(em["shader"]["params"]["MASK_STRENGTH"].get<double>() == doctest::Approx(0.3));

    // every written shader param belongs to the selected preset
    for (const auto &p : s.params) {
        if (p.group.rfind("crt.", 0) != 0 || p.path.rfind("/shader/params/", 0) != 0) continue;
        if (p.group == "crt.easymode") continue;
        bool shared = false;
        for (const auto &q : s.params)
            if (q.group == "crt.easymode" && q.path == p.path) shared = true;
        if (!shared) CHECK_MESSAGE(!em["shader"]["params"].contains(p.path.substr(15)), p.id);
    }
}

TEST_CASE("diagnostics are host extras: no schema param, never in the look or presets") {
    // "Debug logging" / "Open logs folder" are defined by the plug-in itself (host_debug_logging,
    // host_open_logs), so they can't reach build_look or preset_value through the schema.
    Schema s = Schema::parse(ntscrt_param_schema_json());
    for (const auto &p : s.params) {
        CAPTURE(p.id);
        CHECK(p.id.find("debug") == std::string::npos);
        CHECK(p.name.rfind("host_", 0) != 0);
        CHECK(p.path.find("debug") == std::string::npos);
    }
    json look, fb;
    build_look(s, [&](size_t i) { return s.params[i].def; }, look, fb);
    CHECK(look.dump().find("debug") == std::string::npos);
    json preset = {{"debug_logging", true}, {"host_debug_logging", true}};
    for (const auto &p : s.params) {
        auto v = preset_value(p, preset, false);
        if (v) CHECK(*v == p.def);  // unknown keys are ignored: every param gets its default
    }
}

TEST_CASE("Digital video controls kept from 0.4.0: written only while non-zero, presets migrate") {
    Schema s = Schema::parse(ntscrt_param_schema_json());
    auto index = [&](const std::string &id) {
        for (size_t i = 0; i < s.params.size(); ++i)
            if (s.params[i].id == id) return i;
        FAIL("no param " << id);
        return size_t(0);
    };
    const size_t legacyQ = index("digital.quality"), q2 = index("digital.quality2"), ccd = index("digital.ccd_response");
    CHECK(s.params[q2].path == "/digital/values/quality");
    CHECK(s.params[legacyQ].path == "/digital/legacy/quality");
    // A new instance: nothing under /digital/legacy.
    json look, fb;
    build_look(s, [&](size_t i) { return s.params[i].def; }, look, fb);
    CHECK_FALSE(look["digital"].contains("legacy"));
    CHECK(look["digital"]["values"]["quality"] == 0.749);
    CHECK(look["digital"]["values"]["fields"] == 1);
    // A 0.4.0 instance (old Quality 0.22 stored under the old id): written.
    build_look(s, [&](size_t i) { return i == legacyQ ? json(0.22) : s.params[i].def; }, look, fb);
    CHECK(look["digital"]["legacy"]["quality"] == 0.22);
    CHECK_FALSE(look["digital"]["legacy"].contains("lens_blur"));  // never written: no effect
    // Applying a version 1 preset: Quality converted, Exposure / Black level become CCD response,
    // the legacy params reset.
    json v1 = {{"version", 1},
               {"digital", {{"enabled", true}, {"values", {{"quality", 0.22}, {"exposure", 0.3}, {"black_level", 0.0}}}}}};
    CHECK(std::fabs(preset_value(s.params[q2], v1, false)->get<double>() - 0.749) < 0.001);
    CHECK(std::fabs(preset_value(s.params[ccd], v1, false)->get<double>() - 0.5) < 1e-9);
    CHECK(*preset_value(s.params[legacyQ], v1, false) == 0.0);
    // Today's preset: as written.
    json v2 = {{"version", 2}, {"digital", {{"enabled", true}, {"values", {{"quality", 0.22}}}}}};
    CHECK(*preset_value(s.params[q2], v2, false) == 0.22);
}

TEST_CASE("bundled preset menu keeps released presets at their index") {
    json arr = json::array();
    for (const char *n : {"15 second glitches", "5 second blips", "CD-ROM (low quality)", "Chunky webcam", "Clean CRT",
                          "Webcam (low quality)", "Zzz new"})
        arr.push_back({{"name", n}, {"kind", "look"}, {"json", json::object()}});
    auto p = parse_presets(arr.dump().c_str());
    REQUIRE(p.size() == 7);
    CHECK(p[0].name == "15 second glitches");
    CHECK(p[2].name == "Clean CRT");
    CHECK(p[3].name == "Webcam (low quality)");
    CHECK(p[4].name == "CD-ROM (low quality)");  // new ones after the released ones, alphabetically
    CHECK(p[5].name == "Chunky webcam");
    CHECK(p[6].name == "Zzz new");
    // v0.4.0 shipped 32 presets (looks and Screen Loops); none may move.
    CHECK(released_preset_order().size() == 32);
}
