// Tests the host-independent FxPlug parameter model (runs on all platforms).
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "../model/param_model.h"

using namespace ntscrt_fx;

namespace {
json load(const std::string &rel) {
    std::ifstream f(std::string(NTSCRT_SOURCE_DIR) + "/" + rel, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return json::parse(ss.str());
}

const json kSchema = json::parse(R"({
  "version": 1,
  "groups": [ {"id": "a", "label": "A"}, {"id": "b", "label": "B", "parent": "a"}, {"id": "fb", "label": "Loop"} ],
  "params": [
    {"id": "a.on", "group": "a", "label": "On", "type": "bool", "default": true, "path": "/a/enabled", "animatable": false},
    {"id": "a.width", "group": "a", "label": "Width", "type": "int", "default": 320, "min": 64, "max": 1920, "path": "/a/width"},
    {"id": "b.amount", "group": "b", "label": "Amount", "type": "float", "default": 0.05, "min": 0, "max": 1,
     "display_min": 0, "display_max": 0.5, "step": 0.001, "path": "/a/b/amount", "enabled_if": "/a/enabled"},
    {"id": "b.mode", "group": "b", "label": "Mode", "type": "choice", "default": "y",
     "options": [{"value": "x", "label": "X"}, {"value": "y", "label": "Y"}, {"value": 3, "label": "Three"}], "path": "/a/b/mode"},
    {"id": "b.tint", "group": "b", "label": "Tint", "type": "color", "default": "#ff8000", "path": "/a/b/tint",
     "visible_if": {"path": "/a/b/mode", "equals": "x"}},
    {"id": "fb.copies", "group": "fb", "label": "Copies", "type": "int", "default": 7, "min": 1, "max": 30, "path": "fb:/copies"}
  ]})");

json ids_for(const json &schema) {
    json ids = json::object();
    uint32_t n = kFirstMappedID;
    for (const auto &g : schema["groups"]) ids["group:" + g["id"].get<std::string>()] = n++;
    for (const auto &p : schema["params"]) ids[p["id"].get<std::string>()] = n++;
    return json{{"ids", ids}};
}
}  // namespace

TEST_CASE("model: layout nests groups and keeps schema order") {
    std::vector<std::string> warn;
    Model m = Model::from_schema(kSchema, ids_for(kSchema), &warn);
    CHECK(warn.empty());
    REQUIRE(m.params.size() == 6);
    // a{ on width b{ amount mode tint } } fb{ copies }
    std::vector<std::string> seq;
    for (auto &it : m.layout) {
        if (it.type == UiItem::StartGroup) seq.push_back("<" + m.groups[it.index].id);
        else if (it.type == UiItem::EndGroup) seq.push_back(">");
        else seq.push_back(m.params[it.index].id);
    }
    CHECK(seq == std::vector<std::string>{"<a", "a.on", "a.width", "<b", "b.amount", "b.mode", "b.tint", ">", ">",
                                          "<fb", "fb.copies", ">"});
    CHECK(m.find("a.width")->host_id == kFirstMappedID + 4);
    CHECK(m.find("b.amount")->slider_max == doctest::Approx(0.5));
    CHECK_FALSE(m.find("a.on")->animatable);
    CHECK(m.find("fb.copies")->feedback);
}

TEST_CASE("model: unmapped params get fallback ids and a warning") {
    std::vector<std::string> warn;
    Model m = Model::from_schema(kSchema, json::object(), &warn);
    CHECK(warn.size() == 9);
    for (auto &p : m.params) CHECK((p.host_id >= kFirstFallbackID && p.host_id <= kLastFallbackID));
    std::set<uint32_t> s;
    for (auto &p : m.params) s.insert(p.host_id);
    for (auto &g : m.groups) s.insert(g.host_id);
    CHECK(s.size() == 9);
    // deterministic
    Model m2 = Model::from_schema(kSchema, json::object(), nullptr);
    CHECK(m2.find("b.mode")->host_id == m.find("b.mode")->host_id);
}

