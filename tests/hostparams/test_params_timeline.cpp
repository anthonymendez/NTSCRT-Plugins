// Animated presets (upstream timeline keys), choice "applies", Digital video
// formats in the Preset menu, schema constants: the OFX param model.
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include "ntscrt_host/params.h"
#include "../../core/stages/digital/migrate.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace ntscrt::host;
namespace tl = ntscrt::timeline;

namespace {
json read_preset(const std::string &name) {
    std::ifstream f(std::string(NTSCRT_SOURCE_DIR) + "/presets/" + name + ".json", std::ios::binary);
    REQUIRE_MESSAGE(f, name);
    std::stringstream ss;
    ss << f.rdbuf();
    return json::parse(ss.str());
}
const Schema &schema() {
    static Schema s = Schema::parse(ntscrt_param_schema_json());
    return s;
}
size_t idx(const std::string &id) {
    const auto &ps = schema().params;
    for (size_t i = 0; i < ps.size(); ++i)
        if (ps[i].id == id) return i;
    FAIL("no param " << id);
    return 0;
}
// Value of a param's keyframes at frame n as a host interpolating linearly gives it.
json keyed_value(size_t i, const std::vector<std::pair<int64_t, json>> &keys, double n) {
    return interpolated_key_value(schema().params[i], keys, n);
}
// The same keys through a smooth spline (Fusion's default: Bezier handles a third of the way to the
// neighbouring keys, along the line between the keys either side), for numbers.
json spline_value(size_t i, const std::vector<std::pair<int64_t, json>> &keys, double n) {
    const ParamDef &p = schema().params[i];
    if (p.type == PType::Bool || p.type == PType::Choice || p.type == PType::Color || keys.size() < 2)
        return keyed_value(i, keys, n);
    auto hi = std::lower_bound(keys.begin(), keys.end(), n, [](const auto &k, double x) { return double(k.first) < x; });
    if (hi == keys.begin() || hi == keys.end() || double(hi->first) == n) return keyed_value(i, keys, n);
    const size_t b = size_t(hi - keys.begin()), a = b - 1;
    auto t = [&](size_t k) { return double(keys[k].first); };
    auto v = [&](size_t k) { return keys[k].second.get<double>(); };
    auto slope = [&](size_t k) {
        const size_t l = k == 0 ? k : k - 1, r = k + 1 >= keys.size() ? k : k + 1;
        return t(r) == t(l) ? 0.0 : (v(r) - v(l)) / (t(r) - t(l));
    };
    const double h = t(b) - t(a), u = (n - t(a)) / h;
    const double h00 = 2 * u * u * u - 3 * u * u + 1, h10 = u * u * u - 2 * u * u + u, h01 = -2 * u * u * u + 3 * u * u,
                 h11 = u * u * u - u * u;
    const double x = h00 * v(a) + h10 * h * slope(a) + h01 * v(b) + h11 * h * slope(b);
    if (p.type == PType::Int) return json(int64_t(std::llround(x)));
    return json(x);
}
// json equality with numbers equal to 1e-9 (presets carry 0.029 and 0.028999999999999998).
bool near(const json &a, const json &b) {
    if (a.is_number() && b.is_number()) return std::fabs(a.get<double>() - b.get<double>()) < 1e-9;
    if (a.type() != b.type() || a.size() != b.size()) return false;
    if (a.is_object()) {
        for (auto &[k, v] : a.items())
            if (!b.contains(k) || !near(v, b[k])) return false;
        return true;
    }
    if (a.is_array()) {
        for (size_t i = 0; i < a.size(); ++i)
            if (!near(a[i], b[i])) return false;
        return true;
    }
    return a == b;
}
// The look a host holding `pk` sends at frame n (spline: interpolating like Fusion).
json host_look(const PresetKeys &pk, const json &preset, int64_t n, bool spline = false) {
    const Schema &s = schema();
    json look, fb;
    build_look(s, [&](size_t i) {
        auto it = pk.keys.find(i);
        if (it != pk.keys.end() && !it->second.empty())
            return spline ? spline_value(i, it->second, double(n)) : keyed_value(i, it->second, double(n));
        return s.params[i].def;
    }, look, fb);
    (void)preset;
    return look;
}
std::vector<std::string> animated_presets() {
    std::vector<std::string> names;
    for (const auto &e : std::filesystem::directory_iterator(std::string(NTSCRT_SOURCE_DIR) + "/presets")) {
        if (e.path().extension() != ".json") continue;
        const std::string n = e.path().stem().string();
        const json j = read_preset(n);
        if (j.contains("timeline") && !j["timeline"].value("keys", json::array()).empty()) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    return names;
}
// The look the host would send if its params held `look` (preset_value of every param).
json as_host_look(const json &look) {
    const Schema &s = schema();
    json out, fb;
    build_look(s, [&](size_t i) { return preset_value(s.params[i], look, false).value_or(s.params[i].def); }, out, fb);
    return out;
}
}  // namespace

TEST_CASE("timeline: easing, segments, frame mapping and looping (upstream Timeline.swift)") {
    CHECK(tl::ease(tl::Easing::Linear, 0.25) == doctest::Approx(0.25));
    CHECK(tl::ease(tl::Easing::EaseIn, 0.5) == doctest::Approx(0.25));
    CHECK(tl::ease(tl::Easing::EaseOut, 0.5) == doctest::Approx(0.75));
    CHECK(tl::ease(tl::Easing::EaseInOut, 0.25) == doctest::Approx(0.125));
    CHECK(tl::ease(tl::Easing::EaseInOut, 0.75) == doctest::Approx(0.875));
    CHECK(tl::ease(tl::Easing::Hold, 0.9) == 0);
    CHECK(tl::parse_easing("Ease in-out") == tl::Easing::EaseInOut);
    CHECK(tl::parse_easing("bogus") == tl::Easing::Linear);

    json look = json::parse(R"({
      "shader": {"preset": "aperture", "params": {"SHARPNESS_IMAGE": 1.0, "GLOW_WIDTH": 0.5}},
      "ntsc": {"settings": {"composite_noise_intensity": 0.1, "head_switching_height": 8, "chroma_noise": false}},
      "glitch": {"values": {"signal_strength": 1.0, "closed_captions": 1}},
      "digital": {"values": {"quality": 0.5, "frame_rate": 15, "codec": 0}},
      "timeline": {"duration": 1, "fps": 10, "keys": [
        {"t": 1.0, "easing": "Linear", "shader": {"SHARPNESS_IMAGE": 5}, "ntsc": {"composite_noise_intensity": 0.5,
         "head_switching_height": 20, "chroma_noise": true}, "glitch": {"signal_strength": 0.0, "closed_captions": 0},
         "digital": {"quality": 0.1, "frame_rate": 30, "codec": 1}},
        {"t": 0.0, "easing": "Ease in", "shader": {"SHARPNESS_IMAGE": 1}, "ntsc": {"composite_noise_intensity": 0.1,
         "head_switching_height": 8, "chroma_noise": false}, "glitch": {"signal_strength": 1.0, "closed_captions": 1},
         "digital": {"quality": 0.5, "frame_rate": 15, "codec": 0}}
      ]}})");
    const auto t = tl::parse(look);
    REQUIRE(t.keys.size() == 2);
    CHECK(t.keys[0].t == 0.0);  // sorted
    CHECK(tl::pass_frames(t, 25) == 25);
    CHECK(tl::pass_frames(t, 29.97) == 30);
    CHECK(tl::time_of_frame(0, 25) == 0);
    CHECK(tl::time_of_frame(24, 25) == 1);
    CHECK(tl::time_of_frame(12, 25) == doctest::Approx(0.5));
    CHECK(tl::time_of_frame(25, 25) == 0);   // loops
    CHECK(tl::time_of_frame(37, 25) == doctest::Approx(0.5));
    CHECK(tl::time_of_frame(5, 1) == 0);

