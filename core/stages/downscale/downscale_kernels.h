// downscale_kernels.h — the Downscaler's GPU resampler, shared with the CRT
// stage (supersample integrate-down, ShaderBypass). Port of upstream
// Downscaler.encode.
#pragma once

#include <memory>
#include <string>

#include "../../gpu/gpu.h"
#include "../../src/internal.h"

namespace ntscrt {

enum class DownscaleMethod { Nearest, NearestAA, Bilinear, Bicubic, Lanczos, Area };

// Upstream raw values: "nearest", "nearest+", "bilinear", "bicubic",
// "lanczos", "area". Unknown -> Nearest.
DownscaleMethod parse_downscale_method(const std::string &s);

// Upstream AppState.downscaleHeight: height follows the source aspect,
// rounded to even lines, at least 16.
int downscale_height(int width, int src_w, int src_h);

// Resamples src into dst (any sizes; dst needs shader_write). The separable
// methods keep a (dst.w x src.h) scratch texture in `scratch`, reused while
// its size/format match.
void downscale_encode(gpu::Device &dev, const gpu::Texture &src, gpu::Texture &dst, DownscaleMethod m,
                      std::shared_ptr<gpu::Texture> &scratch);

}  // namespace ntscrt
