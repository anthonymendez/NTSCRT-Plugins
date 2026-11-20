// bench_digital — Digital video seek timings over the C ABI (docs/dev/DIGITAL-CHECKPOINTS.md).
// Not a ctest test: run by hand. NTSCRT_DIGITAL_GPU=0 / NTSCRT_NTSC_GPU=0 for the CPU routes.
//
//   bench_digital [--w 1920] [--h 1080] [--format datamosh] [--look JSON] [--fps 30]
//                 [--seek 1850,108900] [--then 30] [--back 10] [--scale 0.25] [--only-digital]
//
// The look is the plug-in's default (every control at its default: NTSC, CRT on) with a
// Digital video format applied as the Preset menu does; --only-digital turns the other
// stages off. Sources are generated in memory and served through fetch_source like a
// host, with the fetch time counted separately.
//   seek:  for each frame n, a fresh instance renders it cold (what a jump into a clip costs).
//   then:  plays on that many frames after it.
//   back:  steps back one frame at a time that many frames from n (the left-arrow key),
//          then jumps back to n - 60 and n - 300.
//   scale: renders n at that render scale (a host's viewer) and then n + 1 full size again.
#include <ntscrt/ntscrt.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ntscrt_host/params.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

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
    std::map<int64_t, std::vector<uint8_t>> live;
    std::map<int64_t, ntscrt_image> imgs;
    int fetches = 0;
    double fetch_ms = 0;
    void fill(std::vector<uint8_t> &px, int64_t f, int w_, int h_) const {
        px.resize(size_t(w_) * h_ * 4);
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                uint8_t *p = &px[(size_t(y) * w_ + x) * 4];
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
    s->fill(px, f, s->w, s->h);
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
json plugin_default() {
    const auto schema = ntscrt::host::Schema::parse(ntscrt_param_schema_json());
    return ntscrt::host::build_look(schema, [&](size_t i) { return schema.params[i].def; }).look;
}
bool with_format(json &look, const std::string &id) {
    const json s = json::parse(ntscrt_param_schema_json());
    for (const json &f : s["digital_formats"])
        if (f.value("id", std::string()) == id) {
            for (auto it = f["applies"].begin(); it != f["applies"].end(); ++it) {
                if (it.value().is_object())
                    for (auto v = it.value().begin(); v != it.value().end(); ++v)
                        ntscrt::host::set_at(look, it.key() + "/" + v.key(), v.value());
                else ntscrt::host::set_at(look, it.key(), it.value());
            }
            return true;
        }
    return false;
}
int captures(const std::string &tm) {
    const size_t p = tm.find("digital_captures=");
    return p == std::string::npos ? -1 : std::atoi(tm.c_str() + p + 17);
}
}  // namespace

