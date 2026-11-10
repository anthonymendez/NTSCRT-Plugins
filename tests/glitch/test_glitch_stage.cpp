// Port of upstream Tests/CrtCoreTests/GlitchStageTests.swift (GlitchStageTests +
// GlitchCheckpointTests) against the CPU reference of the GPU passes, plus
// host-style (FrameContext) checks: keyframe history, seeking, seeds, presets.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <tuple>

#include "stages/glitch/glitch_cpu.h"

using namespace ntscrt;
using namespace ntscrt::glitch;

namespace {

constexpr int W = 320, A = 240;

ImageBGRA8 texture(const std::function<std::tuple<float, float, float>(int, int)> &pixel) {
    ImageBGRA8 im;
    im.resize(W, A);
    for (int y = 0; y < A; y++)
        for (int x = 0; x < W; x++) {
            auto [r, g, b] = pixel(x, y);
            uint8_t *p = im.row(y) + x * 4;
            p[0] = uint8_t(b * 255);
            p[1] = uint8_t(g * 255);
            p[2] = uint8_t(r * 255);
            p[3] = 255;
        }
    return im;
}

struct YIQ {
    double y, i, q;
};
YIQ yiq(const ImageBGRA8 &px, int x, int y) {
    const uint8_t *p = px.row(y) + x * 4;
    double b = p[0] / 255.0, g = p[1] / 255.0, r = p[2] / 255.0;
    return {0.299 * r + 0.587 * g + 0.114 * b, 0.596 * r - 0.274 * g - 0.322 * b, 0.211 * r - 0.523 * g + 0.312 * b};
}

json look_with(const std::map<std::string, double> &values) {
    json v = json::object();
    for (auto &[k, x] : values) v[k] = x;
    return json{{"glitch", {{"enabled", true}, {"values", v}}}};
}

// A host frame request with a constant look (no keyframes).
FrameContext ctx_for(const json &look, int64_t frame, double fps = 30, uint64_t seed = 0) {
    FrameContext c;
    c.frame = frame;
    c.fps = fps;
    c.seed = seed;
    c.look = &look;
    c.look_at = [&look](int64_t) -> const json & { return look; };
    return c;
}

// Keyframed host: the look at each frame comes from a function of time.
struct History {
    double fps;
    std::function<std::map<std::string, double>(double)> values_at;
    mutable std::map<int64_t, json> cache;
    const json &at(int64_t f) const {
        auto it = cache.find(f);
        if (it != cache.end()) return it->second;
        return cache.emplace(f, look_with(values_at(double(f) / fps))).first->second;
    }
    FrameContext ctx(int64_t frame) const {
        FrameContext c;
        c.frame = frame;
        c.fps = fps;
        c.look = &at(frame);
        c.look_at = [this](int64_t f) -> const json & { return at(f); };
        return c;
    }
};

ImageBGRA8 render(const ImageBGRA8 &input, double time = 1.0, const std::map<std::string, double> &values = {}) {
    json look = look_with(values);
    GlitchCpuStage stage;
    ImageBGRA8 img = input;
    stage.process(img, ctx_for(look, int64_t(std::llround(time * 30)), 30));
    return img;
}

ImageBGRA8 render(const ImageBGRA8 &input, const GlitchFieldPlan &plan) {
    ImageBGRA8 out;
    cpu_render_plan(plan, input, out);
    return out;
}

GlitchFieldPlan healthyPlan() {
    ReceiverSimulator sim(ReceiverRaster(W, A));
    sim.advance(1.0, GlitchSettings());
    return sim.plan();
}

bool same(const ImageBGRA8 &a, const ImageBGRA8 &b) {
    return a.width == b.width && a.height == b.height && a.px == b.px;
}

json load_preset(const std::string &name) {
    std::ifstream f(std::string(NTSCRT_SOURCE_DIR) + "/presets/" + name + ".json");
    REQUIRE(f.good());
    return json::parse(f);
}

}  // namespace

// ---- GlitchStageTests -------------------------------------------------------------------

