// Screen Loop through the C ABI on the GPU: whatever order the host asks for
// frames in (cold seeks, scrubbing back and forth, playing on after a seek),
// every frame is byte-identical to playing from frame 0 in order; the frame
// after a seek continues instead of re-running; checkpoints change nothing;
// NTSC on the GPU (the default on D3D11 hardware) renders what ntsc-rs renders.
#include <doctest/doctest.h>

#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../test_sizes.h"

namespace {

bool gpu_required() {
    const char *v = std::getenv("NTSCRT_REQUIRE_GPU");
    return v && *v && std::string(v) != "0";
}
void set_env(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

// 192x108 on a hardware GPU; 192x54 (32:9) on WARP (CI): Screen Loop's feedback picture is
// ~1300-2000 pixels wide whatever the frame's width, so its cost on a software rasterizer goes
// with the frame's height over its width (tests/test_sizes.h). Nothing checked depends on it.
constexpr int W = 192;
int frame_h() {
    static const int h = testsizes::pick(108, 54);
    return h;
}

struct Source {
    std::map<int64_t, std::vector<uint8_t>> px;
    std::map<int64_t, ntscrt_image> img;
    int fetches = 0;
    size_t held_max = 0;            // host images handed out and not yet released, at most
    int64_t first = -1, last = -1;  // the earliest / latest frame fetched since the counters were reset
    int w = W, h = frame_h();       // the size frames are fetched at (a render at a reduced scale: smaller)
    static std::vector<uint8_t> make(int64_t f, int w = W, int h = frame_h()) {
        std::vector<uint8_t> p(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t *q = &p[(size_t(y) * w + x) * 4];
                const bool bar = ((x + f * 5) / 24) % 2 == 0;
                q[0] = uint8_t((x * 3 + f * 9) % 256);
                q[1] = uint8_t((y * 4 + f * 3) % 256);
                q[2] = uint8_t(bar ? 30 : 220);
                q[3] = 255;
            }
        return p;
    }
};
const ntscrt_image *fetch(void *u, int64_t f) {
    auto *s = static_cast<Source *>(u);
    if (f < 0) return nullptr;
    ++s->fetches;
    if (s->first < 0 || f < s->first) s->first = f;
    if (f > s->last) s->last = f;
    s->px[f] = Source::make(f, s->w, s->h);
    s->img[f] = {s->w, s->h, ptrdiff_t(s->w) * 4, NTSCRT_RGBA8, s->px[f].data()};
    if (s->img.size() > s->held_max) s->held_max = s->img.size();
    return &s->img[f];
}
void release(void *u, const ntscrt_image *im) {
    auto *s = static_cast<Source *>(u);
    for (auto it = s->img.begin(); it != s->img.end(); ++it)
        if (&it->second == im) {
            s->px.erase(it->first);
            s->img.erase(it);
            return;
        }
}

std::string read_file(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A bundled Screen Loop preset with a short window, so a few dozen frames
// cross several window starts.
std::string feedback(const std::string &preset, int window) {
    auto j = nlohmann::json::parse(read_file(std::string(NTSCRT_SOURCE_DIR) + "/presets/Screen Loop/" + preset + ".json"));
    j["window"] = window;
    return j.dump();
}

struct Inst {
    ntscrt_instance *p = nullptr;
    Source src;
    std::string look, fb;
    std::string timings;  // of the last render
    bool ok = false;
    Inst(std::string look_, std::string fb_) : look(std::move(look_)), fb(std::move(fb_)) {
        const std::string res = NTSCRT_BUILT_RESOURCES_DIR;
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, res.c_str()};
        REQUIRE(ntscrt_create(&ci, &p) == NTSCRT_OK);
        ok = std::string(ntscrt_last_error(p)).empty();
        if (!ok) {
            if (gpu_required()) FAIL("no GPU and NTSCRT_REQUIRE_GPU is set: " << ntscrt_last_error(p));
            MESSAGE("no GPU: " << ntscrt_last_error(p));
        }
    }
    ~Inst() { ntscrt_destroy(p); }
    // scale < 1: a host's reduced render scale (its viewer): frames that much smaller.
    std::vector<uint8_t> render(int64_t f, double scale = 1) {
        src.w = int(W * scale);
        src.h = int(frame_h() * scale);
        std::vector<uint8_t> in = Source::make(f, src.w, src.h), out(in.size());
        ntscrt_image s{src.w, src.h, ptrdiff_t(src.w) * 4, NTSCRT_RGBA8, in.data()};
        ntscrt_image d{src.w, src.h, ptrdiff_t(src.w) * 4, NTSCRT_RGBA8, out.data()};
        ntscrt_render_args ra{};
        ra.frame_index = f;
        ra.fps = 30;
        ra.look_json = look.c_str();
        ra.feedback_json = fb.c_str();
        ra.render_scale = scale;
        ra.fetch_source = fetch;
        ra.release_source = release;
        ra.user = &src;
        src.fetches = 0;
        src.held_max = 0;
        src.first = src.last = -1;
        const ntscrt_status st = ntscrt_render_cpu(p, &ra, &s, &d);
        CHECK_MESSAGE(st == NTSCRT_OK, ntscrt_last_error(p));
        timings = ntscrt_last_timings(p);
        return out;
    }
};

}  // namespace

TEST_CASE("Screen Loop: seeks and scrubbing render what playing from frame 0 renders") {
    // Auto exposure + loop (a long run-up), and neither.
    for (const char *preset : {"7 copies slow luma", "22 copies bottom left"}) {
        CAPTURE(preset);
        const std::string fb = feedback(preset, 8);
        const int64_t frames = 40;
        std::vector<std::vector<uint8_t>> seq;
        {
            Inst a("{}", fb);
            if (!a.ok) return;
            for (int64_t f = 0; f < frames; ++f) seq.push_back(a.render(f));
        }
        for (const char *mb : {"256", "0"}) {  // checkpoints on / off
            CAPTURE(mb);
            set_env("NTSCRT_SCREENLOOP_CHECKPOINT_MB", mb);
            set_env("NTSCRT_SOURCE_CACHE_MB", "0");  // 8 frames kept: re-runs fetch again
            set_env("NTSCRT_SCREENLOOP_SOURCE_MB", "0");
            Inst b("{}", fb);
            set_env("NTSCRT_SCREENLOOP_CHECKPOINT_MB", nullptr);
            set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
            set_env("NTSCRT_SCREENLOOP_SOURCE_MB", nullptr);
            // Cold seek far in, play on; back inside the window; across window
            // starts both ways; inside the first window; the very start.
            const int64_t order[] = {37, 38, 39, 33, 34, 30, 25, 26, 17, 16, 15, 31, 5, 6, 2, 0, 1, 24, 23, 39};
            for (int64_t f : order) {
                CAPTURE(f);
                const auto out = b.render(f);
                CHECK(out == seq[size_t(f)]);
            }
            // The frame after a seek continues the history: no source fetched
            // (a re-run would fetch frames 0..4 again, gone from the cache).
            b.render(12);
            CHECK(b.src.fetches > 0);
            b.render(13);
            CHECK(b.src.fetches == 0);
        }
    }
}

// The window of the two cases below: the default (240) on a hardware GPU; on WARP (CI) 48, the
// default scaled down by 5 with the frames they seek to (tests/test_sizes.h): the same windows
// and positions in them, a fifth of the frames to render.
int64_t big_window() {
    static const int64_t b = testsizes::pick<int64_t>(240, 48);
    return b;
}

TEST_CASE("Screen Loop: default window, a seek past two windows and back") {
    const int64_t B = big_window(), n = 2 * B + 1;  // 481 with the default window
    const std::string fb = feedback("Bottom to top", int(B));
    std::vector<uint8_t> want;
    {
        Inst a("{}", fb);
        if (!a.ok) return;
        for (int64_t f = 0; f <= n; ++f) {
            auto out = a.render(f);
            if (f == n) want = out;
        }
    }
    Inst b("{}", fb);
    CHECK(b.render(n) == want);  // window start B
}

TEST_CASE("Screen Loop: a far cold seek has a bounded cost and holds one host frame at a time") {
    // Default window (240 frames; on WARP 48: big_window()): a cold seek to N re-runs the loop from its window start
    // A(N) = (floor(N / 240) - 1) * 240, so it fetches at most 2 * 240 - 1 earlier source frames,
    // never one before A(N), and gives each host image back before asking for the next one (a
    // host's UHD float frame is ~133 MB; Resolve keeps every frame fetched: docs/dev/SCREENLOOP-MEMORY.md).
    // The frames after it continue: no fetch at all, window starts included.
    const int64_t B = big_window();
    const std::string fb = feedback("Bottom to top", int(B));
    // 1850 and 2159 with the default window: 170 frames into window 7, the last frame of window 8.
    for (int64_t n : {7 * B + B * 170 / 240, 9 * B - 1}) {
        CAPTURE(n);
        set_env("NTSCRT_SOURCE_CACHE_MB", "0");  // nothing served from the cache: every history frame is fetched
        set_env("NTSCRT_SCREENLOOP_SOURCE_MB", "0");
        Inst a("{}", fb);
        set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
        set_env("NTSCRT_SCREENLOOP_SOURCE_MB", nullptr);
        if (!a.ok) return;
        a.render(n);
        const int64_t anchor = (n / B - 1) * B;
        CHECK(a.src.first == anchor);
        CHECK(a.src.last == n - 1);
        CHECK(a.src.fetches == n - anchor);  // each history frame once (the frame itself is the render's source)
        CHECK(a.src.fetches < 2 * B);
        CHECK(a.src.held_max == 1);
        CHECK(a.src.img.empty());  // all given back
        // What the host is told it may be asked for covers exactly that.
        ntscrt_render_args ra{};
        ra.frame_index = n;
        ra.fps = 30;
        ra.look_json = "{}";
        ra.feedback_json = fb.c_str();
        int64_t first = -1;
        CHECK(ntscrt_source_frames_needed(&ra, &first) == NTSCRT_OK);
        CHECK(first == anchor);
        // Playing on across the next window start (1920, 2160): its run went along with the
        // one shown, so nothing is fetched again, and the frames are the cold seek's.
        const int64_t last = (n / B + 1) * B + 5;
        std::vector<uint8_t> played;
        for (int64_t f = n + 1; f <= last; ++f) {
            CAPTURE(f);
            played = a.render(f);
            CHECK(a.src.fetches == 0);
        }
        Inst b("{}", fb);
        CHECK(b.render(last) == played);
    }
}

TEST_CASE("Screen Loop: the run ahead deferred past a cold seek catches up while playing, the same frames") {
    // NTSCRT_SCREENLOOP_AHEAD=defer (window 24 here: the default's 240 scaled down): the cold seek's
    // re-run leaves out the run of window start W = floor(n / 24) * 24; the frames played after it
    // catch it up, fetching W..n and the frames played meanwhile again, a few each, so the next window
    // start re-runs nothing. A seek to the frame before a window start: that one re-runs.
    const int64_t B = 24;
    const std::string fb = feedback("Bottom to top", int(B));
    for (int64_t n : {int64_t(185), int64_t(215)}) {
        CAPTURE(n);
        set_env("NTSCRT_SOURCE_CACHE_MB", "0");
        set_env("NTSCRT_SCREENLOOP_SOURCE_MB", "0");
        set_env("NTSCRT_SCREENLOOP_AHEAD", "defer");
        Inst a("{}", fb);
        set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
        set_env("NTSCRT_SCREENLOOP_SOURCE_MB", nullptr);
        set_env("NTSCRT_SCREENLOOP_AHEAD", nullptr);
        if (!a.ok) return;
        a.render(n);
        const int64_t anchor = (n / B - 1) * B;
        CHECK(a.src.fetches == n - anchor);
        const int64_t w = n / B * B, next_start = w + B, last = next_start + 5;
        std::vector<uint8_t> played;
        int again = 0, most = 0;
        for (int64_t f = n + 1; f <= last; ++f) {
            CAPTURE(f);
            played = a.render(f);
            CHECK(a.src.held_max <= 1);
            if (f > next_start + (n + 1 == next_start)) CHECK(a.src.fetches == 0);
            if (f >= next_start) continue;
            again += a.src.fetches;
            most = std::max(most, a.src.fetches);
        }
        if (n + 1 < next_start) {
            CHECK(again < next_start - w);
            CHECK(again >= n - w - 8);
            const int64_t owed = n + 2 - w, left = next_start - n - 1;
            CHECK(most <= int(std::max<int64_t>(4, (owed + left - 1) / left)) + 1);
        }
        Inst b("{}", fb);
        CHECK(b.render(last) == played);
    }
}

TEST_CASE("Screen Loop: its window of source frames is kept: re-runs ask the host for them once") {
    // An edit at the playhead (a Screen Loop setting: its own loop) re-runs the window; the
    // source frames come from the store (NTSCRT_SCREENLOOP_SOURCE_MB), the same pictures.
    // A store smaller than the window keeps the frames closest below the playhead (the run ahead
    // catching up reads those) and keeps them through a re-run, so each re-run finds the same ones.
    auto zoomed = [](double z) {
        auto j = nlohmann::json::parse(feedback("Bottom to top", 24));
        j["values"]["zoom"] = z;
        return j.dump();
    };
    const int64_t n = 185, anchor = (n / 24 - 1) * 24;  // the default window (240) scaled down
    std::vector<std::vector<uint8_t>> want;
    for (double z : {0.75, 0.6, 0.5}) {
        Inst c("{}", zoomed(z));
        if (!c.ok) return;
        want.push_back(c.render(n));
    }
    set_env("NTSCRT_SOURCE_CACHE_MB", "0");
    Inst a("{}", zoomed(0.75));  // the default store: the whole window at this size
    set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
    CHECK(a.render(n) == want[0]);
    CHECK(a.src.fetches == n - anchor);
    CHECK(a.timings.find("loop_sources=") != std::string::npos);
    a.fb = zoomed(0.6);
    CHECK(a.render(n) == want[1]);
    CHECK(a.src.fetches == 0);
    a.fb = zoomed(0.5);
    CHECK(a.render(n) == want[2]);
    CHECK(a.src.fetches == 0);

    const size_t frame_bytes = size_t(W) * frame_h() * 4;
    const std::string mb = std::to_string(double(20 * frame_bytes) / (1024.0 * 1024.0) + 0.0001);
    set_env("NTSCRT_SOURCE_CACHE_MB", "0");
    set_env("NTSCRT_SCREENLOOP_SOURCE_MB", mb.c_str());  // 20 frames: the frame itself and 19 before it
    Inst b("{}", zoomed(0.75));
    set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
    set_env("NTSCRT_SCREENLOOP_SOURCE_MB", nullptr);
    CHECK(b.render(n) == want[0]);
    CHECK(b.src.fetches == n - anchor);
    for (int k = 1; k <= 2; ++k) {
        CAPTURE(k);
        b.fb = zoomed(k == 1 ? 0.6 : 0.5);
        CHECK(b.render(n) == want[size_t(k)]);
        CHECK(b.src.fetches == n - anchor - 19);
        CHECK(b.src.first == anchor);
        CHECK(b.src.last == n - 20);  // 166..184 came from the store
    }
}

TEST_CASE("Screen Loop: a render at another scale in between keeps the full-size history") {
    // Resolve renders the effect's frame for its viewer / thumbnails at a reduced render scale
    // while the render queue renders full size. That render has a loop of its own: the
    // full-size one goes on without re-running its window (no fetch), with the same frames.
    const std::string fb = feedback("Bottom to top", 8);
    std::vector<std::vector<uint8_t>> seq;
    {
        Inst a("{}", fb);
        if (!a.ok) return;
        for (int64_t f = 0; f <= 30; ++f) seq.push_back(a.render(f));
    }
    set_env("NTSCRT_SOURCE_CACHE_MB", "0");
    set_env("NTSCRT_SCREENLOOP_SOURCE_MB", "0");
    Inst b("{}", fb);
    set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
    set_env("NTSCRT_SCREENLOOP_SOURCE_MB", nullptr);
    for (int64_t f = 0; f <= 20; ++f) CHECK(b.render(f) == seq[size_t(f)]);
    b.render(20, 0.5);  // the viewer: its own loop, its own (smaller) fetches
    CHECK(b.src.fetches > 0);
    for (int64_t f = 21; f <= 30; ++f) {
        CAPTURE(f);
        CHECK(b.render(f) == seq[size_t(f)]);
        CHECK(b.src.fetches == 0);
        if (f == 25) {
            b.render(25, 0.5);  // and again: that loop continues too
            b.render(25, 0.25);  // more sizes than loops kept: the full-size one is never the one dropped
            b.render(25, 0.375);
        }
    }
}

TEST_CASE("Screen Loop: other settings in between, then back: the history goes on, the same frames") {
    // Trying Screen Loop presets in an editor's Inspector: each one's loop is kept (parked) while
    // another is shown, so going back to one continues its history instead of re-running its
    // window. The frames are those of an instance that only ever had that setting.
    const std::string fa = feedback("Bottom to top", 8), fb = feedback("12 copies luma key", 8),
                      fc = feedback("22 copies bottom left", 8);
    std::vector<std::vector<uint8_t>> seq_a, seq_b;
    {
        Inst a("{}", fa);
        if (!a.ok) return;
        for (int64_t f = 0; f <= 30; ++f) seq_a.push_back(a.render(f));
        Inst b("{}", fb);
        for (int64_t f = 0; f <= 30; ++f) seq_b.push_back(b.render(f));
    }
    set_env("NTSCRT_SOURCE_CACHE_MB", "0");  // 8 frames kept: a re-run fetches again
    set_env("NTSCRT_SCREENLOOP_SOURCE_MB", "0");
    Inst x("{}", fa);
    set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
    set_env("NTSCRT_SCREENLOOP_SOURCE_MB", nullptr);
    CHECK(x.render(20) == seq_a[20]);  // a cold seek
    CHECK(x.src.fetches > 0);
    x.render(20, 0.5);  // the viewer's size, its own loop
    x.fb = fb;
    CHECK(x.render(20) == seq_b[20]);  // new settings: their own run
    CHECK(x.src.fetches > 0);
    x.fb = fc;
    x.render(20);
    x.fb = fa;  // back: A's history goes on
    CHECK(x.render(20) == seq_a[20]);
    CHECK(x.src.fetches == 0);
    x.render(20, 0.5);
    CHECK(x.src.fetches == 0);
    CHECK(x.render(21) == seq_a[21]);
    CHECK(x.src.fetches <= 3);  // no re-run (the run ahead catching up: a few)
    x.fb = fb;  // and B's
    CHECK(x.render(21) == seq_b[21]);
    CHECK(x.src.fetches <= 3);
    CHECK(x.render(22) == seq_b[22]);
    CHECK(x.src.fetches <= 3);
    // Seeking back after coming back: a parked loop kept only its last checkpoint (here the
    // window start's); the frame is still the one an undisturbed instance renders.
    CHECK(x.render(9) == seq_b[9]);
    // A budget smaller than one loop's pictures (UHD): the setting just left is still kept.
    set_env("NTSCRT_SCREENLOOP_KEEP_MB", "0.001");
    set_env("NTSCRT_SOURCE_CACHE_MB", "0");
    set_env("NTSCRT_SCREENLOOP_SOURCE_MB", "0");
    Inst z("{}", fa);
    set_env("NTSCRT_SCREENLOOP_KEEP_MB", nullptr);
    set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
    set_env("NTSCRT_SCREENLOOP_SOURCE_MB", nullptr);
    z.render(20);
    z.fb = fb;
    z.render(20);
    z.fb = fa;
    CHECK(z.render(20) == seq_a[20]);
    CHECK(z.src.fetches == 0);
    // NTSCRT_SCREENLOOP_KEEP_MB=0: nothing is kept, going back re-runs (the same frame).
    set_env("NTSCRT_SCREENLOOP_KEEP_MB", "0");
    set_env("NTSCRT_SOURCE_CACHE_MB", "0");
    set_env("NTSCRT_SCREENLOOP_SOURCE_MB", "0");
    Inst y("{}", fa);
    set_env("NTSCRT_SCREENLOOP_KEEP_MB", nullptr);
    set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
    set_env("NTSCRT_SCREENLOOP_SOURCE_MB", nullptr);
    y.render(20);
    y.fb = fb;
    y.render(20);
    y.fb = fa;
    CHECK(y.render(20) == seq_a[20]);
    CHECK(y.src.fetches > 0);
}

#ifndef __APPLE__  // crt-sim can hang the macOS CI runners' virtual GPU
// crt-sim keeps a feedback, which the CRT stage resets every frame (docs/dev/CRT.md): its TV
// pictures are a function of the pass, so the loop keeps checkpoints and they change no frame.
TEST_CASE("Screen Loop with the temporal CRT preset: checkpoints change no frame") {
    const std::string fb = feedback("12 copies luma key", 8);
    const std::string look = R"({"shader": {"enabled": true, "preset": "sim"}})";
    const int64_t order[] = {30, 20, 21, 18, 31};
    std::vector<std::vector<uint8_t>> with, without;
    {
        Inst a(look, fb);
        if (!a.ok) return;
        for (int64_t f : order) with.push_back(a.render(f));
    }
    set_env("NTSCRT_SCREENLOOP_CHECKPOINT_MB", "0");
    {
        Inst a(look, fb);
        for (int64_t f : order) without.push_back(a.render(f));
    }
    set_env("NTSCRT_SCREENLOOP_CHECKPOINT_MB", nullptr);
    for (size_t i = 0; i < with.size(); ++i) {
        CAPTURE(order[i]);
        CHECK(with[i] == without[i]);
    }
}
#endif

TEST_CASE("Screen Loop: NTSC on the GPU renders what ntsc-rs renders, every bundled preset") {
    // The default look (NTSC on); every Screen Loop preset; play, then cold seeks
    // (history passes) on fresh instances. NTSCRT_NTSC_GPU=0 is the reference.
    const char *presets[] = {"7 copies slow luma", "12 copies luma key", "15 copies color drift",
                             "22 copies bottom left", "Bottom to top"};
    const int64_t frames = 24;
    const int64_t seeks[] = {23, 17, 9};
    for (const char *preset : presets) {
        CAPTURE(preset);
        const std::string fb = feedback(preset, 8);
        std::vector<std::vector<uint8_t>> ref;
        std::vector<std::vector<uint8_t>> ref_seek;
        set_env("NTSCRT_NTSC_GPU", "0");
        {
            Inst a("{}", fb);
            if (!a.ok) {
                set_env("NTSCRT_NTSC_GPU", nullptr);
                return;
            }
            for (int64_t f = 0; f < frames; ++f) ref.push_back(a.render(f));
            CHECK(a.timings.find("ntsc_route=cpu (NTSCRT_NTSC_GPU=0)") != std::string::npos);
        }
        set_env("NTSCRT_NTSC_GPU", nullptr);
        bool gpu = false;
        {
            Inst b("{}", fb);
            for (int64_t f = 0; f < frames; ++f) {
                CAPTURE(f);
                CHECK(b.render(f) == ref[size_t(f)]);
                gpu = gpu || b.timings.find("ntsc_route=gpu") != std::string::npos;
            }
            MESSAGE((std::string(preset) + ": " + b.timings));
        }
        for (int64_t f : seeks) {
            CAPTURE(f);
            Inst c("{}", fb);
            CHECK(c.render(f) == ref[size_t(f)]);  // history passes re-rendered
            if (gpu) {
                CHECK(c.timings.find("ntsc_loop=gpu:") != std::string::npos);
                CHECK(c.timings.find(",cpu:0") != std::string::npos);
            }
            CHECK(c.render(f - 1) == ref[size_t(f - 1)]);
        }
    }
}

TEST_CASE("Screen Loop with a Digital video look: Digital is off there, on either route, seeks included") {
    // Upstream runs no Digital video stage while Screen Loop is on (stages.h): a Digital
    // look under Screen Loop renders what the same look without Digital renders, whatever
    // NTSCRT_DIGITAL_GPU says, played and after cold seeks (docs/dev/DIGITAL-GPU.md).
    const std::string fb = feedback("7 copies slow luma", 8);
    const std::string digital = R"({"digital": {"enabled": true, "values": {"aliasing": 0.7, "datamosh": 1,
        "keyframe_interval": 1, "frame_rate": 30}}, "downscale": {"enabled": true, "width": 320}})";
    const std::string plain = R"({"digital": {"enabled": false}, "downscale": {"enabled": true, "width": 320}})";
    const int64_t frames = 20;
    std::vector<std::vector<uint8_t>> ref;
    {
        Inst a(plain, fb);
        if (!a.ok) return;
        for (int64_t f = 0; f < frames; ++f) ref.push_back(a.render(f));
    }
    for (const char *env : {"0", static_cast<const char *>(nullptr)}) {
        CAPTURE((env ? env : "unset"));
        set_env("NTSCRT_DIGITAL_GPU", env);
        {
            Inst b(digital, fb);
            for (int64_t f = 0; f < frames; ++f) {
                CAPTURE(f);
                CHECK(b.render(f) == ref[size_t(f)]);
                CHECK(b.timings.find("digital_route=") == std::string::npos);  // the stage didn't run
            }
        }
        for (int64_t f : {int64_t(19), int64_t(11), int64_t(4)}) {
            CAPTURE(f);
            Inst c(digital, fb);
            CHECK(c.render(f) == ref[size_t(f)]);
            CHECK(c.render(f - 1) == ref[size_t(f - 1)]);
        }
        set_env("NTSCRT_DIGITAL_GPU", nullptr);
    }
}
