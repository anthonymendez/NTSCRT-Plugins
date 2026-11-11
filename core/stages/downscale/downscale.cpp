// Downscale stage — port of upstream Sources/CrtCore/Downscaler.swift.
// Look key "downscale": {"enabled": bool, "method": "...", "width": int}.
// Width-only: the height follows the chain input's aspect (AppState
// downscaleHeight). Defaults follow upstream AppState: enabled, 320, nearest.
// With ctx.line_height 2 (One field) it gives the chain the field's lines.
#include <algorithm>
#include <cmath>

#include "../stages.h"
#include "downscale_kernels.h"
#include "../digital/fields.h"

namespace ntscrt {

DownscaleMethod parse_downscale_method(const std::string &s) {
    if (s == "nearest+") return DownscaleMethod::NearestAA;
    if (s == "bilinear") return DownscaleMethod::Bilinear;
    if (s == "bicubic") return DownscaleMethod::Bicubic;
    if (s == "lanczos") return DownscaleMethod::Lanczos;
    if (s == "area") return DownscaleMethod::Area;
    return DownscaleMethod::Nearest;
}

int downscale_height(int width, int src_w, int src_h) {
    double aspect = std::max(0.05, src_h > 0 ? double(src_w) / double(src_h) : 1.0);
    // Swift .rounded() = schoolbook rounding (half away from zero) = std::round.
    return std::max(16, 2 * int(std::round(double(width) / aspect / 2.0)));
}

void downscale_encode(gpu::Device &dev, const gpu::Texture &src, gpu::Texture &dst, DownscaleMethod m,
                      std::shared_ptr<gpu::Texture> &scratch) {
    const auto &s = src.desc(), &d = dst.desc();
    uint32_t dims[4] = {uint32_t(s.width), uint32_t(s.height), uint32_t(d.width), uint32_t(d.height)};
    gpu::Bindings b;
    b.constants = dims;
    b.constants_size = sizeof dims;
    if (m == DownscaleMethod::Nearest || m == DownscaleMethod::Area) {
        b.textures = {&src};
        b.out_textures = {&dst};
        dev.dispatch(dev.kernel(m == DownscaleMethod::Area ? "downscale/area" : "downscale/nearest"), b, d.width,
                     d.height);
        return;
    }
    const char *base = m == DownscaleMethod::NearestAA ? "downscale/nearestaa"
                     : m == DownscaleMethod::Bilinear  ? "downscale/bilinear"
                     : m == DownscaleMethod::Bicubic   ? "downscale/bicubic"
                                                       : "downscale/lanczos";
    if (!scratch || scratch->desc().width != d.width || scratch->desc().height != s.height ||
        scratch->desc().format != d.format)
        scratch = dev.create_texture({d.width, s.height, d.format, true, false});
    b.textures = {&src};
    b.out_textures = {scratch.get()};
    dev.dispatch(dev.kernel(std::string(base) + "_h"), b, d.width, s.height);
    b.textures = {scratch.get()};
    b.out_textures = {&dst};
    dev.dispatch(dev.kernel(std::string(base) + "_v"), b, d.width, d.height);
}

struct Downscaler::Impl {
    gpu::Device *dev;
    std::shared_ptr<gpu::Texture> scratch, out;
};

Downscaler::Downscaler(gpu::Device &dev) : impl_(std::make_unique<Impl>(Impl{&dev, nullptr, nullptr})) {}
Downscaler::~Downscaler() = default;

std::shared_ptr<gpu::Texture> Downscaler::run(const std::shared_ptr<gpu::Texture> &in, const FrameContext &ctx) {
    static const json empty = json::object();
    const json &look = ctx.look ? *ctx.look : empty;
    const json ds = look.is_object() && look.contains("downscale") && look["downscale"].is_object() ? look["downscale"]
                                                                                                    : json::object();
    if (!in || !ds.value("enabled", true)) return in;
    int width = 320;
    if (ds.contains("width") && ds["width"].is_number()) width = int(std::lround(ds["width"].get<double>()));
    width = std::clamp(width, 1, 16384);
    std::string method = ds.contains("method") && ds["method"].is_string() ? ds["method"].get<std::string>() : "nearest";
    const auto &sd = in->desc();
    // One field (Digital video): half the lines, each two rows tall.
    int height = digital::field_lines(downscale_height(width, sd.width, sd.height), ctx.line_height);

    // Reuse the output while nobody else still holds the previous one.
    auto &o = impl_->out;
    if (!o || o.use_count() > 1 || o->desc().width != width || o->desc().height != height ||
        o->desc().format != sd.format)
        o = impl_->dev->create_texture({width, height, sd.format, true, false});
    downscale_encode(*impl_->dev, *in, *o, parse_downscale_method(method), impl_->scratch);
    return o;
}

}  // namespace ntscrt
