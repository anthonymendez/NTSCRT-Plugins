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
#include <cfloat>
#include <climits>
#include <cstdlib>
#include <cmath>
#include <cstring>
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
#else
#  include <dlfcn.h>
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

        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, g_.res_dir.c_str()};
        if (ntscrt_create(&ci, &inst_) != NTSCRT_OK) inst_ = nullptr;
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
        // Temporal caches in the core only sample keyframe history, so any param
        // change (user edit, keyframe add/remove, preset apply/load = plugin edit)
        // must drop them. Pure time changes don't.
        if (args.reason != OFX::eChangeTime) {
            std::lock_guard<std::mutex> lk(mu_);
            if (inst_) ntscrt_flush_caches(inst_);
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
            // preset apply/load changed many params: flush once more afterwards
            std::lock_guard<std::mutex> lk(mu_);
            if (inst_) ntscrt_flush_caches(inst_);
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
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (inst_) ntscrt_flush_caches(inst_);
        }
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
    void report_error(const std::string &msg) {
        had_error_ = true;
        try {
            if (OFX::getImageEffectHostDescription()->supportsMessageSuiteV2)
                setPersistentMessage(OFX::Message::eMessageError, "", msg);
            else
                sendMessage(OFX::Message::eMessageError, "", msg);
        } catch (...) {
        }
    }

    const Globals &g_;
    OFX::Clip *dst_ = nullptr, *src_ = nullptr;
    std::vector<OFX::Param *> params_;
    OFX::ChoiceParam *preset_ = nullptr, *loop_preset_ = nullptr;
    OFX::StringParam *preset_file_ = nullptr;
    OFX::IntParam *seed_ = nullptr;
    ntscrt_instance *inst_ = nullptr;
    std::mutex mu_;
    bool applying_ = false;
    bool had_error_ = false;
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
    std::string look_buf, fb_buf;

    static const ntscrt_image *fetch(void *u, int64_t frame) {
        auto *c = static_cast<RenderCtx *>(u);
        double t = c->start + double(frame);
        if (t < c->start || t > c->end) return nullptr;
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
        try {
            c->fx->look_at_time(c->start + double(frame), c->look_buf, c->fb_buf);
            return c->look_buf.c_str();
        } catch (...) {
            return nullptr;
        }
    }
};

// Diagnostics for host differences: set NTSCRT_OFX_LOG=<file> to append the
// reason a render failed (e.g. the OFX property a host doesn't provide).
static void debug_log(const std::string &msg) {
    const char *path = std::getenv("NTSCRT_OFX_LOG");
    if (!path || !*path) return;
    std::ofstream f(std::filesystem::u8path(path), std::ios::app);
    f << msg << '\n';
}

void NtscrtEffect::render(const OFX::RenderArguments &args) {
    try {
        render_frame(args);
    } catch (const OFX::Exception::PropertyUnknownToHost &e) {
        debug_log(std::string("render: property unknown to host: ") + e.what());
        report_error(std::string("NTSCRT: this host doesn't provide ") + e.what());
        throw;
    } catch (const OFX::Exception::HostInadequate &e) {
        debug_log(std::string("render: host inadequate: ") + e.what());
        throw;
    } catch (const OFX::Exception::Suite &e) {
        debug_log(std::string("render: suite error: ") + e.what());
        throw;
    }
}

void NtscrtEffect::render_frame(const OFX::RenderArguments &args) {
    std::unique_ptr<OFX::Image> dst(dst_->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(src_->fetchImage(args.time));
    if (!dst || !src) OFX::throwSuiteStatusException(kOfxStatFailed);
    if (src->getPixelDepth() != dst->getPixelDepth() || src->getPixelComponents() != dst->getPixelComponents())
        OFX::throwSuiteStatusException(kOfxStatErrImageFormat);

    OfxRectI win = dst->getBounds();
    ntscrt_image s{}, d{};
    std::vector<uint8_t> src_padded;
    if (!wrap(*dst, win, d) || !wrap_padded(*src, win, s, src_padded)) {
        report_error("NTSCRT: unsupported image format or bounds");
        OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
    }

    std::string look, fb;
    look_at_time(args.time, look, fb);
    int seed = 0;
    seed_->getValueAtTime(args.time, seed);

    OfxRangeD range = source_frame_range();
    RenderCtx ctx{this, range.min, range.max, win, {}, {}, {}};

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
    ntscrt_status st = ntscrt_render_cpu(inst_, &ra, &s, &d);
    if (st != NTSCRT_OK && st != NTSCRT_ERR_SOURCE_UNAVAILABLE) {
        report_error(std::string("NTSCRT: ") + ntscrt_last_error(inst_));
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
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
    NtscrtFactory()
        : PluginFactoryHelper<NtscrtFactory>(kPluginId, kOfxIdentityMajor,
                                             NTSCRT_OFX_VERSION_MAJOR * 10000 + NTSCRT_OFX_VERSION_MINOR * 100 +
                                                 NTSCRT_OFX_VERSION_PATCH) {}
    void load() override {}
    void unload() override {}
    void describe(OFX::ImageEffectDescriptor &desc) override;
    void describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum ctx) override;
    OFX::ImageEffect *createInstance(OfxImageEffectHandle h, OFX::ContextEnum) override { return new NtscrtEffect(h); }
};

void NtscrtFactory::describe(OFX::ImageEffectDescriptor &desc) {
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