    const tl::Rules &r = schema().timeline;
    const json mid = tl::look_at(look, t, r, 0.5);  // ease in: u = 0.25
    // shader: lerp in Float, snapped to aperture's SHARPNESS_IMAGE step 1: 1 + 4 * 0.25 = 2
    CHECK(mid["shader"]["params"]["SHARPNESS_IMAGE"].get<double>() == doctest::Approx(2.0));
    CHECK(mid["shader"]["params"]["GLOW_WIDTH"].get<double>() == doctest::Approx(0.5));  // not keyed: kept
    CHECK(mid["ntsc"]["settings"]["composite_noise_intensity"].get<double>() == doctest::Approx(0.2));
    CHECK(mid["ntsc"]["settings"]["head_switching_height"] == 11);  // int: 8 + 12 * 0.25 rounded
    CHECK(mid["ntsc"]["settings"]["chroma_noise"] == false);        // bool holds
    CHECK(mid["glitch"]["values"]["signal_strength"].get<double>() == doctest::Approx(0.75));
    CHECK(mid["glitch"]["values"]["closed_captions"] == 1);         // toggle holds
    CHECK(mid["digital"]["values"]["quality"].get<double>() == doctest::Approx(0.4));
    CHECK(mid["digital"]["values"]["frame_rate"] == 15);            // structure: never keyed (look's own)
    CHECK(mid["digital"]["values"]["codec"] == 0);                  // choice / structure holds
    const json end = tl::look_at(look, t, r, 1.0);
    CHECK(end["ntsc"]["settings"]["chroma_noise"] == true);
    CHECK(end["shader"]["params"]["SHARPNESS_IMAGE"].get<double>() == 5);
    CHECK(end["digital"]["values"]["frame_rate"] == 15);
    // before the first / after the last key: clamped
    const json lone = tl::look_at(look, tl::parse(json{{"timeline", {{"keys", json::array({look["timeline"]["keys"][0]})}}}}), r, 0.0);
    CHECK(lone["ntsc"]["settings"]["composite_noise_intensity"].get<double>() == doctest::Approx(0.5));
    // no keys: the look as it is
    CHECK(tl::look_at(look, tl::Timeline{}, r, 0.3) == look);
}

