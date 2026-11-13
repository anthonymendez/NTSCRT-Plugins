// A frame's CRT picture is a function of the frame alone: every bundled preset,
// with the plug-in's default look and alone, renders a frame to the same bytes
// in any render order a host uses (a fresh instance; after another frame;
// repeated, as Fusion does; backwards, as when scrubbing; forwards, as in
// playback; interleaved between two instances, as with render-queue chunks or a
// second effect) and in another process, with and without shared chains.
//
// Run twice by ctest: test_crt_order (shared chains) writes every frame's hash to
// NTSCRT_ORDER_HASHES; test_crt_order_unshared (NTSCRT_SHARED_CHAINS=0, a second
// process) checks its own frames against them.
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ntscrt_host/params.h"

namespace {

constexpr int W = 200, H = 150;
constexpr double kFps = 30;
const int kFrames[] = {0, 75, 1850};  // the first frame, one in, one past a Glitch segment start

const char *kPresetIds[] = {"aperture", "easymode", "glow_gauss", "glow_lanczos", "hyllian", "royale", "sim"};

// RGBA32F, top row first; content moves with the frame so frames differ.
std::vector<float> source(int frame) {
    std::vector<float> px(size_t(W) * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float *p = &px[(size_t(y) * W + x) * 4];
            p[0] = float((x * 7 + frame * 13) % W) / float(W);
            p[1] = float((y * 5 + x + frame * 3) % H) / float(H);
            p[2] = 0.5f + 0.45f * std::sin(0.05f * float(x + y) + 0.3f * float(frame));
            p[3] = 1.f;
            if (((x / 16) + (y / 16) + frame) % 5 == 0) p[0] = p[1] = p[2] = 1.f;  // hard bright edges
        }
    return px;
}

uint64_t fnv(const std::vector<float> &v) {
    uint64_t h = 1469598103934665603ull;
    const auto *b = reinterpret_cast<const uint8_t *>(v.data());
    for (size_t i = 0; i < v.size() * 4; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

size_t values_off(const std::vector<float> &a, const std::vector<float> &b) {
    size_t n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) n += a[i] != b[i];
    return n;
}

struct Instance {
    ntscrt_instance *inst = nullptr;
    Instance() {
        const std::string res = NTSCRT_BUILT_RESOURCES_DIR;
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, res.c_str()};
        REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
    }
    ~Instance() { ntscrt_destroy(inst); }
    Instance(const Instance &) = delete;
    Instance &operator=(const Instance &) = delete;

    std::vector<float> render(const std::string &look, int frame) {
        std::vector<float> src = source(frame), out(size_t(W) * H * 4, -1.f);
        ntscrt_render_args ra{};
        ra.frame_index = frame;
        ra.fps = kFps;
        ra.look_json = look.c_str();
        ra.render_scale = 1.0;
        ntscrt_image s{W, H, ptrdiff_t(W) * 16, NTSCRT_RGBA32F, src.data()};
        ntscrt_image d{W, H, ptrdiff_t(W) * 16, NTSCRT_RGBA32F, out.data()};
        const ntscrt_status st = ntscrt_render_cpu(inst, &ra, &s, &d);
        INFO(ntscrt_last_error(inst));
        REQUIRE(st == NTSCRT_OK);
        return out;
    }
};

struct Look {
    std::string name, json;
};

// GitHub's macOS runners: Apple's paravirtual GPU hangs on crt-sim (same policy as
// tests/crt/test_crt.cpp and tests/core/test_shared_device.cpp: only there is it left out).
bool ci_virtual_gpu() {
#ifdef __APPLE__
    const char *gha = std::getenv("GITHUB_ACTIONS");
    return gha && std::string(gha) == "true";
#else
    return false;
#endif
}

std::vector<Look> looks() {
    if (ci_virtual_gpu()) MESSAGE("WARNING: macOS CI runner: crt-sim left out (it hangs the virtual GPU)");
    const auto schema = ntscrt::host::Schema::parse(ntscrt_param_schema_json());
    const nlohmann::json plugin_default =
        ntscrt::host::build_look(schema, [&](size_t i) { return schema.params[i].def; }).look;
    std::vector<Look> v;
    for (const char *id : kPresetIds) {
        nlohmann::json l = plugin_default;
        l.merge_patch({{"shader", {{"enabled", true}, {"preset", id}}}});
        v.push_back({std::string(id) + " (default look)", l.dump()});
        v.push_back({std::string(id) + " (CRT only)",
                     nlohmann::json{{"shader", {{"enabled", true}, {"preset", id}}}}.dump()});
    }
    // Sim with its feedback pushed: persistence that never fades (x10 per frame), full bleed.
    nlohmann::json pushed = plugin_default;
    pushed.merge_patch({{"shader",
                         {{"enabled", true},
                          {"preset", "sim"},
                          {"params",
                           {{"Tuning_Persistence_R", 1.0},
                            {"Tuning_Persistence_G", 1.0},
                            {"Tuning_Persistence_B", 1.0},
                            {"Tuning_Bleed", 1.0},
                            {"Tuning_Artifacts", 1.0},
                            {"Tuning_Sharp", 1.0}}}}}});
    v.push_back({"sim (default look, feedback pushed)", pushed.dump()});
    nlohmann::json mid = pushed;  // fading, slowly (x0.9 per frame)
    mid["shader"]["params"] = {{"Tuning_Persistence_R", 0.09}, {"Tuning_Persistence_G", 0.09},
                               {"Tuning_Persistence_B", 0.09}, {"Tuning_Bleed", 0.0}};
    v.push_back({"sim (default look, slow fade)", mid.dump()});
    if (const char *only = std::getenv("NTSCRT_ORDER_ONLY"); only && *only)  // by hand: one look
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Look &l) { return l.name.find(only) == std::string::npos; }),
                v.end());
    if (ci_virtual_gpu())
        v.erase(std::remove_if(v.begin(), v.end(), [](const Look &l) { return l.name.rfind("sim ", 0) == 0; }),
                v.end());
    return v;
}

