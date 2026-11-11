// Screen Loop end to end on the CPU reference kernels: port of the upstream
// HowlaroundTests "the loop, end to end" group (which drives the GIF exporter
// with the CRT shader off — so each frame's output is the camera's picture and
// the TV shows that picture resized, exactly CpuOps below), plus random-access
// determinism of the engine.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <random>

#include "stages/howlaround/howl_cpu.h"
#include "stages/howlaround/howl_engine.h"

using namespace ntscrt;
using namespace ntscrt::howl;

namespace {

using Pic = std::shared_ptr<const ImageF>;

struct CpuOps {
    using Pic = ::Pic;
    std::function<Pic(int64_t)> source;  // null = host can't supply past frames
    int fb_scale = 2;                    // TV picture = chain size x fb_scale (0 = upstream size)
    std::vector<HowlPass> passes;

    Pic scene(int64_t f) { return source ? source(f) : nullptr; }

    Pic camera(const Pic &scene, const Pic &tv, const CameraUniforms &u, double sigma, const CounterBitmap *counter,
               bool measure, double &scene_luma, double &camera_luma) {
        const ImageF *t = tv.get();
        ImageF a, b;
        if (t && sigma > 0) {
            BlurUniforms h{{1, 0}, float(sigma), blur_radius(sigma)}, v{{0, 1}, float(sigma), blur_radius(sigma)};
            a = blur_ref(*t, h);
            b = blur_ref(a, v);
            t = &b;
        }
        ImageF cimg;
        if (counter) cimg = counter_image(*counter);
        auto out = std::make_shared<const ImageF>(camera_ref(*scene, t, counter ? &cimg : nullptr, u));
        if (measure) {
            scene_luma = mean_luma(*scene);
            camera_luma = mean_luma(*out);
        }
        return out;
    }

    Pic tv_pass(const Pic &cam, const HowlPass &p) {
        passes.push_back(p);
        int w = cam->width * fb_scale, h = cam->height * fb_scale;
        if (fb_scale == 0) {
            Size s = feedback_size(cam->width, cam->height);
            w = s.width;
            h = s.height;
        }
        return std::make_shared<const ImageF>(resample_ref(*cam, w, h));
    }
};

constexpr int N = 240;

Pic texture(std::function<std::array<uint8_t, 3>(int, int)> pixel, int n = N) {
    std::vector<uint8_t> bgra(size_t(n) * n * 4, 255);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            auto c = pixel(x, y);
            size_t i = (size_t(y) * n + x) * 4;
            bgra[i] = c[2]; bgra[i + 1] = c[1]; bgra[i + 2] = c[0];
        }
    return std::make_shared<const ImageF>(ImageF::from_bgra8(bgra.data(), n, n));
}

// A red frame (the outer tenth) round a blue picture.
const Pic &framed() {
    static Pic p = texture([](int x, int y) {
        int edge = std::min({x, y, N - 1 - x, N - 1 - y});
        return edge < N / 10 ? std::array<uint8_t, 3>{230, 20, 20} : std::array<uint8_t, 3>{20, 20, 230};
    });
    return p;
}
// A white frame round mid gray.
const Pic &gray() {
    static Pic p = texture([](int x, int y) {
        int edge = std::min({x, y, N - 1 - x, N - 1 - y});
        return edge < N / 10 ? std::array<uint8_t, 3>{240, 240, 240} : std::array<uint8_t, 3>{128, 128, 128};
    });
    return p;
}
// Bright in the middle, dark around it.
const Pic &spot() {
    static Pic p = texture([](int x, int y) {
        double dx = x - N / 2, dy = y - N / 2;
        return std::sqrt(dx * dx + dy * dy) < N * 0.12 ? std::array<uint8_t, 3>{250, 250, 250}
                                                       : std::array<uint8_t, 3>{30, 30, 30};
    });
    return p;
}

