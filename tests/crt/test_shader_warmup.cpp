// The shader cache warm-up (core/stages/crt/shader_warm.h, core/src/warmup.cpp):
// - ntscrt_warm_shader_cache into an empty folder, then renders of every preset that
//   compile nothing (the installer's warm-up, in one process; test_ofx_after_warmup
//   checks the plug-in binary after the real ntscrt-warm tool, across processes);
// - nothing harmful without librashader or with a folder that can't be written;
// - the background warm-up: once per process, only between renders, stopped by the
//   last ntscrt_destroy, and the pictures stay byte for byte the same.
//
// One process configures the shader cache once (its first chain), so each case runs in
// a process of its own (tests/crt/CMakeLists.txt: NTSCRT_TEST_WARMUP selects it), in a
// temporary folder of its own.
#include <ntscrt/ntscrt.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../gpu/gpu_test_util.h"
#include "src/testing.h"
#include "stages/crt/crt_libra.h"
#include "stages/crt/shader_cache.h"
#include "stages/stages.h"

using namespace gputest;
namespace fs = std::filesystem;

namespace {

bool mode(const char *m) {
    const char *e = std::getenv("NTSCRT_TEST_WARMUP");
    return e && std::string(e) == m;
}

std::string u8(const fs::path &p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// A new empty folder as this process's cache, before its first chain.
fs::path fresh_cache(const char *tag) {
    const fs::path root = fs::temp_directory_path() /
                          (std::string("ntscrt-warmup-test-") + tag + "-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(root);
    set_env("NTSCRT_SHADER_CACHE", u8(root).c_str());
    return root;
}

size_t bin_files(const fs::path &root) {
    size_t n = 0;
    std::error_code ec;
    for (auto &e : fs::recursive_directory_iterator(root, ec)) n += e.path().extension() == ".bin";
    return n;
}

struct Lines {
    std::mutex m;
    std::vector<std::string> v;
    static void add(void *user, const char *line) {
        auto *l = static_cast<Lines *>(user);
        std::lock_guard<std::mutex> lock(l->m);
        l->v.emplace_back(line);
    }
    size_t built() {  // lines of a built preset ("royale: 412 ms (cache found ...")
        std::lock_guard<std::mutex> lock(m);
        size_t n = 0;
        for (auto &s : v) n += s.find(" ms (cache found ") != std::string::npos;
        return n;
    }
    std::string all() {
        std::lock_guard<std::mutex> lock(m);
        std::string s;
        for (auto &l : v) s += l + "\n";
        return s;
    }
};

// Every bundled preset (what a warm-up builds). Rendered: all of them but on GitHub's macOS
// runners, whose paravirtual GPU hangs on Royale and Sim (test_shader_cache.cpp).
const std::vector<std::string> &preset_ids() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v;
        for (auto &[id, rel] : crt_bundled_chain_files()) v.push_back(id);
        return v;
    }();
    return ids;
}
const std::vector<std::string> &rendered_ids() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v;
#ifdef __APPLE__
        const char *gha = std::getenv("GITHUB_ACTIONS");
        const bool vm = gha && std::string(gha) == "true";
#else
        const bool vm = false;
#endif
        for (const std::string &id : preset_ids())
            if (!vm || (id != "royale" && id != "sim")) v.push_back(id);
        return v;
    }();
    return ids;
}

uint64_t misses() {
    std::string err;
    crt::Library *lib = crt::Library::load(NTSCRT_RESOURCES_DIR, err);
    REQUIRE_MESSAGE(lib, err);
    uint64_t h, m, w;
    lib->shader_cache_stats(h, m, w);
    return m;
}

