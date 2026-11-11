// ntscrt_ofx.cpp — NTSCRT OpenFX image effect (DaVinci Resolve 21.1.2+).
//
// Params are generated at describe time from ntscrt_param_schema_json(); per
// frame the plugin evaluates them into the look JSON (docs/params.md) and calls
// ntscrt_render_cpu. Temporal stages pull other source frames / looks through
// the core's fetch_source / look_at callbacks.
#include <ntscrt/ntscrt.h>

#include "ofx_params.h"
#include "ofxsImageEffect.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>
#include <cfloat>
#include <climits>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <objbase.h>
#  include <shellapi.h>
#else
#  include <dlfcn.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  ifdef __APPLE__
#    include <crt_externs.h>
#  endif
extern char **environ;
#endif

using namespace ntscrt_ofx;

namespace {
const char kPluginId[] = "com.ntscrt.plugins.ntscrt";
const unsigned kOfxIdentityMajor = 1;
const char kVersionParam[] = "host_version";
const char kPresetParam[] = "host_preset";
const char kLoopPresetParam[] = "host_screenloop_preset";
const char kPresetFileParam[] = "host_preset_file";
const char kLoadParam[] = "host_load_preset";
const char kSaveParam[] = "host_save_preset";
const char kSeedParam[] = "host_seed";
const char kDebugParam[] = "host_debug_logging";
const char kOpenLogsParam[] = "host_open_logs";
const char kHostGroup[] = "grp_host_presets";
const char kNoPreset[] = "(none)";

std::string u8(const std::filesystem::path &p) {
    auto s = p.u8string();
    return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

// <bundle>/Contents/<arch>/NTSCRT.ofx -> <bundle>/Contents/Resources
std::string resource_dir() {
    namespace fs = std::filesystem;
    fs::path bin;
#ifdef _WIN32
    HMODULE h = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&resource_dir), &h)) {
        std::wstring buf(32768, L'\0');
        DWORD n = GetModuleFileNameW(h, buf.data(), DWORD(buf.size()));
        buf.resize(n);
        bin = fs::path(buf);
    }
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&resource_dir), &info) && info.dli_fname) bin = fs::u8path(info.dli_fname);
#endif
    if (bin.empty()) return std::string();
    return u8(bin.parent_path().parent_path() / "Resources");
}

// Schema + bundled presets, shared by describe and every instance (indices of
// the preset choice params must match between them).
struct Globals {
    Schema schema;
    std::vector<Preset> looks, loops;
    std::string res_dir;
};
const Globals &globals() {
    static Globals *g = [] {
        auto *gl = new Globals;
        gl->res_dir = resource_dir();
        gl->schema = Schema::parse(ntscrt_param_schema_json());
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, gl->res_dir.c_str()};
        ntscrt_instance *tmp = nullptr;
        if (ntscrt_create(&ci, &tmp) == NTSCRT_OK && tmp) {
            for (auto &p : parse_presets(ntscrt_bundled_presets_json(tmp)))
                (p.feedback ? gl->loops : gl->looks).push_back(std::move(p));
            ntscrt_destroy(tmp);
        }
        return gl;
    }();
    return *g;
}

ntscrt_pixel_format to_format(OFX::BitDepthEnum d) {
    switch (d) {
    case OFX::eBitDepthUByte: return NTSCRT_RGBA8;
    case OFX::eBitDepthUShort: return NTSCRT_RGBA16;
    case OFX::eBitDepthHalf: return NTSCRT_RGBA16F;
    case OFX::eBitDepthFloat: return NTSCRT_RGBA32F;
    default: return ntscrt_pixel_format(-1);
    }
}

// Wrap an OFX image (bottom-up rows) as an ntscrt_image covering `r`.
bool wrap(OFX::Image &im, const OfxRectI &r, ntscrt_image &out) {
    if (im.getPixelComponents() != OFX::ePixelComponentRGBA) return false;
    ntscrt_pixel_format f = to_format(im.getPixelDepth());
    if (int(f) < 0) return false;
    OfxRectI b = im.getBounds();
    if (r.x1 < b.x1 || r.y1 < b.y1 || r.x2 > b.x2 || r.y2 > b.y2 || r.x2 <= r.x1 || r.y2 <= r.y1) return false;
    out.width = r.x2 - r.x1;
    out.height = r.y2 - r.y1;
    out.format = f;
    out.row_bytes = -ptrdiff_t(im.getRowBytes());
    out.data = im.getPixelAddress(r.x1, r.y2 - 1);  // top row
    return out.data != nullptr;
}

size_t pixel_bytes(ntscrt_pixel_format f) {
    switch (f) {
    case NTSCRT_RGBA8:
    case NTSCRT_BGRA8: return 4;
    case NTSCRT_RGBA16:
    case NTSCRT_RGBA16F: return 8;
    default: return 16;
    }
}

// Like wrap, but `r` may extend past the image's bounds (Fusion hands over only
// the source's domain of definition, e.g. a Text+ title's bounding box): the
// image is then copied into `buf`, transparent everywhere outside its bounds.
bool wrap_padded(OFX::Image &im, const OfxRectI &r, ntscrt_image &out, std::vector<uint8_t> &buf) {
    if (wrap(im, r, out)) return true;
    if (im.getPixelComponents() != OFX::ePixelComponentRGBA) return false;
    ntscrt_pixel_format f = to_format(im.getPixelDepth());
    if (int(f) < 0 || r.x2 <= r.x1 || r.y2 <= r.y1) return false;
    const size_t pb = pixel_bytes(f);
    out.width = r.x2 - r.x1;
    out.height = r.y2 - r.y1;
    out.format = f;
    out.row_bytes = ptrdiff_t(size_t(out.width) * pb);
    buf.assign(size_t(out.height) * size_t(out.row_bytes), 0);
    out.data = buf.data();
    const OfxRectI b = im.getBounds();
    const int x1 = std::max(r.x1, b.x1), x2 = std::min(r.x2, b.x2);
    const int y1 = std::max(r.y1, b.y1), y2 = std::min(r.y2, b.y2);
    for (int y = y1; y < y2 && x1 < x2; ++y) {
        const void *src = im.getPixelAddress(x1, y);
        if (!src) continue;
        uint8_t *dst = buf.data() + size_t(r.y2 - 1 - y) * size_t(out.row_bytes) + size_t(x1 - r.x1) * pb;
        std::memcpy(dst, src, size_t(x2 - x1) * pb);
    }
    return true;
}
}  // namespace

