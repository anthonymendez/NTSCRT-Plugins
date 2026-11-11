// Façade caches and failure handling: look history (NULL = "same as now",
// identical strings parsed once), the source cache (byte budget, keyed by
// scale / size / alpha / crop), CRT fallback, device loss, the no-GPU path
// with a crop, and the opaque fast path of the pixel conversion.
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pixel_convert.h"
#include "testing.h"

using namespace ntscrt;

namespace {

ntscrt_instance *make(ntscrt_gpu_backend b = NTSCRT_GPU_AUTO) {
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, b, nullptr, nullptr, NTSCRT_BUILT_RESOURCES_DIR};
    ntscrt_instance *inst = nullptr;
    REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
    return inst;
}

// An RGBA8 clip whose frames differ (so temporal stages see motion).
struct Clip {
    int w, h;
    std::map<int64_t, std::vector<uint8_t>> frames;
    std::vector<int64_t> fetched;
    std::vector<int64_t> look_asked;
    std::map<const ntscrt_image *, std::unique_ptr<ntscrt_image>> live;
    // look_at answer per frame: NULL, or a string
    std::function<const char *(int64_t)> look;

    Clip(int w_, int h_) : w(w_), h(h_) {}
    std::vector<uint8_t> &frame(int64_t f) {
        auto &px = frames[f];
        if (px.empty()) {
            px.resize(size_t(w) * h * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    uint8_t *p = &px[(size_t(y) * w + x) * 4];
                    p[0] = uint8_t((x * 7 + f * 13) & 255);
                    p[1] = uint8_t((y * 5 + x) & 255);
                    p[2] = uint8_t(((x ^ y) * 3 + f * 29) & 255);
                    p[3] = 255;
                }
        }
        return px;
    }
    ntscrt_image image(int64_t f) { return {w, h, ptrdiff_t(w) * 4, NTSCRT_RGBA8, frame(f).data()}; }

    static const ntscrt_image *fetch(void *u, int64_t f) {
        auto *c = static_cast<Clip *>(u);
        if (f < 0) return nullptr;
        c->fetched.push_back(f);
        auto im = std::make_unique<ntscrt_image>(c->image(f));
        const ntscrt_image *k = im.get();
        c->live[k] = std::move(im);
        return k;
    }
    static void release(void *u, const ntscrt_image *im) { static_cast<Clip *>(u)->live.erase(im); }
    static const char *look_at(void *u, int64_t f) {
        auto *c = static_cast<Clip *>(u);
        c->look_asked.push_back(f);
        return c->look ? c->look(f) : nullptr;
    }
};

struct Result {
    ntscrt_status st;
    std::vector<uint8_t> px;
};

Result render(ntscrt_instance *inst, Clip &clip, int64_t frame, const std::string &look, bool with_look_at = true,
              double scale = 1.0) {
    ntscrt_render_args a{};
    a.frame_index = frame;
    a.fps = 30;
    a.look_json = look.c_str();
    a.render_scale = scale;
    a.fetch_source = &Clip::fetch;
    a.release_source = &Clip::release;
    a.look_at = with_look_at ? &Clip::look_at : nullptr;
    a.user = &clip;
    ntscrt_image src = clip.image(frame);
    Result r;
    r.px.assign(size_t(clip.w) * clip.h * 4, 7);
    ntscrt_image dst{clip.w, clip.h, ptrdiff_t(clip.w) * 4, NTSCRT_RGBA8, r.px.data()};
    r.st = ntscrt_render_cpu(inst, &a, &src, &dst);
    return r;
}

bool has_gpu(ntscrt_instance *inst) { return testing::stats(inst).has_gpu; }

}  // namespace

TEST_CASE("look_at: NULL, the current text and no callback render the same, parsing nothing extra") {
    // Glitch on: the receiver reads every frame's look up to 40.
    const std::string look = R"({"glitch":{"enabled":true}})";
    std::vector<uint8_t> ref;
    for (int mode = 0; mode < 3; ++mode) {
        CAPTURE(mode);
        ntscrt_instance *inst = make();
        Clip clip(96, 72);
        if (mode == 1) clip.look = [&](int64_t) { return look.c_str(); };  // same text as now
        Result r = render(inst, clip, 40, look, mode != 2);
        REQUIRE(r.st == NTSCRT_OK);
        if (mode == 0) {
            ref = r.px;
            if (has_gpu(inst)) CHECK(!clip.look_asked.empty());  // Glitch asked for history
        } else {
            CHECK(r.px == ref);
        }
        CHECK(testing::stats(inst).look_parses == 1);  // only the current look
        ntscrt_destroy(inst);
    }
}