TEST_CASE("healthy set reproduces its input exactly") {
    std::mt19937 rng(42);
    std::vector<float> table(W * A);
    for (auto &v : table) v = float(rng() % 256) / 255;
    auto input = texture([&](int x, int y) {
        float v = table[y * W + x];
        return std::tuple{v, float((x * 7 + y * 3) % 256) / 255, 1 - v};
    });
    for (double t : {0.0, 1.0, 3.3}) CHECK_MESSAGE(same(render(input, t), input), "t=" << t);
}

TEST_CASE("healthy set is identity at other sizes and seeds") {
    for (auto [w, h] : std::vector<std::pair<int, int>>{{256, 332}, {640, 480}, {123, 77}}) {
        ImageBGRA8 in;
        in.resize(w, h);
        for (size_t i = 0; i < in.px.size(); i++) in.px[i] = (i % 4 == 3) ? 255 : uint8_t((i * 37) % 251);
        json look = look_with({});
        GlitchCpuStage stage;
        ImageBGRA8 img = in;
        stage.process(img, ctx_for(look, 17, 24, 777));
        CHECK_MESSAGE(same(img, in), w << "x" << h);
    }
}

TEST_CASE("rolling picture shows the blanking bar") {
    auto input = texture([](int, int) { return std::tuple{0.6f, 0.6f, 0.6f}; });
    int V = ReceiverRaster(W, A).vbiLines;
    json look = look_with({{"vertical_hold", 0.6}});
    GlitchCpuStage stage;  // plays frames 30.. in order, as upstream's loop of fresh renders would show
    bool found = false;
    for (int i = 0; i < 40 && !found; i++) {
        ImageBGRA8 px = input;
        stage.process(px, ctx_for(look, 30 + i));
        std::vector<bool> dark(A);
        for (int y = 0; y < A; y++) dark[y] = yiq(px, W / 2, y).y < 0.05;
        int first = -1;
        for (int y = 0; y < A; y++)
            if (dark[y]) {
                first = y;
                break;
            }
        if (first <= 0) continue;
        int run = 0;
        while (first + run < A && dark[first + run]) run++;
        if (first + run < A) {
            CHECK(std::abs(run - V) <= 3);
            found = true;
        }
    }
    CHECK_MESSAGE(found, "the blanking bar crosses the screen while rolling");
}

TEST_CASE("torn picture shows diagonal blanking") {
    auto input = texture([](int, int) { return std::tuple{0.6f, 0.6f, 0.6f}; });
    auto px = render(input, 1.0, {{"horizontal_hold", 0.95}});
    std::vector<int> starts;
    for (int y = 0; y < A; y++) {
        int s = -1;
        for (int x = 0; x < W; x++)
            if (yiq(px, x, y).y < 0.05) {
                s = x;
                break;
            }
        if (s < 0) continue;
        int run = 0;
        while (s + run < W && yiq(px, s + run, y).y < 0.05) run++;
        if (run >= 20) starts.push_back(s);
    }
    REQUIRE(int(starts.size()) > A / 4);
    CHECK(*std::max_element(starts.begin(), starts.end()) - *std::min_element(starts.begin(), starts.end()) > W / 3);
}

TEST_CASE("ghost is shifted with its color rotated by the delay") {
    auto input = texture([](int x, int) {
        return (x >= 100 && x < 116) ? std::tuple{0.8f, 0.2f, 0.2f} : std::tuple{0.5f, 0.5f, 0.5f};
    });
    double delay = 3.0;
    auto px = render(input, 1.0, {{"ghost_level", 0.5}, {"ghost_delay", delay}});
    int shift = int(std::lround(delay / (NTSCTiming::activeLength / double(W))));
    int y = A / 2;
    auto ghost = yiq(px, 108 + shift, y);
    auto background = yiq(px, 180, y);
    auto original = yiq(input, 108, y);
    double gi = ghost.i - background.i, gq = ghost.q - background.q;
    CHECK(std::sqrt(gi * gi + gq * gq) > 0.03);
    const double pi = 3.14159265358979323846;
    double expected = std::atan2(original.q, original.i) - 2 * pi * NTSCTiming::subcarrierMHz * delay;
    double diff = std::fmod(std::atan2(gq, gi) - expected, 2 * pi);
    if (diff > pi) diff -= 2 * pi;
    else if (diff < -pi) diff += 2 * pi;
    CHECK(std::abs(diff) < 0.2);
    double barY = 0.299 * 0.8 + 0.587 * 0.2 + 0.114 * 0.2;
    CHECK(std::abs((ghost.y - background.y) - 0.5 * (barY - 0.5)) <= 0.02);
}

