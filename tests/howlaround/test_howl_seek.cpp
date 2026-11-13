// Screen Loop through the C ABI on the GPU: whatever order the host asks for
// frames in (cold seeks, scrubbing back and forth, playing on after a seek),
// every frame is byte-identical to playing from frame 0 in order; the frame
// after a seek continues instead of re-running; checkpoints change nothing;
// NTSC on the GPU (the default on D3D11 hardware) renders what ntsc-rs renders.
#include <doctest/doctest.h>

#include <ntscrt/ntscrt.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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

constexpr int W = 192, H = 108;

struct Source {
    std::map<int64_t, std::vector<uint8_t>> px;
    std::map<int64_t, ntscrt_image> img;
    int fetches = 0;
    size_t held_max = 0;            // host images handed out and not yet released, at most
    int64_t first = -1, last = -1;  // the earliest / latest frame fetched since the counters were reset
    int w = W, h = H;               // the size frames are fetched at (a render at a reduced scale: smaller)
    static std::vector<uint8_t> make(int64_t f, int w = W, int h = H) {
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
        src.h = int(H * scale);
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
            Inst b("{}", fb);
            set_env("NTSCRT_SCREENLOOP_CHECKPOINT_MB", nullptr);
            set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
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

TEST_CASE("Screen Loop: default window, a seek past two windows and back") {
    const std::string fb = feedback("Bottom to top", 240);
    std::vector<uint8_t> want;
    {
        Inst a("{}", fb);
        if (!a.ok) return;
        for (int64_t f = 0; f <= 481; ++f) {
            auto out = a.render(f);
            if (f == 481) want = out;
        }
    }
    Inst b("{}", fb);
    CHECK(b.render(481) == want);  // window start 240
}

TEST_CASE("Screen Loop: a far cold seek has a bounded cost and holds one host frame at a time") {
    // Default window (240 frames): a cold seek to N re-runs the loop from its window start
    // A(N) = (floor(N / 240) - 1) * 240, so it fetches at most 2 * 240 - 1 earlier source frames,
    // never one before A(N), and gives each host image back before asking for the next one (a
    // host's UHD float frame is ~133 MB; Resolve keeps every frame fetched: docs/dev/SCREENLOOP-MEMORY.md).
    // The frames after it continue: no fetch at all, window starts included.
    const std::string fb = feedback("Bottom to top", 240);
    for (int64_t n : {int64_t(1850), int64_t(2159)}) {
        CAPTURE(n);
        set_env("NTSCRT_SOURCE_CACHE_MB", "0");  // nothing served from the cache: every history frame is fetched
        Inst a("{}", fb);
        set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
        if (!a.ok) return;
        a.render(n);
        const int64_t anchor = (n / 240 - 1) * 240;
        CHECK(a.src.first == anchor);
        CHECK(a.src.last == n - 1);
        CHECK(a.src.fetches == n - anchor);  // each history frame once (the frame itself is the render's source)
        CHECK(a.src.fetches < 2 * 240);
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
        const int64_t last = (n / 240 + 1) * 240 + 5;
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
    Inst b("{}", fb);
    set_env("NTSCRT_SOURCE_CACHE_MB", nullptr);
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