// ---------------------------------------------------------------------------
// Diagnostic log, always on and small: plug-in load (version, path, host), each
// instance, the first renders of each instance and every failure. For bug reports.
//   Windows: %LOCALAPPDATA%\NTSCRT\ntscrt-ofx.log    macOS: ~/Library/Logs/NTSCRT/ntscrt-ofx.log
//   NTSCRT_OFX_LOG=<file> logs there instead ("off" disables), NTSCRT_OFX_LOG_VERBOSE=1 logs every frame.
//   The per-instance "Debug logging" checkbox logs every frame of that instance in detail.
// Rotated to ntscrt-ofx.old.log at 1 MB.
namespace dlog {
std::mutex &mu() { static std::mutex m; return m; }
std::filesystem::path default_path() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    const char *base = std::getenv("LOCALAPPDATA");
    return base ? fs::u8path(base) / "NTSCRT" / "ntscrt-ofx.log" : fs::path();
#else
    const char *home = std::getenv("HOME");
    return home ? fs::u8path(home) / "Library" / "Logs" / "NTSCRT" / "ntscrt-ofx.log" : fs::path();
#endif
}
std::filesystem::path path() {
    static const std::filesystem::path p = [] {
        namespace fs = std::filesystem;
        if (const char *e = std::getenv("NTSCRT_OFX_LOG"); e && *e) return std::string(e) == "off" ? fs::path() : fs::u8path(e);
        return default_path();
    }();
    return p;
}
// Folder "Open logs folder" shows: the log's folder (the default one when logging is off).
std::filesystem::path folder() {
    std::filesystem::path p = path();
    if (p.empty()) p = default_path();
    return p.empty() ? p : p.parent_path();
}
bool verbose() {
    static const bool v = [] { const char *e = std::getenv("NTSCRT_OFX_LOG_VERBOSE"); return e && *e && *e != '0'; }();
    return v;
}
void write(const std::string &msg) {
    try {
        namespace fs = std::filesystem;
        const fs::path p = path();
        if (p.empty()) return;
        std::lock_guard<std::mutex> lk(mu());
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (fs::exists(p, ec) && fs::file_size(p, ec) > (1u << 20)) {
            fs::path old = p;
            old.replace_filename("ntscrt-ofx.old.log");
            fs::rename(p, old, ec);
        }
        std::ofstream f(p, std::ios::app);
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
        char ts[32];
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
        std::ostringstream tid;
        tid << std::this_thread::get_id();
        f << ts << '.' << (ms < 100 ? (ms < 10 ? "00" : "0") : "") << ms << " [" << tid.str() << "] " << msg << '\n';
    } catch (...) {
    }
}
const char *depth_name(OFX::BitDepthEnum d) {
    switch (d) {
    case OFX::eBitDepthUByte: return "8u";
    case OFX::eBitDepthUShort: return "16u";
    case OFX::eBitDepthHalf: return "16f";
    case OFX::eBitDepthFloat: return "32f";
    default: return "?";
    }
}
std::string rect(const OfxRectI &r) {
    std::ostringstream o;
    o << r.x1 << ',' << r.y1 << '-' << r.x2 << ',' << r.y2 << " (" << (r.x2 - r.x1) << 'x' << (r.y2 - r.y1) << ')';
    return o.str();
}

// Is there a desktop to show a folder on? Not when the host runs headless
// (Resolve -nogui) or NTSCRT_OFX_NO_OPEN is set (tests, CI).
bool has_gui() {
    if (const char *e = std::getenv("NTSCRT_OFX_NO_OPEN"); e && *e && *e != '0') return false;
#ifdef _WIN32
    std::wstring cl = GetCommandLineW();
    std::transform(cl.begin(), cl.end(), cl.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    if (cl.find(L"-nogui") != std::wstring::npos) return false;
    HWINSTA ws = GetProcessWindowStation();
    USEROBJECTFLAGS uof{};
    if (ws && GetUserObjectInformationW(ws, UOI_FLAGS, &uof, sizeof uof, nullptr) && !(uof.dwFlags & WSF_VISIBLE))
        return false;  // service / non-interactive session
    return true;
#elif defined(__APPLE__)
    char **argv = *_NSGetArgv();
    for (int i = 0, n = *_NSGetArgc(); i < n && argv && argv[i]; ++i)
        if (std::strcmp(argv[i], "-nogui") == 0) return false;
    return true;
#else
    return std::getenv("DISPLAY") != nullptr;
#endif
}

// "Open logs folder": create the folder, log the press, open it in Explorer / Finder
// on a background thread. Never blocks the host and never throws.
void open_folder(int instance) {
    try {
        namespace fs = std::filesystem;
        const fs::path dir = folder();
        std::ostringstream o;
        o << "instance #" << instance << " open logs folder: " << u8(dir);
        if (dir.empty()) {
            write(o.str() + " (no folder: HOME / LOCALAPPDATA unset)");
            return;
        }
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (!has_gui()) {
            write(o.str() + " (no GUI: not opened)");
            return;
        }
        write(o.str());
#ifdef _WIN32
        std::thread([w = dir.wstring()] {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (r <= 32) write("open logs folder: ShellExecute failed (" + std::to_string(r) + ")");
            if (SUCCEEDED(hr)) CoUninitialize();
        }).detach();
#else
        std::thread([s = u8(dir)] {
#  ifdef __APPLE__
            const char *tool = "/usr/bin/open";
#  else
            const char *tool = "xdg-open";
#  endif
            const char *argv[] = {tool, s.c_str(), nullptr};
            pid_t pid = 0;
            int rc = posix_spawnp(&pid, tool, nullptr, nullptr, const_cast<char *const *>(argv), environ);
            if (rc != 0) {
                write("open logs folder: spawn failed (" + std::to_string(rc) + ")");
                return;
            }
            int status = 0;
            waitpid(pid, &status, 0);
        }).detach();
#endif
    } catch (...) {
    }
}
}  // namespace dlog

