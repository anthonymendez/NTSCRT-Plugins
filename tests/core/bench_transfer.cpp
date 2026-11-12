// bench_transfer — per-frame time of ntscrt_render_cpu as an OpenFX host
// calls it (bottom-up host buffers, premultiplied RGBA, frames in order, a
// source fetch for the temporal stages), with where the time goes: convert
// in, CPU stages (ntsc-rs, Digital video), upload, GPU stages (submission),
// readback (with the wait for the GPU), convert out (testing::last_phases).
// Not a ctest test: run by hand, on an otherwise idle machine.
//
//   bench_transfer [--sizes 3840x2160,1920x1080] [--formats rgba32f,rgba16,rgba8]
//                  [--looks default,clean_crt,ntsc_off] [--frames 300] [--warmup 30]
//                  [--fps 60] [--label NAME] [--resources DIR] [--quiet]
//                  [--pool-threads N]   (worker pool size; 1 = no pool)
//                  [--alpha]            (a source with transparent and half-transparent areas)
//                  [--dump DIR]         (writes each case's last frame to DIR/<case>.raw, to compare
//                                        builds or NTSCRT_FAST_TRANSFER=0 byte for byte)
//
// Looks: "default" = every param at its schema default (what a new effect
// renders), "clean_crt" = the bundled "Clean CRT" preset (NTSC off),
// "ntsc_off" = the default look with the NTSC signal unticked. Also "spill"
// (ntsc_off, Transparency: Let effect spill) and "crop" (ntsc_off, 4:3 crop).
// "ntsc_gpu_passes" = the default look with the NTSC stages phase 1 of the GPU
// port lacked turned off (docs/dev/NTSC-GPU.md); "preset:<name>" = the bundled
// preset presets/<name>.json (e.g. "preset:Mild VHS"; not the Screen Loop ones).
// Run with and without NTSCRT_NTSC_GPU=1 to compare ntsc-rs with the GPU port
// (the "ntsc" column is the NTSC stage's share of cpu_stg; "gpu%" the frames
// that took the GPU route).
// Frames are rendered back to back (fps only sets the frame rate the look's
// timing uses). One JSON line per case (prefix "JSON "), then a table.
#include <ntscrt/ntscrt.h>

#include <nlohmann/json.hpp>

#include "ntscrt_host/params.h"
#include "stages/digital/parallel.h"
#include "testing.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#pragma comment(lib, "dxgi.lib")
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!item.empty()) out.push_back(item);
    return out;
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double k = p * double(v.size() - 1);
    const size_t i = size_t(k);
    const double f = k - double(i);
    return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}

struct Fmt {
    std::string name;
    ntscrt_pixel_format f;
    size_t bpp;
};
bool parse_fmt(const std::string &n, Fmt &o) {
    if (n == "rgba32f") o = {n, NTSCRT_RGBA32F, 16};
    else if (n == "rgba16f") o = {n, NTSCRT_RGBA16F, 8};
    else if (n == "rgba16") o = {n, NTSCRT_RGBA16, 8};
    else if (n == "rgba8") o = {n, NTSCRT_RGBA8, 4};
    else if (n == "bgra8") o = {n, NTSCRT_BGRA8, 4};
    else return false;
    return true;
}

uint16_t to_half(float x) {
    uint32_t f;
    std::memcpy(&f, &x, 4);
    uint32_t s = (f >> 16) & 0x8000u;
    int e = int((f >> 23) & 0xffu) - 127 + 15;
    uint32_t m = f & 0x7fffffu;
    if (e <= 0) return uint16_t(s);
    if (e >= 31) return uint16_t(s | 0x7c00u);
    return uint16_t(s | (uint32_t(e) << 10) | ((m + 0x1000u) >> 13));
}