TEST_CASE("look_at: keyframed looks are parsed once per distinct text, and give the right history") {
    // Glitch "smoothing" keyed over time: distinct strings per frame.
    auto look_for = [](int64_t f) {
        return std::string(R"({"glitch":{"enabled":true,"values":{"signal_strength":)") + std::to_string(0.2 + 0.01 * double(f % 7)) + "}}}";
    };
    std::map<int64_t, std::string> texts;
    for (int64_t f = 0; f <= 60; ++f) texts[f] = look_for(f);

    ntscrt_instance *a = make(), *b = make();
    Clip ca(96, 72), cb(96, 72);
    ca.look = [&](int64_t f) { return texts[f].c_str(); };
    cb.look = ca.look;
    Result r1 = render(a, ca, 60, texts[60]);
    REQUIRE(r1.st == NTSCRT_OK);
    const size_t parses = testing::stats(a).look_parses;
    CHECK(parses <= 8);  // 7 distinct texts (+ the current one shares one of them)
    // Again: nothing new to parse.
    Result r2 = render(a, ca, 60, texts[60]);
    CHECK(testing::stats(a).look_parses == parses);
    CHECK(r2.px == r1.px);
    // A fresh instance reaching frame 60 frame by frame gives the same picture.
    Result rb;
    for (int64_t f = 0; f <= 60; ++f) rb = render(b, cb, f, texts[f]);
    CHECK(rb.px == r1.px);
    ntscrt_destroy(a);
    ntscrt_destroy(b);
}

TEST_CASE("source cache: byte budget, LRU, keyed by scale, size and alpha mode") {
    // Digital reads earlier source frames through the cache.
    const std::string look = R"({"digital":{"enabled":true},"glitch":{"enabled":false},"ntsc":{"enabled":false}})";
    ntscrt_instance *inst = make();
    Clip clip(64, 48);
    const size_t frame_bytes = size_t(64) * 48 * 4;
    REQUIRE(render(inst, clip, 11, look).st == NTSCRT_OK);  // a seek: Digital encodes its run-up
    testing::Stats s = testing::stats(inst);
    CHECK(s.source_budget == size_t(512) << 20);
    CHECK(s.source_bytes == s.source_frames * frame_bytes);
    if (clip.fetched.empty()) {
        MESSAGE("Digital fetched no earlier frames: cache checks skipped");
        ntscrt_destroy(inst);
        return;
    }
    // Rendering it again from scratch (codec flushed) takes the history from the cache.
    ntscrt_flush_caches_mask(inst, NTSCRT_CACHE_DIGITAL);
    clip.fetched.clear();
    render(inst, clip, 11, look);
    const size_t refetch_same = clip.fetched.size();
    CHECK(refetch_same == 0);

    // A proxy render (scale 0.5) or another alpha mode must not reuse those frames.
    ntscrt_flush_caches_mask(inst, NTSCRT_CACHE_DIGITAL);
    clip.fetched.clear();
    render(inst, clip, 11, look, true, 0.5);
    CHECK(!clip.fetched.empty());
    ntscrt_flush_caches_mask(inst, NTSCRT_CACHE_DIGITAL);
    clip.fetched.clear();
    const std::string spill = R"({"alpha":{"mode":"spill"},"digital":{"enabled":true},"glitch":{"enabled":false},"ntsc":{"enabled":false}})";
    render(inst, clip, 11, spill);
    CHECK(!clip.fetched.empty());

    // More history (seeks elsewhere), then a budget of 3 frames: kept down to the
    // minimum of 8 frames, least recently used first.
    for (int64_t f : {21, 31, 41, 51, 61}) {
        ntscrt_flush_caches_mask(inst, NTSCRT_CACHE_DIGITAL);
        render(inst, clip, f, look);
    }
    REQUIRE(testing::stats(inst).source_frames > 8);
    clip.fetched.clear();
    testing::set_source_budget(inst, frame_bytes * 3);
    s = testing::stats(inst);
    CHECK(s.source_frames == 8);
    CHECK(s.source_bytes == 8 * frame_bytes);
    ntscrt_flush_caches_mask(inst, NTSCRT_CACHE_DIGITAL);
    render(inst, clip, 61, look);  // the most recent history survived
    CHECK(clip.fetched.empty());
    ntscrt_flush_caches_mask(inst, NTSCRT_CACHE_SOURCE);
    CHECK(testing::stats(inst).source_frames == 0);
    ntscrt_destroy(inst);
}