// ---------------------------------------------------------------------------
class NtscrtEffect : public OFX::ImageEffect {
public:
    NtscrtEffect(OfxImageEffectHandle h) : ImageEffect(h), g_(globals()) {
        dst_ = fetchClip(kOfxImageEffectOutputClipName);
        src_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
        const Schema &s = g_.schema;
        params_.resize(s.params.size(), nullptr);
        for (size_t i = 0; i < s.params.size(); ++i) {
            const ParamDef &p = s.params[i];
            switch (p.type) {
            case PType::Bool: params_[i] = fetchBooleanParam(p.name); break;
            case PType::Int: params_[i] = fetchIntParam(p.name); break;
            case PType::Float: params_[i] = fetchDoubleParam(p.name); break;
            case PType::Choice: params_[i] = fetchChoiceParam(p.name); break;
            case PType::Color:
                params_[i] = p.color_n == 4 ? static_cast<OFX::Param *>(fetchRGBAParam(p.name))
                                            : static_cast<OFX::Param *>(fetchRGBParam(p.name));
                break;
            }
        }
        preset_ = fetchChoiceParam(kPresetParam);
        loop_preset_ = g_.loops.empty() ? nullptr : fetchChoiceParam(kLoopPresetParam);
        preset_file_ = fetchStringParam(kPresetFileParam);
        seed_ = fetchIntParam(kSeedParam);
        debug_ = fetchBooleanParam(kDebugParam);
        for (size_t i = 0; i < s.params.size(); ++i) by_name_[s.params[i].name] = i;

        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, g_.res_dir.c_str()};
        if (ntscrt_create(&ci, &inst_) != NTSCRT_OK) inst_ = nullptr;
        id_ = ++instance_count();
        {
            std::ostringstream o;
            o << "instance #" << id_ << " created, context " << getContext() << ", core "
              << (inst_ ? "ok" : "FAILED to initialise");
            if (inst_ && *ntscrt_last_error(inst_)) o << " (" << ntscrt_last_error(inst_) << ")";
            dlog::write(o.str());
        }
        update_dependents(0.0);
    }
    ~NtscrtEffect() override {
        if (inst_) ntscrt_destroy(inst_);
    }

    // The output covers the whole frame (project size, or the source's area if
    // larger), not just the source's: Fusion sources such as Text+ only define
    // their bounding box, and the effect (glow, ghosting, spill) reaches past it.
    bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) override {
        rod = src_->getRegionOfDefinition(args.time);
        const OfxPointD off = getProjectOffset(), ext = getProjectExtent();
        if (ext.x > 0 && ext.y > 0) {
            rod.x1 = std::min(rod.x1, off.x);
            rod.y1 = std::min(rod.y1, off.y);
            rod.x2 = std::max(rod.x2, off.x + ext.x);
            rod.y2 = std::max(rod.y2, off.y + ext.y);
        }
        return true;
    }
    void getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args, OFX::RegionOfInterestSetter &rois) override {
        OfxRectD rod;
        OFX::RegionOfDefinitionArguments ra;
        ra.time = args.time;
        ra.renderScale = args.renderScale;
        getRegionOfDefinition(ra, rod);
        rois.setRegionOfInterest(*src_, rod);  // non-local effect: always the whole frame
    }

    void getClipPreferences(OFX::ClipPreferencesSetter &prefs) override {
        prefs.setOutputFrameVarying(true);  // temporal stages: output differs per frame
        // Output keeps the source's alpha convention (the core writes it that way).
        prefs.setOutputPremultiplication(source_premultiplication());
    }

    void purgeCaches() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (inst_) ntscrt_flush_caches(inst_);
    }

    void changedParam(const OFX::InstanceChangedArgs &args, const std::string &name) override {
        // Diagnostics only: they never touch the look, so no cache flush.
        if (name == kOpenLogsParam) {
            dlog::open_folder(id_);
            return;
        }
        if (name == kDebugParam) {
            bool on = false;
            try { debug_->getValue(on); } catch (...) {}
            dlog::write("instance #" + std::to_string(id_) + " debug logging " + (on ? "on" : "off"));
            last_look_hash_ = 0;  // log the full look on the next frame
            return;
        }
        // Any param change (user edit, keyframe add/remove, preset apply/load =
        // plugin edit) can change the looks of other frames: the look history
        // and the key counts are re-read, and the core drops the caches the
        // param can affect (cache_mask). Pure time changes don't.
        if (args.reason != OFX::eChangeTime) {
            ++look_gen_;
            keys_dirty_ = true;
            if (uint32_t m = cache_mask(name)) {
                std::lock_guard<std::mutex> lk(mu_);
                if (inst_) ntscrt_flush_caches_mask(inst_, m);
            }
        }
        if (applying_) return;
        if (args.reason == OFX::eChangeUserEdit) {
            if (name == kPresetParam) {
                int i = 0;
                preset_->getValue(i);
                if (i > 0 && size_t(i) <= g_.looks.size()) apply_preset(g_.looks[size_t(i - 1)].data, false, args.time);
            } else if (loop_preset_ && name == kLoopPresetParam) {
                int i = 0;
                loop_preset_->getValue(i);
                if (i > 0 && size_t(i) <= g_.loops.size()) apply_preset(g_.loops[size_t(i - 1)].data, true, args.time);
            } else if (name == kLoadParam) {
                load_preset_file(args.time);
            } else if (name == kSaveParam) {
                save_preset_file(args.time);
            }
            // preset apply/load changed many params (keyframes too): flush once
            // more afterwards. Source frames are keyed by what shapes them.
            if (name == kPresetParam || name == kLoopPresetParam || name == kLoadParam) {
                std::lock_guard<std::mutex> lk(mu_);
                if (inst_) ntscrt_flush_caches_mask(inst_, NTSCRT_CACHE_ALL & ~NTSCRT_CACHE_SOURCE);
            }
        }
        update_dependents(args.time);
    }

    void render(const OFX::RenderArguments &args) override;
    void render_frame(const OFX::RenderArguments &args);

    // Clip properties some hosts leave out (Fusion: a Text+ input has no frame
    // rate). Fall back to the effect's own value, then a sensible default.
    template <class F, class T> static T optional_prop(F get, T fallback) {
        try {
            return get();
        } catch (const OFX::Exception::PropertyUnknownToHost &) {
            return fallback;
        } catch (const OFX::Exception::Suite &) {
            return fallback;
        }
    }
    double source_frame_rate() {
        double fps = optional_prop([&] { return src_->getFrameRate(); }, 0.0);
        if (fps <= 0) fps = optional_prop([&] { return getFrameRate(); }, 0.0);
        return fps > 0 ? fps : 24.0;
    }
    OfxRangeD source_frame_range() {
        OfxRangeD none{0.0, 0.0};
        OfxRangeD r = optional_prop([&] { return src_->getFrameRange(); }, none);
        if (r.max <= r.min) {
            double d = optional_prop([&] { return getEffectDuration(); }, 0.0);
            if (d > 0) r = {0.0, d - 1};
        }
        if (r.max <= r.min) r = {0.0, 1e9};  // unknown: frame numbers = host time
        return r;
    }
    OFX::PreMultiplicationEnum source_premultiplication() {
        return optional_prop([&] { return src_->getPreMultiplication(); }, OFX::eImagePreMultiplied);
    }

    // ---- values -----------------------------------------------------------
    json value_at(size_t i, double t) const {
        const ParamDef &p = g_.schema.params[i];
        OFX::Param *q = params_[i];
        switch (p.type) {
        case PType::Bool: return static_cast<OFX::BooleanParam *>(q)->getValueAtTime(t);
        case PType::Int: return static_cast<OFX::IntParam *>(q)->getValueAtTime(t);
        case PType::Float: return static_cast<OFX::DoubleParam *>(q)->getValueAtTime(t);
        case PType::Choice: {
            int k = 0;
            static_cast<OFX::ChoiceParam *>(q)->getValueAtTime(t, k);
            if (k < 0 || size_t(k) >= p.options.size()) k = std::max(0, choice_index(p, p.def));
            return p.options[size_t(k)].first;
        }
        case PType::Color: {
            std::vector<double> c(size_t(p.color_n), 1.0);
            if (p.color_n == 4) static_cast<OFX::RGBAParam *>(q)->getValueAtTime(t, c[0], c[1], c[2], c[3]);
            else static_cast<OFX::RGBParam *>(q)->getValueAtTime(t, c[0], c[1], c[2]);
            return color_value(p, c);
        }
        }
        return json();
    }

    void look_at_time(double t, std::string &look, std::string &fb) const {
        json lj, fj;
        build_look(g_.schema, [&](size_t i) { return value_at(i, t); }, lj, fj);
        look = lj.dump();
        fb = fj.is_null() ? std::string() : fj.dump();
    }

    // ---- look history for the core's look_at ------------------------------
    // Re-counts keyframes when a param changed since the last count: with none
    // anywhere every frame has the current look, and look_at answers NULL.
    bool animated() {
        if (keys_dirty_.exchange(false)) {
            bool any = false;
            for (OFX::Param *q : params_) {
                try {
                    if (static_cast<OFX::ValueParam *>(q)->getNumKeys() > 0) {
                        any = true;
                        break;
                    }
                } catch (...) {
                    any = true;  // can't tell: assume animated
                    break;
                }
            }
            animated_ = any;
        }
        return animated_;
    }
    // The look at time t, built once per param state (look_gen_). The string
    // stays valid until the next call. Called under mu_.
    const std::string &cached_look(double t) {
        const uint64_t gen = look_gen_;
        if (gen != look_cache_gen_ || look_cache_.size() >= 2048) {
            look_cache_.clear();
            look_cache_gen_ = gen;
        }
        auto it = look_cache_.find(t);
        if (it != look_cache_.end()) return it->second;
        std::string look, fb;
        look_at_time(t, look, fb);
        ++look_builds_;
        return look_cache_.emplace(t, std::move(look)).first->second;
    }
    // Under mu_, with the current frame's freshly built look: remembered for
    // the next frames' history lookups. A cached look for the same time that
    // differs means params changed without a changedParam (a host that doesn't
    // report some keyframe edit): the history is dropped.
    void note_current_look(double t, const std::string &look) {
        if (look_cache_gen_ == look_gen_) {
            auto it = look_cache_.find(t);
            if (it != look_cache_.end()) {
                if (it->second == look) return;
                ++look_gen_;
                keys_dirty_ = true;
                dlog::write("instance #" + std::to_string(id_) + " look at t=" + std::to_string(t) +
                            " changed without a param change notification: caches dropped");
                if (inst_) ntscrt_flush_caches_mask(inst_, NTSCRT_CACHE_ALL & ~NTSCRT_CACHE_SOURCE);
            }
        }
        if (look_cache_gen_ != look_gen_ || look_cache_.size() >= 2048) {
            look_cache_.clear();
            look_cache_gen_ = look_gen_;
        }
        look_cache_[t] = look;
    }

    // Core caches a change of host param `name` can invalidate (0 = none).
    uint32_t cache_mask(const std::string &name) {
        // Host-only params: never part of the look (presets / files apply their
        // values as edits of the params below, which report themselves).
        for (const char *h : {kVersionParam, kPresetParam, kLoopPresetParam, kPresetFileParam, kLoadParam, kSaveParam,
                              kDebugParam, kOpenLogsParam})
            if (name == h) return 0;
        auto it = by_name_.find(name);
        if (it == by_name_.end()) return NTSCRT_CACHE_ALL;  // the seed, anything unknown
        const ParamDef &p = g_.schema.params[it->second];
        if (p.feedback) return NTSCRT_CACHE_SCREENLOOP;
        // Screen Loop recomputes earlier frames through every stage: always.
        uint32_t m = NTSCRT_CACHE_SCREENLOOP;
        const size_t slash = p.path.find('/', 1);
        const std::string top = p.path.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
        if (top == "crop" || top == "alpha" || top == "downscale") {
            m |= NTSCRT_CACHE_DIGITAL | NTSCRT_CACHE_CRT;  // Digital encodes the cropped source; CRT history
        } else if (top == "digital") {
            m |= NTSCRT_CACHE_DIGITAL;
        } else if (top == "shader") {
            m |= NTSCRT_CACHE_CRT;
        } else if (top == "glitch") {
            // The receiver re-checks its history, which catches edits that hold
            // for every frame; a keyframed value can change between its probes.
            bool keyed = true;
            try {
                keyed = static_cast<OFX::ValueParam *>(params_[it->second])->getNumKeys() > 0;
            } catch (...) {
            }
            if (keyed) m |= NTSCRT_CACHE_GLITCH;
        } else if (top != "ntsc") {  // ntsc-rs keeps no history
            m = NTSCRT_CACHE_ALL & ~NTSCRT_CACHE_SOURCE;
        }
        return m;
    }

