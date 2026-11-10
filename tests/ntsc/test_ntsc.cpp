// NTSC stage (ntsc-rs via rust/ntscrs-capi).
#include <doctest/doctest.h>
#include <ntscrs_capi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "stages/ntsc/ntsc_internal.h"
#include "stages/stages.h"

using namespace ntscrt;
namespace fs = std::filesystem;

namespace {

json read_json(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "cannot open " << p.string());
    return json::parse(f);
}

// A deterministic test picture with detail, color and flat areas.
ImageBGRA8 test_picture(int w = 320, int h = 240) {
    ImageBGRA8 im;
    im.resize(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = im.row(y) + x * 4;
            bool check = ((x / 16) + (y / 16)) % 2 == 0;
            p[0] = uint8_t(x * 255 / (w - 1));         // B
            p[1] = uint8_t(check ? 200 : 40);          // G
            p[2] = uint8_t(y * 255 / (h - 1));         // R
            p[3] = 255;
        }
    return im;
}

std::vector<fs::path> look_presets() {
    std::vector<fs::path> out;
    for (auto &e : fs::directory_iterator(fs::path(NTSCRT_SOURCE_DIR) / "presets"))
        if (e.path().extension() == ".json") out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

ImageBGRA8 run(NtscStage &st, const json &look, int64_t frame, ImageBGRA8 im, double render_scale = 1.0,
               uint64_t seed = 0) {
    FrameContext ctx;
    ctx.frame = frame;
    ctx.look = &look;
    ctx.render_scale = render_scale;
    ctx.seed = seed;
    st.process(im, ctx);
    return im;
}

}  // namespace

TEST_CASE("descriptors are a non-empty JSON array of named settings") {
    json d = json::parse(NtscStage::descriptors_json());
    REQUIRE(d.is_array());
    CHECK(d.size() > 20);
    for (auto &s : d) {
        CHECK(s.contains("name"));
        CHECK(s.contains("kind"));
    }
}

TEST_CASE("house defaults: ntsc-rs defaults overlaid with upstream app defaults") {
    json eff = json::parse(ntsc_effective_settings_json(json::object()));
    CHECK(eff["version"] == 1);
    CHECK(eff["bandwidth_scale"].get<double>() == doctest::Approx(0.3));
    CHECK(eff["vhs_tape_speed"] == 3);
    CHECK(eff["scale_with_video_size"] == true);
    // A partial settings object overrides only its keys; host-style numbers are coerced.
    json look = {{"ntsc", {{"settings", {{"filter_type", 1.0}, {"snow_intensity", 0.5}, {"head_switching", 0}}}}}};
    json e2 = json::parse(ntsc_effective_settings_json(look));
    CHECK(e2["filter_type"].is_number_integer());
    CHECK(e2["filter_type"] == 1);
    CHECK(e2["snow_intensity"].get<double>() == doctest::Approx(0.5));
    CHECK(e2["head_switching"] == false);
    CHECK(e2["bandwidth_scale"].get<double>() == doctest::Approx(0.3));
    // ntsc-rs accepts it.
    NtscInstance *inst = ntsc_new();
    char *err = nullptr;
    CHECK(ntsc_settings_from_json(inst, ntsc_effective_settings_json(look).c_str(), &err) == 0);
    if (err) ntsc_string_free(err);
    ntsc_free(inst);
}

