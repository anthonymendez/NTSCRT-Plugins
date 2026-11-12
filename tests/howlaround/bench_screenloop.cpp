// bench_screenloop — Screen Loop playback and seek timings over the C ABI.
// Not a ctest test: run by hand. NTSCRT_NTSC_GPU=0 for the ntsc-rs route, unset for the default.
//
//   bench_screenloop [--w 1920] [--h 1080] [--preset "7 copies slow luma"] [--look JSON]
//                    [--play 30] [--seek 600,1000] [--scrub] [--fps 30]
//
// Sources are generated in memory (a moving picture), served through
// fetch_source like a host, with the fetch time counted separately.
//   play:  frames 0..play-1 in order on one instance (ms per frame).
//   seek:  for each frame, a fresh instance renders it cold, then the next two
//          frames (what a host asks for when playback starts after a seek).
//   scrub: on one instance, after the cold seek: back 30, forward 10, back 100.
#include <ntscrt/ntscrt.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

namespace {
struct Src {
    int w = 0, h = 0;
    std::map<int64_t, std::vector<uint8_t>> live;
    std::map<int64_t, ntscrt_image> imgs;
    int fetches = 0;
    double fetch_ms = 0;
    void fill(std::vector<uint8_t> &px, int64_t f) const {
        px.resize(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t *p = &px[(size_t(y) * w + x) * 4];
                const bool bar = ((x + f * 7) / 64) % 2 == 0;
                p[0] = uint8_t((x * 3 + f * 5) % 256);
                p[1] = uint8_t((y * 2 + f * 3) % 256);
                p[2] = uint8_t(bar ? 40 : 210);
                p[3] = 255;
            }
    }
};
const ntscrt_image *fetch(void *u, int64_t f) {
    auto *s = static_cast<Src *>(u);
    if (f < 0) return nullptr;
    auto t0 = Clock::now();
    auto &px = s->live[f];
    s->fill(px, f);
    s->imgs[f] = {s->w, s->h, ptrdiff_t(s->w) * 4, NTSCRT_RGBA8, px.data()};
    s->fetch_ms += ms(t0, Clock::now());
    ++s->fetches;
    return &s->imgs[f];
}
void release(void *u, const ntscrt_image *im) {
    auto *s = static_cast<Src *>(u);
    for (auto it = s->imgs.begin(); it != s->imgs.end(); ++it)
        if (&it->second == im) {
            s->live.erase(it->first);
            s->imgs.erase(it);
            return;
        }
}
std::string read_file(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

int main(int argc, char **argv) {
    int w = 1920, h = 1080, play = 30;
    double fps = 30;
    bool scrub = false;
    std::string preset = "7 copies slow luma", look = "{}";
    std::vector<int64_t> seeks;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (a == "--w") w = std::atoi(next().c_str());
        else if (a == "--h") h = std::atoi(next().c_str());
        else if (a == "--preset") preset = next();
        else if (a == "--look") look = next();
        else if (a == "--play") play = std::atoi(next().c_str());
        else if (a == "--fps") fps = std::atof(next().c_str());
        else if (a == "--scrub") scrub = true;
        else if (a == "--seek") {
            std::stringstream ss(next());
            std::string t;
            while (std::getline(ss, t, ',')) seeks.push_back(std::atoll(t.c_str()));
        } else {
            std::fprintf(stderr, "unknown arg %s\n", a.c_str());
            return 2;
        }
    }
    const std::string res = NTSCRT_BUILT_RESOURCES_DIR;
    const std::string fb = read_file(std::string(NTSCRT_SOURCE_DIR) + "/presets/Screen Loop/" + preset + ".json");
    if (fb.empty()) {
        std::fprintf(stderr, "no preset %s\n", preset.c_str());
        return 2;
    }
    Src src;
    src.w = w;
    src.h = h;
    std::vector<uint8_t> in_px, out_px(size_t(w) * h * 4);
    auto make = [&](ntscrt_instance **inst) {
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, res.c_str()};
        return ntscrt_create(&ci, inst) == NTSCRT_OK;
    };
    auto render = [&](ntscrt_instance *inst, int64_t f) {
        src.fill(in_px, f);
        ntscrt_image s{w, h, ptrdiff_t(w) * 4, NTSCRT_RGBA8, in_px.data()};
        ntscrt_image d{w, h, ptrdiff_t(w) * 4, NTSCRT_RGBA8, out_px.data()};
        ntscrt_render_args ra{};
        ra.frame_index = f;
        ra.fps = fps;
        ra.look_json = look.c_str();
        ra.feedback_json = fb.c_str();
        ra.render_scale = 1;
        ra.fetch_source = fetch;
        ra.release_source = release;
        ra.user = &src;
        src.fetches = 0;
        src.fetch_ms = 0;
        auto t0 = Clock::now();
        ntscrt_status st = ntscrt_render_cpu(inst, &ra, &s, &d);
        double t = ms(t0, Clock::now());
        if (st != NTSCRT_OK) std::fprintf(stderr, "frame %lld: status %d %s\n", (long long)f, st, ntscrt_last_error(inst));
        return t;
    };
    std::printf("%dx%d preset \"%s\"\n", w, h, preset.c_str());
    if (play > 0) {
        ntscrt_instance *inst = nullptr;
        if (!make(&inst)) return 1;
        double first = render(inst, 0), total = 0, worst = 0;
        int fetches = 0;
        for (int f = 1; f < play; ++f) {
            double t = render(inst, f);
            total += t;
            fetches += src.fetches;
            if (t > worst) worst = t;
        }
        const std::string tm = ntscrt_last_timings(inst);
        const size_t r = tm.find(" ntsc_route=");
        std::printf("ntsc route: %s\n", r == std::string::npos ? "-" : tm.substr(r + 12, tm.find(')', r) - r - 11).c_str());
        std::printf("play: frame 0 (run-up) %.0f ms; frames 1..%d %.1f ms/frame (worst %.0f), %.1f fetches/frame\n",
                    first, play - 1, play > 1 ? total / (play - 1) : 0.0, worst,
                    play > 1 ? double(fetches) / (play - 1) : 0.0);
        ntscrt_destroy(inst);
    }
    for (int64_t n : seeks) {
        ntscrt_instance *inst = nullptr;
        if (!make(&inst)) return 1;
        double t = render(inst, n);
        int fetches = src.fetches;
        double fetch_ms = src.fetch_ms;
        double t1 = render(inst, n + 1), t2 = render(inst, n + 2);
        std::printf("seek %lld: %.0f ms (%d source fetches, %.0f ms of it fetching); then %.1f, %.1f ms\n",
                    (long long)n, t, fetches, fetch_ms, t1, t2);
        if (scrub) {
            for (int64_t m : {n - 30, n - 20, n + 10, n - 100, n - 99}) {
                if (m < 0) continue;
                double tm = render(inst, m);
                std::printf("  then %lld: %.0f ms (%d fetches)\n", (long long)m, tm, src.fetches);
            }
        }
        ntscrt_destroy(inst);
    }
    return 0;
}