TEST_CASE("animated preset -> host keyframes match the upstream evaluation") {
    const Schema &s = schema();
    const double fps = 24;
    // every bundled look with timeline keys (all of them must be playable by the host)
    const std::vector<std::string> names = animated_presets();
    CHECK(names.size() >= 16);
    for (const std::string &name : names) {
        CAPTURE(name);
        const json preset = read_preset(name);
        const PresetKeys pk = preset_keys(s, preset, fps, 120);
        REQUIRE(pk.animated());
        CHECK(pk.pass == int64_t(std::llround(preset["timeline"]["duration"].get<double>() * fps)));
        CHECK(pk.first == 0);
        CHECK(pk.frames <= 120);
        CHECK(pk.frames >= std::min<int64_t>(120, pk.pass));
        size_t moving = 0, total = 0;
        for (const auto &[i, k] : pk.keys) {
            REQUIRE_FALSE(k.empty());
            CHECK(k.front().first == 0);
            total += k.size();
            if (k.size() > 1) {
                ++moving;
                CHECK(k.back().first <= pk.frames - 1);
                CHECK(k.size() <= size_t(pk.frames));
            }
            CHECK_FALSE(s.params[i].feedback);
        }
        MESSAGE(name << ": " << moving << " params move, " << total << " keyframes over " << pk.frames << " frames");
        CHECK(moving > 0);
        // Every frame, including a second pass: the host's look == the upstream evaluation at
        // that frame's normalized time, as the host params hold it, within the keys' tolerance.
        json first = host_look(pk, preset, 0);
        bool changed = false;
        for (int64_t n = 0; n < pk.frames; ++n) {
            CAPTURE(n);
            const json want = as_host_look(preset_look_at_frame(s, preset, n, fps));
            std::string where;
            const json got = host_look(pk, preset, n);
            CHECK_MESSAGE(within_key_tolerance(s, got, want, &where), where);
            if (!near(got, first)) changed = true;
        }
        CHECK(changed);
        // Past the keyed stretch the last key holds.
        CHECK(host_look(pk, preset, 500) == host_look(pk, preset, pk.frames - 1));
    }
}

