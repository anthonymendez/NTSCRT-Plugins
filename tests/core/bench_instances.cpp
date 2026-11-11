// bench_instances — instance creation / first frame / steady frame timings and
// memory for N core instances (what N OFX instances in one host process cost).
// Not a ctest test: run by hand.
//
//   bench_instances [--n 8] [--w 1920] [--h 1080] [--format rgba8|rgba16f|rgba32f]
//                   [--preset glow_gauss] [--look JSON] [--frames 5] [--threads]
//
// --threads renders the instances' frames concurrently (one thread each);
// otherwise one after the other.
#include <ntscrt/ntscrt.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dxgi.lib")

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static double private_mb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof pmc);
    return double(pmc.PrivateUsage) / (1024.0 * 1024.0);
}
// This process's local video memory use on adapter 0 (hardware devices only).
static double gpu_mb() {
    Microsoft::WRL::ComPtr<IDXGIFactory4> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return -1;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
    if (FAILED(f->EnumAdapters1(0, &a))) return -1;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> a3;
    if (FAILED(a.As(&a3))) return -1;
    DXGI_QUERY_VIDEO_MEMORY_INFO i{};
    if (FAILED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &i))) return -1;
    return double(i.CurrentUsage) / (1024.0 * 1024.0);
}

int main(int argc, char **argv) {
    int n = 8, w = 1920, h = 1080, frames = 5;
    bool threads = false;
    std::string preset = "glow_gauss", fmt = "rgba8", look_arg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--n") n = std::atoi(next().c_str());
        else if (a == "--w") w = std::atoi(next().c_str());
        else if (a == "--h") h = std::atoi(next().c_str());
        else if (a == "--frames") frames = std::atoi(next().c_str());
        else if (a == "--preset") preset = next();
        else if (a == "--format") fmt = next();
        else if (a == "--threads") threads = true;
        else if (a == "--look") look_arg = next();
    }
    ntscrt_pixel_format pf = NTSCRT_RGBA8;
    size_t bpp = 4;
    if (fmt == "rgba16f") pf = NTSCRT_RGBA16F, bpp = 8;
    else if (fmt == "rgba32f") pf = NTSCRT_RGBA32F, bpp = 16;

    std::vector<uint8_t> src(size_t(w) * h * bpp, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float v[4] = {float(x) / w, float(y) / h, float((x ^ y) & 255) / 255.f, 1.f};
            uint8_t *p = &src[(size_t(y) * w + x) * bpp];
            if (pf == NTSCRT_RGBA8) for (int c = 0; c < 4; ++c) p[c] = uint8_t(v[c] * 255.f + 0.5f);
            else if (pf == NTSCRT_RGBA32F) std::memcpy(p, v, 16);
            else for (int c = 0; c < 4; ++c) reinterpret_cast<uint16_t *>(p)[c] = c == 3 ? 0x3c00 : 0x3800;  // 0.5
        }

    const double mem0 = private_mb(), gmem0 = gpu_mb();
    std::vector<ntscrt_instance *> inst(static_cast<size_t>(n), nullptr);
    std::vector<double> create_ms(static_cast<size_t>(n));
    auto t0 = Clock::now();
    for (int i = 0; i < n; ++i) {
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_BUILT_RESOURCES_DIR};
        auto a = Clock::now();
        if (ntscrt_create(&ci, &inst[size_t(i)]) != NTSCRT_OK) return 1;
        create_ms[size_t(i)] = ms(a, Clock::now());
    }
    const double create_total = ms(t0, Clock::now());
    const double mem1 = private_mb(), gmem1 = gpu_mb();

    const std::string look =
        !look_arg.empty() ? look_arg : "{\"shader\":{\"enabled\":true,\"preset\":\"" + preset + "\"}}";
    std::vector<std::vector<uint8_t>> dst(size_t(n), std::vector<uint8_t>(src.size()));
    std::vector<std::vector<double>> frame_ms(static_cast<size_t>(n));
    auto render = [&](int i, int frame) {
        ntscrt_render_args ra{};
        ra.frame_index = frame;
        ra.fps = 30;
        ra.look_json = look.c_str();
        ra.render_scale = 1;
        ntscrt_image s{w, h, ptrdiff_t(w * bpp), pf, src.data()};
        ntscrt_image d{w, h, ptrdiff_t(w * bpp), pf, dst[size_t(i)].data()};
        auto a = Clock::now();
        ntscrt_status st = ntscrt_render_cpu(inst[size_t(i)], &ra, &s, &d);
        frame_ms[size_t(i)].push_back(ms(a, Clock::now()));
        if (st != NTSCRT_OK) std::fprintf(stderr, "instance %d frame %d: status %d %s\n", i, frame, int(st),
                                          ntscrt_last_error(inst[size_t(i)]));
    };
    auto t1 = Clock::now();
    double first_wall = 0, mem2 = 0, gmem2 = 0;
    for (int f = 0; f < frames; ++f) {
        if (threads) {
            std::vector<std::thread> th;
            for (int i = 0; i < n; ++i) th.emplace_back(render, i, f);
            for (auto &t : th) t.join();
        } else {
            for (int i = 0; i < n; ++i) render(i, f);
        }
        if (f == 0) {
            first_wall = ms(t1, Clock::now());
            mem2 = private_mb();
            gmem2 = gpu_mb();
        }
    }
    const double all_wall = ms(t1, Clock::now());
    double first_sum = 0, first_max = 0, steady_sum = 0;
    int steady_n = 0;
    for (int i = 0; i < n; ++i) {
        first_sum += frame_ms[size_t(i)][0];
        first_max = std::max(first_max, frame_ms[size_t(i)][0]);
        for (size_t k = 1; k < frame_ms[size_t(i)].size(); ++k, ++steady_n) steady_sum += frame_ms[size_t(i)][k];
    }
    std::printf("instances=%d %dx%d %s preset=%s %s\n", n, w, h, fmt.c_str(), preset.c_str(),
                threads ? "concurrent" : "sequential");
    std::printf("create: first %.1f ms, mean of rest %.1f ms, total %.1f ms\n", create_ms[0],
                n > 1 ? (create_total - create_ms[0]) / (n - 1) : 0.0, create_total);
    std::printf("first frame: instance0 %.1f ms, mean %.1f ms, max %.1f ms, wall %.1f ms\n", frame_ms[0][0],
                first_sum / n, first_max, first_wall);
    if (steady_n)
        std::printf("later frames: mean %.1f ms/frame/instance, wall %.1f ms for %d frames x %d instances\n",
                    steady_sum / steady_n, all_wall - first_wall, frames - 1, n);
    std::printf("private MB: start %.0f, after create %.0f (+%.0f), after first frame %.0f (+%.0f)\n", mem0, mem1,
                mem1 - mem0, mem2, mem2 - mem0);
    if (gmem0 >= 0)
        std::printf("GPU local MB (process): start %.0f, after create %.0f (+%.0f), after first frame %.0f (+%.0f)\n",
                    gmem0, gmem1, gmem1 - gmem0, gmem2, gmem2 - gmem0);
    auto td = Clock::now();
    for (auto *p : inst) ntscrt_destroy(p);
    std::printf("destroy: %.1f ms\n", ms(td, Clock::now()));
    return 0;
}
