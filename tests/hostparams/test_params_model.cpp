// Shared host param model: the rules where the two plug-ins used to differ
// (docs/params.md, "One model for both plug-ins").
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include "ofx_host_sim.h"

#include <climits>
#include <filesystem>

using namespace hostsim;

namespace {
const Schema &tiny() {
    static const Schema s = Schema::parse(json::parse(R"({
      "constants": {"/version": 2, "fb:/kind": "ntscrt-video-feedback", "fb:/version": 2},
      "params": [
        {"id": "a.on", "type": "bool", "default": true, "path": "/a/enabled", "animatable": false},
        {"id": "a.n", "type": "int", "default": 3, "min": 1, "max": 9, "path": "/a/n", "enabled_if": "/a/enabled"},
        {"id": "a.f", "type": "float", "default": 0.5, "min": 0, "max": 1, "path": "/a/f",
         "enabled_if": {"path": "/a/n", "equals": 3.0000000001}},
        {"id": "a.mode", "type": "choice", "default": 1, "path": "/a/mode",
         "options": [{"value": 0, "label": "Zero"}, {"value": 1, "label": "One"}]},
        {"id": "a.zero_only", "type": "float", "default": 0.1, "path": "/a/zero",
         "visible_if": {"path": "/a/mode", "equals": 0}},
        {"id": "a.tick", "type": "bool", "default": false, "encode": "number", "path": "/a/tick"},
        {"id": "a.tint", "type": "color", "default": "#ff8000", "path": "/a/tint"},
        {"id": "a.rgba", "type": "color", "default": [1, 0, 0, 0.5], "path": "/a/rgba"},
        {"id": "a.ghost", "type": "float", "default": 0, "path": "/a/ghost", "visible_if": {"path": "/nobody/owns"}},
        {"id": "fb.on", "type": "bool", "default": false, "path": "fb:/enabled"},
        {"id": "fb.copies", "type": "int", "default": 7, "min": 1, "max": 30, "path": "fb:/copies", "enabled_if": "fb:/enabled"}
      ]})"));
    return s;
}
ValueFn defaults(const Schema &s) {
    return [&s](size_t i) { return s.params[i].def; };
}
}  // namespace

TEST_CASE("model: conditions take a pointer or {path, equals}, numbers compare to 1e-9, on param values") {
    const Schema &s = tiny();
    const auto &n = s.params[index_of(s, "a.n")], &f = s.params[index_of(s, "a.f")], &z = s.params[index_of(s, "a.zero_only")];
    CHECK(is_enabled(s, n, defaults(s)));
    CHECK(is_enabled(s, f, defaults(s)));  // 3 == 3.0000000001
    auto off = [&](size_t i) { return s.params[i].id == "a.on" ? json(false) : s.params[i].def; };
    CHECK_FALSE(is_enabled(s, n, off));
    CHECK_FALSE(is_visible(s, z, defaults(s)));
    auto zero = [&](size_t i) { return s.params[i].id == "a.mode" ? json(0.0) : s.params[i].def; };
    CHECK(is_visible(s, z, zero));  // 0.0 equals 0
    // A condition on a path no param owns: shown (and written).
    CHECK(is_visible(s, s.params[index_of(s, "a.ghost")], defaults(s)));
    // Screen Loop's switch greys out its params, whatever the feedback object holds.
    CHECK_FALSE(is_enabled(s, s.params[index_of(s, "fb.copies")], defaults(s)));
}

TEST_CASE("model: the look is built from nothing: constants, encode number, hidden params left out") {
    const Schema &s = tiny();
    const Built b = build_look(s, defaults(s));
    CHECK(b.look == json::parse(R"({"version": 2, "a": {"enabled": true, "n": 3, "f": 0.5, "mode": 1, "tick": 0,
                                    "tint": "#ff8000", "rgba": [1.0, 0.0, 0.0, 0.5], "ghost": 0.0}})"));
    CHECK(b.feedback == json::parse(R"({"kind": "ntscrt-video-feedback", "version": 2, "enabled": false, "copies": 7})"));
    CHECK_FALSE(screen_loop_on(b.feedback));
    CHECK(screen_loop_on(json::object()));  // the core's rule: missing means on
    CHECK_FALSE(screen_loop_on(json()));
}

TEST_CASE("model: coercion accepts every spelling a preset file uses, clamps to the schema range") {
    const Schema &s = tiny();
    auto P = [&](const char *id) -> const ParamDef & { return s.params[index_of(s, id)]; };
    CHECK(*coerce(P("a.on"), 0) == false);
    CHECK(*coerce(P("a.tick"), 1.0) == true);
    CHECK(*coerce(P("a.n"), 4.6) == 5);
    CHECK(*coerce(P("a.n"), 40) == 9);  // clamped (hosts clamp to the control's range too)
    CHECK(*coerce(P("a.n"), true) == 1);
    CHECK(coerce(P("a.f"), -2.0)->get<double>() == 0.0);
    CHECK(*coerce(P("a.mode"), "Zero") == 0);  // a label
    CHECK(*coerce(P("a.mode"), 1.0) == 1);
    CHECK_FALSE(coerce(P("a.mode"), 7).has_value());
    CHECK(*coerce(P("a.tint"), "00ff00") == "#00ff00");
    CHECK(*coerce(P("a.tint"), json::array({0, 0, 1})) == "#0000ff");
    CHECK(*coerce(P("a.rgba"), "#00ff0080") == json::array({0.0, 1.0, 0.0, 128 / 255.0}));
    CHECK(*coerce(P("a.rgba"), json::array({0, 1, 0})) == json::array({0.0, 1.0, 0.0, 1.0}));
    CHECK_FALSE(coerce(P("a.tint"), "red").has_value());
}

