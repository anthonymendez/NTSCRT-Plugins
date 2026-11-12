// facade.h — what the C ABI entry points outside ntscrt.cpp (host_gpu.cpp) need
// from the façade: the shared frame renderer and the instance's GPU state.
#pragma once

#include <ntscrt/ntscrt.h>

#include <functional>
#include <memory>
#include <string>

#include "../gpu/gpu.h"
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
// The GPU path's source when no CPU stage needs the picture: the frame already
// on the core's device, converted for `mode` and cut to `look`'s crop (BGRA8,
// shader-readable); raw_w x raw_h = the frame's size before the crop. nullptr:
// can't (Clip mode with transparent pixels needs to_bgra8's hole filling), the
// renderer then calls get_src.
using DirectFn = std::function<std::shared_ptr<gpu::Texture>(AlphaMode mode, const json &look, int &raw_w,
                                                             int &raw_h)>;

ntscrt_status render(ntscrt_instance *inst, const ntscrt_render_args *args, int out_w, int out_h,
                     gpu::Format out_fmt, const SourceFn &get_src, const EmitFn &emit,
                     const DirectFn *direct = nullptr);

// The CRT output format for a host picture of high (16-bit / float) or 8-bit precision.
gpu::Format output_format(bool high_precision_host);

ntscrt_status fail(ntscrt_instance *inst, ntscrt_status s, std::string msg);
gpu::Device *device(ntscrt_instance *inst);  // nullptr: no GPU
std::unique_ptr<hostgpu::Bridge> &bridge(ntscrt_instance *inst);
PhaseTimes &times(ntscrt_instance *inst);

}  // namespace ntscrt::facade