TEST_CASE("sliding picture keeps its color") {
    auto input = texture([](int x, int) {
        return (x / 20) % 2 == 0 ? std::tuple{0.9f, 0.2f, 0.2f} : std::tuple{0.2f, 0.3f, 0.9f};
    });
    auto saturation = [](const ImageBGRA8 &px) {
        double total = 0;
        for (int y = 20; y < A - 20; y += 4)
            for (int x = 40; x < W - 40; x += 4) {
                auto c = yiq(px, x, y);
                total += std::sqrt(c.i * c.i + c.q * c.q);
            }
        return total;
    };
    double reference = saturation(input);
    double sliding = saturation(render(input, 1.0, {{"horizontal_hold", 0.3}}));
    CHECK(sliding > reference * 0.7);
}

TEST_CASE("color killer removes chroma without burst") {
    auto input = texture([](int x, int) {
        return (x / 20) % 2 == 0 ? std::tuple{0.9f, 0.2f, 0.2f} : std::tuple{0.2f, 0.3f, 0.9f};
    });
    auto plan = healthyPlan();
    for (auto &r : plan.rows) r.burstScale = 0;
    auto px = render(input, plan);
    auto saturation = [&](int y) {
        double s = 0;
        for (int x = 0; x < W; x++) {
            auto c = yiq(px, x, y);
            s += std::sqrt(c.i * c.i + c.q * c.q);
        }
        return s / W;
    };
    CHECK(saturation(2) > 0.1);
    CHECK(saturation(A - 1) < 0.01);
}

TEST_CASE("dropout compensation replays the line before") {
    auto input = texture([](int x, int y) { return std::tuple{float(y % 7) / 7, float(x % 5) / 5, 0.5f}; });
    ReceiverRaster raster(W, A);
    auto plan = healthyPlan();
    GlitchDropout d;
    d.fieldLine = raster.vbiLines + 50;
    d.uStart = 20;
    d.uLength = 10;
    plan.dropouts = {d};
    auto px = render(input, plan);
    int x = int((25 - NTSCTiming::activeStart) / (NTSCTiming::activeLength / double(W)));
    for (int c = 0; c < 3; c++) CHECK(px.row(50)[x * 4 + c] == input.row(49)[x * 4 + c]);

    int tickX = int((20.3 - NTSCTiming::activeStart) / (NTSCTiming::activeLength / double(W)));
    CHECK(yiq(px, tickX, 50).y > 0.95);

    auto tall = plan;
    GlitchDropout d2 = d;
    d2.fieldLine = raster.vbiLines + 49;
    tall.dropouts.push_back(d2);
    auto smear = render(input, tall);
    for (int c = 0; c < 3; c++) {
        CHECK(smear.row(50)[x * 4 + c] == input.row(48)[x * 4 + c]);
        CHECK(smear.row(49)[x * 4 + c] == input.row(48)[x * 4 + c]);
    }

    auto off = plan;
    off.settings = GlitchSettings();
    off.settings[kDropoutCompensation] = 0;
    auto streak = render(input, off);
    CHECK(yiq(streak, x, 50).y > 0.95);
}

