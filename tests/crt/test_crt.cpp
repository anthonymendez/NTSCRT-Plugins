// CRT stage: the 7 bundled librashader presets, output sizing (letterbox,
// ScanlineGrid supersampling) and the ShaderBypass port (upstream
// Tests/CrtCoreTests/ShaderBypassTests.swift).
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <ntscrt/ntscrt.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <string>

#include "../gpu/gpu_test_util.h"
#include "stages/crt/crt_sizing.h"
#include "stages/downscale/downscale_kernels.h"
#include "stages/stages.h"

using namespace gputest;
namespace fs = std::filesystem;

namespace {

const char *kPresetIds[] = {"aperture", "easymode", "glow_gauss", "glow_lanczos", "hyllian", "royale", "sim"};

std::vector<uint8_t> load_bgra(const std::string &path, int &w, int &h) {
    int n = 0;
    unsigned char *d = stbi_load(path.c_str(), &w, &h, &n, 4);
    REQUIRE(d);
    std::vector<uint8_t> px(d, d + size_t(w) * h * 4);
    stbi_image_free(d);
    for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);  // RGBA -> BGRA
    return px;
}

struct Stats {
    double mean = 0, stddev = 0;
};
Stats stats(const std::vector<uint8_t> &px) {
    Stats s;
    size_t n = 0;
    for (size_t i = 0; i < px.size(); i += 4)
        for (int c = 0; c < 3; ++c) { s.mean += px[i + c]; ++n; }
    s.mean /= double(n);
    for (size_t i = 0; i < px.size(); i += 4)
        for (int c = 0; c < 3; ++c) s.stddev += (px[i + c] - s.mean) * (px[i + c] - s.mean);
    s.stddev = std::sqrt(s.stddev / double(n));
    return s;
}

// Bypass render through CrtStage (shader off) into w x h.
std::vector<std::vector<int>> bypass(gpu::Device &d, const gpu::Texture &src, int w, int h) {
    CrtStage crt(d, NTSCRT_RESOURCES_DIR);
    json look = {{"shader", {{"enabled", false}}}};
    FrameContext ctx;
    ctx.look = &look;
    auto out = d.create_texture({w, h, gpu::Format::BGRA8, true, true});
    crt.render(src, *out, ctx);
    return green(read(d, *out), w, h);
}

}  // namespace

TEST_CASE("ScanlineGrid.supersampleFactor port") {
    CHECK(crt::supersample_factor(240, 480) == 1);   // exact multiple
    CHECK(crt::supersample_factor(240, 1080) == 1);  // 4.5 rows per line
    CHECK(crt::supersample_factor(416, 702) == 4);   // upstream's banding example
    CHECK(crt::supersample_factor(1080, 540) == 2);  // minifying: stepped down
    CHECK(crt::supersample_factor(2160, 540) == 1);  // would be > 4x the target
    CHECK(crt::supersample_factor(1200, 1300) == 2); // 4x would exceed 4096 rows
    CHECK(crt::supersample_factor(0, 100) == 1);
}

TEST_CASE("fit_rect letterboxes to the chain input aspect") {
    CHECK(crt::fit_rect(320, 180, 1920, 1080) == crt::Rect{0, 0, 1920, 1080});
    CHECK(crt::fit_rect(320, 240, 1920, 1080) == crt::Rect{240, 0, 1440, 1080});  // pillarbox
    CHECK(crt::fit_rect(256, 100, 640, 480) == crt::Rect{0, 115, 640, 250});     // letterbox
    CHECK(crt::fit_rect(1000, 999, 1000, 1000) == crt::Rect{0, 0, 1000, 1000});  // within 1%: fill
}

// ---- ShaderBypassTests port ----

TEST_CASE("bypass: whole multiple is an exact nearest enlargement") {
    auto d = device();
    if (!d) return;
    auto src = bgra(*d, 4, 3, gray_image(4, 3, [](int x, int y) { return 20 + 17 * x + 50 * y; }));
    auto out = bypass(*d, *src, 16, 12);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x) CHECK(out[y][x] == 20 + 17 * (x / 4) + 50 * (y / 4));
}

TEST_CASE("bypass: fractional enlargement keeps block centres and brightness") {
    auto d = device();
    if (!d) return;
    auto src = bgra(*d, 8, 6, gray_image(8, 6, [](int x, int y) { return (x + y) % 2 == 0 ? 40 : 200; }));
    auto out = bypass(*d, *src, 28, 21);  // 3.5x
    for (int sy = 0; sy < 6; ++sy)
        for (int sx = 0; sx < 8; ++sx) {
            int cx = int((sx + 0.5) * 3.5), cy = int((sy + 0.5) * 3.5);
            CHECK(std::abs(out[cy][cx] - ((sx + sy) % 2 == 0 ? 40 : 200)) <= 2);
        }
    double sum = 0;
    for (auto &row : out)
        for (int v : row) sum += v;
    CHECK(std::abs(sum / (28 * 21) - 120) <= 2);
}

