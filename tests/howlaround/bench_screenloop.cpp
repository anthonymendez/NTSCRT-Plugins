// bench_screenloop — Screen Loop playback and seek timings over the C ABI.
// Not a ctest test: run by hand. NTSCRT_NTSC_GPU=0 for the ntsc-rs route, unset for the default.
//
//   bench_screenloop [--w 1920] [--h 1080] [--preset "7 copies slow luma"] [--look JSON]
//                    [--play 30] [--seek 600,1000] [--then 300] [--scrub] [--fps 30]
//
// Sources are generated in memory (a moving picture: windows of one wide picture, served
// without a copy, so fetching costs next to nothing), through fetch_source like a host.
//   play:  frames 0..play-1 in order on one instance (ms per frame).
//   seek:  for each frame, a fresh instance renders it cold, then the next two
//          frames (what a host asks for when playback starts after a seek).
//   then:  after each cold seek, play on that many frames (crossing window starts) and
//          report the source fetches and time of those frames.
//   scrub: on one instance, after the cold seek: back 30, forward 10, back 100.
//   float: sources (fetched and the frame's own) as RGBA32F, as Resolve hands them over.
//   phases: print the core's timings line (ntscrt_last_timings) of each cold seek.
//   edits: after each cold seek, that many Screen Loop zoom edits, each followed by a render of
//          the frame (an Inspector edit at the playhead: a re-run of its window).
#include <ntscrt/ntscrt.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

using Clock = std::chrono::steady_clock;
// Peak private memory of this process so far, MB (0 where not measured).
static double peak_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pm{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pm), sizeof pm))
        return double(pm.PeakPagefileUsage) / (1024.0 * 1024.0);
#endif
    return 0;
}
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