TEST_CASE("caption data on line 21") {
    auto input = texture([](int, int) { return std::tuple{0.6f, 0.6f, 0.6f}; });
    ReceiverRaster raster(W, A);
    auto plan = healthyPlan();
    plan.rows[10].fieldLine = raster.ccLine();
    plan.ccBits = 0b1010101010101010100;
    auto px = render(input, plan);
    double hi = -1, lo = 2;
    for (int x = W / 3; x < W; x++) {
        hi = std::max(hi, yiq(px, x, 10).y);
        lo = std::min(lo, yiq(px, x, 10).y);
    }
    CHECK(hi > 0.4);
    CHECK(lo < 0.05);
}

TEST_CASE("brightness reveals blanking below black") {
    auto input = texture([](int, int) { return std::tuple{0.6f, 0.6f, 0.6f}; });
    ReceiverRaster raster(W, A);
    auto plan = healthyPlan();
    plan.rows[20].fieldLine = raster.postEqEnd + 1;
    auto normal = render(input, plan);
    CHECK(yiq(normal, W / 2, 20).y < 0.02);
    plan.settings = GlitchSettings();
    plan.settings[kBrightness] = 1;
    auto bright = render(input, plan);
    CHECK(std::abs(yiq(bright, W / 2, 20).y - 40.0 / 140.0) <= 0.02);
}

// ---- GlitchCheckpointTests (host frames instead of seconds) -----------------------------

TEST_CASE("seeking back matches a full re-run with constant settings") {
    json look = look_with({{"vertical_hold", 0.5}, {"horizontal_hold", 0.65}, {"signal_strength", 0.3},
                           {"crinkle", 0.7}, {"head_switch", 3}});
    GlitchTimeline seeker;
    seeker.plan_for(ctx_for(look, 270), W, A);  // 9 s: records checkpoints
    CHECK(seeker.checkpoint_count() >= 9);
    auto viaCheckpoint = seeker.plan_for(ctx_for(look, 131), W, A);  // jumps back
    CHECK(seeker.steps_frames_last() < 30);                          // resumed, not re-run from zero
    GlitchTimeline fresh;
    auto full = fresh.plan_for(ctx_for(look, 131), W, A);
    CHECK(viaCheckpoint.rows == full.rows);
    CHECK(viaCheckpoint.dropouts == full.dropouts);
    CHECK(viaCheckpoint.chromaPhase == full.chromaPhase);
}

TEST_CASE("seeking back matches a full re-run with keyframed settings") {
    History h{24, [](double t) {
                  return std::map<std::string, double>{{"vertical_hold", std::min(0.9, t / 6)},
                                                       {"horizontal_hold", t > 3 ? 0.7 : 0.0}};
              }};
    GlitchTimeline seeker;
    seeker.plan_for(h.ctx(168), W, A);  // 7 s
    auto viaCheckpoint = seeker.plan_for(h.ctx(100), W, A);
    GlitchTimeline fresh;
    auto full = fresh.plan_for(h.ctx(100), W, A);
    CHECK(viaCheckpoint.rows == full.rows);

    // ...and equals upstream's resimulate(settingsAt:) over the same history.
    ReceiverSimulator sim(ReceiverRaster(W, A));
    sim.resimulate(100.0 / 24, 1.0 / 24, [&](double t) {
        return GlitchSettings::from_look(h.at(int64_t(std::llround(t * 24))));
    });
    CHECK(sim.plan().rows == full.rows);
}

TEST_CASE("playing through equals seeking") {
    json look = look_with({{"vertical_hold", 0.7}, {"timebase_jitter", 1}, {"dropouts", 0.5}, {"hum", 0.6}});
    auto input = texture([](int x, int y) { return std::tuple{float(x % 32) / 32, float(y % 24) / 24, 0.4f}; });
    GlitchCpuStage player;
    ImageBGRA8 last;
    for (int f = 0; f <= 45; f++) {
        last = input;
        player.process(last, ctx_for(look, f));
        CHECK(player.timeline().steps_frames_last() <= 1);
    }
    GlitchCpuStage seeker;
    ImageBGRA8 direct = input;
    seeker.process(direct, ctx_for(look, 45));
    CHECK(same(last, direct));
    // Same moment rendered again (any order) looks the same.
    ImageBGRA8 earlier = input;
    seeker.process(earlier, ctx_for(look, 12));
    ImageBGRA8 again = input;
    seeker.process(again, ctx_for(look, 45));
    CHECK(same(again, direct));
    CHECK_FALSE(same(direct, input));
}

