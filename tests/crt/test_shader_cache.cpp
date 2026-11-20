// librashader's shader cache (core/stages/crt/shader_cache.h, rust/librashader-patch):
// pictures from cached shaders are the ones compiled from scratch, byte for byte; damaged
// cache files are ignored and replaced; concurrent builds share the folder; the folder's
// trimming only touches its own files; NTSCRT_SHADER_CACHE=0 caches nothing.
//
// One process configures the cache once (first chain), so each test run below gets its
// own folder: ctest runs test_shader_cache (a fresh temporary folder) and
// test_shader_cache_off (NTSCRT_SHADER_CACHE=0) as separate processes.
#include <ntscrt/ntscrt.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../gpu/gpu_test_util.h"
#include "stages/crt/crt_libra.h"
#include "stages/crt/shader_cache.h"
#include "stages/stages.h"

using namespace gputest;
namespace fs = std::filesystem;

namespace {

// GitHub's macOS runners are VMs whose paravirtualized GPU hangs on the heaviest shaders
// (test_crt.cpp): there the test leaves Royale and Sim out, and a hang is only a warning.
bool gha_macos() {
#ifdef __APPLE__
    const char *gha = std::getenv("GITHUB_ACTIONS");
    return gha && std::string(gha) == "true";
#else
    return false;
#endif
}
std::vector<const char *> preset_ids() {
    if (gha_macos()) return {"aperture", "easymode", "glow_gauss", "glow_lanczos", "hyllian"};
    return {"aperture", "easymode", "glow_gauss", "glow_lanczos", "hyllian", "royale", "sim"};
}

struct Counters {
    uint64_t hits = 0, misses = 0, writes = 0;
};
Counters counters() {
    std::string err;
    crt::Library *lib = crt::Library::load(NTSCRT_RESOURCES_DIR, err);
    REQUIRE_MESSAGE(lib, err);
    Counters c;
    lib->shader_cache_stats(c.hits, c.misses, c.writes);
    return c;
}

// A 160x120 test card (bars, ramps, a checkerboard) on the GPU.
std::shared_ptr<gpu::Texture> card(gpu::Device &d) {
    const int w = 160, h = 120;
    std::vector<uint8_t> px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(x * 255 / (w - 1));
            p[1] = uint8_t(y * 255 / (h - 1));
            p[2] = uint8_t(((x / 8 + y / 8) & 1) ? 230 : 20);
        }
    return bgra(d, w, h, px);
}

// One preset through a new CrtStage (a new chain: the stage before it is gone).
std::vector<uint8_t> render(gpu::Device &d, const gpu::Texture &in, const char *id) {
    CrtStage crt(d, NTSCRT_RESOURCES_DIR);
    json look = {{"shader", {{"enabled", true}, {"preset", id}, {"params", json::object()}}}};
    FrameContext ctx;
    ctx.frame = 10;
    ctx.look = &look;
    auto out = d.create_texture({640, 480, gpu::Format::BGRA8, true, true});
    crt.render(in, *out, ctx);
    if (crt.fallback_status()) FAIL(id << ": " << crt.fallback_reason());
    return read(d, *out);
}

std::vector<fs::path> cache_files(const fs::path &root) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto &e : fs::recursive_directory_iterator(root, ec))
        if (e.is_regular_file() && e.path().extension() == ".bin") out.push_back(e.path());
    return out;
}

void write_file(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

void age(const fs::path &p, int days) {
    fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::hours(24 * days));
}

bool there(const fs::path &p) { return fs::exists(p); }

}  // namespace

TEST_CASE("shader cache root and key folder") {
    std::string why;
    const char *was = std::getenv("NTSCRT_SHADER_CACHE");
    const std::string saved = was ? was : "";
    set_env("NTSCRT_SHADER_CACHE", "0");
    CHECK(crt::shader_cache_root(why).empty());
    CHECK(why == "NTSCRT_SHADER_CACHE=0");
    set_env("NTSCRT_SHADER_CACHE", "off");
    CHECK(crt::shader_cache_root(why).empty());
    set_env("NTSCRT_SHADER_CACHE", "C:/some/where");
    CHECK(crt::shader_cache_root(why) == fs::u8path("C:/some/where"));
    set_env("NTSCRT_SHADER_CACHE", "1");  // the default folder
    fs::path def = crt::shader_cache_root(why);
#ifdef _WIN32
    CHECK(def.filename() == "cache");
    CHECK(def.parent_path().filename() == "NTSCRT");
#elif defined(__APPLE__)
    CHECK(def.filename() == "NTSCRT");
    CHECK(def.parent_path().filename() == "Caches");
#endif
    set_env("NTSCRT_SHADER_CACHE", was ? saved.c_str() : nullptr);  // never the user's own folder later
    CHECK(crt::shader_cache_folder_name("a") != crt::shader_cache_folder_name("b"));
    CHECK(crt::shader_cache_folder_name("a").rfind("shaders-", 0) == 0);
}