TEST_CASE("every bundled preset's ntsc block parses") {
    int n = 0;
    for (auto &p : look_presets()) {
        json look = read_json(p);
        if (!look.contains("ntsc")) continue;
        ++n;
        INFO(p.filename().string());
        REQUIRE(look["ntsc"].contains("settings"));
        NtscInstance *inst = ntsc_new();
        REQUIRE(inst);
        // As stored (what upstream pushes to ntsc-rs) ...
        char *err = nullptr;
        int rc = ntsc_settings_from_json(inst, look["ntsc"]["settings"].dump().c_str(), &err);
        CHECK_MESSAGE(rc == 0, (err ? err : ""));
        if (err) { ntsc_string_free(err); err = nullptr; }
        // ... and through the stage's merge.
        rc = ntsc_settings_from_json(inst, ntsc_effective_settings_json(look).c_str(), &err);
        CHECK_MESSAGE(rc == 0, (err ? err : ""));
        if (err) ntsc_string_free(err);
        ntsc_free(inst);
        // Merging house defaults under a full preset changes nothing.
        json eff = json::parse(ntsc_effective_settings_json(look));
        for (auto it = look["ntsc"]["settings"].begin(); it != look["ntsc"]["settings"].end(); ++it)
            CHECK_MESSAGE(eff[it.key()] == it.value(), it.key());
    }
    CHECK(n >= 20);
}

TEST_CASE("active: upstream default is on") {
    NtscStage st;
    CHECK(st.active(json::object()));
    CHECK(st.active(json{{"ntsc", {{"settings", json::object()}}}}));
    CHECK_FALSE(st.active(json{{"ntsc", {{"enabled", false}}}}));
    CHECK(st.active(json{{"ntsc", {{"enabled", true}}}}));
}

TEST_CASE("same frame is deterministic, noise changes across frames") {
    json look = read_json(fs::path(NTSCRT_SOURCE_DIR) / "presets" / "Medium VHS.json");
    ImageBGRA8 src = test_picture();
    NtscStage a, b;
    ImageBGRA8 f5a = run(a, look, 5, src);
    ImageBGRA8 f5b = run(b, look, 5, src);
    ImageBGRA8 f5c = run(a, look, 5, src);  // cached settings path
    CHECK(f5a.px == f5b.px);
    CHECK(f5a.px == f5c.px);
    CHECK(f5a.px != src.px);  // it did something
    ImageBGRA8 f6 = run(a, look, 6, src);
    CHECK(f6.px != f5a.px);
    CHECK(f5a.width == src.width);
    CHECK(f5a.height == src.height);
}

TEST_CASE("settings changes are picked up (cache keyed on settings)") {
    json look = read_json(fs::path(NTSCRT_SOURCE_DIR) / "presets" / "Medium VHS.json");
    ImageBGRA8 src = test_picture();
    NtscStage st;
    ImageBGRA8 a = run(st, look, 3, src);
    json look2 = look;
    look2["ntsc"]["settings"]["snow_intensity"] = 0.9;
    ImageBGRA8 b = run(st, look2, 3, src);
    CHECK(a.px != b.px);
    ImageBGRA8 c = run(st, look, 3, src);
    CHECK(a.px == c.px);
}

TEST_CASE("seed offset and render scale") {
    json look = read_json(fs::path(NTSCRT_SOURCE_DIR) / "presets" / "Medium VHS.json");
    ImageBGRA8 src = test_picture();
    NtscStage st;
    ImageBGRA8 base = run(st, look, 2, src);
    CHECK(run(st, look, 2, src, 1.0, 0).px == base.px);
    CHECK(run(st, look, 2, src, 1.0, 12345).px != base.px);
    // scale_with_video_size on: render scale is already accounted for by ntsc-rs.
    CHECK(run(st, look, 2, src, 0.5).px == base.px);
    json fixed = look;
    fixed["ntsc"]["settings"]["scale_with_video_size"] = false;
    ImageBGRA8 full = run(st, fixed, 2, src, 1.0);
    ImageBGRA8 half = run(st, fixed, 2, src, 0.5);
    CHECK(full.px != half.px);
}

TEST_CASE("odd sizes and a 1x1 picture don't crash") {
    json look = read_json(fs::path(NTSCRT_SOURCE_DIR) / "presets" / "Obliterated.json");
    NtscStage st;
    for (auto [w, h] : std::vector<std::pair<int, int>>{{1, 1}, {3, 7}, {641, 359}}) {
        ImageBGRA8 im = test_picture(std::max(w, 2), std::max(h, 2));
        im.resize(w, h);
        CHECK_NOTHROW(run(st, look, 1, im));
    }
}