TEST_CASE("source cache never mixes sizes (proxy switch)") {
    const std::string look = R"({"digital":{"enabled":true},"glitch":{"enabled":false},"ntsc":{"enabled":false}})";
    ntscrt_instance *inst = make();
    Clip big(64, 48), small(32, 24);
    for (int64_t f = 0; f < 6; ++f) render(inst, big, f, look);
    ntscrt_flush_caches_mask(inst, NTSCRT_CACHE_DIGITAL);  // what a host does on a size change: codec only
    small.fetched.clear();
    Result r = render(inst, small, 5, look);
    CHECK(r.st == NTSCRT_OK);
    // Same result as a fresh instance at that size.
    ntscrt_instance *fresh = make();
    Clip small2(32, 24);
    Result r2 = render(fresh, small2, 5, look);
    CHECK(r.px == r2.px);
    ntscrt_destroy(fresh);
    ntscrt_destroy(inst);
}

TEST_CASE("CRT: a missing preset renders the plain picture, reports it, and recovers") {
    ntscrt_instance *inst = make();
    if (!has_gpu(inst)) {
        ntscrt_destroy(inst);
        return;
    }
    Clip clip(96, 72);
    const std::string off = R"({"shader":{"enabled":false},"glitch":{"enabled":false}})";
    const std::string bad = R"({"shader":{"enabled":true,"preset":"no_such_preset"},"glitch":{"enabled":false}})";
    const std::string good = R"({"shader":{"enabled":true,"preset":"glow_gauss"},"glitch":{"enabled":false}})";
    Result plain = render(inst, clip, 3, off);
    REQUIRE(plain.st == NTSCRT_OK);
    for (int i = 0; i < 3; ++i) {
        Result r = render(inst, clip, 3, bad);
        CHECK(r.st == NTSCRT_ERR_MISSING_RESOURCE);
        CHECK(std::string(ntscrt_last_error(inst)).find("no_such_preset") != std::string::npos);
        CHECK(r.px == plain.px);
    }
    Result r = render(inst, clip, 3, good);
    CHECK(r.st == NTSCRT_OK);
    CHECK(std::string(ntscrt_last_error(inst)).empty());
    CHECK(r.px != plain.px);
    ntscrt_destroy(inst);
}

TEST_CASE("device loss: a new device once, else the CPU stages") {
    ntscrt_instance *inst = make();
    if (!has_gpu(inst)) {
        ntscrt_destroy(inst);
        return;
    }
    Clip clip(96, 72);
    const std::string look = R"({"glitch":{"enabled":false}})";
    Result ref = render(inst, clip, 2, look);
    REQUIRE(ref.st == NTSCRT_OK);

    testing::simulate_device_lost(inst);
    Result r = render(inst, clip, 2, look);
    CHECK(r.st == NTSCRT_OK);
    CHECK(r.px == ref.px);
    CHECK(testing::stats(inst).has_gpu);
    CHECK(testing::stats(inst).device_resets == 0);  // reset by the good frame after it

    testing::simulate_device_lost(inst, true);
    r = render(inst, clip, 2, look);
    CHECK(r.st == NTSCRT_ERR_GPU);
    CHECK(std::string(ntscrt_last_error(inst)).find("GPU device lost") != std::string::npos);
    CHECK(!testing::stats(inst).has_gpu);
    CHECK(std::count(r.px.begin(), r.px.end(), uint8_t(7)) < r.px.size() / 10);  // written
    r = render(inst, clip, 3, look);  // stays on the CPU path, still says so
    CHECK(r.st == NTSCRT_ERR_GPU);
    ntscrt_destroy(inst);
}