// A small test card through a whole instance with one CRT preset.
constexpr int W = 160, H = 120;
std::vector<uint8_t> card() {
    std::vector<uint8_t> px(size_t(W) * H * 4, 255);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t *p = &px[(size_t(y) * W + x) * 4];
            p[0] = uint8_t(x * 255 / (W - 1));
            p[1] = uint8_t(y * 255 / (H - 1));
            p[2] = uint8_t(((x / 8 + y / 8) & 1) ? 230 : 20);
        }
    return px;
}
ntscrt_instance *make_instance() {
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_RESOURCES_DIR};
    ntscrt_instance *inst = nullptr;
    REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
    return inst;
}
// Empty on failure (callable from other threads: no doctest macros).
std::vector<uint8_t> render(ntscrt_instance *inst, const std::string &preset, int frame = 5) {
    static const std::vector<uint8_t> src_px = card();
    const std::string look = R"({"shader":{"enabled":true,"preset":")" + preset + R"("}})";
    ntscrt_render_args a{};
    a.frame_index = frame;
    a.fps = 30;
    a.look_json = look.c_str();
    a.render_scale = 1;
    ntscrt_image src{W, H, ptrdiff_t(W) * 4, NTSCRT_RGBA8, const_cast<uint8_t *>(src_px.data())};
    std::vector<uint8_t> out(size_t(W) * H * 4);
    ntscrt_image dst{W, H, ptrdiff_t(W) * 4, NTSCRT_RGBA8, out.data()};
    if (ntscrt_render_cpu(inst, &a, &src, &dst) != NTSCRT_OK) return {};
    return out;
}

}  // namespace

TEST_CASE("shader warm-up into an empty folder: then every preset renders without compiling" *
          doctest::skip(!mode("fresh"))) {
    if (!device()) return;
    const fs::path root = fresh_cache("fresh");
    Lines lines;
    ntscrt_warm_result r{};
    const ntscrt_status st = ntscrt_warm_shader_cache(NTSCRT_RESOURCES_DIR, &Lines::add, &lines, &r);
    MESSAGE(lines.all());
    REQUIRE(st == NTSCRT_OK);
    CHECK(r.cache_on == 1);
    CHECK(r.presets == int(preset_ids().size()));
    CHECK(r.built == r.presets);
    CHECK(r.failed == 0);
    CHECK(r.compiled > 10);
    CHECK(lines.built() == preset_ids().size());
    const size_t objects = bin_files(root);
    CHECK(objects > 10);
    std::string err;
    crt::Library *lib = crt::Library::load(NTSCRT_RESOURCES_DIR, err);
    REQUIRE(lib);
    auto d = device();
    const crt::ShaderCacheState &cache = crt::configure_shader_cache(*lib, *d);
    REQUIRE(cache.on);
    CHECK(fs::u8path(cache.dir).parent_path() == root);
    std::string list;
    for (auto &[id, rel] : crt_bundled_chain_files()) list += (list.empty() ? "" : " ") + rel;
    CHECK(crt::shader_cache_warm_complete(fs::u8path(cache.dir), list));
    CHECK(!crt::shader_cache_warm_complete(fs::u8path(cache.dir), list + " x"));

    // Renders: new chains, every shader object from the cache.
    const uint64_t m0 = misses();
    ntscrt_instance *inst = make_instance();
    for (const std::string &id : rendered_ids()) {
        CAPTURE(id);
        CHECK(!render(inst, id).empty());
        const std::string t = ntscrt_last_timings(inst);
        CHECK_MESSAGE(t.find("compiled 0)") != std::string::npos, t);
        CHECK(t.find("cache found 0 ") == std::string::npos);
    }
    ntscrt_destroy(inst);
    CHECK(misses() == m0);
    CHECK(bin_files(root) == objects);  // nothing new written

    // Again: everything found.
    ntscrt_warm_result r2{};
    REQUIRE(ntscrt_warm_shader_cache(NTSCRT_RESOURCES_DIR, nullptr, nullptr, &r2) == NTSCRT_OK);
    CHECK(r2.compiled == 0);
    CHECK(r2.found >= objects);
    fs::remove_all(root);
}

