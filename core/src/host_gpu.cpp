// host_gpu.cpp — ntscrt_render_host_gpu and its helpers (ntscrt.h, docs/dev/HOST-GPU.md).
//
// Where the frame is at each step (CUDA host, Windows):
//   host's CUDA buffer --cuMemcpy2DAsync (GPU->GPU)--> registered D3D11 texture (host layout)
//   --hostgpu_in/convert_in--> BGRA8 picture (cropped)
//     no CPU stage on ("gpu-direct"):  Downscale -> Glitch -> CRT, all on the GPU
//     ntsc-rs / Digital / Screen Loop on ("gpu-download"): the BGRA8 picture comes down
//       (a quarter of a float frame's bytes), the CPU stages run, it goes up again
//     Clip mode with transparent pixels ("gpu-cpuconvert"): the float frame comes down
//       and to_bgra8 fills the holes as on the CPU path
//   CRT result + source alpha --hostgpu_out/merge_out--> registered D3D11 texture (host
//   layout and format) --cuMemcpy2DAsync--> host's CUDA buffer.
#include "host_gpu.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "../stages/crop/crop_internal.h"
#include "facade.h"

namespace ntscrt::hostgpu {

#ifdef _WIN32
std::unique_ptr<Bridge> make_cuda_bridge(void *stream, std::string &err);
bool cuda_available();
#endif
#ifdef __APPLE__
std::unique_ptr<Bridge> make_metal_bridge(void *queue, std::string &err);
#endif

bool hal_format(ntscrt_pixel_format f, gpu::Format &out) {
    switch (f) {
    case NTSCRT_RGBA32F: out = gpu::Format::RGBA32F; return true;
    case NTSCRT_RGBA16F: out = gpu::Format::RGBA16F; return true;
    case NTSCRT_RGBA8: out = gpu::Format::RGBA8; return true;
    default: return false;  // BGRA8 (OpenFX never sends it), RGBA16 unorm (no HAL format)
    }
}

bool supported(ntscrt_host_gpu_api api) {
#ifdef _WIN32
    return api == NTSCRT_HOST_GPU_CUDA && cuda_available();
#elif defined(__APPLE__)
    return api == NTSCRT_HOST_GPU_METAL;
#else
    (void)api;
    return false;
#endif
}

std::unique_ptr<Bridge> make_bridge(const ntscrt_host_gpu &host, std::string &err) {
    switch (host.api) {
#ifdef _WIN32
    case NTSCRT_HOST_GPU_CUDA: return make_cuda_bridge(host.queue, err);
#endif
#ifdef __APPLE__
    case NTSCRT_HOST_GPU_METAL: return make_metal_bridge(host.queue, err);
#endif
    case NTSCRT_HOST_GPU_OPENCL:
        err = "OpenCL host frames aren't supported (an OpenCL context the host made can't share the core's "
              "Direct3D / Metal textures; docs/dev/HOST-GPU.md)";
        return nullptr;
    default: err = "this build has no host GPU path for this API"; return nullptr;
    }
}

namespace {

size_t pixel_bytes(ntscrt_pixel_format f) {
    switch (f) {
    case NTSCRT_RGBA32F: return 16;
    case NTSCRT_RGBA16F:
    case NTSCRT_RGBA16: return 8;
    default: return 4;
    }
}

bool valid(const ntscrt_gpu_image *im) {
    gpu::Format f;
    return im && im->data && im->width > 0 && im->height > 0 && hal_format(im->format, f) &&
           im->row_bytes >= ptrdiff_t(size_t(im->width) * pixel_bytes(im->format));
}

// The bridge for `host` on this instance: the existing one if it serves the
// same context, else a new one. nullptr + err.
Bridge *bridge_for(ntscrt_instance *inst, const ntscrt_host_gpu &host, std::string &err) {
    auto &br = facade::bridge(inst);
    if (br && br->rebind(host)) return br.get();
    br.reset();
    br = make_bridge(host, err);
    return br.get();
}

// Layout of InParams in hostgpu_in.slang / hostgpu_flags.slang.
struct InParams {
    uint32_t frame_w, frame_h, src_w, src_h;
    int32_t src_x, src_y, bottom_up, mode;
    uint32_t cx, cy, cw, ch;
    uint32_t premult, pad0, pad1, pad2;
};
// Layout of OutParams in hostgpu_out.slang.
struct OutParams {
    uint32_t dst_w, dst_h, src_w, src_h;
    int32_t src_x, src_y, bottom_up, mode;
    uint32_t use_alpha, premult, pad0, pad1;
};

}  // namespace
}  // namespace ntscrt::hostgpu