private:
    template <class P, class... V>
    static void set_param(P *p, double t, V... v) {
        if (p->getNumKeys() > 0) p->setValueAtTime(t, v...);
        else p->setValue(v...);
    }

    void set_value(size_t i, const json &v, double t) {
        const ParamDef &p = g_.schema.params[i];
        OFX::Param *q = params_[i];
        switch (p.type) {
        case PType::Bool: set_param(static_cast<OFX::BooleanParam *>(q), t, v.get<bool>()); break;
        case PType::Int: set_param(static_cast<OFX::IntParam *>(q), t, int(v.get<int64_t>())); break;
        case PType::Float: set_param(static_cast<OFX::DoubleParam *>(q), t, v.get<double>()); break;
        case PType::Choice: {
            int k = choice_index(p, v);
            if (k >= 0) set_param(static_cast<OFX::ChoiceParam *>(q), t, k);
            break;
        }
        case PType::Color: {
            auto c = color_components(p, v);
            if (p.color_n == 4) set_param(static_cast<OFX::RGBAParam *>(q), t, c[0], c[1], c[2], c[3]);
            else set_param(static_cast<OFX::RGBParam *>(q), t, c[0], c[1], c[2]);
            break;
        }
        }
    }

    void apply_preset(const json &preset, bool feedback, double t) {
        applying_ = true;
        beginEditBlock(feedback ? "NTSCRT Screen Loop preset" : "NTSCRT preset");
        try {
            for (size_t i = 0; i < g_.schema.params.size(); ++i)
                if (auto v = preset_value(g_.schema.params[i], preset, feedback)) set_value(i, *v, t);
        } catch (...) {
            endEditBlock();
            applying_ = false;
            throw;
        }
        endEditBlock();
        applying_ = false;
    }

    void update_dependents(double t) {
        const Schema &s = g_.schema;
        auto val = [&](size_t i) { return value_at(i, t); };
        for (size_t i = 0; i < s.params.size(); ++i) {
            const ParamDef &p = s.params[i];
            if (p.enabled_if) params_[i]->setEnabled(cond_true(s, *p.enabled_if, val));
            if (p.visible_if) params_[i]->setIsSecret(!cond_true(s, *p.visible_if, val));
        }
    }

    void load_preset_file(double t) {
        std::string path;
        preset_file_->getValue(path);
        if (path.empty()) {
            sendMessage(OFX::Message::eMessageError, "", "Choose a preset file first.");
            return;
        }
        std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
        json j = f ? json::parse(f, nullptr, false) : json();
        if (!j.is_object()) {
            sendMessage(OFX::Message::eMessageError, "", "Could not read an NTSCRT preset from " + path);
            return;
        }
        apply_preset(j, is_feedback_preset(j), t);
        update_dependents(t);
    }

    void save_preset_file(double t) {
        std::string path;
        preset_file_->getValue(path);
        if (path.empty()) {
            sendMessage(OFX::Message::eMessageError, "", "Enter a file path to save the preset to.");
            return;
        }
        std::string look, fb;
        look_at_time(t, look, fb);
        std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
        if (f) f << json::parse(look).dump(2) << "\n";
        if (!f) sendMessage(OFX::Message::eMessageError, "", "Could not write " + path);
    }

    // Errors during render: persistent message (no modal dialog per frame) when
    // the host has message suite V2, else a plain message.
    void report_error(const std::string &msg, OFX::Message::MessageTypeEnum type = OFX::Message::eMessageError) {
        had_error_ = true;
        try {
            if (OFX::getImageEffectHostDescription()->supportsMessageSuiteV2)
                setPersistentMessage(type, "", msg);
            else
                sendMessage(type, "", msg);
        } catch (...) {
        }
    }

    const Globals &g_;
    OFX::Clip *dst_ = nullptr, *src_ = nullptr;
    std::vector<OFX::Param *> params_;
    OFX::ChoiceParam *preset_ = nullptr, *loop_preset_ = nullptr;
    OFX::StringParam *preset_file_ = nullptr;
    OFX::IntParam *seed_ = nullptr;
    OFX::BooleanParam *debug_ = nullptr;
    size_t last_look_hash_ = 0;  // debug logging: full look only when it changes
    std::map<std::string, size_t> by_name_;  // host param name -> schema index
    std::atomic<uint64_t> look_gen_{0};      // bumped by every param change
    std::atomic<bool> keys_dirty_{true};     // re-count keyframes before the next render
    bool animated_ = true;
    std::map<double, std::string> look_cache_;  // render thread, under mu_
    uint64_t look_cache_gen_ = ~uint64_t(0);
    size_t look_builds_ = 0;
    std::string last_warning_;  // degraded-frame message shown / logged
    ntscrt_instance *inst_ = nullptr;
    std::mutex mu_;
    bool applying_ = false;
    bool had_error_ = false;
    int id_ = 0;
    long renders_ = 0, failures_ = 0;
    static int &instance_count() { static int n = 0; return n; }
    friend struct RenderCtx;
};