TEST_CASE("model: value conversion round trips") {
    Model m = Model::from_schema(kSchema, ids_for(kSchema), nullptr);
    const Param &mode = *m.find("b.mode");
    CHECK(m.default_host_value(mode) == 1);
    CHECK(m.to_look_value(mode, 0) == "x");
    CHECK(m.to_look_value(mode, 99) == "y");  // outside the menu: the default option (as OpenFX reads it)
    CHECK(m.to_host_value(mode, "Three") == json(2));  // a label works too
    CHECK(m.to_host_value(mode, 3) == json(2));
    CHECK_FALSE(m.to_host_value(mode, "nope").has_value());

    const Param &tint = *m.find("b.tint");
    json h = m.default_host_value(tint);
    REQUIRE(h.is_array());
    CHECK(h[0].get<double>() == doctest::Approx(1.0));
    CHECK(h[1].get<double>() == doctest::Approx(128.0 / 255));
    CHECK(m.to_look_value(tint, h) == "#ff8000");

    const Param &w = *m.find("a.width");
    CHECK(m.to_look_value(w, 5000.4) == 1920);
    CHECK(m.to_host_value(w, 100.6) == json(101));
}

TEST_CASE("model: builds look and feedback JSON from the params alone") {
    Model m = Model::from_schema(kSchema, ids_for(kSchema), nullptr);
    auto get = [&](const Param &p) { return m.default_host_value(p); };
    const host::Built b = m.build(get);
    CHECK(b.look == json::parse(R"({"a": {"enabled": true, "width": 320, "b": {"amount": 0.05, "mode": "y"}}})"));
    CHECK(b.feedback == json::parse(R"({"kind": "ntscrt-video-feedback", "version": 2, "copies": 7})"));
}

TEST_CASE("model: preset values and UI gating") {
    Model m = Model::from_schema(kSchema, ids_for(kSchema), nullptr);
    json preset = json::parse(R"({"a": {"enabled": false, "b": {"mode": "x", "amount": 2.0}}})");
    const HostPlan plan = m.plan_preset(preset, 30, 100);
    CHECK(plan.replaces_look);
    CHECK_FALSE(plan.animated());
    CHECK(plan.feedback.empty());
    std::map<uint32_t, json> v;
    for (const auto &[id, k] : plan.look) {
        REQUIRE(k.size() == 1);
        v[id] = k.front().second;
    }
    CHECK(v.size() == 5);
    CHECK(v[m.find("a.on")->host_id] == false);
    CHECK(v[m.find("b.mode")->host_id] == 0);
    CHECK(v[m.find("b.amount")->host_id].get<double>() == doctest::Approx(1.0));  // clamped to max
    CHECK(v[m.find("a.width")->host_id] == 320);                                  // missing -> default
    json loop = json::parse(R"({"kind": "ntscrt-video-feedback", "copies": 12})");
    CHECK(m.plan_preset(loop, 30, 100).feedback.at(m.find("fb.copies")->host_id) == 12);
    CHECK(m.plan_preset(loop, 30, 100).look.empty());

    auto from = [&](const std::map<uint32_t, json> &vals) {
        return [&m, vals](const Param &p) {
            auto it = vals.find(p.host_id);
            return it != vals.end() ? it->second : m.default_host_value(p);
        };
    };
    CHECK_FALSE(m.is_enabled(*m.find("b.amount"), from(v)));
    CHECK(m.is_visible(*m.find("b.tint"), from(v)));
    CHECK(m.is_enabled(*m.find("b.amount"), from({})));
    CHECK_FALSE(m.is_visible(*m.find("b.tint"), from({})));  // mode "y"
}

TEST_CASE("model: render state, frame math") {

    RenderState s;
    s.frame = 42; s.fps = 29.97; s.history = 3; s.look = R"({"a":1})"; s.feedback = "";
    s.seed = 0xFFFFFFFFFFFFFFF0ull; s.past_looks[41] = "{}"; s.past_looks[-1] = "{\"b\":2}";
    auto r = RenderState::parse(s.serialize());
    REQUIRE(r.has_value());
    CHECK(r->frame == 42);
    CHECK(r->seed == s.seed);
    CHECK(r->past_looks.size() == 2);
    CHECK(r->past_looks[-1] == "{\"b\":2}");
    CHECK_FALSE(RenderState::parse("garbage").has_value());

    // 1001/30000 frames: 10 frames = 10010/30000 s
    CHECK(frame_from_offset(10010, 30000, 1001, 30000) == 10);
    CHECK(frame_from_offset(1, 3, 1, 30) == 10);
    CHECK(frame_from_offset(0, 600, 1, 24) == 0);
}