Settings settings(std::map<std::string, double> v = {}) {
    std::map<std::string, double> base = {
        {"zoom", 0.5},   {"center_x", 0}, {"center_y", 0},    {"roll", 0},      {"turn", 0},
        {"tilt", 0},     {"shake", 0},    {"seed", 1},        {"drift_x", 0},   {"drift_y", 0},
        {"push", 0},     {"spin", 0},     {"loop", 0},        {"focus", 0},     {"brightness", 1},
        {"contrast", 1}, {"color_drift", 0}, {"hue_drift", 0}, {"auto_exposure", 0}, {"key", 0},
        {"key_invert", 0}, {"delay", 1},  {"counter", 0}};
    for (auto &[k, x] : v) base[k] = x;
    return Settings(base);
}

EngineConfig config(const Settings &s, int w, int h, double fps, double length, int64_t window = kDefaultWindow) {
    EngineConfig c;
    c.settings = s;
    c.fps = fps;
    c.length = length;
    c.aspect = double(w) / h;
    c.chain_w = w;
    c.chain_h = h;
    c.window = window;
    return c;
}

// The main path for frame n: camera, then (the chain is the identity here)
// the TV picture of this frame back into the loop.
Pic render(Engine<CpuOps> &e, CpuOps &ops, int64_t n, const Pic &scene) {
    Pic cam = e.camera(ops, n, scene);
    if (e.wants_tv(n)) e.store_tv(n, ops.tv_pass(cam, e.normal_pass(n)));
    return cam;
}

// Upstream gif(): a still exported for `frames` frames at 12 fps; the last frame.
Pic gif(const Pic &source, const Settings &s, int frames = 3) {
    CpuOps ops;
    ops.source = [&](int64_t) { return source; };
    Engine<CpuOps> e(config(s, N, N, 12, frames / 12.0));
    Pic last;
    for (int f = 0; f < frames; ++f) last = render(e, ops, f, source);
    return last;
}

struct RGB { double r, g, b; };
RGB pixel(const Pic &img, double fx, double fy) {
    int x = int(fx * img->width), y = int(fy * img->height);
    const float *p = img->at(x, y);
    return {p[0], p[1], p[2]};
}
bool is_red(RGB c) { return c.r > 0.6 && c.b < 0.35; }
bool is_blue(RGB c) { return c.b > 0.6 && c.r < 0.35; }
double luma3(RGB c) { return (c.r + c.g + c.b) / 3; }

}  // namespace

TEST_CASE("testNestedCopiesLandWhereTheCameraPutsThem") {
    Pic img = gif(framed(), settings());
    CHECK(is_red(pixel(img, 0.5, 0.04)));   // room frame
    CHECK(is_blue(pixel(img, 0.5, 0.18)));  // room
    CHECK(is_red(pixel(img, 0.5, 0.27)));   // first copy's frame
    CHECK(is_blue(pixel(img, 0.5, 0.33)));  // first copy
    CHECK(is_red(pixel(img, 0.5, 0.385)));  // second copy's frame
}

TEST_CASE("testZoomZeroIsExactlyTheNormalExport") {
    // Zero zoom: every frame is exactly the scene — with the auto exposure
    // running too (it measures the same picture, so it stays at 1).
    for (double ae : {0.0, 0.7}) {
        CpuOps ops;
        ops.source = [&](int64_t) { return framed(); };
        Engine<CpuOps> e(config(settings({{"zoom", 0}, {"counter", 0}, {"auto_exposure", ae}}), N, N, 12, 0.25));
        for (int f = 0; f < 3; ++f) CHECK(*render(e, ops, f, framed()) == *framed());
        CHECK(ops.passes.size() <= 3);  // no run-up: the camera doesn't see the TV
    }
}

TEST_CASE("testSameSettingsRenderTheSameFrames") {
    Settings s = settings({{"roll", 7}, {"focus", 0.3}, {"auto_exposure", 0.5}});
    Pic a = gif(framed(), s), b = gif(framed(), s);
    CHECK(*a == *b);
    CHECK_FALSE(*a == *framed());
}

TEST_CASE("testColorDriftCompoundsWithDepth") {
    Pic img = gif(gray(), settings({{"color_drift", -1}}));
    auto cool = [&](double fy) { RGB c = pixel(img, 0.5, fy); return c.b - c.r; };
    double room = cool(0.18), first = cool(0.33), second = cool(0.41);
    CHECK(std::abs(room) < 0.03);
    CHECK(first > room + 0.04);
    CHECK(second > first + 0.03);
}