// A video-like opaque picture: smooth gradients, a few edges, mild noise.
// alpha: the left eighth transparent, the next eighth half transparent
// (premultiplied), like a title over nothing.
std::vector<uint8_t> make_source(int w, int h, const Fmt &fmt, bool alpha) {
    std::vector<uint8_t> px(size_t(w) * h * fmt.bpp);
    uint32_t rng = 12345;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            rng = rng * 1664525u + 1013904223u;
            const float n = float((rng >> 8) & 0xff) / 255.f * 0.04f - 0.02f;
            const float fx = float(x) / float(w), fy = float(y) / float(h);
            float v[4] = {fx, fy, ((x / 64 + y / 64) & 1) ? 0.8f : 0.2f, 1.f};
            for (int c = 0; c < 3; ++c) v[c] = std::clamp(v[c] + n, 0.f, 1.f);
            if (alpha && x < w / 4) {
                v[3] = x < w / 8 ? 0.f : 0.5f;
                for (int c = 0; c < 3; ++c) v[c] *= v[3];
            }
            uint8_t *p = &px[(size_t(y) * w + x) * fmt.bpp];
            for (int c = 0; c < 4; ++c) {
                switch (fmt.f) {
                case NTSCRT_RGBA8: p[c] = uint8_t(std::lround(v[c] * 255.f)); break;
                case NTSCRT_BGRA8: p[c == 3 ? 3 : 2 - c] = uint8_t(std::lround(v[c] * 255.f)); break;
                case NTSCRT_RGBA16: reinterpret_cast<uint16_t *>(p)[c] = uint16_t(std::lround(v[c] * 65535.f)); break;
                case NTSCRT_RGBA16F: reinterpret_cast<uint16_t *>(p)[c] = to_half(v[c]); break;
                case NTSCRT_RGBA32F: reinterpret_cast<float *>(p)[c] = v[c]; break;
                }
            }
        }
    return px;
}

// OpenFX layout: rows bottom-up in memory; the image points at the top row.
ntscrt_image bottom_up(std::vector<uint8_t> &buf, int w, int h, const Fmt &fmt) {
    const ptrdiff_t rb = ptrdiff_t(w) * ptrdiff_t(fmt.bpp);
    return ntscrt_image{w, h, -rb, fmt.f, buf.data() + size_t(h - 1) * size_t(rb)};
}

struct Host {
    const ntscrt_image *src;
};
const ntscrt_image *fetch(void *u, int64_t) { return static_cast<Host *>(u)->src; }
void release(void *, const ntscrt_image *) {}

std::string read_file(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string adapter_name(ntscrt_instance *inst) {
#ifdef _WIN32
    auto *dev = static_cast<ID3D11Device *>(ntscrt::testing::stats(inst).gpu_device);
    if (!dev) return "none";
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
    DXGI_ADAPTER_DESC d{};
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&ad)) || FAILED(ad->GetDesc(&d)))
        return "?";
    char buf[256];
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, buf, sizeof buf, nullptr, nullptr);
    return buf;
#else
    return ntscrt::testing::stats(inst).has_gpu ? "Metal" : "none";
#endif
}

}  // namespace