TEST_CASE("shader cache trim: old folders, the size cap, nothing foreign") {
    const fs::path root = fs::temp_directory_path() / ("ntscrt-trim-test-" + std::to_string(std::rand()));
    fs::remove_all(root);
    const fs::path keep = root / "shaders-0000000000000001";
    const fs::path old = root / "shaders-0000000000000002";
    const fs::path recent = root / "shaders-0000000000000003";
    const std::string kb(1024, 'x');
    for (int i = 0; i < 4; ++i) write_file(keep / ("dxbc-" + std::to_string(i) + ".bin"), kb);
    write_file(keep / "key.txt", "k");
    write_file(old / "spirv-1.bin", kb);
    write_file(old / "key.txt", "o");
    write_file(old / "notes.doc", "someone else's");  // not ours: kept, so the folder stays
    write_file(recent / "spirv-1.bin", kb);
    write_file(root / "photo.bin", kb);  // outside the shaders-* folders: never touched
    write_file(keep / ".dxbc-9.bin.123-0.tmp", "half");
    write_file(keep / ".dxbc-8.bin.123-1.tmp", "half");
    for (auto &p : {old / "spirv-1.bin", old / "key.txt", old / "notes.doc", keep / ".dxbc-9.bin.123-0.tmp"}) age(p, 9);
    age(keep / "dxbc-0.bin", 3);  // the least recently used of `keep`
    age(keep / "dxbc-1.bin", 2);

    crt::trim_shader_cache(root, keep, 1 << 20);  // under the cap: only the stale parts go
    CHECK(!there(old / "spirv-1.bin"));
    CHECK(!there(old / "key.txt"));
    CHECK(there(old / "notes.doc"));
    CHECK(there(recent / "spirv-1.bin"));  // used 0 days ago
    CHECK(there(root / "photo.bin"));
    CHECK(!there(keep / ".dxbc-9.bin.123-0.tmp"));  // a dead writer's
    CHECK(there(keep / ".dxbc-8.bin.123-1.tmp"));   // maybe still being written
    for (int i = 0; i < 4; ++i) CHECK(there(keep / ("dxbc-" + std::to_string(i) + ".bin")));

    // 5 KB of *.bin over a 4 KB cap: oldest first down to 3 KB.
    crt::trim_shader_cache(root, keep, 4096);
    CHECK(!there(keep / "dxbc-0.bin"));
    CHECK(!there(keep / "dxbc-1.bin"));
    CHECK(there(keep / "dxbc-2.bin"));
    CHECK(there(keep / "dxbc-3.bin"));
    CHECK(there(recent / "spirv-1.bin"));
    CHECK(there(keep / "key.txt"));
    CHECK(there(root / "photo.bin"));

    crt::trim_shader_cache(root / "missing", keep, 1);  // no folder: nothing to do, no throw
    fs::remove_all(root);
}

TEST_CASE("shader cache: off" * doctest::skip(std::getenv("NTSCRT_TEST_SHADER_CACHE_OFF") == nullptr)) {
    // ctest: test_shader_cache_off (NTSCRT_SHADER_CACHE=0).
    auto d = device();
    if (!d) return;
    auto in = card(*d);
    render(*d, *in, "glow_gauss");
    std::string err;
    crt::Library *lib = crt::Library::load(NTSCRT_RESOURCES_DIR, err);
    REQUIRE(lib);
    const crt::ShaderCacheState &s = crt::configure_shader_cache(*lib, *d);
    CHECK(!s.on);
    CHECK(s.why == "NTSCRT_SHADER_CACHE=0");
    Counters c = counters();
    CHECK(c.hits + c.misses + c.writes == 0);
}