TEST_CASE("model: a Screen Loop file without \"enabled\" turns Screen Loop on") {
    const Schema &s = tiny();
    const PresetPlan p = plan_preset(s, json::parse(R"({"kind": "ntscrt-video-feedback", "copies": 12})"), 30, 0);
    CHECK_FALSE(p.replaces_look);
    CHECK(p.look.keys.empty());
    std::map<std::string, json> v;
    for (const auto &[i, x] : p.feedback) v[s.params[i].id] = x;
    CHECK(v["fb.on"] == true);
    CHECK(v["fb.copies"] == 12);
    const PresetPlan off = plan_preset(s, json::parse(R"({"kind": "ntscrt-video-feedback", "enabled": false})"), 30, 0);
    for (const auto &[i, x] : off.feedback)
        if (s.params[i].id == "fb.on") CHECK(x == false);
    // Every bundled Screen Loop preset: on once chosen.
    for (const auto &pr : parse_presets(bundled())) {
        if (!pr.feedback) continue;
        CAPTURE(pr.name);
        OfxHost h(schema());
        h.apply_preset(pr.data, 30, 0);
        CHECK(screen_loop_on(h.look(0).feedback));
    }
}

TEST_CASE("model: look presets reset every look param presets set, never the other half") {
    const Schema &s = tiny();
    const PresetPlan p = plan_preset(s, json::parse(R"({"a": {"n": 2}, "screen_loop": {"kind": "ntscrt-video-feedback", "copies": 3}})"), 30, 0);
    CHECK(p.replaces_look);
    CHECK(p.look.keys.size() == 9);  // every look param, defaults for the missing ones
    CHECK(p.look.keys.at(index_of(s, "a.n")).front().second == 2);
    CHECK(p.look.keys.at(index_of(s, "a.on")).front().second == true);
    CHECK(p.feedback.size() == 2);  // the Screen Loop settings riding along (our Save preset)
}

TEST_CASE("model: seed range and render seed are the same in both plug-ins") {
    CHECK(kSeedMin == 0);
    CHECK(kSeedMax == INT_MAX);
    CHECK(render_seed(-5) == 0);
    CHECK(render_seed(123) == 123);
    CHECK(render_seed(int64_t(INT_MAX) + 10) == uint64_t(INT_MAX));
}

TEST_CASE("model: temporal stages, saved presets") {
    CHECK_FALSE(temporal_stages_on(json::parse(R"({"digital":{"enabled":false}})"), json()));
    CHECK(temporal_stages_on(json::parse(R"({"glitch":{"enabled":true}})"), json()));
    CHECK_FALSE(temporal_stages_on(json::object(), json::parse(R"({"enabled": false})")));
    CHECK(temporal_stages_on(json::object(), json::parse(R"({"kind": "ntscrt-video-feedback"})")));
    const json look = {{"a", 1}}, on = {{"enabled", true}}, off = {{"enabled", false}};
    CHECK(saved_preset(look, off) == json{{"a", 1}, {"screen_loop", off}});
    CHECK_FALSE(saved_screen_loop(off).has_value());
    CHECK(*saved_screen_loop(on) == on);
}

TEST_CASE("model: cache masks per param") {
    const Schema &s = schema();
    auto mask = [&](const char *id, bool keyed) { return cache_mask(s.params[index_of(s, id)], keyed); };
    CHECK(mask("screenloop.zoom", false) == NTSCRT_CACHE_SCREENLOOP);
    CHECK(mask("ntsc.enabled", false) == NTSCRT_CACHE_SCREENLOOP);
    CHECK(mask("crt.preset", false) == (NTSCRT_CACHE_SCREENLOOP | NTSCRT_CACHE_CRT));
    CHECK(mask("digital.enabled", false) == (NTSCRT_CACHE_SCREENLOOP | NTSCRT_CACHE_DIGITAL));
    CHECK(mask("downscale.width", false) == (NTSCRT_CACHE_SCREENLOOP | NTSCRT_CACHE_DIGITAL | NTSCRT_CACHE_CRT));
    CHECK(mask("glitch.enabled", false) == NTSCRT_CACHE_SCREENLOOP);
    CHECK(mask("glitch.enabled", true) == (NTSCRT_CACHE_SCREENLOOP | NTSCRT_CACHE_GLITCH));
}

TEST_CASE("model: bundled presets are read without a core instance, as the core lists them") {
    const json mine = json::parse(bundled_presets_json(NTSCRT_BUILT_RESOURCES_DIR));
    REQUIRE(mine.is_array());
    CHECK(mine.size() >= 34);
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_BUILT_RESOURCES_DIR};
    ntscrt_instance *inst = nullptr;
    if (ntscrt_create(&ci, &inst) == NTSCRT_OK && inst) {
        CHECK(json::parse(ntscrt_bundled_presets_json(inst)) == mine);
        ntscrt_destroy(inst);
    }
    CHECK(json::parse(bundled_presets_json("no/such/dir")) == json::array());
}
