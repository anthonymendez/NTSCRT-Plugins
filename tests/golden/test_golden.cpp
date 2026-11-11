// Golden comparison: ntscrt_core vs upstream NTSCRT crt-smoke references.
//
// References are produced by .github/workflows/golden.yml (macOS, upstream
// build) and committed under tests/golden/ref/<id>.png (+ <id>.json sidecar).
// Every entry whose reference is absent is skipped, so this passes on a fresh
// checkout. Environment overrides:
//   NTSCRT_GOLDEN_REF_DIR   reference dir        (default tests/golden/ref)
//   NTSCRT_TESTASSETS_DIR   input images         (default upstream/NTSCRT/TestAssets)
//   NTSCRT_RESOURCE_DIR     core resource_dir    (default NTSCRT_GOLDEN_RESOURCE_DIR)
//   NTSCRT_GOLDEN_OUT_DIR   where actual/diff PNGs of failures are written
//   NTSCRT_GOLDEN_ONLY      run only the entry with this id
//   NTSCRT_GOLDEN_STRICT=1  fail (instead of skip) when a reference is missing
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "golden_common.h"

#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <cstdio>
#include <set>

#ifndef NTSCRT_GOLDEN_RESOURCE_DIR
#define NTSCRT_GOLDEN_RESOURCE_DIR NTSCRT_SOURCE_DIR
#endif
#ifndef NTSCRT_GOLDEN_OUT_DIR
#define NTSCRT_GOLDEN_OUT_DIR "golden-out"
#endif

using namespace golden;

namespace {
const fs::path kRoot = fs::u8path(NTSCRT_SOURCE_DIR);
fs::path manifest_path() { return kRoot / "tests" / "golden" / "manifest.json"; }
fs::path ref_dir() { return fs::u8path(env_or("NTSCRT_GOLDEN_REF_DIR", (kRoot / "tests/golden/ref").string())); }
fs::path assets_dir() {
    return fs::u8path(env_or("NTSCRT_TESTASSETS_DIR", (kRoot / "upstream/NTSCRT/TestAssets").string()));
}
fs::path presets_dir() { return kRoot / "presets"; }
}  // namespace

TEST_CASE("golden: compare_rgb basics") {
    Image a{2, 1, {10, 20, 30, 255, 0, 0, 0, 0}};
    Image b = a;
    auto s = compare_rgb(a, b);
    CHECK(s.same_size);
    CHECK(s.mean_abs == 0.0);
    b.px[0] = 40;      // +30 on one channel
    b.px[3] = 0;       // alpha ignored
    s = compare_rgb(a, b);
    CHECK(s.mean_abs == doctest::Approx(30.0 / 6.0));
    CHECK(s.max_abs == 30);
    CHECK(s.frac_over_16 == doctest::Approx(0.5));
    Image c{1, 1, {0, 0, 0, 0}};
    CHECK_FALSE(compare_rgb(a, c).same_size);
}

TEST_CASE("golden: downscale height matches upstream") {
    CHECK(downscale_height(320, 640, 480) == 240);
    CHECK(downscale_height(256, 320, 240) == 192);
    CHECK(downscale_height(256, 1920, 1080) == 144);
    CHECK(downscale_height(8, 640, 480) == 16);
}

TEST_CASE("golden: manifest is well-formed") {
    auto m = read_json(manifest_path());
    REQUIRE_MESSAGE(m.has_value(), "cannot parse ", manifest_path().string());
    REQUIRE((*m)["entries"].is_array());
    std::set<std::string> ids;
    for (const auto &e : (*m)["entries"]) {
        INFO("entry ", e.dump());
        REQUIRE(e.contains("id"));
        CHECK(ids.insert(e["id"].get<std::string>()).second);  // unique
        CHECK(e.contains("input"));
        CHECK(e.value("shader", std::string()).find(".slangp") != std::string::npos);
        std::string err;
        CHECK_MESSAGE(look_for_entry(e, json(), presets_dir(), &err).has_value(), err);
    }
}