TEST_CASE("shader warm-up without librashader, or into a folder that can't be written: nothing done, no harm" *
          doctest::skip(!mode("unusable"))) {
    if (!device()) return;
    // No librashader (a resource folder without it; nothing loaded it in this process yet).
    const fs::path empty = fs::temp_directory_path() / ("ntscrt-warmup-nolib-" + std::to_string(std::rand()));
    fs::create_directories(empty);
    const fs::path blocker = empty / "not-a-folder";
    std::ofstream(blocker) << "a file where the cache folder would be";
    set_env("NTSCRT_SHADER_CACHE", u8(blocker / "cache").c_str());
    Lines lines;
    ntscrt_warm_result r{};
    ntscrt_status st = ntscrt_warm_shader_cache(u8(empty).c_str(), &Lines::add, &lines, &r);
    MESSAGE(lines.all());
    const bool lib_found_elsewhere = st == NTSCRT_OK;  // e.g. librashader next to the test binary
    if (!lib_found_elsewhere) {
        CHECK(st == NTSCRT_ERR_MISSING_RESOURCE);
        CHECK(r.built == 0);
        CHECK(lines.all().find("librashader") != std::string::npos);
    }
    // The cache folder can't be made (a file is in the way): the cache is off, nothing built.
    lines.v.clear();
    st = ntscrt_warm_shader_cache(NTSCRT_RESOURCES_DIR, &Lines::add, &lines, &r);
    MESSAGE(lines.all());
    CHECK(st == NTSCRT_OK);
    CHECK(r.cache_on == 0);
    CHECK(r.built == 0);
    CHECK(r.compiled == 0);
    CHECK(lines.all().find("shader cache off: can't") != std::string::npos);
    std::ifstream f(blocker);
    std::string text;
    std::getline(f, text);
    CHECK(text == "a file where the cache folder would be");  // untouched
    f.close();
    // The background warm-up has nothing to do either, and renders still work.
    set_env("NTSCRT_SHADER_WARMUP", "1");
    set_env("NTSCRT_SHADER_WARMUP_DELAY_MS", "0");
    ntscrt_instance *inst = make_instance();
    ntscrt_warm_shader_cache_background(inst, nullptr, nullptr);
    CHECK(ntscrt::testing::wait_warmup(30000));
    const auto s = ntscrt::testing::warmup_state();
    CHECK(s.runs == 1);
    CHECK(s.completed == 1);
    CHECK(s.built == 0);
    CHECK(s.why.find("shader cache off") != std::string::npos);
    CHECK(!render(inst, "glow_gauss").empty());
    ntscrt_destroy(inst);
    fs::remove_all(empty);
}

