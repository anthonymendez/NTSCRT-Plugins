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
#include <filesystem>
#include <fstream>
#include <map>
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

std::string preset_path_for(const std::string &id_or_path) {
    for (const auto &p : kPresets)
        if (id_or_path == p.id) return p.path;
    if (id_or_path.size() > 7 && id_or_path.substr(id_or_path.size() - 7) == ".slangp") return id_or_path;
    for (const auto &p : kPresets)  // "crtglow_gauss" style stems
        if (fs::path(p.path).stem().string() == id_or_path) return p.path;
    throw std::runtime_error("unknown CRT shader preset '" + id_or_path + "'");
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

struct CachedChain {
    std::unique_ptr<crt::Chain> chain;
    std::map<std::string, float> applied;
    bool history = false;
    int64_t last_frame = -1000;
};

}  // namespace

struct CrtStage::Impl {
    gpu::Device *dev;
    std::string resource_dir;
    std::map<std::string, CachedChain> chains;  // by preset path
    std::shared_ptr<gpu::Texture> rect_tex, big_tex, rt_tex, scratch, bypass_big;
    bool clear_history = false;

    std::shared_ptr<gpu::Texture> &tex(std::shared_ptr<gpu::Texture> &slot, int w, int h, gpu::Format f) {
        if (!slot || slot->desc().width != w || slot->desc().height != h || slot->desc().format != f)
            slot = dev->create_texture({w, h, f, true, true});
        return slot;
    }

    CachedChain &chain_for(const std::string &rel) {
        auto it = chains.find(rel);
        if (it != chains.end()) return it->second;
        std::string err;
        crt::Library *lib = crt::Library::load(resource_dir, err);
        if (!lib) throw std::runtime_error("CRT shader unavailable: " + err);
        fs::path p = fs::u8path(resource_dir) / "shaders" / fs::u8path(rel);
        std::error_code ec;
        if (!fs::exists(p, ec)) throw std::runtime_error("CRT shader preset missing: " + u8str(p));
        CachedChain c;
        c.chain = lib->create_chain(*dev, u8str(p));
        c.history = uses_history(p);
        return chains.emplace(rel, std::move(c)).first->second;
    }

    void apply_params(CachedChain &c, const std::string &rel, const json &look_params) {
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

    // Shader path into `target` (any size).
    void shade(const gpu::Texture &in, gpu::Texture &target, const json &sh, const FrameContext &ctx) {
        std::string rel = preset_path_for(sh.contains("preset") && sh["preset"].is_string()
                                              ? sh["preset"].get<std::string>()
                                              : std::string("glow_gauss"));
        CachedChain &c = chain_for(rel);
        apply_params(c, rel, sh.contains("params") ? sh["params"] : json());
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
            dst = tex(big_tex, kw, ih * k, gpu::Format::BGRA8).get();
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
            c.chain->frame(in, *dst, frame_count ? frame_count - 1 : 0, true);
            clear = false;
        }
        c.chain->frame(in, *dst, frame_count, clear);
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

    if (enabled) impl_->shade(in, target, sh, ctx);
    else impl_->bypass(in, target);

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
    for (auto &[k, c] : impl_->chains) c.last_frame = -1000;
}

}  // namespace ntscrt