TEST_CASE("golden: ntscrt_core matches upstream crt-smoke references") {
    auto m = read_json(manifest_path());
    REQUIRE(m.has_value());
    const bool strict = env_or("NTSCRT_GOLDEN_STRICT", "") == "1";
    const std::string only = env_or("NTSCRT_GOLDEN_ONLY", "");
    const double default_thr = m->value("default_threshold", 2.0);

    int compared = 0, skipped = 0, hung = 0;
    ntscrt_instance *inst = nullptr;
    for (const auto &e : (*m)["entries"]) {
        const std::string id = e["id"];
        if (!only.empty() && id != only) continue;
        const fs::path ref_png = ref_dir() / (id + ".png");
        if (!fs::exists(ref_png)) {
            ++skipped;
            if (strict) FAIL_CHECK("missing reference ", ref_png.string());
            continue;
        }
        Image src, ref;
        const fs::path in_png = assets_dir() / fs::u8path(e["input"].get<std::string>());
        if (!load_png(in_png, src)) {
            ++skipped;
            MESSAGE("skip ", id, ": input ", in_png.string(), " missing (init upstream/NTSCRT submodule)");
            if (strict) FAIL_CHECK("missing input ", in_png.string());
            continue;
        }
        REQUIRE_MESSAGE(load_png(ref_png, ref), "unreadable ", ref_png.string());

        auto sidecar = read_json(ref_dir() / (id + ".json")).value_or(json());
        std::string err;
        auto look = look_for_entry(e, sidecar, presets_dir(), &err);
        REQUIRE_MESSAGE(look.has_value(), err);

        if (!inst) {
            const std::string res = env_or("NTSCRT_RESOURCE_DIR", NTSCRT_GOLDEN_RESOURCE_DIR);
            ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, res.c_str()};
            REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
        }
        ntscrt_flush_caches(inst);

        Image out{src.w, src.h, std::vector<uint8_t>(src.px.size())};
        ntscrt_image si{src.w, src.h, ptrdiff_t(src.w) * 4, NTSCRT_RGBA8, src.px.data()};
        ntscrt_image di{out.w, out.h, ptrdiff_t(out.w) * 4, NTSCRT_RGBA8, out.px.data()};
        const std::string look_s = look->dump();
        ntscrt_render_args ra{};
        // crt-smoke --frame N feeds ntsc-rs frame N and the shader FrameCount N; ntscrt_core
        // numbers clip frames from 0 and passes frame + 1 to both (internal.h ntsc_frame /
        // shader_frame, upstream's export convention), so N - 1 is the same frame.
        ra.frame_index = e.value("frame", 1) - 1;
        ra.fps = 30.0;
        ra.look_json = look_s.c_str();
        ra.render_scale = 1.0;
        const ntscrt_status st = ntscrt_render_cpu(inst, &ra, &si, &di);
        INFO("entry ", id, " look ", look_s);
        if (st != NTSCRT_OK && gpu_hang_on_gha_macos(ntscrt_last_error(inst))) {
            MESSAGE("WARNING: ", id, " hung the CI runner's virtual GPU: ", std::string(ntscrt_last_error(inst)));
            ++hung;
            ntscrt_destroy(inst);  // fresh instance for the next entry
            inst = nullptr;
            continue;
        }
        REQUIRE_MESSAGE(st == NTSCRT_OK, "render failed (", int(st), "): ", std::string(ntscrt_last_error(inst)));

        Image diff;
        DiffStats s = compare_rgb(out, ref, &diff);
        const double thr = e.value("threshold", default_thr);
        ++compared;
        std::printf("[golden] %-56s mean=%.3f max=%d over16=%.4f thr=%.2f %s\n", id.c_str(), s.mean_abs, s.max_abs,
                    s.frac_over_16, thr, (s.same_size && s.mean_abs <= thr) ? "ok" : "FAIL");
        if (!s.same_size || s.mean_abs > thr) {
            const fs::path od = fs::u8path(env_or("NTSCRT_GOLDEN_OUT_DIR", NTSCRT_GOLDEN_OUT_DIR));
            write_png(od / (id + ".actual.png"), out);
            if (s.same_size) write_png(od / (id + ".diff.png"), diff);
        }
        CHECK_MESSAGE(s.same_size, "size mismatch: ours ", out.w, "x", out.h, " ref ", ref.w, "x", ref.h);
        CHECK_MESSAGE(s.mean_abs <= thr, "mean abs diff ", s.mean_abs, " > ", thr);
    }
    if (inst) ntscrt_destroy(inst);
    if (compared == 0) MESSAGE("no golden references present (", skipped, " entries skipped) - run the 'golden' GitHub workflow and commit its artifact to tests/golden/ref/");
    std::printf("[golden] compared %d, skipped %d, GPU hangs (macOS CI only) %d\n", compared, skipped, hung);
}