namespace {
struct Src {
    int w = 0, h = 0;
    bool fp = false;  // RGBA32F
    std::vector<uint8_t> base;  // a wide picture made once; frame f is a window of it (cheap to serve)
    std::map<int64_t, std::vector<uint8_t>> live;
    std::map<int64_t, ntscrt_image> imgs;
    int fetches = 0;
    double fetch_ms = 0;
    size_t peak_live = 0;  // host images handed out and not yet released, at most
    static constexpr int kPad = 512;
    size_t px_bytes() const { return fp ? 16 : 4; }
    void make_base() {
        const int bw = w + kPad;
        base.assign(size_t(bw) * h * px_bytes(), 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < bw; ++x) {
                const bool bar = (x / 64) % 2 == 0;
                const uint8_t c[4] = {uint8_t((x * 3) % 256), uint8_t((y * 2 + x) % 256), uint8_t(bar ? 40 : 210), 255};
                const size_t i = size_t(y) * bw + x;
                if (fp) {
                    float *q = reinterpret_cast<float *>(base.data()) + i * 4;
                    for (int k = 0; k < 4; ++k) q[k] = float(c[k]) / 255.f;
                } else {
                    std::memcpy(base.data() + i * 4, c, 4);
                }
            }
    }
    // A moving picture: frame f is the base shifted left by (f * 7) % kPad pixels.
    void fill(std::vector<uint8_t> &px, int64_t f) {
        if (base.empty()) make_base();
        const size_t pb = px_bytes(), row = size_t(w) * pb, brow = size_t(w + kPad) * pb;
        const size_t off = size_t((f * 7) % kPad) * pb;
        px.resize(row * h);
        for (int y = 0; y < h; ++y) std::memcpy(px.data() + y * row, base.data() + y * brow + off, row);
    }
    // Frame f without a copy: the host's frame costs nothing to fetch, so the times are the core's.
    ntscrt_image view(int64_t f) {
        if (base.empty()) make_base();
        const size_t pb = px_bytes();
        return {w, h, ptrdiff_t(size_t(w + kPad) * pb), fp ? NTSCRT_RGBA32F : NTSCRT_RGBA8,
                base.data() + size_t((f * 7) % kPad) * pb};
    }
    ntscrt_image image(std::vector<uint8_t> &px) const {
        return {w, h, ptrdiff_t(size_t(w) * px_bytes()), fp ? NTSCRT_RGBA32F : NTSCRT_RGBA8, px.data()};
    }
};
const ntscrt_image *fetch(void *u, int64_t f) {
    auto *s = static_cast<Src *>(u);
    if (f < 0) return nullptr;
    auto t0 = Clock::now();
    auto &px = s->live[f];  // empty: the frame is a window of the base picture (no copy)
    (void)px;
    s->imgs[f] = s->view(f);
    s->fetch_ms += ms(t0, Clock::now());
    ++s->fetches;
    if (s->live.size() > s->peak_live) s->peak_live = s->live.size();
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
    int w = 1920, h = 1080, play = 30, then = 0, edits = 0;
    double fps = 30;
    bool scrub = false, fp = false, phases = false;
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
        else if (a == "--float") fp = true;
        else if (a == "--phases") phases = true;
        else if (a == "--then") then = std::atoi(next().c_str());
        else if (a == "--edits") edits = std::atoi(next().c_str());
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
    std::string fb_cur = fb;  // the feedback rendered (edits change it)
    Src src;
    src.w = w;
    src.h = h;
    src.fp = fp;
    std::vector<uint8_t> in_px, out_px(size_t(w) * h * 4);
    auto make = [&](ntscrt_instance **inst) {
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, res.c_str()};
        return ntscrt_create(&ci, inst) == NTSCRT_OK;
    };
    auto render = [&](ntscrt_instance *inst, int64_t f) {
        src.fill(in_px, f);
        ntscrt_image s = src.image(in_px);
        ntscrt_image d{w, h, ptrdiff_t(w) * 4, NTSCRT_RGBA8, out_px.data()};
        ntscrt_render_args ra{};
        ra.frame_index = f;
        ra.fps = fps;
        ra.look_json = look.c_str();
        ra.feedback_json = fb_cur.c_str();
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
        std::printf("play: frame 0 (run-up) %.0f ms; frames 1..%d %.1f ms/frame (worst %.0f), %.1f fetches/frame; "
                    "%d frames in %.1f s, peak memory %.0f MB\n",
                    first, play - 1, play > 1 ? total / (play - 1) : 0.0, worst,
                    play > 1 ? double(fetches) / (play - 1) : 0.0, play, (first + total) / 1000.0, peak_mb());
        ntscrt_destroy(inst);
    }
    for (int64_t n : seeks) {
        ntscrt_instance *inst = nullptr;
        if (!make(&inst)) return 1;
        double t = render(inst, n);
        const std::string seek_timings = ntscrt_last_timings(inst);
        int fetches = src.fetches;
        double fetch_ms = src.fetch_ms;
        double t1 = render(inst, n + 1), t2 = render(inst, n + 2);
        std::printf("seek %lld: %.0f ms (%d source fetches, %.0f ms of it fetching, at most %zu held at once); "
                    "then %.1f, %.1f ms; peak memory %.0f MB\n",
                    (long long)n, t, fetches, fetch_ms, src.peak_live, t1, t2, peak_mb());
        if (phases) std::printf("  phases: %s\n", seek_timings.c_str());
        if (then > 0) {
            int tf = 0;
            double tt = 0, worst = 0;
            for (int64_t m = n + 3; m < n + 3 + then; ++m) {
                double tm = render(inst, m);
                tt += tm;
                tf += src.fetches;
                if (tm > worst) worst = tm;
            }
            std::printf("  then %lld..%lld: %.1f s, %d source fetches, worst frame %.0f ms; peak memory %.0f MB\n",
                        (long long)(n + 3), (long long)(n + 2 + then), tt / 1000.0, tf, worst, peak_mb());
        }
        for (int k = 1; k <= edits; ++k) {
            // the preset with another zoom (textual: "zoom" : <number>)
            const size_t z = fb_cur.find("\"zoom\"");
            const size_t v = z == std::string::npos ? z : fb_cur.find_first_of("0123456789.-", z + 6);
            const size_t e = v == std::string::npos ? v : fb_cur.find_first_not_of("0123456789.-e", v);
            if (e == std::string::npos) break;
            fb_cur = fb_cur.substr(0, v) + std::to_string(0.5 + 0.05 * k) + fb_cur.substr(e);
            double tm = render(inst, n);
            std::printf("  edit %d at %lld: %.0f ms (%d fetches)\n", k, (long long)n, tm, src.fetches);
            if (phases) std::printf("    phases: %s\n", ntscrt_last_timings(inst));
        }
        fb_cur = fb;
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