TEST_CASE("model: real schema + checked-in id map are complete and valid") {
    json schema = json::parse(ntscrt_param_schema_json());
    json ids = load("fxplug/param_ids.json");
    std::vector<std::string> warn;
    Model m = Model::from_schema(schema, ids, &warn);
    for (auto &w : warn) MESSAGE(w);  // missing ids are warnings, not failures (schema evolves in parallel)
    CHECK(m.params.size() > 0);
    std::set<uint32_t> s;
    for (auto &p : m.params) CHECK(s.insert(p.host_id).second);
    for (auto &g : m.groups) CHECK(s.insert(g.host_id).second);
    for (uint32_t id : s) CHECK((id >= kFirstMappedID && id <= 9998));
    // building a look from defaults must give valid JSON that ntscrt accepts structurally
    json look = m.build([&](const Param &p) { return m.default_host_value(p); }).look;
    CHECK(look.is_object());
}

TEST_CASE("model: only the selected CRT preset's shader params go into the look") {
    json schema = json::parse(ntscrt_param_schema_json());
    Model m = Model::from_schema(schema, load("fxplug/param_ids.json"), nullptr);
    const Param *preset = m.find("crt.preset");
    REQUIRE(preset);
    auto look_for = [&](const char *id) {
        json host = *m.to_host_value(*preset, json(id));
        return m.build([&](const Param &p) { return &p == preset ? host : m.default_host_value(p); }).look;
    };
    // aperture/easymode share GAMMA_*, easymode/hyllian share MASK_STRENGTH (different defaults)
    json ap = look_for("aperture");
    CHECK(ap["shader"]["params"]["GAMMA_INPUT"].get<double>() == doctest::Approx(2.4));
    json em = look_for("easymode");
    CHECK(em["shader"]["params"]["GAMMA_INPUT"].get<double>() == doctest::Approx(2.0));
    CHECK(em["shader"]["params"]["MASK_STRENGTH"].get<double>() == doctest::Approx(0.3));
}

TEST_CASE("model: diagnostics are host extras outside the look, presets and schema ids") {
    json schema = json::parse(ntscrt_param_schema_json());
    Model m = Model::from_schema(schema, load("fxplug/param_ids.json"), nullptr);
    // fixed host ids, below every schema-mapped id, distinct from the other extras
    CHECK(uint32_t(kDebugLogging) <= kLastHostID);
    CHECK(uint32_t(kOpenLogs) <= kLastHostID);
    CHECK(uint32_t(kDebugAll) <= kLastHostID);
    CHECK(kDebugAll != kOpenLogs);
    CHECK(kDebugAll != kDebugLogging);
    CHECK(m.find_host(kDebugAll) == nullptr);
    CHECK(m.cache_mask(kDebugAll, true) == 0);
    CHECK(kLastHostID < kFirstMappedID);
    CHECK(kDebugLogging != kOpenLogs);
    CHECK(m.find_host(kDebugLogging) == nullptr);
    CHECK(m.find_host(kOpenLogs) == nullptr);
    for (const auto &p : m.params) {
        CAPTURE(p.id);
        CHECK(p.id.find("debug") == std::string::npos);
    }
    // presets can't set them, the look never contains them
    json preset = {{"debug_logging", true}, {"debug", true}};
    json loop = preset;
    loop["kind"] = "ntscrt-video-feedback";
    for (const json &p : {preset, loop}) {
        auto v = m.plan_preset(p, 30, 0);
        CHECK(v.look.count(kDebugLogging) == 0);
        CHECK(v.look.count(kOpenLogs) == 0);
        CHECK(v.feedback.count(kDebugLogging) == 0);
    }
    json look = m.build([&](const Param &p) { return m.default_host_value(p); }).look;
    CHECK(look.dump().find("debug") == std::string::npos);

    // the per-frame flag travels in the plug-in state, not in the look
    RenderState s;
    s.look = look.dump();
    CHECK_FALSE(RenderState::parse(s.serialize())->debug);
    CHECK(s.serialize().find("debug") == std::string::npos);  // states from before the flag stay identical
    s.debug = true;
    auto r = RenderState::parse(s.serialize());
    REQUIRE(r.has_value());
    CHECK(r->debug);
    CHECK(r->look == s.look);
}