TEST_CASE("no GPU + crop: the whole destination is written (letterboxed)") {
    ntscrt_instance *inst = make(
#ifdef _WIN32
        NTSCRT_GPU_METAL  // not on Windows: no device
#else
        NTSCRT_GPU_D3D11
#endif
    );
    REQUIRE(!has_gpu(inst));
    const int w = 64, h = 36;
    std::vector<float> src(size_t(w) * h * 4), dst(src.size(), -1.f);
    for (int i = 0; i < w * h; ++i) {
        src[i * 4 + 0] = 0.8f;
        src[i * 4 + 1] = 0.4f;
        src[i * 4 + 2] = 0.2f;
        src[i * 4 + 3] = 1.f;
    }
    ntscrt_image s{w, h, ptrdiff_t(w) * 16, NTSCRT_RGBA32F, src.data()};
    ntscrt_image d{w, h, ptrdiff_t(w) * 16, NTSCRT_RGBA32F, dst.data()};
    const char *look = R"({"crop":{"enabled":true,"ratio":"1:1"},"ntsc":{"enabled":false},"digital":{"enabled":false}})";
    ntscrt_render_args a{};
    a.fps = 30;
    a.look_json = look;
    a.render_scale = 1;
    REQUIRE(ntscrt_render_cpu(inst, &a, &s, &d) == NTSCRT_OK);
    int unwritten = 0;
    for (float v : dst) unwritten += v < 0;
    CHECK(unwritten == 0);
    // 1:1 crop of 64x36 -> 36x36, placed at x = 14: bars black, centre the picture.
    auto px = [&](int x, int y) { return &dst[(size_t(y) * w + x) * 4]; };
    CHECK(px(2, 18)[0] == 0.f);
    CHECK(px(61, 18)[0] == 0.f);
    CHECK(px(2, 18)[3] == 1.f);  // Clip mode: opaque bars, like the GPU bypass
    CHECK(px(32, 18)[0] == doctest::Approx(0.8f).epsilon(0.01));
    CHECK(px(32, 18)[1] == doctest::Approx(0.4f).epsilon(0.01));
    ntscrt_destroy(inst);
}

TEST_CASE("opaque fast path: no alpha buffer, identical pixels") {
    const int w = 7, h = 5;
    for (ntscrt_pixel_format f : {NTSCRT_RGBA8, NTSCRT_BGRA8, NTSCRT_RGBA16, NTSCRT_RGBA16F, NTSCRT_RGBA32F}) {
        CAPTURE(int(f));
        const size_t pb = f == NTSCRT_RGBA32F ? 16 : (f == NTSCRT_RGBA16 || f == NTSCRT_RGBA16F) ? 8 : 4;
        std::vector<uint8_t> buf(size_t(w) * h * pb);
        for (int i = 0; i < w * h; ++i)
            for (int c = 0; c < 4; ++c) {
                const float v = c == 3 ? 1.f : float((i * 37 + c * 11) % 100) / 99.f;
                uint8_t *p = buf.data() + size_t(i) * pb;
                switch (f) {
                case NTSCRT_RGBA8:
                case NTSCRT_BGRA8: p[c] = uint8_t(std::lround(v * 255)); break;
                case NTSCRT_RGBA16: reinterpret_cast<uint16_t *>(p)[c] = uint16_t(std::lround(v * 65535)); break;
                case NTSCRT_RGBA16F: {
                    // exact halves for 0 and 1, otherwise a coarse value
                    uint16_t hbits = v == 1.f ? 0x3c00 : uint16_t(0x3800 + int(v * 1000) % 0x3ff);
                    reinterpret_cast<uint16_t *>(p)[c] = hbits;
                    break;
                }
                case NTSCRT_RGBA32F: reinterpret_cast<float *>(p)[c] = v; break;
                }
            }
        ntscrt_image src{w, h, ptrdiff_t(w * pb), f, buf.data()};
        for (AlphaMode mode : {AlphaMode::Clip, AlphaMode::Spill})
            for (bool premult : {false, true}) {
                ImageBGRA8 im;
                std::vector<float> alpha{0.5f};  // stale content must go
                to_bgra8(src, im, &alpha, mode, premult);
                CHECK(alpha.empty());
                std::vector<uint8_t> a(buf.size()), b(buf.size());
                ntscrt_image da{w, h, ptrdiff_t(w * pb), f, a.data()}, db{w, h, ptrdiff_t(w * pb), f, b.data()};
                std::vector<float> ones(size_t(w) * h, 1.f);
                from_bgra8(im, &alpha, da, mode, premult);  // empty = opaque
                from_bgra8(im, &ones, db, mode, premult);   // the explicit equivalent
                CHECK(a == b);
            }
    }
    // One transparent pixel: alpha is stored for all.
    std::vector<uint8_t> px(size_t(w) * h * 4, 200);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    px[3] = 0;
    ntscrt_image src{w, h, ptrdiff_t(w) * 4, NTSCRT_RGBA8, px.data()};
    ImageBGRA8 im;
    std::vector<float> alpha;
    to_bgra8(src, im, &alpha, AlphaMode::Clip, false);
    REQUIRE(alpha.size() == size_t(w) * h);
    CHECK(alpha[0] == 0.f);
    CHECK(alpha[1] == 1.f);
}