TEST_CASE("testDimScreenFadesTheDeepCopies") {
    Pic img = gif(gray(), settings({{"brightness", 0.7}}));
    double room = luma3(pixel(img, 0.5, 0.18)), first = luma3(pixel(img, 0.5, 0.33)),
           second = luma3(pixel(img, 0.5, 0.41));
    CHECK(first < room - 0.1);
    CHECK(second < first - 0.05);
}

TEST_CASE("testALongDelayStillOpensOnTheWholeTunnel") {
    Pic img = gif(framed(), settings({{"delay", 20}}));
    CHECK(is_red(pixel(img, 0.5, 0.27)));
    CHECK(is_red(pixel(img, 0.5, 0.385)));
}

TEST_CASE("testTheLumaKeyPutsTheBrightPartsOnTop") {
    Pic off = gif(spot(), settings({{"brightness", 0.6}}));
    Pic keyed = gif(spot(), settings({{"brightness", 0.6}, {"key", 0.5}}));
    Pic inverted = gif(spot(), settings({{"brightness", 0.6}, {"key", 0.5}, {"key_invert", 1}}));
    CHECK(luma3(pixel(off, 0.5, 0.5)) < 0.75);
    CHECK(luma3(pixel(keyed, 0.5, 0.5)) > 0.9);
    CHECK(luma3(pixel(inverted, 0.5, 0.5)) < 0.75);
}

TEST_CASE("the run-up follows upstream: lag 1 while building, then the delay, standing still") {
    CpuOps ops;
    ops.source = [&](int64_t) { return framed(); };
    Settings s = settings({{"delay", 3}});
    Engine<CpuOps> e(config(s, N, N, 12, 1));
    render(e, ops, 0, framed());
    auto plan = s.run_up_plan(1, N);
    REQUIRE(ops.passes.size() == size_t(plan.build + plan.settle + 1));
    for (int k = 0; k < plan.build + plan.settle; ++k) {
        CHECK(ops.passes[k].run_up);
        CHECK(ops.passes[k].frame == 0);
        CHECK(ops.passes[k].ntsc_frame == 100000 + k);
        CHECK(ops.passes[k].shader_frame == 100000 + k);
        CHECK(ops.passes[k].glitch_time == 0);
    }
    const HowlPass &main = ops.passes.back();
    CHECK_FALSE(main.run_up);
    CHECK(main.ntsc_frame == 1);  // upstream frameCount: frameIndex + 1
}

TEST_CASE("auto exposure reacts a frame late") {
    // A dim TV darkens the camera's picture; the exposure, measured on frame
    // f's picture, is applied from frame f + 1.
    CpuOps ops;
    ops.source = [&](int64_t) { return gray(); };
    Settings s = settings({{"brightness", 0.6}, {"auto_exposure", 1}});
    Engine<CpuOps> e(config(s, N, N, 12, 1));
    std::vector<Pic> cams;
    for (int f = 0; f < 4; ++f) cams.push_back(render(e, ops, f, gray()));
    // History kept for the last few frames.
    for (int f = 1; f < 4; ++f) {
        auto before = e.exposure_at(f), after = e.exposure_at(f + 1);
        REQUIRE(before);
        REQUIRE(after);
        CHECK(*after == doctest::Approx(exposure_step(*before, mean_luma(*gray()), mean_luma(*cams[f]), 1)));
    }
    CHECK(*e.exposure_at(3) > 1.0);  // brightened to make up for the dim screen
}

TEST_CASE("the counter is burned into the camera's picture") {
    CpuOps ops;
    ops.source = [&](int64_t) { return gray(); };
    Settings s = settings({{"counter", 1}, {"zoom", 0}});
    Engine<CpuOps> e(config(s, N, N, 12, 10));
    Pic cam = render(e, ops, 12 * 4 + 3, gray());  // 4.25 s -> "0:04"
    CounterBitmap c = render_counter("0:04");
    double h = 0.075, w = h * c.aspect();
    int bright = 0, dark = 0, outside_changed = 0;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            double fx = (x + 0.5) / N, fy = (y + 0.5) / N;
            float v = cam->at(x, y)[0], g = gray()->at(x, y)[0];
            bool in = fx >= 0.07 && fx <= 0.07 + w && fy >= 0.86 - h && fy <= 0.86;
            if (in && v > 0.95f) ++bright;
            if (in && v < 0.2f) ++dark;
            if (!in && v != g) ++outside_changed;
        }
    CHECK(bright > 20);
    CHECK(dark > 20);
    CHECK(outside_changed == 0);
}