TEST_CASE("model: FileLog appends timestamped lines and rotates") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "ntscrt_fx_filelog_test" / "Logs" / "NTSCRT";
    std::error_code ec;
    fs::remove_all(dir.parent_path().parent_path(), ec);
    auto u = (dir / "ntscrt-fxplug.log").u8string();
    FileLog log(std::string(u.begin(), u.end()), 200);
    auto f = dir.u8string();
    CHECK(log.folder() == std::string(f.begin(), f.end()));
    log.write("first line");  // creates the folders
    log.write("second line");
    std::ifstream in(dir / "ntscrt-fxplug.log");
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    const std::string text = ss.str();
    CHECK(text.find("] first line\n") != std::string::npos);
    CHECK(text.find("] second line\n") != std::string::npos);
    CHECK(text[4] == '-');  // "YYYY-MM-DD HH:MM:SS.mmm [tid] msg"
    for (int i = 0; i < 10; ++i) log.write("padding padding padding padding padding");
    CHECK(fs::exists(dir / "ntscrt-fxplug.old.log"));
    CHECK(fs::file_size(dir / "ntscrt-fxplug.log") < 400);
    FileLog off("");
    off.write("nothing");  // disabled: no-op, no throw
    CHECK(off.folder().empty());
    fs::remove_all(dir.parent_path().parent_path(), ec);
}

TEST_CASE("model: Digital video legacy controls and version 1 presets") {
    json schema = json::parse(ntscrt_param_schema_json());
    Model m = Model::from_schema(schema, load("fxplug/param_ids.json"), nullptr);
    const Param *legacy = m.find("digital.quality"), *q2 = m.find("digital.quality2"), *ccd = m.find("digital.ccd_response");
    REQUIRE(legacy);
    REQUIRE(q2);
    REQUIRE(ccd);
    json look = m.build([&](const Param &p) { return m.default_host_value(p); }).look;
    CHECK_FALSE(look["digital"].contains("legacy"));
    CHECK(look["digital"]["values"]["quality"].get<double>() == doctest::Approx(0.749));
    look = m.build([&](const Param &p) { return &p == legacy ? json(0.3) : m.default_host_value(p); }).look;
    CHECK(look["digital"]["legacy"]["quality"].get<double>() == doctest::Approx(0.3));
    json v1 = {{"version", 1}, {"digital", {{"enabled", true}, {"values", {{"quality", 0.22}, {"black_level", 0.08}}}}}};
    auto hv = m.plan_preset(v1, 30, 0).look;
    CHECK(hv[q2->host_id].front().second.get<double>() == doctest::Approx(0.749).epsilon(0.002));
    CHECK(hv[ccd->host_id].front().second.get<double>() == doctest::Approx(1.0));
    CHECK(hv[legacy->host_id].front().second.get<double>() == doctest::Approx(0.0));
}

// ---- animated presets, applies, Digital video formats, upstream JSON ----
namespace {
const Model &real_model() {
    static Model m = Model::from_schema(json::parse(ntscrt_param_schema_json()), load("fxplug/param_ids.json"), nullptr);
    return m;
}
json keyed_at(const std::vector<std::pair<int64_t, json>> &keys, int64_t n) {
    json v = keys.front().second;
    for (const auto &[f, x] : keys)
        if (f <= n) v = x;
    return v;
}
// The look a host holding pk's values / keys builds at frame n.
json keyed_look(const Model &m, const HostPlan &pk, int64_t n) {
    return m.build([&](const Param &p) {
        auto it = pk.look.find(p.host_id);
        return it != pk.look.end() ? keyed_at(it->second, n) : m.default_host_value(p);
    }).look;
}
// The look a host builds after applying `look` as a still look (its timeline left out).
json held_look(const Model &m, json look) {
    look.erase("timeline");
    return keyed_look(m, m.plan_preset(look, 30, 0), 0);
}
}  // namespace

