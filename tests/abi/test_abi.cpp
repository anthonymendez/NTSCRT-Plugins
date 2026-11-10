#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>
#include <nlohmann/json.hpp>

#include <vector>

TEST_CASE("schema is valid JSON with params") {
    auto j = nlohmann::json::parse(ntscrt_param_schema_json());
    CHECK(j["version"] == 1);
    CHECK(j["params"].size() > 0);
}

TEST_CASE("bundled presets load") {
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_SOURCE_DIR};
    ntscrt_instance *inst = nullptr;
    REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
    auto j = nlohmann::json::parse(ntscrt_bundled_presets_json(inst));
    CHECK(j.size() >= 20);
    CHECK(j[0]["name"].is_string());
    ntscrt_destroy(inst);
}

TEST_CASE("float RGBA round-trips through an all-off look") {
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_SOURCE_DIR};
    ntscrt_instance *inst = nullptr;
    REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
    const int w = 8, h = 4;
    std::vector<float> src(w * h * 4), dst(w * h * 4, -1.f);
    for (int i = 0; i < w * h; ++i) { src[i * 4] = (i % 7) / 6.f; src[i * 4 + 1] = 0.5f; src[i * 4 + 2] = 1.f; src[i * 4 + 3] = 0.25f; }
    // bottom-up host layout (OpenFX): data points at last row, negative stride
    ntscrt_image s{w, h, -ptrdiff_t(w * 16), NTSCRT_RGBA32F, src.data() + (h - 1) * w * 4};
    ntscrt_image d{w, h, -ptrdiff_t(w * 16), NTSCRT_RGBA32F, dst.data() + (h - 1) * w * 4};
    const char *look = R"({"downscale":{"enabled":false},"ntsc":{"enabled":false},"shader":{"enabled":false},
                         "glitch":{"enabled":false},"digital":{"enabled":false}})";
    ntscrt_render_args a{};
    a.fps = 30; a.look_json = look; a.render_scale = 1;
    REQUIRE(ntscrt_render_cpu(inst, &a, &s, &d) == NTSCRT_OK);
    for (int i = 0; i < w * h * 4; ++i) CHECK(dst[i] == doctest::Approx(src[i]).epsilon(1.0 / 255));
    ntscrt_destroy(inst);
}