// Sparse keys (0.5.0-rc.3): far fewer than one per frame, and the same look within the
// stated tolerance for every bundled animated preset at common frame rates, for hosts that
// interpolate linearly (Resolve's Edit and Fusion pages, the mock host), and with guard keys
// also for smooth splines, anywhere in a long effect.
TEST_CASE("animated preset: sparse keys follow the per-frame curve at every frame") {
    const Schema &s = schema();
    size_t sparse = 0, dense = 0;
    for (const std::string &name : animated_presets()) {
        const json preset = read_preset(name);
        for (double fps : {23.976, 25.0, 30.0, 60.0}) {
            CAPTURE(name);
            CAPTURE(fps);
            const PresetKeys pk = preset_keys(s, preset, fps, 0, 216079);  // unknown length, an hour in
            // With guard keys, for hosts with smooth splines (none of the plug-ins' hosts so far).
            const PresetKeys guarded = preset_keys(s, preset, fps, 0, 216079, true);
            REQUIRE(pk.animated());
            CHECK(pk.first % pk.pass == 0);
            CHECK(pk.first <= 216079);
            CHECK(216079 < pk.first + pk.frames);
            CHECK(pk.frames % pk.pass == 0);
            CHECK(pk.frames <= kMaxKeyedFrames);
            size_t here = 0, by_type[5] = {0, 0, 0, 0, 0};
            for (const auto &[i, k] : pk.keys)
                if (k.size() > 1) {
                    sparse += k.size();
                    here += k.size();
                    by_type[int(s.params[i].type)] += k.size();
                    dense += size_t(pk.frames);
                }
            // About kMaxPresetKeys keys (one pass, whatever it takes, at least).
            CHECK((here <= size_t(kMaxPresetKeys + 64) || pk.frames == pk.pass));
            if (fps == 60.0)
                MESSAGE(name << " @60: " << here << " keys over " << pk.frames << " frames (pass " << pk.pass
                             << "); bool " << by_type[0] << " int " << by_type[1] << " float " << by_type[2]
                             << " choice " << by_type[3]);
            // Two passes at the start of the stretch (and the frame before it: the first values hold)
            // and the last frames of it.
            auto check = [&](const PresetKeys &k, bool spline, double factor) {
                std::vector<int64_t> frames;
                for (int64_t n = k.first - 1; n < k.first + std::min<int64_t>(k.frames, 2 * k.pass); ++n) frames.push_back(n);
                for (int64_t n = k.first + k.frames - 5; n < k.first + k.frames; ++n) frames.push_back(n);
                for (int64_t n : frames) {
                    CAPTURE(n);
                    const json want = as_host_look(preset_look_at_frame(s, preset, std::max(n, k.first), fps));
                    std::string where;
                    CHECK_MESSAGE(within_key_tolerance(s, host_look(k, preset, n, spline), want, &where, factor),
                                  (spline ? "spline " : "linear ") << where);
                }
            };
            check(pk, false, 1.0);
            check(guarded, true, 2.0);
        }
    }
    MESSAGE("sparse keys: " << sparse << " instead of " << dense << " (one per frame)");
    CHECK(sparse * 6 < dense);
}