// State shared with the core's temporal callbacks during one render call.
struct RenderCtx {
    NtscrtEffect *fx;
    double start;  // clip time of frame_index 0
    double end;
    OfxRectI frame;  // pixel rect every fetched frame is padded / cropped to (the output's)
    struct Live {
        std::unique_ptr<OFX::Image> image;
        std::unique_ptr<ntscrt_image> view;
        std::vector<uint8_t> padded;
    };
    std::map<const ntscrt_image *, Live> live;
    bool animated = true;  // some param has keyframes: other frames can differ
    int fetches = 0;  // other frames the core pulled (debug log)

    static const ntscrt_image *fetch(void *u, int64_t frame) {
        auto *c = static_cast<RenderCtx *>(u);
        double t = c->start + double(frame);
        if (t < c->start || t > c->end) return nullptr;
        ++c->fetches;
        try {
            std::unique_ptr<OFX::Image> im(c->fx->src_->fetchImage(t));
            if (!im) return nullptr;
            Live l{std::move(im), std::make_unique<ntscrt_image>(), {}};
            if (!wrap_padded(*l.image, c->frame, *l.view, l.padded)) return nullptr;
            const ntscrt_image *key = l.view.get();
            c->live[key] = std::move(l);
            return key;
        } catch (...) {
            return nullptr;
        }
    }
    static void release(void *u, const ntscrt_image *im) { static_cast<RenderCtx *>(u)->live.erase(im); }
    static const char *look_at(void *u, int64_t frame) {
        auto *c = static_cast<RenderCtx *>(u);
        if (!c->animated) return nullptr;  // "same as the current frame": nothing to build
        try {
            return c->fx->cached_look(c->start + double(frame)).c_str();
        } catch (...) {
            return nullptr;
        }
    }
};