TEST_CASE("model: animated presets become keyframes matching the upstream timeline") {
    const Model &m = real_model();
    for (const std::string name : {"Glitch pop", "V Hold glitch", "Tracking Noise up"}) {
        CAPTURE(name);
        const json preset = load("presets/" + name + ".json");
        const double fps = 30;
        const HostPlan pk = m.plan_preset(preset, fps, 150);
        REQUIRE(pk.animated());
        CHECK(pk.pass == 60);  // 2 s at 30 fps
        CHECK(pk.frames == 150);
        size_t moving = 0;
        for (const auto &[id, k] : pk.look) {
            const Param *p = m.find_host(id);
            REQUIRE(p);
            CHECK_FALSE(p->feedback);
            REQUIRE_FALSE(k.empty());
            CHECK(k.front().first == 0);
            if (k.size() > 1) {
                ++moving;
                if (p->type != Kind::Bool && p->type != Kind::Choice) CHECK(k.size() == 150);
            }
        }
        CHECK(moving > 3);
        bool differs = false;
        const json first = keyed_look(m, pk, 0);
        for (int64_t n : {0, 1, 13, 29, 59, 60, 77, 149}) {
            CAPTURE(n);
            const json want = held_look(m, host::preset_look_at_frame(m.schema, preset, n, fps));
            const json got = keyed_look(m, pk, n);
            CHECK(got == want);
            differs |= got != first;
        }
        CHECK(differs);
    }
    CHECK_FALSE(m.plan_preset(load("presets/Clean VHS.json"), 30, 100).animated());
    CHECK_FALSE(m.plan_preset(load("presets/Screen Loop/Bottom to top.json"), 30, 100).animated());
    CHECK(m.plan_preset(load("presets/Glitch pop.json"), 30, 0).frames == 60);  // unknown length: one pass
}

TEST_CASE("model: Horizontal resolution applies, Custom demotion, Digital video formats") {
    const Model &m = real_model();
    const Param *pre = m.find("downscale.preset"), *w = m.find("downscale.width");
    REQUIRE(pre);
    REQUIRE(w);
    int64_t arcade = -1, custom = -1;
    for (size_t i = 0; i < pre->options.size(); ++i) {
        if (pre->options[i].value == "Arcade (384px)") arcade = int64_t(i);
        if (pre->options[i].value == "Custom") custom = int64_t(i);
    }
    REQUIRE(arcade >= 0);
    auto state = [&](int64_t choice, int64_t width) {
        return [&m, pre, w, choice, width](const Param &p) {
            return &p == pre ? json(choice) : &p == w ? json(width) : m.default_host_value(p);
        };
    };
    auto hv = m.edit_effects(*pre, state(arcade, 320));  // Arcade chosen: sets the width
    REQUIRE(hv.size() == 1);
    CHECK(hv[w->host_id] == 384);
    CHECK(m.edit_effects(*pre, state(custom, 320)).empty());
    CHECK(m.edit_effects(*w, state(arcade, 384)).empty());  // width still the option's
    hv = m.edit_effects(*w, state(arcade, 400));           // width edited by hand: Custom
    REQUIRE(hv.size() == 1);
    CHECK(hv[pre->host_id] == json(custom));

    json bundled = json::array({{{"name", "Clean VHS"}, {"kind", "look"}, {"json", json::object()}},
                                {{"name", "Screen Loop/x"}, {"kind", "feedback"}, {"json", json::object()}}});
    auto menu = m.look_menu(bundled);
    REQUIRE(menu.size() == 7);
    CHECK(menu[0].name == "Clean VHS");
    CHECK(menu[3].name == "Digital video/MiniDV");
    REQUIRE(m.loop_menu(bundled).size() == 1);
    CHECK(m.loop_menu(bundled)[0].name == "Screen Loop/x");
    CHECK(menu[3].format);
    auto fv = m.applies_host_values(menu[3].data);
    CHECK(fv[m.find("digital.enabled")->host_id] == true);
    CHECK(fv[w->host_id] == 720);
    const Param *dp = m.find("downscale.preset");
    CHECK(dp->options[size_t(fv[dp->host_id].get<int64_t>())].value == "MiniDV (720px)");
    CHECK(fv.count(m.find("digital.quality2")->host_id));
    CHECK_FALSE(fv.count(m.find("ntsc.enabled")->host_id));
}

TEST_CASE("model: looks are upstream JSON (constants, numeric toggles) and presets round-trip") {
    const Model &m = real_model();
    auto def = [&](const Param &p) { return m.default_host_value(p); };
    const host::Built b = m.build(def);
    CHECK(b.look["version"] == 2);
    CHECK(b.look["ntsc"]["settings"]["version"] == 1);
    CHECK(b.look["glitch"]["values"]["closed_captions"] == 1);
    CHECK(b.feedback["version"] == 2);
    CHECK(b.feedback["values"]["loop"] == 0);
    CHECK(b.feedback["enabled"] == false);
    for (const std::string name : {"Chunky webcam", "CD-ROM (low quality)", "Webcam (low quality)", "Clean CRT"}) {
        CAPTURE(name);
        const json preset = load("presets/" + name + ".json");
        const json a = held_look(m, preset);
        CHECK(held_look(m, a) == a);  // save -> load -> the same look
    }
}