using namespace ntscrt;
using namespace ntscrt::hostgpu;

extern "C" {

int32_t ntscrt_host_gpu_supported(ntscrt_host_gpu_api api) { return supported(api) ? 1 : 0; }

ntscrt_status ntscrt_host_gpu_read(ntscrt_instance *inst, const ntscrt_host_gpu *host, const ntscrt_gpu_image *src,
                                   ntscrt_image *dst) {
    if (!inst || !host || !valid(src) || !dst || !dst->data || dst->width != src->width ||
        dst->height != src->height || dst->format != src->format)
        return NTSCRT_ERR_INVALID_ARG;
    std::string err;
    try {
        Bridge *br = bridge_for(inst, *host, err);
        if (!br) return facade::fail(inst, NTSCRT_ERR_HOST_GPU, err);
        br->read(*src, dst->data, dst->row_bytes);
        return NTSCRT_OK;
    } catch (const std::exception &e) {
        return facade::fail(inst, NTSCRT_ERR_HOST_GPU, e.what());
    }
}

ntscrt_status ntscrt_host_gpu_write(ntscrt_instance *inst, const ntscrt_host_gpu *host, const ntscrt_image *src,
                                    const ntscrt_gpu_image *dst) {
    if (!inst || !host || !valid(dst) || !src || !src->data || dst->width != src->width ||
        dst->height != src->height || dst->format != src->format)
        return NTSCRT_ERR_INVALID_ARG;
    std::string err;
    try {
        Bridge *br = bridge_for(inst, *host, err);
        if (!br) return facade::fail(inst, NTSCRT_ERR_HOST_GPU, err);
        br->write(src->data, src->row_bytes, *dst);
        return NTSCRT_OK;
    } catch (const std::exception &e) {
        return facade::fail(inst, NTSCRT_ERR_HOST_GPU, e.what());
    }
}

ntscrt_status ntscrt_render_host_gpu(ntscrt_instance *inst, const ntscrt_render_args *args,
                                     const ntscrt_host_gpu *host, const ntscrt_gpu_image *src, int32_t src_x,
                                     int32_t src_y, const ntscrt_gpu_image *dst) {
    if (!inst || !args || !host || !valid(src) || !valid(dst)) return NTSCRT_ERR_INVALID_ARG;
    PhaseTimes &pt = facade::times(inst);
    pt.clear("gpu-direct");
    struct Done {
        PhaseTimes &t;
        ~Done() { t.finish(); }
    } done{pt};

    std::string err;
    gpu::Device *dev0 = facade::device(inst);
    if (!dev0) return facade::fail(inst, NTSCRT_ERR_HOST_GPU, "the core has no GPU device");
    if (!supported(host->api)) {
        make_bridge(*host, err);
        return facade::fail(inst, NTSCRT_ERR_HOST_GPU, err.empty() ? "host GPU API not available" : err);
    }
    try {
        const bool had = facade::bridge(inst) != nullptr;
        Bridge *br = bridge_for(inst, *host, err);
        if (!br) return facade::fail(inst, NTSCRT_ERR_HOST_GPU, err);
        if (!had || facade::bridge(inst).get() != br) pt.note = " new_bridge=\"" + br->describe() + "\"";
        if (std::string why = br->check(*dev0); !why.empty()) return facade::fail(inst, NTSCRT_ERR_HOST_GPU, why);
    } catch (const std::exception &e) {
        return facade::fail(inst, NTSCRT_ERR_HOST_GPU, e.what());
    }

    const int fw = dst->width, fh = dst->height;
    const bool premult = args->source_premultiplied != 0;
    // State of this call, rebuilt if the device changes (lost and replaced).
    gpu::Device *imported_on = nullptr;
    std::shared_ptr<gpu::Texture> src_tex;
    std::optional<bool> transparent;  // Clip mode: does the frame have transparent pixels?
    bool wrote = false;               // dst written (the frame is the result, whatever the status)

    auto bridge = [&]() -> Bridge & {
        std::string e;
        Bridge *b = bridge_for(inst, *host, e);
        if (!b) throw std::runtime_error(e);
        return *b;
    };
    auto source = [&](gpu::Device &dev) {
        if (!src_tex || imported_on != &dev) {
            src_tex = bridge().import(dev, *src);
            imported_on = &dev;
            transparent.reset();
        }
        return src_tex;
    };
    auto in_params = [&](AlphaMode mode) {
        InParams p{};
        p.frame_w = uint32_t(fw);
        p.frame_h = uint32_t(fh);
        p.src_w = uint32_t(src->width);
        p.src_h = uint32_t(src->height);
        p.src_x = src_x;
        p.src_y = src_y;
        p.bottom_up = src->bottom_up ? 1 : 0;
        p.mode = mode == AlphaMode::Spill ? 1 : 0;
        p.cw = uint32_t(fw);
        p.ch = uint32_t(fh);
        p.premult = premult ? 1 : 0;
        return p;
    };
    // Clip mode needs to_bgra8's hole filling when some pixel is transparent.
    auto needs_fill = [&](gpu::Device &dev, AlphaMode mode) {
        if (mode != AlphaMode::Clip) return false;
        if (!transparent) {
            InParams p = in_params(mode);
            const int fx = (fw + 63) / 64;  // one flag per 64 pixels of a row
            auto flags = bridge().scratch(dev, {fx, fh, gpu::Format::R32F, true, false});
            gpu::Bindings b;
            b.constants = &p;
            b.constants_size = sizeof p;
            auto s = source(dev);
            b.textures = {s.get()};
            b.out_textures = {flags.get()};
            dev.dispatch(dev.kernel("hostgpu_flags/row_flags"), b, fx, fh, 1);
            std::vector<float> rows(size_t(fx) * size_t(fh));
            dev.download(*flags, rows.data(), ptrdiff_t(fx) * 4);
            transparent = std::any_of(rows.begin(), rows.end(), [](float f) { return f != 0.f; });
        }
        return *transparent;
    };
    auto convert = [&](gpu::Device &dev, AlphaMode mode, const crop::Rect &r) {
        InParams p = in_params(mode);
        p.cx = uint32_t(r.x);
        p.cy = uint32_t(r.y);
        p.cw = uint32_t(r.width);
        p.ch = uint32_t(r.height);
        auto pic = bridge().scratch(dev, {r.width, r.height, gpu::Format::BGRA8, true, false});
        gpu::Bindings b;
        b.constants = &p;
        b.constants_size = sizeof p;
        auto s = source(dev);
        b.textures = {s.get()};
        b.out_textures = {pic.get()};
        dev.dispatch(dev.kernel("hostgpu_in/convert_in"), b, r.width, r.height, 1);
        return pic;
    };

    // No CPU stage on: the frame stays on the GPU, converted and cropped there.
    const facade::DirectSrc direct{
        fw, fh, true,
        [&](gpu::Device &dev, AlphaMode mode, const json &look, std::vector<float> &alpha,
            const facade::PoolFn &) -> std::shared_ptr<gpu::Texture> {
            if (needs_fill(dev, mode)) return nullptr;
            alpha.clear();  // merge_out reads alpha from the source texture
            crop::Rect r{0, 0, fw, fh};
            if (auto c = crop::from_look(look)) r = c->rect(fw, fh);
            return convert(dev, mode, r);
        }};

    facade::SourceFn get_src = [&](ImageBGRA8 &raw, std::vector<float> &alpha, AlphaMode mode) {
        gpu::Device *dev = facade::device(inst);
        if (dev && !needs_fill(*dev, mode)) {
            // The 8-bit picture the CPU stages work on, made on the GPU: the only download.
            pt.path = "gpu-download";
            auto pic = convert(*dev, mode, crop::Rect{0, 0, fw, fh});
            raw.width = fw;
            raw.height = fh;
            raw.px.resize(size_t(fw) * fh * 4);  // (the last frame's buffer, with fast transfers)
            alpha.clear();                       // merge_out reads alpha from the source texture
            if (facade::fast_transfer()) {
                // Rows copied out of the readback memory band by band, on the thread
                // pool, while the GPU copies the next band.
                const size_t tight = size_t(fw) * 4;
                dev->read(
                    *pic,
                    [&](const uint8_t *rows, ptrdiff_t pitch, int y0, int y1) {
                        facade::for_row_blocks(y1 - y0, tight, [&](int y) {
                            std::memcpy(raw.px.data() + size_t(y0 + y) * tight, rows + ptrdiff_t(y) * pitch, tight);
                        });
                    },
                    facade::readback_bands(tight * size_t(fh)));
            } else {
                dev->download(*pic, raw.px.data(), ptrdiff_t(fw) * 4);
            }
            return;
        }
        // Holes to fill (or no GPU any more): the frame as the CPU path gets it.
        pt.path = "gpu-cpuconvert";
        const size_t pb = pixel_bytes(src->format);
        const int x1 = std::max(0, src_x), x2 = std::min(fw, src_x + src->width);
        std::vector<uint8_t> frame(size_t(fw) * fh * pb, 0);
        // Where memory row `my` of the source goes in the (top-down) frame, or -1.
        auto frame_row = [&](int my) {
            const int fmy = src_y + my;  // ... in the frame's memory order
            if (fmy < 0 || fmy >= fh || x1 >= x2) return -1;
            return src->bottom_up ? fh - 1 - fmy : fmy;
        };
        if (dev && facade::fast_transfer() && src_tex && imported_on == dev) {
            // The source texture needs_fill imported, read band by band straight
            // into its place in the frame (texture rows are in memory order).
            dev->read(
                *src_tex,
                [&](const uint8_t *rows, ptrdiff_t pitch, int y0, int y1) {
                    facade::for_row_blocks(y1 - y0, size_t(src->width) * pb, [&](int y) {
                        const int fy = frame_row(y0 + y);
                        if (fy < 0) return;
                        std::memcpy(frame.data() + (size_t(fy) * fw + x1) * pb,
                                    rows + ptrdiff_t(y) * pitch + ptrdiff_t(x1 - src_x) * ptrdiff_t(pb),
                                    size_t(x2 - x1) * pb);
                    });
                },
                facade::readback_bands(size_t(src->width) * src->height * pb));
            ntscrt_image im{fw, fh, ptrdiff_t(size_t(fw) * pb), src->format, frame.data()};
            to_bgra8(im, raw, &alpha, mode, premult);
            return;
        }
        std::vector<uint8_t> s(size_t(src->width) * src->height * pb);
        bridge().read(*src, s.data(), ptrdiff_t(size_t(src->width) * pb));  // top row first
        for (int ty = 0; ty < src->height; ++ty) {
            const int my = src->bottom_up ? src->height - 1 - ty : ty;  // the row's memory index
            const int fy = frame_row(my);                               // top-down frame row
            if (fy < 0) continue;
            std::memcpy(frame.data() + (size_t(fy) * fw + x1) * pb, s.data() + (size_t(ty) * src->width + (x1 - src_x)) * pb,
                        size_t(x2 - x1) * pb);
        }
        ntscrt_image im{fw, fh, ptrdiff_t(size_t(fw) * pb), src->format, frame.data()};
        to_bgra8(im, raw, &alpha, mode, premult);
    };

    facade::EmitFn emit = [&](const std::shared_ptr<gpu::Texture> &out, const ImageBGRA8 &cpu_img, int raw_w,
                              int raw_h, const std::vector<float> *alpha, AlphaMode mode) {
        if (!out) {
            // The GPU was lost: CPU stages only; the result goes up from the CPU.
            const size_t pb = pixel_bytes(dst->format);
            std::vector<uint8_t> px(size_t(fw) * fh * pb);
            ntscrt_image d{fw, fh, ptrdiff_t(size_t(fw) * pb), dst->format, px.data()};
            from_bgra8(cpu_img, alpha, d, mode, premult);
            bridge().write(px.data(), d.row_bytes, *dst);
            wrote = true;
            return;
        }
        gpu::Device &dev = *facade::device(inst);
        Bridge &br = bridge();
        auto target = br.target(dev, *dst);
        OutParams q{};
        q.dst_w = uint32_t(fw);
        q.dst_h = uint32_t(fh);
        q.src_w = uint32_t(src->width);
        q.src_h = uint32_t(src->height);
        q.src_x = src_x;
        q.src_y = src_y;
        q.bottom_up = dst->bottom_up ? 1 : 0;
        q.mode = mode == AlphaMode::Spill ? 1 : 0;
        q.use_alpha = alpha && raw_w == fw && raw_h == fh ? 1 : 0;
        q.premult = premult ? 1 : 0;
        gpu::Bindings b;
        b.constants = &q;
        b.constants_size = sizeof q;
        auto s = source(dev);
        b.textures = {out.get(), s.get()};
        b.out_textures = {target.get()};
        dev.dispatch(dev.kernel("hostgpu_out/merge_out"), b, fw, fh, 1);
        br.export_(dev, *dst);
        wrote = true;
    };

    const gpu::Format of = facade::output_format(dst->format != NTSCRT_RGBA8);
    ntscrt_status st = facade::render(inst, args, fw, fh, of, get_src, emit, &direct);
    if (st == NTSCRT_ERR_INTERNAL && !wrote) {
        // A driver / interop failure before the result was written: the host can still
        // render this frame on the CPU path.
        const std::string why = ntscrt_last_error(inst);
        facade::bridge(inst).reset();
        return facade::fail(inst, NTSCRT_ERR_HOST_GPU, "host GPU render failed: " + why);
    }
    return st;
}

}  // extern "C"