TEST_CASE("bypass: downscale is applied before enlarging") {
    auto d = device();
    if (!d) return;
    auto src = bgra(*d, 16, 12, gray_image(16, 12, [](int x, int y) { return (x * 13 + y * 29) % 251; }));
    Downscaler ds(*d);
    json look = {{"downscale", {{"enabled", true}, {"method", "nearest"}, {"width", 4}}}};
    FrameContext ctx;
    ctx.look = &look;
    auto small = ds.run(src, ctx);
    REQUIRE(small->desc().height == 16);  // upstream's height rule: at least 16 lines
    // Upstream's test used a 4x3 DownscaleSpec; the width-only stage can't
    // produce that, so drive the same check with an explicit 4x3 raster.
    auto ras = d->create_texture({4, 3, gpu::Format::BGRA8, true, false});
    std::shared_ptr<gpu::Texture> scratch;
    downscale_encode(*d, *src, *ras, DownscaleMethod::Nearest, scratch);
    auto out = bypass(*d, *ras, 16, 12);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x) CHECK(out[y][x] == out[(y / 4) * 4][(x / 4) * 4]);
}

TEST_CASE("bypass: flat input stays flat") {
    auto d = device();
    if (!d) return;
    auto src = bgra(*d, 32, 24, gray_image(32, 24, [](int, int) { return 128; }));
    for (auto [w, h] : {std::pair{96, 72}, std::pair{100, 75}, std::pair{16, 12}}) {
        CAPTURE(w);
        auto out = bypass(*d, *src, w, h);
        for (auto &row : out)
            for (int v : row) CHECK(std::abs(v - 128) <= 1);
    }
}