// MARK: random access

namespace {
// A moving scene (so the history matters), 64x48.
Pic moving(int64_t f) {
    const int w = 64, h = 48;
    std::vector<uint8_t> bgra(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t i = (size_t(y) * w + x) * 4;
            bool bar = ((x + int(f) * 3) / 8) % 2 == 0;
            bgra[i] = uint8_t(bar ? 200 : 40);
            bgra[i + 1] = uint8_t((y * 5 + f * 7) % 256);
            bgra[i + 2] = uint8_t(bar ? 30 : 220);
        }
    return std::make_shared<const ImageF>(ImageF::from_bgra8(bgra.data(), w, h));
}
Settings busy(double zoom = 0.8) {
    return Settings({{"zoom", zoom}, {"delay", 3}, {"shake", 0.7}, {"seed", 4}, {"focus", 0.5},
                     {"auto_exposure", 0.6}, {"counter", 1}, {"drift_x", 0.2}, {"hue_drift", 10}});
}
}  // namespace

TEST_CASE("random access renders the same frames as playing in order") {
    const int64_t window = 5, frames = 23;
    const Settings s = busy();
    auto cfg = config(s, 64, 48, 10, 2.3, window);
    CpuOps ops;
    ops.source = moving;

    std::vector<Pic> seq;
    {
        Engine<CpuOps> e(cfg);
        for (int64_t f = 0; f < frames; ++f) seq.push_back(render(e, ops, f, moving(f)));
        // Playing in order never re-runs a window inside the first 2 windows.
        CHECK(e.resets() >= 1);
    }
    // Shuffled order, re-renders and seeks in one engine.
    std::vector<int64_t> order;
    for (int64_t f = 0; f < frames; ++f) order.push_back(f);
    std::mt19937 rng(1234);
    std::shuffle(order.begin(), order.end(), rng);
    order.insert(order.end(), {7, 7, 8, 9, 6, 22, 21, 20, 0, 1, 2, 3});
    Engine<CpuOps> e(cfg);
    for (int64_t f : order) {
        CAPTURE(f);
        CHECK(*render(e, ops, f, moving(f)) == *seq[size_t(f)]);
    }
    // Every frame from a fresh engine too.
    for (int64_t f : {0, 4, 9, 10, 14, 15, 22}) {
        Engine<CpuOps> fresh(cfg);
        CAPTURE(f);
        CHECK(*render(fresh, ops, f, moving(f)) == *seq[size_t(f)]);
    }
}

TEST_CASE("windows: anchors, continuation and re-render without re-running") {
    CHECK(anchor_for(0, 5) == 0);
    CHECK(anchor_for(9, 5) == 0);
    CHECK(anchor_for(10, 5) == 5);
    CHECK(anchor_for(14, 5) == 5);
    CHECK(anchor_for(15, 5) == 10);
    CHECK(anchor_for(100000, 0) == 0);
    for (int64_t n = 0; n < 200; ++n) CHECK(n - anchor_for(n, 7) >= std::min<int64_t>(n, 7));

    CpuOps ops;
    ops.source = moving;
    Engine<CpuOps> e(config(busy(), 64, 48, 10, 2.3, 50));
    render(e, ops, 0, moving(0));
    CHECK(e.resets() == 1);
    for (int64_t f = 1; f < 30; ++f) render(e, ops, f, moving(f));
    CHECK(e.resets() == 1);  // sequential: one run-up only
    const auto passes = e.tv_passes();
    render(e, ops, 29, moving(29));  // re-render the current frame
    render(e, ops, 27, moving(27));  // and one a little back
    CHECK(e.resets() == 1);
    CHECK(e.tv_passes() == passes);  // no recomputation
    render(e, ops, 35, moving(35));  // skip ahead within the window: continue
    CHECK(e.resets() == 1);
    CHECK(e.tv_passes() == passes + 5);  // frames 30..34
    render(e, ops, 10, moving(10));  // back far: from the checkpoint at the window start (after its run-up)
    CHECK(e.resets() == 1);
    CHECK(e.restores() == 1);
    CHECK(e.tv_passes() == passes + 5 + 10);  // frames 0..9

    // Without checkpoints: the window is run again, run-up and all.
    Engine<CpuOps> off(config(busy(), 64, 48, 10, 2.3, 50));
    off.set_checkpoints(false);
    for (int64_t f = 0; f < 30; ++f) render(off, ops, f, moving(f));
    render(off, ops, 10, moving(10));
    CHECK(off.resets() == 2);
    CHECK(off.restores() == 0);
    CHECK(off.checkpoints() == 0);
}

