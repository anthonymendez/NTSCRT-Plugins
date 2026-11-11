// Screen Loop through the C ABI on the GPU: whatever order the host asks for
// frames in (cold seeks, scrubbing back and forth, playing on after a seek),
// every frame is byte-identical to playing from frame 0 in order; the frame
// after a seek continues instead of re-running; checkpoints change nothing.
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
    static std::vector<uint8_t> make(int64_t f) {
        std::vector<uint8_t> p(size_t(W) * H * 4);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                uint8_t *q = &p[(size_t(y) * W + x) * 4];
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
    s->px[f] = Source::make(f);
    s->img[f] = {W, H, ptrdiff_t(W) * 4, NTSCRT_RGBA8, s->px[f].data()};
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
    std::vector<uint8_t> render(int64_t f) {
        std::vector<uint8_t> in = Source::make(f), out(in.size());
        ntscrt_image s{W, H, ptrdiff_t(W) * 4, NTSCRT_RGBA8, in.data()};
        ntscrt_image d{W, H, ptrdiff_t(W) * 4, NTSCRT_RGBA8, out.data()};
        ntscrt_render_args ra{};
        ra.frame_index = f;
        ra.fps = 30;
        ra.look_json = look.c_str();
        ra.feedback_json = fb.c_str();
        ra.render_scale = 1;
        ra.fetch_source = fetch;
        ra.release_source = release;
        ra.user = &src;
        src.fetches = 0;
        const ntscrt_status st = ntscrt_render_cpu(p, &ra, &s, &d);
        CHECK_MESSAGE(st == NTSCRT_OK, ntscrt_last_error(p));
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

#ifndef __APPLE__  // crt-sim can hang the macOS CI runners' virtual GPU
TEST_CASE("Screen Loop with a temporal CRT preset: no checkpoints, so none changes a frame") {
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