TEST_CASE("animated preset: direct evaluation of a linear segment") {
    // "Glitch pop": every key Linear. Frame n of a 24 fps host sits at t = (n mod 48) / 47.
    const json preset = read_preset("Glitch pop");
    const json &keys = preset["timeline"]["keys"];
    const PresetKeys pk = preset_keys(schema(), preset, 24, 96);
    const size_t ci = idx("ntsc.composite_preemphasis");
    REQUIRE(pk.keys.count(ci));
    CHECK(pk.keys.at(ci).size() < 96 / 2);
    for (int64_t n = 0; n < 96; ++n) {
        CAPTURE(n);
        const double t = double(n % 48) / 47.0;
        size_t a = 0;
        while (a + 1 < keys.size() && keys[a + 1]["t"].get<double>() <= t) ++a;
        double want;
        if (a + 1 >= keys.size()) {
            want = keys[a]["ntsc"]["composite_preemphasis"].get<double>();
        } else {
            const double ta = keys[a]["t"], tb = keys[a + 1]["t"];
            const double va = keys[a]["ntsc"]["composite_preemphasis"], vb = keys[a + 1]["ntsc"]["composite_preemphasis"];
            want = va + (vb - va) * (t - ta) / (tb - ta);
        }
        CHECK(keyed_value(ci, pk.keys.at(ci), double(n)).get<double>() == doctest::Approx(want).epsilon(1e-9));
    }
}

TEST_CASE("presets without keys are not animated; Screen Loop presets never") {
    const Schema &s = schema();
    CHECK_FALSE(preset_keys(s, read_preset("Clean VHS"), 24, 100).animated());
    CHECK_FALSE(preset_keys(s, read_preset("Wavy loop"), 24, 100).animated());  // timeline on, no keys
    CHECK_FALSE(preset_keys(s, read_preset("Screen Loop/Bottom to top"), 24, 100).animated());
    // unknown effect length: whole passes from the origin, or around the playhead
    const json gp = read_preset("Glitch pop");
    PresetKeys pk = preset_keys(s, gp, 30, 0);
    CHECK(pk.pass == 60);
    CHECK(pk.first == 0);
    const int64_t stretch = pk.frames;  // as many passes as kMaxPresetKeys allows
    CHECK(stretch % 60 == 0);
    CHECK(stretch > 10 * 60);
    CHECK(stretch <= kMaxKeyedFrames);
    pk = preset_keys(s, gp, 30, 0, 108000 + 75);  // an hour into a 30 fps timeline
    CHECK(pk.first == (108075 - stretch / 4) / 60 * 60);
    CHECK(pk.frames == stretch);
    // very long effects: capped, from the start while the playhead is in the first stretch
    pk = preset_keys(s, gp, 30, 100000, 100);
    CHECK(pk.first == 0);
    CHECK(pk.frames == stretch);
    // ... and around it later on, never past the effect's end
    pk = preset_keys(s, gp, 30, 100000, 99990);
    CHECK(pk.first % 60 == 0);
    CHECK(pk.first + pk.frames == 100000);
    CHECK(pk.frames > stretch - 60);
    // short effects: all of it
    pk = preset_keys(s, gp, 30, 150, 140);
    CHECK(pk.first == 0);
    CHECK(pk.frames == 150);
}

TEST_CASE("choice applies: Horizontal resolution sets the width, a hand edit demotes it to Custom") {
    const Schema &s = schema();
    const size_t pre = idx("downscale.preset"), w = idx("downscale.width");
    auto a = option_applies(s, pre, "Arcade (384px)");
    REQUIRE(a.size() == 1);
    CHECK(a[0].first == w);
    CHECK(a[0].second == 384);
    CHECK(option_applies(s, pre, "Custom").empty());
    CHECK_FALSE(demoted_choice(s, pre, "Arcade (384px)", w, 384));
    CHECK(demoted_choice(s, pre, "Arcade (384px)", w, 400) == json("Custom"));
    CHECK_FALSE(demoted_choice(s, pre, "Custom", w, 400));
    CHECK_FALSE(demoted_choice(s, pre, "Arcade (384px)", idx("downscale.method"), "area"));
    // the Digital video formats' labels are options too, with their width
    a = option_applies(s, pre, "MiniDV (720px)");
    REQUIRE(a.size() == 1);
    CHECK(a[0].second == 720);
}