namespace {
// CpuOps whose TV passes are begun and finished later (howl_engine.h
// pipelining), checking the engine keeps to the protocol.
struct AsyncCpuOps : CpuOps {
    struct Pending {
        Pic cam;
        HowlPass pass;
        int64_t id;
    };
    int limit = 2;
    int64_t begun = 0, ended = 0, max_open = 0;
    bool in_order = true;
    int max_inflight() const { return limit; }
    Pending tv_begin(const Pic &cam, const HowlPass &p) {
        max_open = std::max(max_open, ++begun - ended);
        return {cam, p, begun - 1};
    }
    Pic tv_end(Pending &&q) {
        in_order = in_order && q.id == ended;
        ++ended;
        return tv_pass(q.cam, q.pass);
    }
    static size_t bytes(const Pic &p) { return size_t(p->width) * p->height * 16; }
};
}  // namespace

TEST_CASE("checkpoints and pipelined passes render what playing in order renders") {
    const int64_t frames = 30;
    Settings s = busy(0.5);  // delay 3: passes can overlap; fewer copies: a shorter run-up
    for (int64_t window : {5}) {  // window starts 0, 5, 10, 15, 20, 25
        auto cfg = config(s, 64, 48, 10, 2.3, window);
        std::vector<Pic> seq;
        {
            CpuOps ops;
            ops.source = moving;
            Engine<CpuOps> e(cfg);
            e.set_checkpoints(false);
            for (int64_t f = 0; f < frames; ++f) seq.push_back(render(e, ops, f, moving(f)));
        }
        std::vector<int64_t> order;
        for (int64_t f = 0; f < frames; ++f) order.push_back(f);
        std::mt19937 rng(77 + int(window));
        std::shuffle(order.begin(), order.end(), rng);
        // Scrubbing: back and forth near each other, then consecutive frames after a seek.
        order.insert(order.end(), {20, 17, 21, 22, 23, 12, 13, 25, 24, 23, 29, 28, 2, 0, 1, 26, 25, 24});
        struct Case {
            bool async;
            int64_t every;
            size_t budget;
        };
        const size_t all = size_t(1) << 30, two = size_t(128) * 96 * 16 * 3 * 2;  // all, or 2 checkpoints
        for (Case k : {Case{false, 0, all}, Case{true, 0, all}, Case{false, 1, all}, Case{true, 4, all},
                       Case{true, 1, two}, Case{false, 4, two}}) {
            CAPTURE(window);
            CAPTURE(k.async);
            CAPTURE(k.every);
            CAPTURE(k.budget);
            EngineConfig c = cfg;
            c.checkpoint_every = k.every;
            c.checkpoint_budget = k.budget;
            AsyncCpuOps ops;
            ops.source = moving;
            ops.limit = k.async ? 2 : 0;
            Engine<AsyncCpuOps> e(c);
            for (int64_t f : order) {
                CAPTURE(f);
                Pic cam = e.camera(ops, f, moving(f));
                if (e.wants_tv(f)) e.store_tv(f, ops.tv_pass(cam, e.normal_pass(f)));
                CHECK(*cam == *seq[size_t(f)]);
            }
            CHECK(ops.in_order);
            CHECK(ops.begun == ops.ended);
            CHECK(ops.max_open <= 2);
            if (k.async) CHECK(ops.begun > 0);
            CHECK(e.checkpoint_bytes() <= k.budget);
            if (k.every == 0) CHECK(e.restores() == 0);
            else if (k.budget == all) CHECK(e.restores() > 0);
        }
    }
}