int main(int argc, char **argv) {
    std::string sizes = "3840x2160,1920x1080", formats = "rgba32f,rgba16,rgba8", looks = "default,clean_crt,ntsc_off";
    std::string label = "", resources = NTSCRT_BUILT_RESOURCES_DIR;
    int frames = 300, warmup = 30;
    double fps = 60;
    bool quiet = false, alpha = false;
    std::string dump;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--sizes") sizes = next();
        else if (a == "--formats") formats = next();
        else if (a == "--looks") looks = next();
        else if (a == "--frames") frames = std::atoi(next().c_str());
        else if (a == "--warmup") warmup = std::atoi(next().c_str());
        else if (a == "--fps") fps = std::atof(next().c_str());
        else if (a == "--label") label = next();
        else if (a == "--resources") resources = next();
        else if (a == "--quiet") quiet = true;
        else if (a == "--alpha") alpha = true;
        else if (a == "--dump") dump = next();
        else if (a == "--pool-threads") ntscrt::digital::set_thread_count(std::atoi(next().c_str()));
        else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }

    using namespace ntscrt::host;
    const Schema schema = Schema::parse(ntscrt_param_schema_json());
    auto look_from = [&](const json *preset) {
        return build_look(schema, [&](size_t i) -> json {
            const ParamDef &p = schema.params[i];
            if (preset)
                if (auto v = preset_value(p, *preset, false)) return *v;
            return p.def;
        });
    };
    struct Look {
        std::string name, look, feedback;
    };
    std::vector<Look> look_list;
    for (const std::string &n : split(looks)) {
        Built b;
        if (n == "default") {
            b = look_from(nullptr);
        } else if (n == "ntsc_off" || n == "spill" || n == "crop") {
            b = look_from(nullptr);
            set_at(b.look, "/ntsc/enabled", false);
            if (n == "spill") set_at(b.look, "/alpha/mode", "spill");
            if (n == "crop") {
                set_at(b.look, "/crop/enabled", true);
                set_at(b.look, "/crop/ratio", "4:3");
            }
        } else if (n == "ntsc_gpu_passes") {
            b = look_from(nullptr);
            const json off = json::parse(R"({
                "composite_noise": false, "snow_intensity": 0, "head_switching": false, "tracking_noise": false,
                "luma_noise": false, "chroma_noise": false, "chroma_delay_horizontal": 0,
                "chroma_delay_vertical": 0, "vhs_edge_wave_enabled": false})");
            for (auto it = off.begin(); it != off.end(); ++it) b.look["ntsc"]["settings"][it.key()] = it.value();
        } else if (n == "clean_crt" || n.rfind("preset:", 0) == 0) {
            const std::string file = n == "clean_crt" ? std::string("Clean CRT") : n.substr(7);
            const std::string text = read_file(resources + "/presets/" + file + ".json");
            json preset = json::parse(text, nullptr, false);
            if (preset.is_discarded()) {
                std::fprintf(stderr, "cannot read %s/presets/%s.json\n", resources.c_str(), file.c_str());
                return 2;
            }
            b = look_from(&preset);
        } else {
            std::fprintf(stderr, "unknown look %s\n", n.c_str());
            return 2;
        }
        look_list.push_back({n, b.look.dump(), b.feedback.is_null() ? std::string() : b.feedback.dump()});
    }

    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, resources.c_str()};
    ntscrt_instance *inst = nullptr;
    if (ntscrt_create(&ci, &inst) != NTSCRT_OK) {
        std::fprintf(stderr, "ntscrt_create failed\n");
        return 1;
    }
    const std::string gpu = adapter_name(inst);
    std::printf("bench_transfer %s gpu=\"%s\" frames=%d warmup=%d fps=%g\n", label.c_str(), gpu.c_str(), frames,
                warmup, fps);

    struct Row {
        std::string name;
        double med, p95, ph[7], ntsc, gpu_frac;
    };
    std::vector<Row> rows;
    int64_t next_frame = 0;  // a fresh stretch of frames for every case (no cache from the last case)
    for (const std::string &sz : split(sizes)) {
        int w = 0, h = 0;
        if (std::sscanf(sz.c_str(), "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
            std::fprintf(stderr, "bad size %s\n", sz.c_str());
            return 2;
        }
        for (const std::string &fn : split(formats)) {
            Fmt fmt;
            if (!parse_fmt(fn, fmt)) {
                std::fprintf(stderr, "unknown format %s\n", fn.c_str());
                return 2;
            }
            std::vector<uint8_t> src_buf = make_source(w, h, fmt, alpha), dst_buf(src_buf.size());
            ntscrt_image src = bottom_up(src_buf, w, h, fmt), dst = bottom_up(dst_buf, w, h, fmt);
            Host host{&src};
            for (const Look &lk : look_list) {
                ntscrt_flush_caches(inst);
                std::vector<double> total;
                std::vector<ntscrt::testing::PhaseTimes> ph;
                int failures = 0, gpu_route = 0;
                std::vector<double> tables_ms;
                std::map<std::string, std::vector<double>> stage_ms;
                std::string err;
                const int64_t first = next_frame;
                for (int f = 0; f < warmup + frames; ++f) {
                    ntscrt_render_args ra{};
                    ra.frame_index = first + f;
                    ra.fps = fps;
                    ra.look_json = lk.look.c_str();
                    ra.feedback_json = lk.feedback.empty() ? nullptr : lk.feedback.c_str();
                    ra.render_scale = 1;
                    ra.seed = 1;
                    ra.fetch_source = fetch;
                    ra.release_source = release;
                    ra.user = &host;
                    ra.source_premultiplied = 1;
                    const auto a = Clock::now();
                    const ntscrt_status st = ntscrt_render_cpu(inst, &ra, &src, &dst);
                    const double t = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
                    if (st != NTSCRT_OK) {
                        ++failures;
                        err = ntscrt_last_error(inst);
                    }
                    if (f >= warmup) {
                        total.push_back(t);
                        ph.push_back(ntscrt::testing::last_phases(inst));
                        const char *tm = ntscrt_last_timings(inst);
                        gpu_route += std::strstr(tm, "ntsc_route=gpu") != nullptr;
                        // GPU NTSC: the CPU time of its plan + tables, and (NTSCRT_PROFILE=1)
                        // the GPU time per pass, summed per stage name.
                        if (const char *q = std::strstr(tm, " ntsc_tables_ms=")) tables_ms.push_back(std::atof(q + 16));
                        if (const char *q = std::strstr(tm, " ntsc_gpu=")) {
                            std::string list(q + 10);
                            list = list.substr(0, list.find(' '));
                            std::map<std::string, double> frame;
                            for (const std::string &item : split(list)) {
                                const size_t c = item.rfind(':');
                                if (c != std::string::npos) frame[item.substr(0, c)] += std::atof(item.c_str() + c + 1);
                            }
                            for (const auto &kv : frame) stage_ms[kv.first].push_back(kv.second);
                        }
                    }
                }
                next_frame += warmup + frames + 1000;
                if (!dump.empty()) {
                    std::string file_name = lk.name;  // "preset:Mild VHS" -> "preset_Mild_VHS"
                    for (char &c : file_name)
                        if (c == ':' || c == ' ' || c == '/' || c == '\\') c = '_';
                    const std::string path = dump + "/" + std::to_string(w) + "x" + std::to_string(h) + "_" + fn +
                                             "_" + file_name + ".raw";
                    std::ofstream o(path, std::ios::binary);
                    o.write(reinterpret_cast<const char *>(dst_buf.data()), std::streamsize(dst_buf.size()));
                }
                auto med_of = [&](auto get) {
                    std::vector<double> v;
                    for (const auto &p : ph) v.push_back(get(p));
                    return pct(v, 0.5);
                };
                Row r;
                r.name = sz + " " + fn + " " + lk.name;
                r.med = pct(total, 0.5);
                r.p95 = pct(total, 0.95);
                r.ph[0] = med_of([](const auto &p) { return p.convert_in; });
                r.ph[1] = med_of([](const auto &p) { return p.cpu_stages; });
                r.ph[2] = med_of([](const auto &p) { return p.upload; });
                r.ph[3] = med_of([](const auto &p) { return p.gpu_stages; });
                r.ph[4] = med_of([](const auto &p) { return p.readback; });
                r.ph[5] = med_of([](const auto &p) { return p.convert_out; });
                r.ph[6] = med_of([](const auto &p) { return p.other(); });
                r.ntsc = med_of([](const auto &p) { return p.ntsc; });
                r.gpu_frac = total.empty() ? 0 : double(gpu_route) / double(total.size());
                rows.push_back(r);
                double mean = 0;
                for (double t : total) mean += t;
                if (!total.empty()) mean /= double(total.size());
                json j = {{"bench", "transfer"},
                          {"label", label},
                          {"gpu", gpu},
                          {"w", w},
                          {"h", h},
                          {"format", fn},
                          {"look", lk.name},
                          {"frames", frames},
                          {"warmup", warmup},
                          {"fps", fps},
                          {"failures", failures},
                          {"median_ms", std::round(r.med * 1000) / 1000},
                          {"p95_ms", std::round(r.p95 * 1000) / 1000},
                          {"mean_ms", std::round(mean * 1000) / 1000}};
                const char *names[7] = {"convert_in", "cpu_stages", "upload", "gpu_stages",
                                        "readback",   "convert_out", "other"};
                json pj = json::object();
                for (int k = 0; k < 7; ++k) pj[names[k]] = std::round(r.ph[k] * 1000) / 1000;
                pj["ntsc"] = std::round(r.ntsc * 1000) / 1000;
                j["phases_median_ms"] = pj;
                j["ntsc_gpu_frames"] = gpu_route;
                if (!tables_ms.empty()) j["ntsc_tables_median_ms"] = std::round(pct(tables_ms, 0.5) * 1000) / 1000;
                if (!stage_ms.empty()) {
                    json sj = json::object();
                    for (auto &kv : stage_ms) sj[kv.first] = std::round(pct(kv.second, 0.5) * 1000) / 1000;
                    j["ntsc_gpu_stage_median_ms"] = sj;
                }
                if (failures) j["error"] = err;
                std::printf("JSON %s\n", j.dump().c_str());
                std::fflush(stdout);
            }
        }
    }
    if (!quiet) {
        std::printf("\n%-28s %8s %8s | %8s %8s %8s %8s %8s %8s %8s | %8s %5s  (phase medians, ms)\n", "case",
                    "median", "p95", "conv_in", "cpu_stg", "upload", "gpu_stg", "readbk", "conv_out", "other", "ntsc",
                    "gpu%");
        for (const Row &r : rows)
            std::printf("%-28s %8.2f %8.2f | %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f | %8.2f %5.0f\n", r.name.c_str(),
                        r.med, r.p95, r.ph[0], r.ph[1], r.ph[2], r.ph[3], r.ph[4], r.ph[5], r.ph[6], r.ntsc,
                        r.gpu_frac * 100);
    }
    ntscrt_destroy(inst);
    return 0;
}
