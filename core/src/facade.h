// facade.h — what the C ABI entry points outside ntscrt.cpp (host_gpu.cpp) need
// from the façade: the shared frame renderer and the instance's GPU state.
#pragma once

#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>

#include "../gpu/gpu.h"
#include "../stages/digital/parallel.h"
#include "internal.h"
#include "phase_times.h"
#include "pixel_convert.h"

namespace ntscrt::hostgpu {
class Bridge;
}

namespace ntscrt::facade {

// get_src: fill `raw` (BGRA8, opaque, prepared for `mode`: to_bgra8) and
//          `alpha` (source alpha per pixel, empty = opaque).
// emit:    deliver the result: `out_gpu` (BGRA8 / RGBA16F, out_w x out_h) when a
//          GPU device exists, else `cpu_img` (CPU stages only, out_w x out_h).
//          `alpha` is NULL when it doesn't line up with the result (crop on).
//          raw_w x raw_h: the source picture's size.
using SourceFn = std::function<void(ImageBGRA8 &raw, std::vector<float> &alpha, AlphaMode mode)>;
using EmitFn = std::function<void(const std::shared_ptr<gpu::Texture> &out_gpu, const ImageBGRA8 &cpu_img,
                                  int raw_w, int raw_h, const std::vector<float> *alpha, AlphaMode mode)>;
// A texture of `desc` from the renderer's per-instance pool (reused across frames).
using PoolFn = std::function<std::shared_ptr<gpu::Texture>(const gpu::TextureDesc &desc)>;

// The source put straight on the GPU, for frames no CPU stage touches (Digital
// video, ntsc-rs and Screen Loop off): no CPU picture in between. Two kinds:
// - the CPU path (ntscrt_render_cpu, unless NTSCRT_FAST_TRANSFER=0): the host's
//   CPU picture converted straight into the upload memory of a pooled texture;
//   `crops` false, so it's only used when the look's crop cuts nothing;
// - the host GPU path (ntscrt_render_host_gpu): the host's GPU frame converted by
//   a kernel on the core's device, cut to the look's crop there (`crops` true).
// make() returns the picture (BGRA8, shader-readable, the crop's size, converted
// for `mode` as get_src + Crop would), filling `alpha` if emit needs it from
// there; nullptr when it can't (Clip mode with transparent pixels needs
// to_bgra8's hole filling): the renderer then calls get_src.
struct DirectSrc {
    int w = 0, h = 0;    // the source picture's size, before the crop
    bool crops = false;  // make() applies the look's crop itself
    std::function<std::shared_ptr<gpu::Texture>(gpu::Device &dev, AlphaMode mode, const json &look,
                                                std::vector<float> &alpha, const PoolFn &pool)>
        make;
};

ntscrt_status render(ntscrt_instance *inst, const ntscrt_render_args *args, int out_w, int out_h,
                     gpu::Format out_fmt, const SourceFn &get_src, const EmitFn &emit,
                     const DirectSrc *direct = nullptr);

// The CRT output format for a host picture of high (16-bit / float) or 8-bit precision.
gpu::Format output_format(bool high_precision_host);

// Transfers (both paths): false with NTSCRT_FAST_TRANSFER=0 (0.5.0-rc.3's way:
// upload() / download() through CPU buffers, no direct CPU source).
bool fast_transfer();
// Bands for gpu::Device::read of `bytes` (NTSCRT_READBACK_BANDS overrides).
int readback_bands(size_t bytes);
// fn(y) for each of h rows of row_bytes: on the thread pool when large
// (blocks of rows, so each task is a decent memcpy).
template <class Fn> void for_row_blocks(int h, size_t row_bytes, Fn &&fn) {
    if (size_t(h) * row_bytes < (size_t(1) << 20)) {
        for (int y = 0; y < h; ++y) fn(y);
        return;
    }
    constexpr int kRows = 8;
    digital::parallel_for((h + kRows - 1) / kRows, [&](int b) {
        for (int y = b * kRows, e = (std::min)(h, y + kRows); y < e; ++y) fn(y);
    });
}

ntscrt_status fail(ntscrt_instance *inst, ntscrt_status s, std::string msg);
gpu::Device *device(ntscrt_instance *inst);  // nullptr: no GPU
std::unique_ptr<hostgpu::Bridge> &bridge(ntscrt_instance *inst);
PhaseTimes &times(ntscrt_instance *inst);

}  // namespace ntscrt::facade