TEST_CASE("missing past sources: checkpoints and pipelining change no frame") {
    // Stand-ins make the history depend on the access order (as it always
    // has); whatever the order, checkpoints and pipelined passes don't change
    // what the plain engine renders. (The frames missing as past sources are
    // never asked for themselves: the engine takes a frame the host renders
    // to be one it can also fetch.)
    auto source = [](int64_t f) -> Pic { return f % 4 == 3 ? nullptr : moving(f); };
    std::vector<int64_t> order;
    for (int64_t f = 0; f < 30; ++f)
        if (f % 4 != 3) order.push_back(f);
    std::mt19937 rng(5);
    std::shuffle(order.begin(), order.end(), rng);
    order.insert(order.end(), {20, 21, 22, 24, 25, 9, 10, 4, 5, 6, 8});
    auto run = [&](int64_t every, int limit) {
        auto c = config(busy(), 64, 48, 10, 2.3, 6);
        c.checkpoint_every = every;
        AsyncCpuOps ops;
        ops.source = source;
        ops.limit = limit;
        Engine<AsyncCpuOps> e(c);
        std::vector<Pic> out;
        for (int64_t f : order) {
            Pic cam = e.camera(ops, f, moving(f));
            if (e.wants_tv(f)) e.store_tv(f, ops.tv_pass(cam, e.normal_pass(f)));
            out.push_back(cam);
        }
        return out;
    };
    const auto plain = run(0, 0), with = run(2, 2);
    for (size_t i = 0; i < order.size(); ++i) {
        CAPTURE(order[i]);
        CHECK(*with[i] == *plain[i]);
    }
}

TEST_CASE("no checkpoint from a history that stood in the asked frame for a missing window start") {
    auto source = [](int64_t f) -> Pic { return f == 6 ? nullptr : moving(f); };
    CpuOps ops;
    ops.source = source;
    auto cfg = config(busy(), 64, 48, 10, 2.3, 6);
    cfg.checkpoint_every = 2;
    Engine<CpuOps> e(cfg);
    e.camera(ops, 13, moving(13));  // anchor 6, whose source is missing: run-up on frame 13
    CHECK(e.checkpoints() == 0);
    e.camera(ops, 2, moving(2));  // anchor 0: fine
    CHECK(e.checkpoints() > 0);
}

TEST_CASE("missing past source frames hold the last one available") {
    CpuOps ops;
    ops.source = [](int64_t f) -> Pic { return f % 3 == 1 ? nullptr : moving(f); };
    Engine<CpuOps> e(config(busy(), 64, 48, 10, 2.3, 5));
    Pic a = render(e, ops, 8, moving(8));
    Engine<CpuOps> e2(config(busy(), 64, 48, 10, 2.3, 5));
    CHECK(*render(e2, ops, 8, moving(8)) == *a);
}

TEST_CASE("CPU reference kernels") {
    // Blur keeps a flat picture flat and conserves the mean.
    ImageF flat(32, 16);
    for (auto &v : flat.px) v = 0.4f;
    BlurUniforms u{{1, 0}, 3.0f, blur_radius(3.0)};
    ImageF b = blur_ref(flat, u);
    for (float v : b.px) CHECK(v == quantize8(0.4f));
    // Resample of a 2x2 checker to 4x4: corners keep their values.
    ImageF c(2, 2);
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) c.px[i * 4 + k] = (i == 0 || i == 3) ? 1.f : 0.f;
    ImageF r = resample_ref(c, 4, 4);
    CHECK(r.at(0, 0)[0] == 1.f);
    CHECK(r.at(3, 0)[0] == 0.f);
    CHECK(r.at(1, 1)[0] == quantize8(0.625f));
    CHECK(mean_luma(flat) == doctest::Approx(0.4));
    // The camera with no TV in view and exposure 1 is the identity.
    Settings z({{"zoom", 0}});
    CameraUniforms cu = camera_uniforms(z, z.base_pose(), 2, 1, 0);
    CHECK(camera_ref(flat, nullptr, nullptr, cu).px.size() == flat.px.size());
}
