// CRT stage — librashader filter chain (upstream Pipeline.encode +
// SupersampledPass) or, with look["shader"]["enabled"] == false, the
// ShaderBypass port. Output sizing: the chain input is fitted into the host
// output at its own aspect (black bars), rendered there with upstream
// ScanlineGrid supersampling.
//
// look["shader"]: {"enabled": bool (default true),
//                  "preset": upstream preset id ("glow_gauss", ...) or a
//                            .slangp path relative to <resources>/shaders,
//                  "params": {"NAME": number}}
// Parameter values each frame: shader initial, then upstream's house
// defaults (AppState.appShaderDefaults), then look params — all re-applied
// so a value from an earlier frame never leaks into this one.
#include <ntscrt/ntscrt.h>

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "../downscale/downscale_kernels.h"
#include "../stages.h"
#include "crt_libra.h"
#include "crt_sizing.h"

namespace fs = std::filesystem;

namespace {
inline std::string u8str(const std::filesystem::path &p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
}  // namespace

namespace ntscrt {

namespace {

struct PresetInfo {
    const char *id, *path;
};
// upstream Sources/CrtCore/Presets.swift
const PresetInfo kPresets[] = {
    {"aperture", "crt/crt-aperture.slangp"},       {"easymode", "crt/crt-easymode.slangp"},
    {"glow_gauss", "crt/crtglow_gauss.slangp"},    {"glow_lanczos", "crt/crtglow_lanczos.slangp"},
    {"hyllian", "crt/crt-hyllian.slangp"},         {"royale", "crt/crt-royale.slangp"},
    {"sim", "crt/crtsim.slangp"},
};

// Something the shader needs isn't there (librashader, the preset file).
struct MissingResource : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string preset_path_for(const std::string &id_or_path) {
    for (const auto &p : kPresets)
        if (id_or_path == p.id) return p.path;
    if (id_or_path.size() > 7 && id_or_path.substr(id_or_path.size() - 7) == ".slangp") return id_or_path;
    for (const auto &p : kPresets)  // "crtglow_gauss" style stems
        if (fs::path(p.path).stem().string() == id_or_path) return p.path;
    throw MissingResource("unknown CRT shader preset '" + id_or_path + "'");
}

// upstream AppState.appShaderDefaults
const std::map<std::string, std::map<std::string, float>> &house_defaults() {
    static const std::map<std::string, std::map<std::string, float>> m = {
        {"crt/crtglow_gauss.slangp", {{"BOOST", 1.1f}, {"GLOW_ROLLOFF", 2.4f}, {"BLOOM_STRENGTH", 0.1f}}},
        {"crt/crtglow_lanczos.slangp", {{"BOOST", 1.1f}, {"GLOW_ROLLOFF", 2.4f}, {"BLOOM_STRENGTH", 0.1f}}},
    };
    return m;
}

// Does any pass read the previous frame (PassFeedbackN / OriginalHistoryN /
// <alias>Feedback)? Those presets are temporal (of the 7 bundled: crtsim).
bool uses_history(const fs::path &slangp) {
    std::ifstream f(slangp);
    std::string line;
    static const std::regex shader_line(R"re(^\s*shader\d+\s*=\s*"?([^"#]+?)"?\s*(#.*)?$)re");
    static const std::regex temporal(R"(\b(PassFeedback\d+|OriginalHistory\d+|\w+Feedback)\b)");
    while (std::getline(f, line)) {
        std::smatch m;
        if (!std::regex_match(line, m, shader_line)) continue;
        std::ifstream s(slangp.parent_path() / fs::u8path(m[1].str()));
        std::stringstream ss;
        ss << s.rdbuf();
        if (std::regex_search(ss.str(), temporal)) return true;
    }
    return false;
}

// One librashader chain and the parameter values it holds now.
struct ChainBox {
    std::unique_ptr<crt::Chain> chain;
    std::map<std::string, float> applied;
    bool broken = false;  // a frame failed on it: users drop it, nobody new gets it
};

struct CachedChain {
    // Presets without history (6 of the 7 bundled) keep no state between
    // frames beyond their parameters, which apply_params sets every frame:
    // one chain per (device, preset) then serves every CrtStage on that
    // device, so a second instance neither recompiles the preset (~140 ms
    // for glow_gauss at 1080p) nor holds its own copy (~70 MB CPU, ~20 MB
    // GPU). Temporal presets (feedback / history) stay per stage.
    std::shared_ptr<ChainBox> box;
    bool history = false;
    int64_t last_frame = -1000;
};

bool share_chains() {
    static const bool on = [] {
        const char *e = std::getenv("NTSCRT_SHARED_CHAINS");
        return !(e && *e == '0');
    }();
    return on;
}

// Process-wide shared chains, by (device, preset). Weak: a chain goes with
// its last user (before its device, which every user also holds). Never
// destroyed itself, so nothing runs at static destruction.
struct ChainRegistry {
    std::mutex m;
    std::map<std::pair<const gpu::Device *, std::string>, std::weak_ptr<ChainBox>> chains;
};
ChainRegistry &registry() {
    static ChainRegistry *r = new ChainRegistry;
    return *r;
}

}  // namespace

struct CrtStage::Impl {
    gpu::Device *dev;
    std::string resource_dir;
    std::map<std::string, CachedChain> chains;  // by preset path
    std::shared_ptr<gpu::Texture> rect_tex, big_tex, rt_tex, scratch, bypass_big;
    bool clear_history = false;
    // Presets that failed to load / run, by the look's preset string: not
    // retried every frame (flush() forgets them).
    struct Failure {
        int status;
        std::string msg;
    };
    std::map<std::string, Failure> failed;
    int fb_status = 0;
    std::string fb_reason;
    size_t attempts = 0;

    std::shared_ptr<gpu::Texture> &tex(std::shared_ptr<gpu::Texture> &slot, int w, int h, gpu::Format f) {
        if (!slot || slot->desc().width != w || slot->desc().height != h || slot->desc().format != f)
            slot = dev->create_texture({w, h, f, true, true});
        return slot;
    }

    CachedChain &chain_for(const std::string &rel) {
        auto it = chains.find(rel);
        if (it != chains.end() && !(it->second.box && it->second.box->broken)) return it->second;
        if (it != chains.end()) chains.erase(it);  // broken by another user: get a new one
        ++attempts;
        std::string err;
        crt::Library *lib = crt::Library::load(resource_dir, err);
        if (!lib) throw MissingResource(err);
        fs::path p = fs::u8path(resource_dir) / "shaders" / fs::u8path(rel);
        std::error_code ec;
        if (!fs::exists(p, ec)) throw MissingResource("shader preset missing: " + u8str(p));
        CachedChain c;
        c.history = uses_history(p);
        auto make = [&] {
            auto b = std::make_shared<ChainBox>();
            b->chain = lib->create_chain(*dev, u8str(p));
            return b;
        };
        if (!c.history && share_chains()) {
            // Held while creating: concurrent first frames build a preset once.
            ChainRegistry &r = registry();
            std::lock_guard<std::mutex> lock(r.m);
            for (auto e = r.chains.begin(); e != r.chains.end();)  // forget dead ones
                e = e->second.expired() ? r.chains.erase(e) : std::next(e);
            std::weak_ptr<ChainBox> &slot = r.chains[{dev, u8str(p)}];
            c.box = slot.lock();
            if (!c.box || c.box->broken) {
                c.box = make();
                slot = c.box;
            }
        } else {
            c.box = make();
        }
        return chains.emplace(rel, std::move(c)).first->second;
    }

    void apply_params(ChainBox &c, const std::string &rel, const json &look_params) {
        std::map<std::string, float> want;
        for (const auto &p : c.chain->params()) want[p.name] = p.initial;
        auto h = house_defaults().find(rel);
        if (h != house_defaults().end())
            for (const auto &[k, v] : h->second)
                if (want.count(k)) want[k] = v;
        if (look_params.is_object())
            for (auto it = look_params.begin(); it != look_params.end(); ++it)
                if (it->is_number() && want.count(it.key())) want[it.key()] = it->get<float>();
        for (const auto &[k, v] : want) {
            auto a = c.applied.find(k);
            if (a != c.applied.end() && a->second == v) continue;
            c.chain->set_param(k, v);
            c.applied[k] = v;
        }
    }

    // Shader path into `target`; false (fb_status / fb_reason set) when the
    // shader can't run, so the caller draws the bypass instead. A lost device
    // is rethrown: the owner has to replace it.
    bool try_shade(const gpu::Texture &in, gpu::Texture &target, const json &sh, const FrameContext &ctx) {
        const std::string id =
            sh.contains("preset") && sh["preset"].is_string() ? sh["preset"].get<std::string>() : "glow_gauss";
        auto f = failed.find(id);
        if (f == failed.end()) {
            std::string rel;
            try {
                rel = preset_path_for(id);
                shade(in, target, rel, sh, ctx);
                return true;
            } catch (const MissingResource &e) {
                f = failed.emplace(id, Failure{NTSCRT_ERR_MISSING_RESOURCE, e.what()}).first;
            } catch (const crt::PresetError &e) {
                f = failed.emplace(id, Failure{NTSCRT_ERR_MISSING_RESOURCE, e.what()}).first;
            } catch (const std::exception &e) {
                if (dev->lost()) throw;
                f = failed.emplace(id, Failure{NTSCRT_ERR_GPU, e.what()}).first;
            }
            if (!rel.empty()) {  // half-built or broken: rebuilt after flush()
                auto c = chains.find(rel);
                if (c != chains.end() && c->second.box) c->second.box->broken = true;
                chains.erase(rel);
            }
        }
        fb_status = f->second.status;
        fb_reason = "CRT shader '" + id + "' unavailable, showing the picture without it: " + f->second.msg;
        return false;
    }

    void shade(const gpu::Texture &in, gpu::Texture &target, const std::string &rel, const json &sh,
               const FrameContext &ctx) {
        CachedChain &c = chain_for(rel);
        ChainBox &box = *c.box;
        // A shared chain has one user at a time: its parameters and its frames
        // go together (the chain's frame() takes this lock as well).
        std::lock_guard<std::recursive_mutex> lock(dev->context_mutex());
        apply_params(box, rel, sh.contains("params") ? sh["params"] : json());
        const auto frame_count = uint64_t(std::max<int64_t>(0, shader_frame(ctx)));
        const int tw = target.desc().width, th = target.desc().height;
        const int iw = in.desc().width, ih = in.desc().height;

        gpu::Texture *dst = &target;
        bool integrate = false;
        int k = crt::supersample_factor(ih, th);
        // SupersampledPass.make: whole multiples of the lines; the width
        // follows the target's (the picture's) shape, not the lines' (one field).
        const int kw = int(std::lround(double(tw) * double(ih * k) / double(th)));
        if (k > 1 && kw > tw && ih * k > th) {
            dst = tex(big_tex, kw, ih * k, target.desc().format).get();  // RGBA16F for high-precision hosts
            integrate = true;
        } else if (!target.desc().render_target) {
            dst = tex(rt_tex, tw, th, target.desc().format).get();
        }
        bool clear = clear_history;
        clear_history = false;
        if (c.history && c.last_frame != int64_t(frame_count) - 1) {
            // Non-sequential access to a temporal preset: prime the feedback
            // with this frame's input so the result doesn't depend on
            // whatever frame was rendered before.
            box.chain->frame(in, *dst, frame_count ? frame_count - 1 : 0, true);
            clear = false;
        }
        box.chain->frame(in, *dst, frame_count, clear);
        c.last_frame = int64_t(frame_count);
        if (integrate) downscale_encode(*dev, *dst, target, DownscaleMethod::Area, scratch);
        else if (dst != &target) dev->copy(*dst, target);
    }

    // ShaderBypass.encode (upstream ShaderBypass.swift), downscale already applied.
    void bypass(const gpu::Texture &px, gpu::Texture &out) {
        const int pw = px.desc().width, ph = px.desc().height;
        const int ow = out.desc().width, oh = out.desc().height;
        const int kx = ow / pw, ky = oh / ph;
        const bool whole = kx >= 1 && kx == ky && pw * kx == ow && ph * ky == oh;
        if (whole && kx == 1 && px.desc().format == out.desc().format) {
            dev->copy(px, out);  // identity
        } else if (whole) {
            downscale_encode(*dev, px, out, DownscaleMethod::Nearest, scratch);  // nearest enlargement
        } else if (ow > pw && oh > ph) {
            int k = std::max(int(std::ceil(double(ow) / pw)), int(std::ceil(double(oh) / ph)));
            auto &big = tex(bypass_big, pw * k, ph * k, out.desc().format);
            downscale_encode(*dev, px, *big, DownscaleMethod::Nearest, scratch);
            downscale_encode(*dev, *big, out, DownscaleMethod::Area, scratch);
        } else {
            downscale_encode(*dev, px, out, DownscaleMethod::Area, scratch);
        }
    }
};

CrtStage::CrtStage(gpu::Device &d, const std::string &resource_dir) : impl_(std::make_unique<Impl>()) {
    impl_->dev = &d;
    impl_->resource_dir = resource_dir;
}
CrtStage::~CrtStage() = default;

void CrtStage::render(const gpu::Texture &in, gpu::Texture &out, const FrameContext &ctx) {
    static const json empty = json::object();
    const json &look = ctx.look && ctx.look->is_object() ? *ctx.look : empty;
    const json sh = look.contains("shader") && look["shader"].is_object() ? look["shader"] : json::object();
    const bool enabled = !sh.contains("enabled") || !sh["enabled"].is_boolean() || sh["enabled"].get<bool>();

    const int ow = out.desc().width, oh = out.desc().height;
    // The picture's shape: with one field each chain line is ctx.line_height rows.
    crt::Rect r = crt::fit_rect(in.desc().width, in.desc().height * std::max(1, ctx.line_height), ow, oh);
    const bool full = r == crt::Rect{0, 0, ow, oh};
    gpu::Texture &target = full ? out : *impl_->tex(impl_->rect_tex, r.w, r.h, out.desc().format);

    impl_->fb_status = 0;
    impl_->fb_reason.clear();
    if (!enabled || !impl_->try_shade(in, target, sh, ctx)) impl_->bypass(in, target);

    if (!full) {
        struct { int32_t ox, oy; uint32_t w, h; } p{r.x, r.y, uint32_t(ow), uint32_t(oh)};
        gpu::Bindings b;
        b.constants = &p;
        b.constants_size = sizeof p;
        b.textures = {&target};
        b.out_textures = {&out};
        impl_->dev->dispatch(impl_->dev->kernel("crt_place/place"), b, ow, oh);
    }
}

void CrtStage::flush() {
    impl_->clear_history = true;
    impl_->failed.clear();
    for (auto &[k, c] : impl_->chains) c.last_frame = -1000;
}

int CrtStage::fallback_status() const { return impl_->fb_status; }
const std::string &CrtStage::fallback_reason() const { return impl_->fb_reason; }
size_t CrtStage::chain_load_attempts() const { return impl_->attempts; }

}  // namespace ntscrt