TEST_CASE("bypass: identity when sizes match") {
    auto d = device();
    if (!d) return;
    std::vector<uint8_t> px(37 * 23 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t(i * 31 + 7);
    auto src = bgra(*d, 37, 23, px);
    CrtStage crt(*d, NTSCRT_RESOURCES_DIR);
    json look = {{"shader", {{"enabled", false}, {"preset", "royale"}}}};
    FrameContext ctx;
    ctx.look = &look;
    auto out = d->create_texture({37, 23, gpu::Format::BGRA8, true, true});
    crt.render(*src, *out, ctx);
    CHECK(read(*d, *out) == px);
}

TEST_CASE("bypass: other aspect is letterboxed with black bars") {
    auto d = device();
    if (!d) return;
    auto src = bgra(*d, 40, 30, gray_image(40, 30, [](int, int) { return 200; }));  // 4:3
    auto out = bypass(*d, *src, 160, 90);                                          // 16:9
    // fit: 120x90 at x=20
    CHECK(out[45][5] == 0);
    CHECK(out[45][155] == 0);
    CHECK(out[45][80] == 200);
    CHECK(out[0][21] == 200);
}

// ---- librashader presets ----

TEST_CASE("each bundled CRT preset renders the test frame") {
    auto d = device();
    if (!d) return;
    int w = 0, h = 0;
    auto px = load_bgra(std::string(NTSCRT_SOURCE_DIR) + "/upstream/NTSCRT/TestAssets/game-frame.png", w, h);
    auto src = bgra(*d, w, h, px);
    Downscaler ds(*d);
    json dlook = {{"downscale", {{"enabled", true}, {"method", "area"}, {"width", 256}}}};
    FrameContext dctx;
    dctx.look = &dlook;
    auto chain_in = ds.run(src, dctx);  // 256x192

    // Reference: the same frame with the shader off.
    auto plain = d->create_texture({640, 480, gpu::Format::BGRA8, true, true});
    {
        CrtStage off(*d, NTSCRT_RESOURCES_DIR);
        json look = {{"shader", {{"enabled", false}}}};
        FrameContext ctx;
        ctx.look = &look;
        off.render(*chain_in, *plain, ctx);
    }
    auto plain_px = read(*d, *plain);

    CrtStage crt(*d, NTSCRT_RESOURCES_DIR);
    for (const char *id : kPresetIds) {
        // 640x480 = 2.5 rows per line -> supersampled 4x; 1024x768 = 4x exact.
        for (auto [ow, oh] : {std::pair{640, 480}, std::pair{1024, 768}}) {
            CAPTURE(id);
            CAPTURE(ow);
            json look = {{"shader", {{"enabled", true}, {"preset", id}, {"params", json::object()}}}};
            FrameContext ctx;
            ctx.frame = 10;
            ctx.look = &look;
            auto out = d->create_texture({ow, oh, gpu::Format::BGRA8, true, true});
            std::vector<uint8_t> o;
            try {
                crt.render(*chain_in, *out, ctx);
                if (crt.fallback_status()) throw std::runtime_error(crt.fallback_reason());  // drew the bypass
                o = read(*d, *out);
            } catch (const std::exception &e) {
                // GitHub's macOS runners are VMs with a paravirtualized GPU that times out
                // ("GPU Hang") on the heaviest shaders (crt-sim). Only there (macOS + GitHub
                // Actions) is that a warning; anywhere else, real hardware included, it fails.
                bool gha_macos = false;
#ifdef __APPLE__
                if (const char *gha = std::getenv("GITHUB_ACTIONS")) gha_macos = std::string(gha) == "true";
#endif
                if (gha_macos && std::string(e.what()).find("Hang") != std::string::npos) {
                    MESSAGE("WARNING: " << std::string(id) << " " << ow << "x" << oh << " hung the CI runner's virtual GPU: " << std::string(e.what()));
                    break;  // the command queue may be unusable for this preset now
                }
                FAIL(id << " " << ow << "x" << oh << ": " << e.what());
            }
            Stats s = stats(o);
            MESSAGE(id << " " << ow << "x" << oh << ": mean " << s.mean << " stddev " << s.stddev);
            CHECK(s.mean > 3);      // not black
            CHECK(s.stddev > 10);   // has structure
            if (ow == 640) {
                size_t diff = 0;
                for (size_t i = 0; i < o.size(); i += 4) diff += std::abs(int(o[i + 1]) - int(plain_px[i + 1])) > 8;
                CHECK(diff > o.size() / 4 / 20);  // the shader changed >5% of pixels
            }
        }
    }
}

TEST_CASE("shader params and preset ids/paths are honoured") {
    auto d = device();
    if (!d) return;
    auto src = bgra(*d, 64, 48, gray_image(64, 48, [](int x, int y) { return (x / 4 + y / 4) % 2 ? 180 : 60; }));
    CrtStage crt(*d, NTSCRT_RESOURCES_DIR);
    auto render = [&](const json &shader) {
        json look = {{"shader", shader}};
        FrameContext ctx;
        ctx.look = &look;
        auto out = d->create_texture({256, 192, gpu::Format::BGRA8, true, true});
        crt.render(*src, *out, ctx);
        return read(*d, *out);
    };
    auto base = render({{"preset", "glow_gauss"}});
    auto dim = render({{"preset", "glow_gauss"}, {"params", {{"BOOST", 0.5}}}});
    auto again = render({{"preset", "glow_gauss"}});  // param must not leak from the previous frame
    auto by_path = render({{"preset", "crt/crtglow_gauss.slangp"}});
    CHECK(stats(dim).mean < stats(base).mean - 5);
    CHECK(again == base);
    CHECK(by_path == base);
    CHECK(crt.fallback_status() == 0);
}

// A preset that can't load draws the bypass picture, says why, and isn't
// retried every frame (only after a flush).
TEST_CASE("missing or broken presets fall back to the bypass") {
    auto d = device();
    if (!d) return;
    auto src = bgra(*d, 64, 48, gray_image(64, 48, [](int x, int y) { return (x / 4 + y / 4) % 2 ? 180 : 60; }));
    CrtStage crt(*d, NTSCRT_RESOURCES_DIR);
    std::string step = "start";
    CAPTURE(step);
    auto render = [&](const json &shader) {
        step = shader.dump();
        json look = {{"shader", shader}};
        FrameContext ctx;
        ctx.look = &look;
        auto out = d->create_texture({256, 192, gpu::Format::BGRA8, true, true});
        crt.render(*src, *out, ctx);
        return read(*d, *out);
    };
    const auto plain = render({{"enabled", false}});
    CHECK(crt.fallback_status() == 0);

    const fs::path dir = fs::temp_directory_path() / "ntscrt_test_crt_broken";
    fs::create_directories(dir);
    const fs::path broken = dir / "broken.slangp";
    { std::ofstream(broken) << "shaders = 1\nshader0 = \"does_not_exist.slang\"\nscale_type0 = garbage\n"; }
    auto u8 = broken.u8string();
    const std::string broken_path(u8.begin(), u8.end());  // absolute: replaces <resources>/shaders

    for (const std::string preset : {std::string("no_such_preset"), std::string("crt/nope.slangp"), broken_path}) {
        CAPTURE(preset);
        const size_t before = crt.chain_load_attempts();
        CHECK(render({{"preset", preset}}) == plain);
        CHECK((crt.fallback_status() == NTSCRT_ERR_MISSING_RESOURCE || crt.fallback_status() == NTSCRT_ERR_GPU));
        CHECK(crt.fallback_reason().find(preset) != std::string::npos);
        const size_t after_first = crt.chain_load_attempts();
        CHECK(after_first - before <= 1);
        for (int i = 0; i < 3; ++i) CHECK(render({{"preset", preset}}) == plain);  // not retried
        CHECK(crt.chain_load_attempts() == after_first);
        CHECK(crt.fallback_status() != 0);
        crt.flush();  // an edit: tried once more
        render({{"preset", preset}});
        CHECK(crt.chain_load_attempts() - after_first == after_first - before);
    }
    // A good preset afterwards runs normally.
    render({{"preset", "glow_gauss"}});
    CHECK(crt.fallback_status() == 0);
    CHECK(crt.fallback_reason().empty());
    fs::remove_all(dir);
}