int main(int argc, char **argv) {
    int w = 1920, h = 1080, then = 0, back = 0;
    double fps = 30, scale = 0;
    bool only = false;
    std::string format = "datamosh", look_arg;
    std::vector<int64_t> seeks;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (a == "--w") w = std::atoi(next().c_str());
        else if (a == "--h") h = std::atoi(next().c_str());
        else if (a == "--format") format = next();
        else if (a == "--look") look_arg = next();
        else if (a == "--fps") fps = std::atof(next().c_str());
        else if (a == "--then") then = std::atoi(next().c_str());
        else if (a == "--back") back = std::atoi(next().c_str());
        else if (a == "--scale") scale = std::atof(next().c_str());
        else if (a == "--only-digital") only = true;
        else if (a == "--seek") {
            std::stringstream ss(next());
            std::string t;
            while (std::getline(ss, t, ',')) seeks.push_back(std::atoll(t.c_str()));
        } else {
            std::fprintf(stderr, "unknown arg %s\n", a.c_str());
            return 2;
        }
    }
    json lj = look_arg.empty() ? plugin_default() : json::parse(look_arg);
    if (look_arg.empty() && !with_format(lj, format)) {
        std::fprintf(stderr, "no Digital video format %s\n", format.c_str());
        return 2;
    }
    if (only)
        for (const char *s : {"ntsc", "glitch", "crt", "shader"})
            if (lj.contains(s) && lj[s].is_object()) lj[s]["enabled"] = false;
    const std::string look = lj.dump();
    const std::string res = NTSCRT_BUILT_RESOURCES_DIR;
    Src src;
    std::vector<uint8_t> in_px, out_px;
    auto make = [&](ntscrt_instance **inst) {
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, res.c_str()};
        return ntscrt_create(&ci, inst) == NTSCRT_OK;
    };
    std::string tm;
    json off = lj;
    off["digital"]["enabled"] = false;
    const std::string look_off = off.dump();
    const std::string *cur = &look;
    auto render = [&](ntscrt_instance *inst, int64_t f, double sc = 1) {
        src.w = std::max(16, int(w * sc));
        src.h = std::max(16, int(h * sc));
        src.fill(in_px, f, src.w, src.h);
        out_px.resize(in_px.size());
        ntscrt_image s{src.w, src.h, ptrdiff_t(src.w) * 4, NTSCRT_RGBA8, in_px.data()};
        ntscrt_image d{src.w, src.h, ptrdiff_t(src.w) * 4, NTSCRT_RGBA8, out_px.data()};
        ntscrt_render_args ra{};
        ra.frame_index = f;
        ra.fps = fps;
        ra.look_json = cur->c_str();
        ra.render_scale = sc;
        ra.fetch_source = fetch;
        ra.release_source = release;
        ra.user = &src;
        src.fetches = 0;
        src.fetch_ms = 0;
        auto t0 = Clock::now();
        ntscrt_status st = ntscrt_render_cpu(inst, &ra, &s, &d);
        double t = ms(t0, Clock::now());
        if (st != NTSCRT_OK) std::fprintf(stderr, "frame %lld: status %d %s\n", (long long)f, st, ntscrt_last_error(inst));
        tm = ntscrt_last_timings(inst);
        return t;
    };
    std::printf("%dx%d @ %g fps, format \"%s\"%s\n", w, h, fps, format.c_str(), only ? " (Digital only)" : "");
    for (int64_t n : seeks) {
        ntscrt_instance *inst = nullptr;
        if (!make(&inst)) return 1;
        // Warm-up (GPU device, shaders, tables) on a far frame with Digital video off.
        cur = &look_off;
        const double warm = render(inst, n + 100000);
        cur = &look;
        double t = render(inst, n);
        const size_t r = tm.find(" digital_route=");
        std::printf("seek %lld: %.0f ms, %d source fetches (%.0f ms of it fetching), %d captures coded; route %s; "
                    "peak memory %.0f MB (warm-up %.0f ms)\n",
                    (long long)n, t, src.fetches, src.fetch_ms, captures(tm),
                    r == std::string::npos ? "-" : tm.substr(r + 15, tm.find(')', r) - r - 14).c_str(), peak_mb(), warm);
        if (const size_t k = tm.find(" digital_checkpoints="); k != std::string::npos)
            std::printf("  checkpoints after it (count:MB + trail count:MB): %s\n",
                        tm.substr(k + 21, tm.find(' ', k + 1) - k - 21).c_str());
        if (then > 0) {
            int tf = 0;
            double tt = 0, worst = 0;
            for (int64_t m = n + 1; m <= n + then; ++m) {
                double x = render(inst, m);
                tt += x;
                tf += src.fetches;
                if (x > worst) worst = x;
            }
            std::printf("  then %lld..%lld: %.1f ms/frame (worst %.0f), %d source fetches\n", (long long)(n + 1),
                        (long long)(n + then), tt / then, worst, tf);
        }
        if (back > 0) {
            int tf = 0, tc = 0;
            double tt = 0, worst = 0;
            for (int64_t m = n - 1; m >= n - back && m >= 0; --m) {
                double x = render(inst, m);
                tt += x;
                tf += src.fetches;
                tc += captures(tm);
                if (x > worst) worst = x;
            }
            std::printf("  back one at a time %lld..%lld: %.1f ms/frame (worst %.0f), %d source fetches, %d captures\n",
                        (long long)(n - 1), (long long)(n - back), tt / back, worst, tf, tc);
            for (int64_t m : {n - 60, n - 300}) {
                if (m < 0) continue;
                double x = render(inst, m);
                std::printf("  then jump to %lld: %.0f ms, %d source fetches, %d captures\n", (long long)m, x,
                            src.fetches, captures(tm));
            }
        }
        if (scale > 0) {
            double x = render(inst, n, scale);
            std::printf("  %lld at render scale %g: %.0f ms, %d source fetches, %d captures\n", (long long)n, scale, x,
                        src.fetches, captures(tm));
            x = render(inst, n + 1);
            std::printf("  then %lld full size: %.0f ms, %d source fetches, %d captures\n", (long long)(n + 1), x,
                        src.fetches, captures(tm));
        }
        ntscrt_destroy(inst);
    }
    return 0;
}