TEST_CASE("changing settings at the current frame re-simulates") {
    json a = look_with({{"vertical_hold", 0.0}});
    json b = look_with({{"vertical_hold", 0.8}});
    GlitchTimeline t;
    t.plan_for(ctx_for(a, 60), W, A);
    auto retuned = t.plan_for(ctx_for(b, 60), W, A);  // standing still, knob turned
    GlitchTimeline fresh;
    auto full = fresh.plan_for(ctx_for(b, 60), W, A);
    CHECK(retuned.rows == full.rows);
    CHECK_FALSE(retuned.rows == fresh.plan_for(ctx_for(a, 60), W, A).rows);
}

TEST_CASE("a keyframe edit after the resume point is picked up") {
    // Constant until 2 s, then the edit moves the knob from 2 s on: frames
    // before stay valid (resume from a checkpoint), later ones re-simulate.
    History before{30, [](double) { return std::map<std::string, double>{{"vertical_hold", 0.5}}; }};
    History after{30, [](double t) {
                      return std::map<std::string, double>{{"vertical_hold", t >= 2 ? -0.7 : 0.5}};
                  }};
    GlitchTimeline t;
    t.plan_for(before.ctx(150), W, A);
    auto edited = t.plan_for(after.ctx(150), W, A);
    CHECK(t.steps_frames_last() < 150);
    GlitchTimeline fresh;
    CHECK(edited.rows == fresh.plan_for(after.ctx(150), W, A).rows);
}

TEST_CASE("flush clears checkpoints") {
    json look = look_with({{"vertical_hold", 0.5}});
    GlitchTimeline t;
    t.plan_for(ctx_for(look, 90), W, A);
    CHECK(t.checkpoint_count() > 0);
    t.flush();
    CHECK(t.checkpoint_count() == 0);
    CHECK(t.checkpoint_bytes() == 0);
}

TEST_CASE("ctx.seed changes the glitch; seed 0 is upstream's") {
    json look = look_with({{"horizontal_hold", 0.95}, {"signal_strength", 0.3}});
    GlitchTimeline t0, t1, t2;
    auto p0 = t0.plan_for(ctx_for(look, 30, 30, 0), W, A);
    auto p1 = t1.plan_for(ctx_for(look, 30, 30, 5), W, A);
    auto p2 = t2.plan_for(ctx_for(look, 30, 30, 5), W, A);
    CHECK(p1.rows == p2.rows);
    CHECK_FALSE(p0.rows == p1.rows);
    ReceiverSimulator upstream(ReceiverRaster(W, A));
    upstream.resimulate(1.0, 1.0 / 30, [&](double) { return GlitchSettings::from_look(look); });
    CHECK(upstream.plan().rows == p0.rows);
    CHECK(receiver_seed(0) == ReceiverSimulator::kDefaultSeed);
    CHECK(noise_seed(0) == kDefaultNoiseSeed);
}

// ---- presets ------------------------------------------------------------------------------

TEST_CASE("strongly glitched presets change the image") {
    auto input = texture([](int x, int y) {
        return std::tuple{float(x % 64) / 64, float(y % 48) / 48, float((x + y) % 80) / 80};
    });
    for (const char *name : {"V Hold glitch", "Vertical Freak Out"}) {
        json look = load_preset(name);
        GlitchCpuStage stage;
        REQUIRE(stage.active(look));
        ImageBGRA8 img = input;
        stage.process(img, ctx_for(look, 30, 24));
        size_t changed = 0;
        for (size_t i = 0; i < img.px.size(); i++) changed += img.px[i] != input.px[i];
        CHECK_MESSAGE(changed > img.px.size() / 10, name << ": " << changed << " bytes changed");
    }
    json clean = load_preset("Clean CRT");
    CHECK_FALSE(GlitchCpuStage().active(clean));
}