TEST_CASE("Digital video formats: in the Preset menu after the released presets, applying switches the stage on") {
    const Schema &s = schema();
    REQUIRE(s.digital_formats.size() == 6);
    json arr = json::array();
    for (const char *n : {"Clean VHS", "Zzz new", "CD-ROM (low quality)", "Chunky webcam"})
        arr.push_back({{"name", n}, {"kind", "look"}, {"json", json::object()}});
    auto menu = parse_presets(arr.dump().c_str(), s.digital_formats);
    REQUIRE(menu.size() == 10);
    CHECK(menu[0].name == "Clean VHS");
    CHECK(menu[1].name == "CD-ROM (low quality)");
    CHECK(menu[3].name == "Digital video/Webcam (2005)");
    CHECK(menu[3].format);
    CHECK(menu[8].name == "Digital video/Datamosh");
    CHECK(menu[9].name == "Zzz new");  // presets newer than the formats follow them
    // released order: v0.4.0's 32 untouched, then 0.5.0's looks and formats
    const auto &rel = released_preset_order();
    REQUIRE(rel.size() == 40);
    CHECK(rel[0] == "15 second glitches");
    CHECK(rel[31] == "Webcam (low quality)");
    CHECK(rel[34] == "Digital video/Webcam (2005)");

    const json &minidv = s.digital_formats[2]["applies"];
    auto vals = applies_values(s, minidv);
    std::map<std::string, json> by_id;
    for (const auto &[i, v] : vals) by_id[s.params[i].id] = v;
    CHECK(by_id["digital.enabled"] == true);
    CHECK(by_id["downscale.enabled"] == true);
    CHECK(by_id["downscale.width"] == 720);
    CHECK(by_id["downscale.preset"] == "MiniDV (720px)");
    CHECK(by_id.count("digital.quality2"));     // /digital/values spread over the params
    CHECK(by_id.count("digital.frame_rate"));
    CHECK(by_id["digital.exposure"] == 0.0);    // legacy controls cleared
    CHECK_FALSE(by_id.count("ntsc.enabled"));   // the rest of the look stays
}

TEST_CASE("looks are upstream JSON: constants and numeric toggles") {
    const Schema &s = schema();
    json look, fb;
    build_look(s, [&](size_t i) { return s.params[i].def; }, look, fb);
    CHECK(look["version"] == 2);
    CHECK(look["ntsc"]["settings"]["version"] == 1);
    CHECK(fb["kind"] == "ntscrt-video-feedback");
    CHECK(fb["version"] == 2);
    CHECK(look["glitch"]["values"]["closed_captions"].is_number());
    CHECK(look["glitch"]["values"]["closed_captions"] == 1);
    CHECK(fb["values"]["loop"] == 0);
    CHECK(look["ntsc"]["enabled"].is_boolean());  // plain bools stay bools
}

TEST_CASE("bundled presets round-trip through the params") {
    const Schema &s = schema();
    for (const std::string name : {"Clean VHS", "Chunky webcam", "CD-ROM (low quality)", "V Hold glitch", "Webcam (low quality)"}) {
        CAPTURE(name);
        const json preset = read_preset(name);
        const json a = as_host_look(preset);
        // save -> load: the saved look applied again gives the same look
        CHECK(as_host_look(a) == a);
        // every value the (upgraded) preset sets is in the look as it is
        const json up = ntscrt::digital::migrate::upgraded(preset);
        for (const auto &p : s.params) {
            if (p.feedback || !p.in_presets) continue;
            json::json_pointer ptr(p.path);
            if (!up.contains(ptr) || !a.contains(ptr)) continue;
            const json &pv = up.at(ptr), &lv = a.at(ptr);
            CAPTURE(p.id);
            if (pv.is_number() && lv.is_number()) CHECK(lv.get<double>() == doctest::Approx(pv.get<double>()));
            else if (pv.is_boolean() && lv.is_number()) CHECK(lv.get<double>() == (pv.get<bool>() ? 1.0 : 0.0));
            else CHECK(lv == pv);
        }
    }
}