std::string hash_file() {
    const char *e = std::getenv("NTSCRT_ORDER_HASHES");
    return e && *e ? e : "";
}

}  // namespace

TEST_CASE("a frame's CRT picture doesn't depend on the render order") {
    std::map<std::string, uint64_t> mine;  // "<look> | <frame>" -> hash
    for (const Look &lk : looks()) {
        CAPTURE(lk.name);
        // Reference: each frame first thing in a fresh instance.
        std::map<int, std::vector<float>> ref;
        for (int f : kFrames) {
            Instance a;
            ref[f] = a.render(lk.json, f);
            mine[lk.name + " | " + std::to_string(f)] = fnv(ref[f]);
        }
        auto check = [&](const std::string &order, int f, const std::vector<float> &got) {
            CAPTURE(order);
            CAPTURE(f);
            const size_t off = values_off(ref.at(f), got);
            size_t per[4] = {0, 0, 0, 0};
            float worst = 0;
            for (size_t i = 0; i < got.size(); ++i)
                if (got[i] != ref.at(f)[i]) {
                    ++per[i % 4];
                    worst = std::max(worst, std::fabs(got[i] - ref.at(f)[i]));
                }
            CHECK_MESSAGE(off == 0, lk.name << ", frame " << f << " " << order << ": " << off << " of "
                                            << got.size() << " values differ from a fresh instance's (R " << per[0]
                                            << " G " << per[1] << " B " << per[2] << " A " << per[3]
                                            << ", at most " << worst * 255 << "/255)");
        };
        Instance live;  // one instance through every order, as a host's long-lived effect
        for (int f : kFrames) {
            const int other = f == 1850 ? 0 : 1850;
            live.render(lk.json, other);
            check("after another frame", f, live.render(lk.json, f));
            check("repeated (2nd)", f, live.render(lk.json, f));
            check("repeated (3rd)", f, live.render(lk.json, f));
            live.render(lk.json, f + 2);
            live.render(lk.json, f + 1);
            check("backwards (f+2, f+1, f)", f, live.render(lk.json, f));
            if (f >= 2) {
                live.render(lk.json, f - 2);
                live.render(lk.json, f - 1);
                check("forwards (f-2, f-1, f)", f, live.render(lk.json, f));
            }
        }
        // Two instances taking turns (chunks of a render queue, or two copies of the effect).
        {
            Instance a, b;
            for (int f : kFrames) {
                if (f >= 1) a.render(lk.json, f - 1);
                check("interleaved (instance B)", f, b.render(lk.json, f));
                check("interleaved (instance A)", f, a.render(lk.json, f));
                b.render(lk.json, f + 1);
                check("interleaved (A again)", f, a.render(lk.json, f));
            }
        }
    }

    // Across processes (and shared against unshared chains).
    const std::string path = hash_file();
    const char *sh = std::getenv("NTSCRT_SHARED_CHAINS");
    const bool unshared = sh && *sh == '0';
    if (path.empty()) {
        MESSAGE("NTSCRT_ORDER_HASHES not set: no comparison with another process");
    } else if (!unshared) {
        std::ofstream o(path, std::ios::trunc);
        for (const auto &[k, h] : mine) o << h << " " << k << "\n";
    } else {
        std::ifstream in(path);
        REQUIRE_MESSAGE(in.good(), "no hashes from the shared-chains run at " << path);
        std::map<std::string, uint64_t> theirs;
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            uint64_t h = 0;
            ls >> h;
            std::string k;
            std::getline(ls >> std::ws, k);
            theirs[k] = h;
        }
        CHECK(theirs.size() == mine.size());
        for (const auto &[k, h] : mine) {
            CAPTURE(k);
            CHECK_MESSAGE(theirs.count(k), "missing in the other process's hashes");
            if (theirs.count(k)) CHECK_MESSAGE(theirs[k] == h, "differs from the other process (shared chains)");
        }
    }
}