namespace {
std::string u8(const fs::path &p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
// render() for other threads: no doctest macros; empty on any failure.
std::vector<uint8_t> render_quiet(gpu::Device &d, const gpu::Texture &in, const char *id) {
    try {
        CrtStage crt(d, NTSCRT_RESOURCES_DIR);
        json look = {{"shader", {{"enabled", true}, {"preset", id}, {"params", json::object()}}}};
        FrameContext ctx;
        ctx.frame = 10;
        ctx.look = &look;
        auto out = d.create_texture({640, 480, gpu::Format::BGRA8, true, true});
        crt.render(in, *out, ctx);
        if (crt.fallback_status()) return {};
        d.finish();
        std::vector<uint8_t> px(size_t(640) * 480 * 4);
        d.download(*out, px.data(), 640 * 4);
        return px;
    } catch (...) {
        return {};
    }
}
}  // namespace

// One body, in order (the process's cache folder is chosen once, at its first chain).
void cache_case(gpu::Device &dev);

TEST_CASE("shader cache: cached shaders give the same pictures" *
          doctest::skip(std::getenv("NTSCRT_TEST_SHADER_CACHE_OFF") != nullptr)) {
    auto d = device();
    if (!d) return;
    try {
        cache_case(*d);
    } catch (const std::exception &e) {
        if (gha_macos() && std::string(e.what()).find("Hang") != std::string::npos) {
            MESSAGE("WARNING: the CI runner's virtual GPU hung: " << std::string(e.what()));
            return;
        }
        throw;
    }
}

void cache_case(gpu::Device &dev) {
    gpu::Device *d = &dev;
    const std::vector<const char *> presets = preset_ids();
    // Our own empty folder, before the process's first chain (configure_shader_cache).
    const fs::path root =
        fs::temp_directory_path() /
        ("ntscrt-shader-cache-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(root);
    set_env("NTSCRT_SHADER_CACHE", u8(root).c_str());
    auto in = card(*d);

    // Cold: everything compiled (and written); the reference pictures.
    std::map<std::string, std::vector<uint8_t>> ref;
    const Counters c0 = counters();
    for (const char *id : presets) ref[id] = render(*d, *in, id);
    const Counters c1 = counters();
    std::string err;
    crt::Library *lib = crt::Library::load(NTSCRT_RESOURCES_DIR, err);
    REQUIRE(lib);
    const crt::ShaderCacheState &state = crt::configure_shader_cache(*lib, *d);
    REQUIRE_MESSAGE(state.on, state.why);
    CHECK(fs::u8path(state.dir).parent_path() == root);
    CHECK(there(fs::u8path(state.dir) / "key.txt"));
    CHECK(c1.writes - c0.writes > 10);  // macOS: SPIR-V only (15 for 5 presets)
    CHECK(c1.misses - c0.misses > 10);
    const auto files = cache_files(root);
    CHECK(files.size() > 10);
    MESSAGE(files.size() << " cached objects, " << (c1.writes - c0.writes) << " writes");

    {
        INFO("warm: every object from the cache, same pictures");
        for (const char *id : presets) {
            CAPTURE(id);
            CHECK(render(*d, *in, id) == ref[id]);
        }
        const Counters c2 = counters();
        CHECK(c2.misses == c1.misses);  // nothing compiled
        CHECK(c2.hits - c1.hits >= files.size());
        CHECK(c2.writes == c1.writes);
    }

    {
        INFO("damaged files are compiled again and replaced");
        const Counters c1b = counters();
        int n = 0;
        for (const fs::path &f : files) {
            std::string data;
            {
                std::ifstream s(f, std::ios::binary);
                data.assign(std::istreambuf_iterator<char>(s), {});
            }
            switch (n++ % 4) {
            case 0: data.resize(data.size() / 2); break;      // cut short
            case 1: data[data.size() - 1] ^= 0x5a; break;     // one byte of the shader
            case 2: data = "not a cache file"; break;         // foreign
            default: data.replace(0, 8, "NTSCRTc9"); break;   // another format version
            }
            std::ofstream(f, std::ios::binary | std::ios::trunc) << data;
        }
        for (const char *id : presets) {
            CAPTURE(id);
            CHECK(render(*d, *in, id) == ref[id]);
        }
        const Counters c2 = counters();
        CHECK(c2.misses - c1b.misses >= files.size());
        CHECK(c2.writes - c1b.writes >= files.size());
        for (const char *id : presets) CHECK(render(*d, *in, id) == ref[id]);  // rewritten whole
        CHECK(counters().misses == c2.misses);
    }

    {
        INFO("concurrent first builds share the folder");
        // Several effects (threads) building the same presets from an empty folder at
        // once write the same files; renames over each other are fine.
        for (const fs::path &f : cache_files(root)) fs::remove(f);
        const std::vector<const char *> order = presets;
        std::atomic<int> bad{0};
        std::vector<std::thread> th;
        for (int t = 0; t < 4; ++t)
            th.emplace_back([&, t] {
                for (size_t k = 0; k < order.size(); ++k) {
                    const char *id = order[(k + size_t(t)) % order.size()];  // some threads compile while others read
                    if (render_quiet(*d, *in, id) != ref[id]) ++bad;
                }
            });
        for (auto &t : th) t.join();
        CHECK(bad == 0);
        size_t tmp = 0;
        std::error_code ec;
        for (auto &e : fs::recursive_directory_iterator(root, ec)) tmp += e.path().extension() == ".tmp";
        CHECK(tmp == 0);  // no temporary file left behind
        CHECK(cache_files(root).size() > 10);
    }

    {
        INFO("a folder that disappears is no failure");
        fs::remove_all(root);
        for (const char *id : {presets[0], presets[1]}) CHECK(render(*d, *in, id) == ref[id]);
        CHECK(!there(root));  // not created again behind the plug-in's back
    }
    fs::remove_all(root);
}
