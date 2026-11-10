// crt_libra_metal.mm — librashader's Metal runtime on the HAL device, driven
// like upstream LibrashaderBridge.m. Compiled with -fobjc-arc.
#import <Metal/Metal.h>

#define LIBRA_RUNTIME_METAL
#include <librashader.h>

#include <stdexcept>

#include "../../gpu/metal/metal_interop.h"
#include "crt_libra.h"

namespace ntscrt::crt {

namespace {

class MetalChain final : public Chain {
public:
    MetalChain(Library &lib, gpu::Device &dev, const std::string &preset_path) : lib_(lib), dev_(dev) {
        create_ = reinterpret_cast<PFN_libra_mtl_filter_chain_create>(lib.sym("libra_mtl_filter_chain_create"));
        frame_ = reinterpret_cast<PFN_libra_mtl_filter_chain_frame>(lib.sym("libra_mtl_filter_chain_frame"));
        set_param_ = reinterpret_cast<PFN_libra_mtl_filter_chain_set_param>(lib.sym("libra_mtl_filter_chain_set_param"));
        free_ = reinterpret_cast<PFN_libra_mtl_filter_chain_free>(lib.sym("libra_mtl_filter_chain_free"));
        if (!create_ || !frame_ || !set_param_ || !free_)
            throw std::runtime_error("librashader at " + lib.path + " lacks the Metal runtime");
        void *preset = lib.open_preset(preset_path, params_);
        libra_shader_preset_t p = static_cast<libra_shader_preset_t>(preset);
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)gpu::metal::command_queue(dev);
        if (!queue) throw std::runtime_error("librashader: not a Metal HAL device");
        if (libra_error_t e = create_(&p, queue, nullptr, &chain_)) {
            lib.free_preset(p);
            throw std::runtime_error("librashader mtl_filter_chain_create(" + preset_path + "): " + lib.take_error(e));
        }
    }
    ~MetalChain() override {
        if (chain_) free_(&chain_);
    }

    void frame(const gpu::Texture &in, gpu::Texture &out, uint64_t frame_count, bool clear_history) override {
        id<MTLTexture> src = (__bridge id<MTLTexture>)in.native();
        id<MTLTexture> dst = (__bridge id<MTLTexture>)out.native();
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)dev_.native_context();
        libra_viewport_t vp{0.f, 0.f, uint32_t(out.desc().width), uint32_t(out.desc().height)};
        frame_mtl_opt_t opt{};
        opt.version = LIBRASHADER_CURRENT_VERSION;
        opt.clear_history = true;
        opt.frame_direction = 1;
        opt.total_subframes = 1;
        opt.current_subframe = 1;
        opt.frames_per_second = 1.f;
        if (libra_error_t e = frame_(&chain_, cb, size_t(frame_count), src, dst, &vp, nullptr,
                                     clear_history ? &opt : nullptr))
            throw std::runtime_error("librashader mtl_filter_chain_frame: " + lib_.take_error(e));
    }

    void set_param(const std::string &name, float v) override {
        if (libra_error_t e = set_param_(&chain_, name.c_str(), v)) lib_.take_error(e);
    }

private:
    Library &lib_;
    gpu::Device &dev_;
    libra_mtl_filter_chain_t chain_ = nullptr;
    PFN_libra_mtl_filter_chain_create create_ = nullptr;
    PFN_libra_mtl_filter_chain_frame frame_ = nullptr;
    PFN_libra_mtl_filter_chain_set_param set_param_ = nullptr;
    PFN_libra_mtl_filter_chain_free free_ = nullptr;
};

}  // namespace

std::unique_ptr<Chain> create_metal_chain(Library &lib, gpu::Device &dev, const std::string &preset_path) {
    return std::make_unique<MetalChain>(lib, dev, preset_path);
}

}  // namespace ntscrt::crt
