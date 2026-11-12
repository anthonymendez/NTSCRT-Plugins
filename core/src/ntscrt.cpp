// ntscrt.cpp — C ABI façade. Wave 0: CPU stages wired, GPU stages wired but
// skipped when no gpu::Device is available (stub backend). Wave 2 finishes
// the Metal entry point, caching and Howlaround ordering.
#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "../stages/digital/fields.h"
#include "../stages/digital/parallel.h"
#include "../stages/crop/crop_internal.h"
#include "../stages/ntsc/gpu/ntsc_gpu.h"
#include "../stages/stages.h"
#include "embedded_schema.h"
#include "facade.h"
#include "host_gpu.h"
#include "pixel_convert.h"
#include "testing.h"

using namespace ntscrt;

namespace {

// Parsed look JSON by content: the host sends the same string for every frame
// of an unanimated look, and the same strings again for history frames.
struct LookStore {
    struct Entry {
        std::string text;
        std::shared_ptr<const json> j;
        uint64_t used = 0;
    };
    static constexpr size_t kMax = 64;
    std::unordered_map<size_t, Entry> m;
    uint64_t tick = 0;
    size_t parses = 0;

    // nullptr if `s` isn't valid JSON.
    std::shared_ptr<const json> get(std::string_view s) {
        const size_t h = std::hash<std::string_view>{}(s);
        auto it = m.find(h);
        if (it != m.end() && it->second.text == s) {
            it->second.used = ++tick;
            return it->second.j;
        }
        ++parses;
        json j = json::parse(s, nullptr, false);
        if (j.is_discarded()) return nullptr;
        auto p = std::make_shared<const json>(std::move(j));
        if (it == m.end() && m.size() >= kMax) {
            auto lru = std::min_element(m.begin(), m.end(),
                                        [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
            m.erase(lru);
        }
        m[h] = Entry{std::string(s), p, ++tick};
        return p;
    }
};

// Converted + cropped source frames, keyed by everything that shapes them (so
// a proxy switch or an alpha / crop edit never reuses a wrong picture), LRU
// within a byte budget.
struct SourceKey {
    int64_t frame;
    double scale;
    int width, height;  // the host picture's size, before the crop
    int mode, premult;
    size_t crop;  // hash of that frame's look["crop"]
    bool operator<(const SourceKey &o) const {
        return std::tie(frame, scale, width, height, mode, premult, crop) <
               std::tie(o.frame, o.scale, o.width, o.height, o.mode, o.premult, o.crop);
    }
};

size_t default_source_budget() {
    if (const char *e = std::getenv("NTSCRT_SOURCE_CACHE_MB"); e && *e) {
        char *end = nullptr;
        double mb = std::strtod(e, &end);
        if (end != e && mb >= 0) return size_t(mb * 1024 * 1024);
    }
    return size_t(512) << 20;  // ~60 frames at 1080p, ~15 at UHD
}

struct SourceCache {
    static constexpr size_t kMaxFrames = 65;  // the furthest any stage looks back, plus this frame
    static constexpr size_t kMinFrames = 8;   // kept even past the budget (huge frames)
    struct Entry {
        std::shared_ptr<const ImageBGRA8> img;
        uint64_t used = 0;
    };
    std::map<SourceKey, Entry> m;
    size_t bytes = 0, budget = default_source_budget();
    uint64_t tick = 0;

    std::shared_ptr<const ImageBGRA8> get(const SourceKey &k) {
        auto it = m.find(k);
        if (it == m.end()) return nullptr;
        it->second.used = ++tick;
        return it->second.img;
    }
    void put(const SourceKey &k, std::shared_ptr<const ImageBGRA8> img) {
        Entry &e = m[k];
        if (e.img) bytes -= e.img->px.size();
        bytes += img->px.size();
        e.img = std::move(img);
        e.used = ++tick;
        evict();
    }
    void evict() {
        while (m.size() > kMaxFrames || (bytes > budget && m.size() > kMinFrames)) {
            auto lru = std::min_element(m.begin(), m.end(),
                                        [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
            bytes -= lru->second.img->px.size();
            m.erase(lru);
        }
    }
    void clear() {
        m.clear();
        bytes = 0;
    }
};

size_t crop_hash(const json &look) {
    auto c = look.find("crop");
    return c == look.end() ? 0 : std::hash<std::string>{}(c->dump());
}

}  // namespace

struct ntscrt_instance {
    std::string resource_dir;
    ntscrt_gpu_backend backend = NTSCRT_GPU_AUTO;
    void *native_device = nullptr, *native_queue = nullptr;  // the host's, when shared
    // Our own device is the process-wide shared one (gpu::Device::shared):
    // kernels compile once per process, and one D3D11 device / Metal queue
    // serves every instance. A host device (native_device) is this instance's.
    std::shared_ptr<gpu::Device> dev;
    DigitalStage digital;
    NtscStage ntsc;
    NtscStage howl_ntsc[2];  // Screen Loop history passes on ntsc-rs, two at a time on worker threads (render_once)
    std::unique_ptr<NtscGpu> ntsc_gpu;  // GPU port of the NTSC stage (NTSC-GPU.md; NTSCRT_NTSC_GPU)
    NtscGpuPlan ntsc_plan;              // its plan of the frame (one-entry cache: the same frame again reuses it)
    NtscGpuPlan howl_plan;              // ... of Screen Loop's history passes
    // Whether this instance's NTSC frames take the GPU route (ntsc_gpu_on): decided on
    // the first NTSC frame, again when NTSCRT_NTSC_GPU changes or the device is replaced.
    struct NtscRoute {
        bool decided = false, on = false;
        std::string env, why;  // the variable's value it was decided for; the reason either way
        int failures = 0;      // GPU errors in a row (not a lost device)
        bool disabled = false; // too many: ntsc-rs for this instance from then on
    } ntsc_route;
    std::unique_ptr<Downscaler> downscale;
    std::unique_ptr<GlitchStage> glitch;
    std::unique_ptr<CrtStage> crt;
    std::unique_ptr<CrtStage> howl_crt;  // Screen Loop's own CRT chain (upstream HowlaroundFeedback)
    std::unique_ptr<Howlaround> howl;
    std::vector<std::shared_ptr<gpu::Texture>> tex_pool;  // upload / output textures, reused across frames
    // GPU results read back for the host, kept between frames (no 8-33 MB
    // allocation and clear per frame): BGRA8, or RGBA16F for 16-bit / float hosts.
    ImageBGRA8 result8;
    std::vector<uint16_t> result16;
    std::string last_error;
    std::string gpu_gone;  // set when the GPU was lost and couldn't be replaced: CPU stages from then on
    int device_resets = 0;  // devices recreated since the last good GPU frame
    bool test_lose_device = false, test_recreate_fails = false;
    int test_ntsc_gpu_errors = 0;  // testing::simulate_ntsc_gpu_errors
    std::string presets_cache;
    SourceCache sources;
    LookStore looks;
    std::vector<uint8_t> spare_px;  // last frame's picture buffers, reused (fast_transfer)
    std::vector<float> spare_alpha;
    // Host GPU frames (host_gpu.cpp): the bridge to the host's GPU memory, made
    // on the first such render.
    std::unique_ptr<hostgpu::Bridge> bridge;
    PhaseTimes times;  // where the last render call's time went (ntscrt_last_timings, testing::last_phases)
};

namespace {
ntscrt_status fail(ntscrt_instance *i, ntscrt_status s, std::string msg) {
    if (i) i->last_error = std::move(msg);
    return s;
}
gpu::Backend pick_backend(ntscrt_gpu_backend b) {
    if (b == NTSCRT_GPU_D3D11) return gpu::Backend::D3D11;
    if (b == NTSCRT_GPU_METAL) return gpu::Backend::Metal;
#ifdef _WIN32
    return gpu::Backend::D3D11;
#else
    return gpu::Backend::Metal;
#endif
}

void drop_gpu(ntscrt_instance *inst) {
    // The stages and pooled textures belong to the device: they go first.
    inst->bridge.reset();
    inst->tex_pool.clear();
    inst->howl.reset();
    inst->howl_crt.reset();
    inst->crt.reset();
    inst->glitch.reset();
    inst->downscale.reset();
    inst->ntsc_gpu.reset();
    inst->ntsc_route = {};
    inst->dev.reset();
}

// Creates the device and the GPU stages; false (and why) when there's no GPU.
bool init_gpu(ntscrt_instance *inst, std::string &err) {
    try {
        const gpu::Backend b = pick_backend(inst->backend);
        if (inst->native_device) inst->dev = gpu::Device::create(b, inst->native_device, inst->native_queue);
        else inst->dev = gpu::Device::shared(b);
    } catch (const std::exception &e) {
        err = e.what();
    }
    if (!inst->dev) {
        if (err.empty()) err = std::string("GPU unavailable: ") + gpu::Device::last_create_error();
        return false;
    }
    inst->downscale = std::make_unique<Downscaler>(*inst->dev);
    inst->glitch = std::make_unique<GlitchStage>(*inst->dev);
    inst->crt = std::make_unique<CrtStage>(*inst->dev, inst->resource_dir);
    inst->howl_crt = std::make_unique<CrtStage>(*inst->dev, inst->resource_dir);
    inst->howl = std::make_unique<Howlaround>(*inst->dev);
    inst->ntsc_gpu = std::make_unique<NtscGpu>(*inst->dev);
    return true;
}

std::atomic<int> &live_instances() {
    static std::atomic<int> n{0};
    return n;
}

bool device_lost(const ntscrt_instance *inst) {
    return inst->dev && (inst->test_lose_device || inst->dev->lost());
}

// After a device loss: one new device of our own per loss, at most 3 in a row
// without a good frame in between. The host's device can't be replaced here.
bool recreate_gpu(ntscrt_instance *inst, std::string &err) {
    if (inst->native_device) {
        err = "the host's GPU device was lost";
        return false;
    }
    if (inst->device_resets >= 3) {
        err = "the GPU device keeps getting lost";
        return false;
    }
    ++inst->device_resets;
    drop_gpu(inst);
    inst->test_lose_device = false;
    if (inst->test_recreate_fails) {
        err = "simulated: no new device";
        return false;
    }
    return init_gpu(inst, err);
}
}  // namespace

namespace ntscrt::testing {
Stats stats(ntscrt_instance *inst) {
    Stats s;
    if (!inst) return s;
    s.source_frames = inst->sources.m.size();
    s.source_bytes = inst->sources.bytes;
    s.source_budget = inst->sources.budget;
    s.look_parses = inst->looks.parses;
    s.device_resets = inst->device_resets;
    s.has_gpu = inst->dev != nullptr;
    s.gpu_device = inst->dev ? inst->dev->native_device() : nullptr;
    return s;
}
void set_source_budget(ntscrt_instance *inst, size_t bytes) {
    inst->sources.budget = bytes;
    inst->sources.evict();
}
void simulate_ntsc_gpu_errors(ntscrt_instance *inst, int n) { inst->test_ntsc_gpu_errors = n; }
void simulate_device_lost(ntscrt_instance *inst, bool recreate_fails) {
    inst->test_lose_device = true;
    inst->test_recreate_fails = recreate_fails;
}
PhaseTimes last_phases(ntscrt_instance *inst) {
    PhaseTimes p;
    if (!inst) return p;
    const auto &t = inst->times;
    using P = ntscrt::PhaseTimes;
    p.convert_in = t.ms[P::In];
    p.cpu_stages = t.ms[P::Digital] + t.ms[P::Ntsc];
    p.ntsc = t.ms[P::Ntsc];
    p.upload = t.ms[P::Upload];
    p.gpu_stages = t.ms[P::Downscale] + t.ms[P::Glitch] + t.ms[P::Crt] + t.ms[P::Loop];
    p.readback = t.ms[P::Readback];
    p.convert_out = t.ms[P::Out];
    p.total = t.total_ms;
    return p;
}
}  // namespace ntscrt::testing

extern "C" {

uint32_t ntscrt_abi_version(void) { return NTSCRT_ABI_VERSION; }

const char *ntscrt_param_schema_json(void) { return ntscrt_embedded_schema(); }

ntscrt_status ntscrt_create(const ntscrt_create_info *info, ntscrt_instance **out) {
    if (!info || !out || info->abi_version != NTSCRT_ABI_VERSION) return NTSCRT_ERR_INVALID_ARG;
    auto inst = std::make_unique<ntscrt_instance>();
    inst->resource_dir = info->resource_dir ? info->resource_dir : "";
    inst->backend = info->backend;
    inst->native_device = info->native_device;
    inst->native_queue = info->native_queue;
    std::string err;
    if (!init_gpu(inst.get(), err)) inst->last_error = err;
    *out = inst.release();
    live_instances().fetch_add(1);
    return NTSCRT_OK;
}

void ntscrt_destroy(ntscrt_instance *inst) {
    if (!inst) return;
    delete inst;  // drops its reference to the shared device (freed with the last one)
    // The last instance: join the Digital stage's worker threads now, from
    // ordinary code, rather than ever in a static destructor (DLL unload).
    if (live_instances().fetch_sub(1) == 1) digital::shutdown_threads();
}

const char *ntscrt_last_error(ntscrt_instance *inst) { return inst ? inst->last_error.c_str() : ""; }

void ntscrt_flush_caches_mask(ntscrt_instance *inst, uint32_t which) {
    if (!inst) return;
    if (which & NTSCRT_CACHE_SOURCE) inst->sources.clear();
    if (which & NTSCRT_CACHE_DIGITAL) inst->digital.flush();
    if ((which & NTSCRT_CACHE_GLITCH) && inst->glitch) inst->glitch->flush();
    if ((which & NTSCRT_CACHE_CRT) && inst->crt) inst->crt->flush();
    if (which & NTSCRT_CACHE_SCREENLOOP) {
        if (inst->howl_crt) inst->howl_crt->flush();
        if (inst->howl) inst->howl->flush();
    }
}

void ntscrt_flush_caches(ntscrt_instance *inst) { ntscrt_flush_caches_mask(inst, NTSCRT_CACHE_ALL); }

const char *ntscrt_bundled_presets_json(ntscrt_instance *inst) {
    if (!inst) return "[]";
    if (!inst->presets_cache.empty()) return inst->presets_cache.c_str();
    json arr = json::array();
    namespace fs = std::filesystem;
    fs::path dir = fs::u8path(inst->resource_dir) / "presets";
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        std::ifstream f(it->path());
        json j = json::parse(f, nullptr, false);
        if (j.is_discarded()) continue;
        auto rel = fs::relative(it->path(), dir);
        rel.replace_extension();
        bool fb = j.is_object() && j.value("kind", std::string()) == "ntscrt-video-feedback";
        auto u8 = rel.generic_u8string();  // std::u8string in C++20; nlohmann would emit it as a number array
        std::string name(reinterpret_cast<const char *>(u8.data()), u8.size());
        arr.push_back({{"name", name}, {"kind", fb ? "feedback" : "look"}, {"json", j}});
    }
    std::sort(arr.begin(), arr.end(), [](const json &a, const json &b) { return a["name"] < b["name"]; });
    inst->presets_cache = arr.dump();
    return inst->presets_cache.c_str();
}

}  // extern "C"

namespace {
// Shared frame renderer behind ntscrt_render_cpu and ntscrt_render_metal.
// get_src: fill `raw` (BGRA8, opaque, prepared for `mode`: to_bgra8) and
//          `alpha` (source alpha per pixel, empty = opaque).
// emit:    deliver the result: `out_gpu` (BGRA8, out_w x out_h) when a GPU
//          device exists, else `cpu_img` (CPU stages only, out_w x out_h).
//          `alpha` is NULL when it doesn't line up with the result (crop on).
//          raw_w x raw_h: the source picture's size.
// direct:  optional, the source straight on the GPU (facade::DirectSrc).
using facade::DirectSrc;
using facade::EmitFn;
using facade::SourceFn;

// Looks at other frames during one render call. Entries are dropped oldest
// frame first past kMax, so a reference from look_at stays valid for at least
// kMax further lookups (the stages only read it on the spot).
constexpr size_t kLookHistMax = 256;

// The CRT shader runs for this look (CrtStage: look["shader"]["enabled"], default on).
bool crt_shader_on(const json &look) {
    auto sh = look.find("shader");
    if (sh == look.end() || !sh->is_object()) return true;
    auto en = sh->find("enabled");
    return en == sh->end() || !en->is_boolean() || en->get<bool>();
}

// The CRT output format for a host picture: RGBA16F for 16-bit and float
// hosts (the shader's last pass, supersampling and placement keep their
// precision instead of being quantized to 1/255), BGRA8 for 8-bit ones.
// Everything before the CRT stage stays 8-bit: that's where the look is defined.
}  // namespace
gpu::Format facade::output_format(bool high_precision_host) {
    static const bool force8 = [] {
        const char *e = std::getenv("NTSCRT_OUTPUT_8BIT");
        return e && *e && *e != '0';
    }();
    return high_precision_host && !force8 ? gpu::Format::RGBA16F : gpu::Format::BGRA8;
}
// NTSCRT_FAST_TRANSFER=0: the 0.5.0-rc.3 transfer path (A/B testing): upload
// from the CPU picture with upload(), read back into a buffer with download()
// and convert from there, new picture buffers every frame.
bool facade::fast_transfer() {
    static const bool on = [] {
        const char *e = std::getenv("NTSCRT_FAST_TRANSFER");
        return !(e && *e == '0');
    }();
    return on;
}

// Readback bands (gpu::Device::read): about 16 MB each, at most 8, so the
// transfer of the next band overlaps the conversion of this one. (Measured on
// PCIe 5: 1-4 bands are as fast as each other, more get slower: smaller
// conversion jobs on the thread pool. On slower links the overlap helps more.)
// NTSCRT_READBACK_BANDS=N sets the count (1 = no overlap).
int facade::readback_bands(size_t bytes) {
    static const int forced = [] {
        const char *e = std::getenv("NTSCRT_READBACK_BANDS");
        return e && *e ? std::atoi(e) : 0;
    }();
    if (forced > 0) return forced;
    return int(std::clamp<size_t>(bytes / (size_t(16) << 20), 1, 8));
}

namespace {
using facade::fast_transfer;
using facade::for_row_blocks;
using facade::output_format;
using facade::readback_bands;


// ---- NTSC on the GPU (docs/dev/NTSC-GPU.md) ----
// GPU errors (not a lost device) in a row after which an instance stays on ntsc-rs.
constexpr int kNtscGpuMaxFailures = 3;

// Whether this instance's NTSC frames may take the GPU route now; `why` gets the
// reason either way (ntsc_gpu_route's, or the failures that turned it off).
bool ntsc_gpu_on(ntscrt_instance *inst, gpu::Device &dev, std::string &why) {
    auto &r = inst->ntsc_route;
    const std::string env = ntsc_gpu_env_text();
    if (!r.decided || r.env != env) {
        r = {};
        r.decided = true;
        r.env = env;
        r.on = ntsc_gpu_route(dev, &r.why, inst->ntsc_gpu.get());
    }
    why = r.why;
    return r.on && !r.disabled;
}

// A GPU error on the NTSC route: this frame takes ntsc-rs; after
// kNtscGpuMaxFailures in a row, every frame of the instance does.
void ntsc_gpu_failed(ntscrt_instance *inst, const std::string &what, std::string &why) {
    auto &r = inst->ntsc_route;
    why = "GPU error: " + what;
    if (++r.failures >= kNtscGpuMaxFailures && !r.disabled) {
        r.disabled = true;
        r.why = "GPU error " + std::to_string(r.failures) + " frames in a row (last: " + what +
                "): ntsc-rs for this instance from now on";
        why = r.why;
    }
}

// The NTSC stage on the GPU for a w x h picture: the plan, its tables (on another
// thread while `picture` makes the input, when `async_tables`), the passes. The
// result (BGRA8), or nullptr when the frame must take ntsc-rs instead (`why`).
// A lost device throws (render_impl replaces it).
std::shared_ptr<gpu::Texture> ntsc_gpu_run(ntscrt_instance *inst, NtscGpuPlan &plan, const FrameContext &c, int w,
                                           int h, const std::function<std::shared_ptr<gpu::Texture>()> &picture,
                                           bool async_tables, std::string &why) {
    try {
        inst->ntsc.gpu_plan(c, w, h, plan, 0xffffffffu, false);
    } catch (const std::exception &e) {
        why = e.what();  // bad settings: ntsc-rs reports them
        return nullptr;
    }
    if (!plan.supported()) {
        why = "not on the GPU yet: " + plan.unsupported_names();
        return nullptr;
    }
    std::future<void> tables;
    try {
        if (!plan.tables_ready && async_tables)
            tables = std::async(std::launch::async, [inst, &plan] { inst->ntsc.gpu_tables(plan); });
        else
            inst->ntsc.gpu_tables(plan);  // (nothing to do when the cache kept them)
        std::shared_ptr<gpu::Texture> in = picture();
        if (tables.valid()) tables.get();
        if (inst->test_ntsc_gpu_errors > 0) {
            --inst->test_ntsc_gpu_errors;
            throw std::runtime_error("simulated NTSC GPU error");
        }
        if (!in) throw std::runtime_error("no input picture");
        auto out = inst->ntsc_gpu->run(*in, plan);
        inst->ntsc_route.failures = 0;
        return out;
    } catch (const std::exception &e) {
        if (tables.valid()) tables.wait();
        plan.key_valid = false;  // not a plan to keep
        if (device_lost(inst)) throw;
        ntsc_gpu_failed(inst, e.what(), why);
        return nullptr;
    }
}

// The timings note of the NTSC route (ntscrt_last_timings, the OpenFX debug log):
// " ntsc_route=gpu (why)" / " ntsc_route=cpu (why)"; with the GPU, the CPU time of
// the plan and tables and, with NTSCRT_PROFILE=1, the GPU time (total, per pass).
void note_ntsc_route(ntscrt_instance *inst, PhaseTimes &pt, bool gpu, const std::string &why) {
    pt.note += std::string(" ntsc_route=") + (gpu ? "gpu" : "cpu") + " (" + why + ")";
    if (!gpu) return;
    char b[64];
    std::snprintf(b, sizeof b, " ntsc_tables_ms=%.3f%s", inst->ntsc_plan.table_ms, inst->ntsc_plan.cached ? " ntsc_cached=1" : "");
    pt.note += b;
    const auto &times = inst->ntsc_gpu->pass_times();
    if (times.empty()) return;
    double total = 0;
    for (const auto &t : times) total += t.second;
    std::snprintf(b, sizeof b, " ntsc_gpu_ms=%.3f", total);
    pt.note += b;
    pt.note += " ntsc_gpu=";
    for (size_t i = 0; i < times.size(); ++i) {
        std::snprintf(b, sizeof b, "%s%s:%.3f", i ? "," : "", times[i].first.c_str(), times[i].second);
        pt.note += b;
    }
}

// out_fmt: output_format() for the host picture; used when the CRT shader is
// on (its bypass is an 8-bit picture anyway, kept exactly as before).
ntscrt_status render_once(ntscrt_instance *inst, const ntscrt_render_args *args,
                          const std::shared_ptr<const json> &look_p, const json &feedback, int out_w, int out_h,
                          gpu::Format out_fmt, const SourceFn &get_src, const EmitFn &emit,
                          const DirectSrc *direct_src) {
    const json &look = *look_p;
    PhaseTimes &pt = inst->times;
    const std::string_view look_text = args->look_json ? args->look_json : "{}";
    const AlphaMode mode = alpha_mode(look);
    const bool premult = args->source_premultiplied != 0;

    std::map<int64_t, std::shared_ptr<const json>> look_hist;
    FrameContext ctx;
    ctx.frame = args->frame_index;
    ctx.fps = args->fps > 0 ? args->fps : 30.0;
    ctx.render_scale = args->render_scale > 0 ? args->render_scale : 1.0;
    ctx.seed = args->seed;
    ctx.look = &look;
    // NULL from the host, or the current look's own text: this frame's look,
    // nothing parsed. Other strings are parsed once per distinct content.
    auto look_ptr = [&](int64_t f) -> std::shared_ptr<const json> {
        if (f == args->frame_index || !args->look_at) return look_p;
        auto it = look_hist.find(f);
        if (it != look_hist.end()) return it->second;
        const char *s = args->look_at(args->user, f);
        std::shared_ptr<const json> j = look_p;
        if (s && look_text != s) {
            j = inst->looks.get(s);
            if (!j) j = look_p;
        }
        if (look_hist.size() >= kLookHistMax) look_hist.erase(look_hist.begin());
        return look_hist.emplace(f, std::move(j)).first->second;
    };
    ctx.look_at = [&](int64_t f) -> const json & { return *look_ptr(f); };

    int raw_w = 0, raw_h = 0;  // this frame's source size (host frames of a clip share it)
    ctx.source_at = [&](int64_t f) -> std::shared_ptr<const ImageBGRA8> {
        std::shared_ptr<const json> lf = look_ptr(f);
        const SourceKey key{f, ctx.render_scale, raw_w, raw_h, int(mode), int(premult), crop_hash(*lf)};
        if (auto p = inst->sources.get(key)) return p;
        if (!args->fetch_source) return nullptr;
        const ntscrt_image *im = args->fetch_source(args->user, f);
        if (!im) return nullptr;
        ImageBGRA8 raw, cropped;
        {
            PhaseScope ps(pt, PhaseTimes::In);
            to_bgra8(*im, raw, nullptr, mode, premult);
        }
        if (args->release_source) args->release_source(args->user, im);
        Crop::apply(*lf, std::move(raw), cropped);
        auto p = std::make_shared<const ImageBGRA8>(std::move(cropped));
        inst->sources.put(key, p);
        return p;
    };

    // This frame's pictures reuse the last frame's buffers (fast_transfer):
    // no 8-33 MB allocation, page faults and free per frame.
    std::vector<float> alpha;
    ImageBGRA8 img;
    struct Keep {  // hands the buffers back for the next frame, however this returns
        ntscrt_instance *inst;
        ImageBGRA8 &img;
        std::vector<float> &alpha;
        ~Keep() {
            if (!fast_transfer()) return;
            if (img.px.capacity() > inst->spare_px.capacity()) inst->spare_px = std::move(img.px);
            if (alpha.capacity() > inst->spare_alpha.capacity()) inst->spare_alpha = std::move(alpha);
        }
    } keep{inst, img, alpha};
    if (fast_transfer()) alpha = std::move(inst->spare_alpha);
    // NTSC on the GPU (docs/dev/NTSC-GPU.md: by default on D3D11 hardware devices),
    // when this instance's route is on; else (or when a frame can't) ntsc-rs.
    const bool ntsc_on = inst->ntsc.active(look);
    std::string ntsc_why;
    const bool ntsc_gpu = ntsc_on && inst->dev && !inst->test_lose_device && ntsc_gpu_on(inst, *inst->dev, ntsc_why);
    // The source goes straight onto the GPU (direct_src->make, below) when no
    // CPU stage needs the picture (NTSC on the GPU needs none).
    const bool direct = direct_src && inst->dev && !inst->test_lose_device &&
                        !inst->howl->active(args->feedback_json ? &feedback : nullptr) &&
                        !inst->digital.active(look) && (!ntsc_on || ntsc_gpu) &&
                        (direct_src->crops || !Crop::cuts(look, direct_src->w, direct_src->h));
    auto convert_source = [&] {
        ImageBGRA8 raw;
        if (fast_transfer()) raw.px = std::move(inst->spare_px);
        {
            PhaseScope ps(pt, PhaseTimes::In);
            get_src(raw, alpha, mode);
        }
        raw_w = raw.width;
        raw_h = raw.height;
        Crop::apply(look, std::move(raw), img);  // no crop: moved, not copied
        if (fast_transfer() && raw.px.capacity() > inst->spare_px.capacity()) inst->spare_px = std::move(raw.px);
    };
    bool have_cpu_pic = !direct;  // img holds the picture (else it's made on the GPU)
    if (direct) {
        // No pixels in img (only read on the CPU-only path); its size becomes the
        // direct picture's (the crop's) once made.
        raw_w = img.width = direct_src->w;
        raw_h = img.height = direct_src->h;
    } else {
        convert_source();
    }
    // The source alpha, when it lines up with the result (no crop).
    auto aligned_alpha = [&]() -> const std::vector<float> * {
        return img.width == raw_w && img.height == raw_h ? &alpha : nullptr;
    };

    if (!inst->dev) {
        // No GPU: CPU stages only (degraded, but never a black frame).
        if (inst->digital.active(look)) {
            PhaseScope ps(pt, PhaseTimes::Digital);
            inst->digital.process(img, ctx);
            pt.note += inst->digital.profile_note();
        }
        if (inst->ntsc.active(look)) {
            PhaseScope ps(pt, PhaseTimes::Ntsc);
            inst->ntsc.process(img, ctx);
        }
        PhaseScope ps(pt, PhaseTimes::Out);
        if (img.width != out_w || img.height != out_h) {
            // Cropped: placed like the CRT bypass would, so all of dst is written.
            ImageBGRA8 boxed;
            letterbox(img, out_w, out_h, boxed);
            emit(nullptr, boxed, raw_w, raw_h, nullptr, mode);
        } else {
            emit(nullptr, img, raw_w, raw_h, aligned_alpha(), mode);
        }
        if (!inst->gpu_gone.empty()) return fail(inst, NTSCRT_ERR_GPU, inst->gpu_gone);
        return NTSCRT_OK;
    }
    if (inst->test_lose_device) throw std::runtime_error("simulated device removal");
    gpu::Device &dev = *inst->dev;
    const bool howl_on = inst->howl->active(args->feedback_json ? &feedback : nullptr);

    // Textures reused across frames once nothing holds them (GPU work runs in
    // submission order on both backends, so overwriting a released one is safe).
    auto acquire = [&](const gpu::TextureDesc &d) {
        for (auto &t : inst->tex_pool) {
            const gpu::TextureDesc &e = t->desc();
            if (t.use_count() == 1 && e.width == d.width && e.height == d.height && e.format == d.format &&
                e.shader_write == d.shader_write && e.render_target == d.render_target)
                return t;
        }
        auto t = dev.create_texture(d);
        if (inst->tex_pool.size() >= 8) inst->tex_pool.erase(inst->tex_pool.begin());
        inst->tex_pool.push_back(t);
        return t;
    };

    // Upstream order (Mp4Exporter.exportStill / video path):
    //   no Screen Loop:  crop -> Digital -> NTSC -> upload -> Downscale -> Glitch -> CRT
    //   Screen Loop on:  crop -> upload -> camera -> download -> NTSC -> upload -> Downscale
    //                    -> Glitch -> CRT; Digital is OFF whenever Screen Loop is on.
    auto upload = [&](const ImageBGRA8 &im) {
        PhaseScope ps(pt, PhaseTimes::Upload, &dev);
        auto t = acquire({im.width, im.height, gpu::Format::BGRA8, false, false});  // only ever read
        if (fast_transfer()) {
            // Straight into the GPU's upload memory, rows copied on the thread pool.
            const size_t tight = size_t(im.width) * 4;
            dev.write(*t, [&](uint8_t *rows, ptrdiff_t pitch) {
                for_row_blocks(im.height, tight, [&](int y) {
                    std::memcpy(rows + ptrdiff_t(y) * pitch, im.row(y), tight);
                });
            });
        } else {
            dev.upload(*t, im.px.data(), ptrdiff_t(im.width) * 4);
        }
        return t;
    };
    auto download = [&](const gpu::Texture &t) {
        PhaseScope ps(pt, PhaseTimes::Readback);
        ImageBGRA8 im;
        im.resize(t.desc().width, t.desc().height);
        dev.download(t, im.px.data(), ptrdiff_t(im.width) * 4);  // waits for the work before it
        return im;
    };

    std::shared_ptr<gpu::Texture> chain;
    if (howl_on) {
        // Context whose stage counters (internal.h helpers) yield the pass's
        // counters; `pin` keeps the pass's look alive while the context is used.
        auto at = [&](int64_t counter_frame, const std::shared_ptr<const json> &pin) {
            FrameContext c = ctx;
            c.frame = counter_frame;
            c.look = pin.get();
            return c;
        };
        // The pass after NTSC: upload, Downscale, Glitch (on this thread, in frame order).
        auto gpu_half = [&](const ImageBGRA8 &im, const std::shared_ptr<const json> &pin, const HowlPass &p) {
            FrameContext c = at(p.frame, pin);
            auto ch = inst->downscale->run(upload(im), c);
            FrameContext cg = c;
            cg.frame = int64_t(std::llround(p.glitch_time * ctx.fps));  // glitch_time(cg)
            if (inst->glitch->active(*c.look)) ch = inst->glitch->run(ch, cg);
            return ch;
        };
        // NTSC of a loop pass on the GPU, on the camera picture itself (no download,
        // no ntsc-rs, no upload; bit-identical to them): the NTSC result, or
        // nullptr when the pass must take ntsc-rs (`loop_why`).
        int loop_gpu = 0, loop_cpu = 0;  // passes per route in this call (timings)
        std::string loop_why;
        auto loop_ntsc_gpu = [&](const std::shared_ptr<gpu::Texture> &cam, const FrameContext &cn) {
            if (!ntsc_gpu_on(inst, dev, loop_why)) return std::shared_ptr<gpu::Texture>();
            PhaseScope ps(pt, PhaseTimes::Ntsc, &dev);
            return ntsc_gpu_run(inst, inst->howl_plan, cn, cam->desc().width, cam->desc().height,
                                [&] { return cam; }, false, loop_why);
        };
        // Downscale and Glitch of a loop pass whose NTSC ran on the GPU (gpu_half without the upload).
        auto gpu_rest = [&](const std::shared_ptr<gpu::Texture> &ntsc_out, const std::shared_ptr<const json> &pin,
                            const HowlPass &p) {
            FrameContext c = at(p.frame, pin);
            auto ch = inst->downscale->run(ntsc_out, c);
            FrameContext cg = c;
            cg.frame = int64_t(std::llround(p.glitch_time * ctx.fps));  // glitch_time(cg)
            if (inst->glitch->active(*c.look)) ch = inst->glitch->run(ch, cg);
            return ch;
        };
        HowlHooks h;
        h.chain_input = [&](const std::shared_ptr<gpu::Texture> &cam, const HowlPass &p) {
            const auto pin = look_ptr(p.frame);
            FrameContext cn = at(p.ntsc_frame - 1, pin);  // ntsc_frame(cn) == p.ntsc_frame
            const bool on = inst->ntsc.active(*pin);
            if (on) {
                if (auto g = loop_ntsc_gpu(cam, cn)) {
                    ++loop_gpu;
                    return gpu_rest(g, pin, p);
                }
                ++loop_cpu;
            }
            ImageBGRA8 im = download(*cam);
            if (on) {
                PhaseScope ps(pt, PhaseTimes::Ntsc);
                inst->ntsc.process(im, cn);
            }
            return gpu_half(im, pin, p);
        };
        // The loop's history passes, pipelined: NTSC of a pass runs on a
        // worker (each of the two in flight on its own NtscStage) while the
        // loop exposes the next cameras; ntsc-rs's result depends only on its
        // settings, frame and seed, and the GPU half runs here in frame order,
        // so the pictures are chain_input's. On the GPU route the pass's NTSC is
        // queued on the GPU at once (reading the camera) and the rest is the second half.
        struct LoopJob {
            ImageBGRA8 im;
            std::shared_ptr<const json> pin;
            std::shared_ptr<gpu::Texture> gpu;  // the NTSC result, GPU route
            std::future<void> cpu;  // last member: destroyed (waited for) first
        };
        constexpr int kJobs = 2;  // NTSC passes running at once (Howlaround allows 2 in flight)
        std::shared_ptr<LoopJob> jobs[kJobs];
        int64_t job_count = 0;
        h.chain_input_begin = [&](const std::shared_ptr<gpu::Texture> &cam,
                                  const HowlPass &p) -> std::function<std::shared_ptr<gpu::Texture>()> {
            auto job = std::make_shared<LoopJob>();
            job->pin = look_ptr(p.frame);
            const bool on = inst->ntsc.active(*job->pin);
            if (on) {
                job->gpu = loop_ntsc_gpu(cam, at(p.ntsc_frame - 1, job->pin));
                if (job->gpu) ++loop_gpu;
                else ++loop_cpu;
            }
            if (job->gpu) {
                return [&gpu_rest, job, p] { return gpu_rest(job->gpu, job->pin, p); };
            }
            job->im = download(*cam);
            auto &slot = jobs[job_count % kJobs];
            if (slot && slot->cpu.valid()) slot->cpu.wait();
            if (on) {
                FrameContext cn;  // what NtscStage reads, nothing that points into this call
                cn.frame = p.ntsc_frame - 1;
                cn.fps = ctx.fps;
                cn.render_scale = ctx.render_scale;
                cn.seed = ctx.seed;
                cn.look = job->pin.get();
                LoopJob *j = job.get();  // owns the future, so outlives the task
                NtscStage *ntsc = &inst->howl_ntsc[job_count % kJobs];
                j->cpu = std::async(std::launch::async, [j, ntsc, cn] { ntsc->process(j->im, cn); });
            }
            slot = job;
            ++job_count;
            return [&gpu_half, job, p] {
                if (job->cpu.valid()) job->cpu.get();  // rethrows the worker's failure
                return gpu_half(job->im, job->pin, p);
            };
        };
        h.tv_keeps_state = [inst] { return inst->howl_crt->ran_temporal_preset(); };
        h.render_tv = [&](const gpu::Texture &ch, gpu::Texture &tv, const HowlPass &p) {
            const auto pin = look_ptr(p.frame);
            inst->howl_crt->render(ch, tv, at(p.shader_frame - 1, pin));  // shader_frame == p.shader_frame
        };
        inst->howl->set_hooks(std::move(h));  // lambdas capture this call's locals: re-set every render

        std::shared_ptr<gpu::Texture> cam;
        {
            // The loop's own uploads, readbacks and NTSC passes count as theirs.
            PhaseScope ps(pt, PhaseTimes::Loop, &dev);
            cam = inst->howl->camera(upload(img), feedback, ctx);
        }
        std::shared_ptr<gpu::Texture> up;
        if (ntsc_gpu) {
            PhaseScope ps(pt, PhaseTimes::Ntsc, &dev);
            up = ntsc_gpu_run(inst, inst->ntsc_plan, ctx, cam->desc().width, cam->desc().height,
                              [&] { return cam; }, false, ntsc_why);
        }
        if (ntsc_on) note_ntsc_route(inst, pt, up != nullptr, ntsc_why);
        if (loop_gpu + loop_cpu > 0) {
            pt.note += " ntsc_loop=gpu:" + std::to_string(loop_gpu) + ",cpu:" + std::to_string(loop_cpu);
            if (loop_cpu && !loop_why.empty()) pt.note += " (" + loop_why + ")";
        }
        if (!up) {
            ImageBGRA8 im = download(*cam);
            if (ntsc_on) {
                PhaseScope ps(pt, PhaseTimes::Ntsc);
                inst->ntsc.process(im, ctx);
            }
            up = upload(im);
        }
        cam.reset();
        {
            PhaseScope ps(pt, PhaseTimes::Downscale, &dev);
            chain = inst->downscale->run(up, ctx);
        }
        up.reset();
        if (inst->glitch->active(look)) {
            PhaseScope ps(pt, PhaseTimes::Glitch, &dev);
            chain = inst->glitch->run(chain, ctx);
        }
        PhaseScope ps(pt, PhaseTimes::Loop, &dev);
        inst->howl->after_frame(*chain, feedback, ctx);  // feeds the loop the chain input
    } else {
        if (inst->digital.active(look)) {
            PhaseScope ps(pt, PhaseTimes::Digital);
            inst->digital.process(img, ctx);
            pt.note += inst->digital.profile_note();
        }
        ctx.line_height = inst->digital.active(look) ? digital::chain_line_height(look) : 1;  // One field
        // The pipeline's 8-bit picture on the GPU: made there from the source
        // (direct), or the CPU picture uploaded.
        bool use_direct = direct;
        auto picture = [&]() -> std::shared_ptr<gpu::Texture> {
            std::shared_ptr<gpu::Texture> t;
            if (use_direct) {
                PhaseScope ps(pt, PhaseTimes::In, &dev);
                t = direct_src->make(dev, mode, look, alpha, facade::PoolFn(acquire));
                if (t) {
                    img.width = t->desc().width;
                    img.height = t->desc().height;
                }
            }
            if (!t) {
                if (!have_cpu_pic) {  // needs the hole filling: the usual way
                    convert_source();
                    have_cpu_pic = true;
                }
                t = upload(img);
            }
            return t;
        };
        std::shared_ptr<gpu::Texture> pic;
        if (ntsc_gpu) {
            // NTSC on the GPU: the plan's tables (CPU: seeds, shifts, speckles) are
            // built while the picture is made / uploads; then the passes run on it.
            PhaseScope ps(pt, PhaseTimes::Ntsc, &dev);
            int w = img.width, h = img.height;  // direct: the source's size; cut to the crop's
            if (use_direct && direct_src->crops)
                if (auto c = crop::from_look(look)) {
                    const crop::Rect r = c->rect(w, h);
                    w = r.width;
                    h = r.height;
                }
            pic = ntsc_gpu_run(inst, inst->ntsc_plan, ctx, w, h, picture, true, ntsc_why);
        }
        if (ntsc_on) {
            note_ntsc_route(inst, pt, pic != nullptr, ntsc_why);
            if (!pic) {
                // ntsc-rs, on the CPU picture, which then goes up (not the direct one).
                if (!have_cpu_pic) {
                    convert_source();
                    have_cpu_pic = true;
                }
                use_direct = false;
                PhaseScope ps(pt, PhaseTimes::Ntsc);
                inst->ntsc.process(img, ctx);
            }
        }
        if (!pic) pic = picture();
        {
            PhaseScope ps(pt, PhaseTimes::Downscale, &dev);
            chain = inst->downscale->run(pic, ctx);
        }
        pic.reset();
        if (inst->glitch->active(look)) {
            PhaseScope ps(pt, PhaseTimes::Glitch, &dev);
            chain = inst->glitch->run(chain, ctx);
        }
    }
    const gpu::Format fmt = crt_shader_on(look) ? out_fmt : gpu::Format::BGRA8;
    auto out = acquire({out_w, out_h, fmt, true, true});
    {
        PhaseScope ps(pt, PhaseTimes::Crt, &dev);
        inst->crt->render(*chain, *out, ctx);
    }
    chain.reset();
    // No finish() here: emit's download (or the Metal path's own finish) waits.
    {
        PhaseScope ps(pt, PhaseTimes::Out);
        emit(out, img, raw_w, raw_h, aligned_alpha(), mode);
    }
    inst->device_resets = 0;  // a good GPU frame

    // The CRT shader couldn't run: the frame shows the picture without it.
    const CrtStage *bad = inst->crt->fallback_status() ? inst->crt.get()
                          : howl_on && inst->howl_crt->fallback_status() ? inst->howl_crt.get()
                                                                         : nullptr;
    if (bad) return fail(inst, ntscrt_status(bad->fallback_status()), bad->fallback_reason());
    return NTSCRT_OK;
}

ntscrt_status render_impl(ntscrt_instance *inst, const ntscrt_render_args *args, int out_w, int out_h,
                          gpu::Format out_fmt, const SourceFn &get_src, const EmitFn &emit,
                          const DirectSrc *direct_src = nullptr) {
    inst->last_error.clear();
    std::shared_ptr<const json> look = inst->looks.get(args->look_json ? args->look_json : "{}");
    if (!look) return fail(inst, NTSCRT_ERR_BAD_PARAMS, "look_json is not valid JSON");
    json feedback;
    if (args->feedback_json) {
        feedback = json::parse(args->feedback_json, nullptr, false);
        if (feedback.is_discarded()) return fail(inst, NTSCRT_ERR_BAD_PARAMS, "feedback_json is not valid JSON");
    }
    bool recreated = false;
    for (;;) {
        try {
            return render_once(inst, args, look, feedback, out_w, out_h, out_fmt, get_src, emit, direct_src);
        } catch (const std::exception &e) {
            if (!device_lost(inst)) return fail(inst, NTSCRT_ERR_INTERNAL, e.what());
            // The device is gone (driver reset, GPU removed): try a new one once,
            // else carry on with the CPU stages. Either way, render this frame again.
            const std::string why = e.what();
            std::string err;
            if (!recreated && recreate_gpu(inst, err)) {
                recreated = true;
                continue;
            }
            drop_gpu(inst);
            inst->gpu_gone = "GPU device lost (" + why + (err.empty() ? "" : "; " + err) +
                             "): rendering without the GPU stages (downscale, glitch, CRT)";
        }
    }
}

}  // namespace

ntscrt_status facade::render(ntscrt_instance *inst, const ntscrt_render_args *args, int out_w, int out_h,
                             gpu::Format out_fmt, const SourceFn &get_src, const EmitFn &emit,
                             const DirectSrc *direct) {
    return render_impl(inst, args, out_w, out_h, out_fmt, get_src, emit, direct);
}
ntscrt_status facade::fail(ntscrt_instance *inst, ntscrt_status s, std::string msg) {
    return ::fail(inst, s, std::move(msg));
}
gpu::Device *facade::device(ntscrt_instance *inst) { return inst->dev.get(); }
std::unique_ptr<hostgpu::Bridge> &facade::bridge(ntscrt_instance *inst) { return inst->bridge; }
PhaseTimes &facade::times(ntscrt_instance *inst) { return inst->times; }

extern "C" {

const char *ntscrt_last_timings(ntscrt_instance *inst) { return inst ? inst->times.text.c_str() : ""; }

ntscrt_status ntscrt_render_cpu(ntscrt_instance *inst, const ntscrt_render_args *args, const ntscrt_image *src,
                                ntscrt_image *dst) {
    if (!inst || !args || !src || !dst || !src->data || !dst->data) return NTSCRT_ERR_INVALID_ARG;
    const bool premult = args->source_premultiplied != 0;
    const bool high = dst->format == NTSCRT_RGBA16 || dst->format == NTSCRT_RGBA16F || dst->format == NTSCRT_RGBA32F;
    PhaseTimes &pt = inst->times;
    pt.clear("cpu");
    struct Done {  // the phases, however render_impl returns
        PhaseTimes &t;
        ~Done() { t.finish(); }
    } done{pt};
    // The host picture converted straight into upload memory (not with
    // NTSCRT_FAST_TRANSFER=0: the CPU picture is always made then).
    const DirectSrc direct{src->width, src->height, false,
                           [&](gpu::Device &dev, AlphaMode mode, const json &, std::vector<float> &alpha,
                               const facade::PoolFn &pool) -> std::shared_ptr<gpu::Texture> {
                               auto t = pool({src->width, src->height, gpu::Format::BGRA8, false, false});
                               bool ok = true;
                               dev.write(*t, [&](uint8_t *rows, ptrdiff_t pitch) {
                                   ok = to_bgra8_rows(*src, rows, pitch, &alpha, mode, premult);
                               });
                               return ok ? t : nullptr;
                           }};
    return render_impl(
        inst, args, dst->width, dst->height, output_format(high),
        [&](ImageBGRA8 &raw, std::vector<float> &alpha, AlphaMode mode) {
            to_bgra8(*src, raw, &alpha, mode, premult);
        },
        [&](const std::shared_ptr<gpu::Texture> &out, const ImageBGRA8 &cpu_img, int raw_w, int raw_h,
            const std::vector<float> *alpha, AlphaMode mode) {
            if (!out) {
                from_bgra8(cpu_img, alpha, *dst, mode, premult);
                return;
            }
            const bool same = dst->width == raw_w && dst->height == raw_h;
            const std::vector<float> *a = same ? alpha : nullptr;
            if (fast_transfer()) {
                // Converted band by band straight from the readback memory
                // (no copy of the result), the next bands' transfer meanwhile.
                const bool f16 = out->desc().format == gpu::Format::RGBA16F;
                const int w = out->desc().width;
                PhaseScope ps(pt, PhaseTimes::Readback);
                inst->dev->read(
                    *out,
                    [&](const uint8_t *rows, ptrdiff_t pitch, int y0, int y1) {
                        PhaseScope pc(pt, PhaseTimes::Out);
                        if (f16) from_rgba16f_rows(rows, pitch, w, y0, y1, a, *dst, mode, premult);
                        else from_bgra8_rows(rows, pitch, w, y0, y1, a, *dst, mode, premult);
                    },
                    readback_bands(size_t(w) * out->desc().height * (f16 ? 8 : 4)));
                return;
            }
            if (out->desc().format == gpu::Format::RGBA16F) {
                std::vector<uint16_t> &px = inst->result16;
                px.resize(size_t(dst->width) * dst->height * 4);  // fully overwritten by the download
                {
                    PhaseScope ps(pt, PhaseTimes::Readback);
                    inst->dev->download(*out, px.data(), ptrdiff_t(dst->width) * 8);
                }
                from_rgba16f(px.data(), dst->width, dst->height, same ? alpha : nullptr, *dst, mode, premult);
                return;
            }
            ImageBGRA8 &result = inst->result8;
            result.width = dst->width;
            result.height = dst->height;
            result.px.resize(size_t(dst->width) * dst->height * 4);  // fully overwritten by the download
            {
                PhaseScope ps(pt, PhaseTimes::Readback);
                inst->dev->download(*out, result.px.data(), ptrdiff_t(dst->width) * 4);
            }
            from_bgra8(result, same ? alpha : nullptr, *dst, mode, premult);
        },
        fast_transfer() ? &direct : nullptr);
}

// Metal path (FxPlug): host textures in and out, on the device given at create.
// The CPU stages (Digital, ntsc-rs) still need the frame on the CPU, so the
// source is read back once; everything after runs on the GPU, and the result
// is written into the host texture by a kernel (or a blit when the host
// texture isn't shader-writable) encoded on the host's command buffer.
ntscrt_status ntscrt_render_metal(ntscrt_instance *inst, const ntscrt_render_args *args, void *src_texture,
                                  void *dst_texture, void *command_buffer) {
    if (!inst || !args || !src_texture || !dst_texture) return NTSCRT_ERR_INVALID_ARG;
    if (!inst->dev || inst->dev->backend() != gpu::Backend::Metal)
        return fail(inst, NTSCRT_ERR_GPU, "ntscrt_render_metal: instance has no Metal device");
    gpu::Device &dev = *inst->dev;
    std::shared_ptr<gpu::Texture> src, dst;
    try {
        src = dev.wrap_native_texture(src_texture);
        dst = dev.wrap_native_texture(dst_texture);
    } catch (const std::exception &e) {
        return fail(inst, NTSCRT_ERR_INVALID_ARG, e.what());
    }
    const int sw = src->desc().width, sh = src->desc().height;
    const int dw = dst->desc().width, dh = dst->desc().height;

    const bool premult = args->source_premultiplied != 0;
    auto get_src = [&](ImageBGRA8 &raw, std::vector<float> &alpha, AlphaMode mode) {
        // Normalise any host format to RGBA32F on the GPU, read back once.
        auto f32 = dev.create_texture({sw, sh, gpu::Format::RGBA32F, true, false});
        dev.copy(*src, *f32);
        if (fast_transfer()) {  // converted straight from the readback memory (one band: all rows)
            dev.read(*f32, [&](const uint8_t *rows, ptrdiff_t pitch, int, int) {
                ntscrt_image im{sw, sh, pitch, NTSCRT_RGBA32F, const_cast<uint8_t *>(rows)};
                to_bgra8(im, raw, &alpha, mode, premult);
            });
            return;
        }
        std::vector<float> px(size_t(sw) * sh * 4);
        dev.download(*f32, px.data(), ptrdiff_t(sw) * 16);
        ntscrt_image im{sw, sh, ptrdiff_t(sw) * 16, NTSCRT_RGBA32F, px.data()};
        to_bgra8(im, raw, &alpha, mode, premult);
    };
    auto emit = [&](const std::shared_ptr<gpu::Texture> &out, const ImageBGRA8 &, int raw_w, int,
                    const std::vector<float> *alpha, AlphaMode mode) {
        // render_impl only calls emit with a texture here (the device exists).
        // Layout matches facade.slang MergeParams.
        struct { uint32_t w, h, use_alpha, mode, premult, pad[3]; } p{
            uint32_t(dw), uint32_t(dh), uint32_t(alpha && sw == dw && sh == dh && raw_w == sw),
            uint32_t(mode == AlphaMode::Spill), uint32_t(premult), {0, 0, 0}};
        auto merge = [&](gpu::Texture &target) {
            gpu::Bindings b;
            b.constants = &p;
            b.constants_size = sizeof p;
            b.textures = {out.get(), p.use_alpha ? src.get() : out.get()};
            b.out_textures = {&target};
            dev.dispatch(dev.kernel("facade/alpha_merge"), b, dw, dh, 1);
        };
        std::shared_ptr<gpu::Texture> staged;
        if (!dst->desc().shader_write) {
            // Merge into an internal texture of the destination's format, then blit.
            staged = dev.create_texture({dw, dh, dst->desc().format, true, false});
            merge(*staged);
        }
        dev.finish();  // our work completes before the host's buffer runs (it may be another queue)
        if (command_buffer) dev.set_external_command_buffer(command_buffer);
        try {
            if (staged) dev.copy(*staged, *dst);  // same format: blit
            else merge(*dst);
        } catch (...) {
            if (command_buffer) dev.set_external_command_buffer(nullptr);
            throw;
        }
        if (command_buffer) dev.set_external_command_buffer(nullptr);
        else dev.finish();
    };
    const gpu::Format df = dst->desc().format;
    inst->times.clear("metal-texture");
    struct Done {
        ntscrt_instance *i;
        ~Done() { i->times.finish(); }
    } done{inst};
    return render_impl(inst, args, dw, dh, output_format(df == gpu::Format::RGBA16F || df == gpu::Format::RGBA32F),
                       get_src, emit);
}

}  // extern "C"