TEST_CASE("background shader warm-up: once per process, only between renders, same pictures" *
          doctest::skip(!mode("background"))) {
    // ctest sets NTSCRT_SHARED_CHAINS=0: every instance builds its own chains.
    if (!device()) return;
    const fs::path root = fresh_cache("background");
    set_env("NTSCRT_SHADER_WARMUP", "1");
    set_env("NTSCRT_SHADER_WARMUP_DELAY_MS", "0");
    set_env("NTSCRT_SHADER_WARMUP_IDLE_MS", "400");
    const auto &ids = rendered_ids();
    const size_t all = preset_ids().size();

    // Reference pictures, before any warm-up (each preset compiled by its render).
    std::map<std::string, std::vector<uint8_t>> ref;
    {
        ntscrt_instance *c = make_instance();
        for (const std::string &id : ids) {
            ref[id] = render(c, id);
            REQUIRE_MESSAGE(!ref[id].empty(), id);
        }
        ntscrt_destroy(c);
    }
    // An empty folder again (same key folder: the process's), so the warm-up compiles.
    for (auto &e : fs::recursive_directory_iterator(root))
        if (e.path().extension() == ".bin") fs::remove(e.path());

    // The warm-up starts; renders run back to back meanwhile.
    Lines lines;
    ntscrt_instance *a = make_instance();
    ntscrt_warm_shader_cache_background(a, &Lines::add, &lines);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let it begin a preset
    const size_t at_start = lines.built();
    size_t mismatches = 0, frames = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3) || frames < ids.size()) {
        const std::string &id = ids[frames % ids.size()];
        if (render(a, id) != ref[id]) ++mismatches;
        ++frames;
    }
    const size_t while_rendering = lines.built() - at_start;
    MESSAGE(frames << " frames while the warm-up waited; presets it finished meanwhile: " << while_rendering);
    CHECK(mismatches == 0);
    // At most the preset it had already begun: none started while renders ran.
    CHECK(while_rendering <= 1);
    CHECK(ntscrt::testing::warmup_state().running);

    // Renders stop: it finishes.
    REQUIRE(ntscrt::testing::wait_warmup(120000));
    MESSAGE(lines.all());
    auto s = ntscrt::testing::warmup_state();
    CHECK(s.runs == 1);
    CHECK(s.completed == 1);
    CHECK(s.built == int(all));
    CHECK(!s.stopped);
    CHECK(lines.all().find("presets ready") != std::string::npos);
    bool warm_txt = false;
    for (auto &e : fs::recursive_directory_iterator(root)) warm_txt = warm_txt || e.path().filename() == "warm.txt";
    CHECK(warm_txt);

    // Once per process: neither this instance nor a new one starts it again.
    ntscrt_warm_shader_cache_background(a, nullptr, nullptr);
    ntscrt_instance *b = make_instance();
    ntscrt_warm_shader_cache_background(b, nullptr, nullptr);
    s = ntscrt::testing::warmup_state();
    CHECK(s.runs == 1);
    CHECK(!s.running);

    // New chains from what it cached: no compiling, the same pictures.
    const uint64_t m0 = misses();
    for (const std::string &id : ids) {
        CAPTURE(id);
        CHECK(render(b, id) == ref[id]);
        CHECK(render(a, id, 7) == render(b, id, 7));
    }
    CHECK(misses() == m0);
    ntscrt_destroy(b);
    ntscrt_destroy(a);

    // A complete folder: a later process's warm-up finds warm.txt and does nothing.
    ntscrt::testing::forget_warmup();
    Lines again;
    ntscrt_instance *c = make_instance();
    ntscrt_warm_shader_cache_background(c, &Lines::add, &again);
    REQUIRE(ntscrt::testing::wait_warmup(30000));
    s = ntscrt::testing::warmup_state();
    CHECK(s.already_complete);
    CHECK(s.built == 0);
    CHECK(again.built() == 0);

    // From an empty folder with no renders: everything compiled on its own thread pool,
    // and chains built from that render the same pictures without compiling.
    ntscrt::testing::forget_warmup();
    for (auto &e : fs::recursive_directory_iterator(root))
        if (e.path().extension() == ".bin" || e.path().filename() == "warm.txt") fs::remove(e.path());
    set_env("NTSCRT_SHADER_WARMUP_IDLE_MS", "0");
    Lines cold;
    ntscrt_warm_shader_cache_background(c, &Lines::add, &cold);
    REQUIRE(ntscrt::testing::wait_warmup(120000));
    MESSAGE(cold.all());
    s = ntscrt::testing::warmup_state();
    CHECK(s.built == int(all));
    CHECK(cold.all().find(" compiled 0 (") == std::string::npos);  // the summary: some compiled
    CHECK(bin_files(root) > 10);
    const uint64_t m1 = misses();
    for (const std::string &id : ids) {
        CAPTURE(id);
        CHECK(render(c, id) == ref[id]);
    }
    CHECK(misses() == m1);

    // The last ntscrt_destroy stops a running warm-up and waits for it.
    ntscrt::testing::forget_warmup();
    for (auto &e : fs::recursive_directory_iterator(root))
        if (e.path().filename() == "warm.txt") fs::remove(e.path());
    ntscrt_warm_shader_cache_background(c, nullptr, nullptr);
    CHECK(ntscrt::testing::warmup_state().running);
    ntscrt_destroy(c);  // the last instance
    s = ntscrt::testing::warmup_state();
    CHECK(!s.running);
    CHECK(s.runs == 1);
    fs::remove_all(root);
}

TEST_CASE("NTSCRT_SHADER_WARMUP=0: no background warm-up" * doctest::skip(!mode("off"))) {
    if (!device()) return;
    const fs::path root = fresh_cache("off");
    set_env("NTSCRT_SHADER_WARMUP", "0");
    ntscrt_instance *a = make_instance();
    ntscrt_warm_shader_cache_background(a, nullptr, nullptr);
    CHECK(ntscrt::testing::warmup_state().runs == 0);
    ntscrt_destroy(a);
    CHECK(!fs::exists(root));  // nothing configured the cache
}