void NtscrtEffect::render(const OFX::RenderArguments &args) {
    auto fail = [&](const std::string &why) {
        ++failures_;
        std::ostringstream o;
        o << "instance #" << id_ << " render t=" << args.time << " FAILED: " << why;
        dlog::write(o.str());
    };
    try {
        render_frame(args);
    } catch (const OFX::Exception::PropertyUnknownToHost &e) {
        fail(std::string("property unknown to host: ") + e.what());
        report_error(std::string("NTSCRT: this host doesn't provide ") + e.what());
        throw;
    } catch (const OFX::Exception::HostInadequate &e) {
        fail(std::string("host inadequate: ") + e.what());
        throw;
    } catch (const OFX::Exception::Suite &e) {
        fail(std::string("suite error: ") + e.what() + " (status " + std::to_string(int(e.status())) + ")");
        throw;
    } catch (const std::exception &e) {
        fail(std::string("exception: ") + e.what());
        throw;
    }
}

void NtscrtEffect::render_frame(const OFX::RenderArguments &args) {
    const auto t_start = std::chrono::steady_clock::now();
    std::unique_ptr<OFX::Image> dst(dst_->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(src_->fetchImage(args.time));
    if (!dst || !src) OFX::throwSuiteStatusException(kOfxStatFailed);
    if (src->getPixelDepth() != dst->getPixelDepth() || src->getPixelComponents() != dst->getPixelComponents())
        OFX::throwSuiteStatusException(kOfxStatErrImageFormat);

    OfxRectI win = dst->getBounds();
    const auto t0 = std::chrono::steady_clock::now();
    ntscrt_image s{}, d{};
    std::vector<uint8_t> src_padded;
    if (!wrap(*dst, win, d) || !wrap_padded(*src, win, s, src_padded)) {
        report_error("NTSCRT: unsupported image format or bounds");
        OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
    }

    const auto t_fetched = std::chrono::steady_clock::now();
    std::string look, fb;
    look_at_time(args.time, look, fb);
    int seed = 0;
    seed_->getValueAtTime(args.time, seed);
    bool debug = false;
    debug_->getValue(debug);
    const auto t_looked = std::chrono::steady_clock::now();

    OfxRangeD range = source_frame_range();
    RenderCtx ctx{this, range.min, range.max, win, {}};

    ntscrt_render_args ra{};
    ra.frame_index = int64_t(std::llround(args.time - range.min));
    ra.fps = source_frame_rate();
    ra.look_json = look.c_str();
    ra.feedback_json = fb.empty() ? nullptr : fb.c_str();
    ra.render_scale = args.renderScale.x > 0 ? args.renderScale.x : 1.0;
    ra.seed = uint64_t(uint32_t(seed));
    ra.fetch_source = &RenderCtx::fetch;
    ra.release_source = &RenderCtx::release;
    ra.look_at = &RenderCtx::look_at;
    ra.user = &ctx;
    ra.source_premultiplied = source_premultiplication() == OFX::eImagePreMultiplied ? 1 : 0;

    std::lock_guard<std::mutex> lk(mu_);
    if (!inst_) {
        report_error("NTSCRT: core failed to initialise");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    ctx.animated = animated();
    if (ctx.animated) note_current_look(args.time, look);
    const size_t builds_before = look_builds_;
    const auto t_core = std::chrono::steady_clock::now();
    ntscrt_status st = ntscrt_render_cpu(inst_, &ra, &s, &d);
    const bool degraded =
        st == NTSCRT_ERR_GPU || st == NTSCRT_ERR_MISSING_RESOURCE || st == NTSCRT_ERR_SOURCE_UNAVAILABLE;
    const std::string err = st != NTSCRT_OK ? ntscrt_last_error(inst_) : std::string();
    const bool new_warning = degraded && err != last_warning_;
    ++renders_;
    const auto t_done = std::chrono::steady_clock::now();
    if (renders_ <= 20 || renders_ % 500 == 0 || dlog::verbose() || debug || (st != NTSCRT_OK && !degraded) ||
        new_warning) {
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::ostringstream o;
        o << "instance #" << id_ << (debug ? " debug" : "") << " render #" << renders_ << " t=" << args.time
          << " frame=" << ra.frame_index << " fps=" << ra.fps << " scale=" << ra.render_scale << " dst=" << dlog::rect(win)
          << " src=" << dlog::rect(src->getBounds()) << " depth=" << dlog::depth_name(src->getPixelDepth())
          << " premult=" << ra.source_premultiplied << " status=" << int(st)
          << (st != NTSCRT_OK ? std::string(degraded ? " degraded=" : " error=") + err : std::string()) << " ms="
          << std::chrono::duration_cast<std::chrono::milliseconds>(t_done - t0).count();
        if (debug) {
            // Per-phase timings (fetch = host images, look = reading every param, core = all stages)
            // and the look: in full whenever it changes, otherwise its hash.
            o.setf(std::ios::fixed);
            o.precision(2);
            o << " fetch_ms=" << ms(t_start, t_fetched) << " look_ms=" << ms(t_fetched, t_looked)
              << " core_ms=" << ms(t_core, t_done) << " seed=" << seed << " temporal_fetches=" << ctx.fetches
              << " animated=" << ctx.animated << " look_builds=" << (look_builds_ - builds_before);
            const size_t h = std::hash<std::string>{}(look) ^ (std::hash<std::string>{}(fb) << 1);
            o << " look#" << std::hex << h << std::dec;
            if (h != last_look_hash_) {
                o << " look=" << look;
                if (!fb.empty()) o << " feedback=" << fb;
                last_look_hash_ = h;
            }
        } else if (renders_ == 1) {
            o << " look=" << look.substr(0, 400) << (look.size() > 400 ? "..." : "");
        }
        dlog::write(o.str());
    }
    if (st != NTSCRT_OK && !degraded) {
        report_error(std::string("NTSCRT: ") + err);
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    if (degraded) {
        // The frame is complete, just without what's missing: say so once (a
        // persistent warning, no dialog per frame) and keep rendering.
        if (new_warning) report_error(std::string("NTSCRT: ") + err, OFX::Message::eMessageWarning);
        last_warning_ = err;
        return;
    }
    last_warning_.clear();
    if (had_error_ && OFX::getImageEffectHostDescription()->supportsMessageSuiteV2) clearPersistentMessage();
    had_error_ = false;
}

// ---------------------------------------------------------------------------
class NtscrtFactory : public OFX::PluginFactoryHelper<NtscrtFactory> {
public:
    // The OFX plug-in version is part of its identity: hosts keep effects in saved
    // projects by (id, major) and load the highest minor. So the major stays at
    // kOfxIdentityMajor (bump it only if old projects truly can't load the new
    // plug-in), and the minor grows with every release (X.Y.Z -> X*10000+Y*100+Z).
    NtscrtFactory() : PluginFactoryHelper<NtscrtFactory>(kPluginId, kOfxIdentityMajor, identity_minor()) {}

    // A development copy loaded next to an installed one with the same version would lose
    // (the host keeps one plug-in per id and version). NTSCRT_OFX_DEV_BUNDLE=<folder> (set by
    // tests/resolve: run_resolve_tests.py --bundle, launch_resolve_dev.ps1) marks the copy
    // inside that folder as a dev build that outranks every installed release.
    static unsigned identity_minor() {
        const unsigned release = NTSCRT_OFX_VERSION_MAJOR * 10000 + NTSCRT_OFX_VERSION_MINOR * 100 + NTSCRT_OFX_VERSION_PATCH;
        return is_dev_copy() ? 999999u : release;
    }
    static bool is_dev_copy() {
        const char *e = std::getenv("NTSCRT_OFX_DEV_BUNDLE");
        if (!e || !*e) return false;
        try {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path dev = fs::weakly_canonical(fs::u8path(e), ec);
            const fs::path res = fs::weakly_canonical(fs::u8path(resource_dir()), ec);
            auto d = dev.native(), r = res.native();
            std::transform(d.begin(), d.end(), d.begin(), [](auto c) { return decltype(c)(c < 128 ? std::tolower(int(c)) : int(c)); });
            std::transform(r.begin(), r.end(), r.begin(), [](auto c) { return decltype(c)(c < 128 ? std::tolower(int(c)) : int(c)); });
            return !d.empty() && r.compare(0, d.size(), d) == 0;
        } catch (...) {
            return false;
        }
    }
    void load() override {}
    void unload() override {}
    void describe(OFX::ImageEffectDescriptor &desc) override;
    void describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum ctx) override;
    OFX::ImageEffect *createInstance(OfxImageEffectHandle h, OFX::ContextEnum) override { return new NtscrtEffect(h); }
};

void NtscrtFactory::describe(OFX::ImageEffectDescriptor &desc) {
    {
        const auto *h = OFX::getImageEffectHostDescription();
        std::ostringstream o;
        o << "NTSCRT " << NTSCRT_VERSION_STRING << (NtscrtFactory::is_dev_copy() ? " (dev copy, NTSCRT_OFX_DEV_BUNDLE)" : "")
          << " loaded; resources " << globals().res_dir << "; host "
          << (h ? h->hostName + " (" + h->hostLabel + ") " + h->versionLabel : std::string("?"));
        dlog::write(o.str());
    }
    desc.setLabels("NTSCRT", "NTSCRT", "NTSCRT analog TV / VHS / CRT");
    desc.setPluginGrouping("NTSCRT");
    desc.setVersion(NTSCRT_OFX_VERSION_MAJOR, NTSCRT_OFX_VERSION_MINOR, NTSCRT_OFX_VERSION_PATCH, 0, NTSCRT_VERSION_STRING);
    desc.setPluginDescription("Analog TV, VHS and CRT look (port of the NTSCRT macOS app). Version " NTSCRT_VERSION_STRING ".");
    desc.addSupportedContext(OFX::eContextFilter);
    desc.addSupportedContext(OFX::eContextGeneral);
    desc.addSupportedBitDepth(OFX::eBitDepthUByte);
    desc.addSupportedBitDepth(OFX::eBitDepthUShort);
    desc.addSupportedBitDepth(OFX::eBitDepthHalf);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(false);
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(true);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipDepths(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);
    // GPU paths come later (Wave 2+): CPU only for now.
    desc.setSupportsOpenCLBuffersRender(false);
    desc.setSupportsCudaRender(false);
    desc.setSupportsMetalRender(false);
}

namespace {
void define_value_param(OFX::ImageEffectDescriptor &desc, const ParamDef &p, OFX::GroupParamDescriptor *grp) {
    OFX::ValueParamDescriptor *vd = nullptr;
    switch (p.type) {
    case PType::Bool: {
        auto *d = desc.defineBooleanParam(p.name);
        d->setDefault(p.def.get<bool>());
        vd = d;
        break;
    }
    case PType::Int: {
        auto *d = desc.defineIntParam(p.name);
        d->setDefault(int(p.def.get<int64_t>()));
        int lo = p.has_min ? int(std::llround(p.min)) : INT_MIN, hi = p.has_max ? int(std::llround(p.max)) : INT_MAX;
        d->setRange(lo, hi);
        d->setDisplayRange(p.has_dmin ? int(std::llround(p.dmin)) : (p.has_min ? lo : 0),
                           p.has_dmax ? int(std::llround(p.dmax)) : (p.has_max ? hi : 100));
        vd = d;
        break;
    }
    case PType::Float: {
        auto *d = desc.defineDoubleParam(p.name);
        d->setDoubleType(OFX::eDoubleTypePlain);
        d->setDefault(p.def.get<double>());
        double lo = p.has_min ? p.min : -DBL_MAX, hi = p.has_max ? p.max : DBL_MAX;
        d->setRange(lo, hi);
        d->setDisplayRange(p.has_dmin ? p.dmin : (p.has_min ? lo : 0.0), p.has_dmax ? p.dmax : (p.has_max ? hi : 1.0));
        if (p.has_step && p.step > 0) {
            d->setIncrement(p.step);
            d->setDigits(std::clamp(int(std::ceil(-std::log10(p.step))), 0, 6));
        }
        vd = d;
        break;
    }
    case PType::Choice: {
        auto *d = desc.defineChoiceParam(p.name);
        for (const auto &o : p.options) d->appendOption(o.second);
        d->setDefault(std::max(0, choice_index(p, p.def)));
        vd = d;
        break;
    }
    case PType::Color: {
        auto c = color_components(p, p.def);
        if (p.color_n == 4) {
            auto *d = desc.defineRGBAParam(p.name);
            d->setDefault(c[0], c[1], c[2], c[3]);
            vd = d;
        } else {
            auto *d = desc.defineRGBParam(p.name);
            d->setDefault(c[0], c[1], c[2]);
            vd = d;
        }
        break;
    }
    }
    vd->setLabel(p.label);
    if (!p.hint.empty()) vd->setHint(p.hint);
    vd->setAnimates(p.animatable);
    if (grp) vd->setParent(*grp);
}
}  // namespace

void NtscrtFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
    OFX::ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(OFX::ePixelComponentRGBA);
    src->setTemporalClipAccess(true);
    src->setSupportsTiles(false);
    src->setIsMask(false);
    OFX::ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(OFX::ePixelComponentRGBA);
    dst->setSupportsTiles(false);

    const Globals &g = globals();
    OFX::PageParamDescriptor *page = desc.definePageParam("Controls");

    // Host extras: presets, preset files, seed.
    auto *hg = desc.defineGroupParam(kHostGroup);
    hg->setLabel("Presets");
    hg->setOpen(true);
    page->addChild(*hg);
    {
        auto *d = desc.defineChoiceParam(kPresetParam);
        d->setLabel("Preset");
        d->setHint("Choosing a preset writes all its values into the parameters below (they can then be keyframed).");
        d->appendOption(kNoPreset);
        for (const auto &p : g.looks) d->appendOption(p.name);
        d->setDefault(0);
        d->setAnimates(false);
        d->setEvaluateOnChange(false);
        d->setParent(*hg);
    }
    if (!g.loops.empty()) {
        auto *d = desc.defineChoiceParam(kLoopPresetParam);
        d->setLabel("Screen Loop preset");
        d->setHint("Choosing a Screen Loop preset writes its values into the Screen Loop parameters.");
        d->appendOption(kNoPreset);
        for (const auto &p : g.loops) d->appendOption(p.name);
        d->setDefault(0);
        d->setAnimates(false);
        d->setEvaluateOnChange(false);
        d->setParent(*hg);
    }
    {
        auto *d = desc.defineStringParam(kPresetFileParam);
        d->setLabel("Preset file");
        d->setHint("Path of an NTSCRT preset .json (look or Screen Loop) for Load / Save.");
        d->setStringType(OFX::eStringTypeFilePath);
        d->setFilePathExists(false);
        d->setAnimates(false);
        d->setEvaluateOnChange(false);
        d->setParent(*hg);
    }
    {
        auto *d = desc.definePushButtonParam(kLoadParam);
        d->setLabel("Load preset...");
        d->setHint("Apply the preset file to the parameters.");
        d->setParent(*hg);
        auto *e = desc.definePushButtonParam(kSaveParam);
        e->setLabel("Save preset...");
        e->setHint("Write the current look (at the playhead) to the preset file.");
        e->setParent(*hg);
    }
    {
        auto *d = desc.defineStringParam(kVersionParam);
        d->setLabel("Version");
        d->setHint("NTSCRT plug-in version. Update by running the installer again (see the README).");
        d->setDefault(NTSCRT_VERSION_STRING);
        d->setStringType(OFX::eStringTypeLabel);
        d->setIsPersistant(false);  // always shows the installed version, not the one a project was saved with
        d->setEnabled(false);
        d->setAnimates(false);
        d->setEvaluateOnChange(false);
        d->setParent(*hg);
    }
    {
        // Diagnostics: never part of the look or of presets, and not saved with the project
        // (nobody ships a project that logs every frame by accident).
        auto *d = desc.defineBooleanParam(kDebugParam);
        d->setLabel("Debug logging");
        d->setHint("Log every frame this effect renders (timings and the full look) to the NTSCRT log, "
                   "for bug reports. Slows rendering slightly; turn it off when done.");
        d->setDefault(false);
        d->setIsPersistant(false);
        d->setAnimates(false);
        d->setEvaluateOnChange(false);
        d->setParent(*hg);
        auto *e = desc.definePushButtonParam(kOpenLogsParam);
        e->setLabel("Open logs folder");
        e->setHint("Show the folder with ntscrt-ofx.log in Explorer / Finder, to attach it to a bug report.");
        e->setParent(*hg);
    }
    {
        auto *d = desc.defineIntParam(kSeedParam);
        d->setLabel("Seed");
        d->setHint("Added to every random stream so two instances can differ. 0 = upstream behaviour.");
        d->setDefault(0);
        d->setRange(0, INT_MAX);
        d->setDisplayRange(0, 1000);
        d->setAnimates(false);
        d->setParent(*hg);
    }

    // Schema groups (parents before children is not guaranteed: define all first).
    std::map<std::string, OFX::GroupParamDescriptor *> groups;
    for (const auto &gd : g.schema.groups) {
        auto *d = desc.defineGroupParam(gd.name);
        d->setLabel(gd.label);
        d->setOpen(false);
        groups[gd.id] = d;
    }
    for (const auto &gd : g.schema.groups) {
        auto it = groups.find(gd.parent);
        if (!gd.parent.empty() && it != groups.end() && it->second != groups[gd.id]) groups[gd.id]->setParent(*it->second);
        else page->addChild(*groups[gd.id]);
    }
    for (const auto &p : g.schema.params) {
        auto it = groups.find(p.group);
        define_value_param(desc, p, it == groups.end() ? nullptr : it->second);
    }
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
    static NtscrtFactory factory;
    ids.push_back(&factory);
}
}  // namespace Plugin
}  // namespace OFX
