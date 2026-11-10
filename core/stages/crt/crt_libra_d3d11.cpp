// crt_libra_d3d11.cpp — librashader's Direct3D 11 runtime on the HAL device.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define LIBRA_RUNTIME_D3D11
#include <librashader.h>

#include <stdexcept>

#include "../../gpu/d3d11/d3d11_interop.h"
#include "crt_libra.h"

namespace ntscrt::crt {

namespace {

class D3D11Chain final : public Chain {
public:
    D3D11Chain(Library &lib, gpu::Device &dev, const std::string &preset_path) : lib_(lib), dev_(dev) {
        create_ = reinterpret_cast<PFN_libra_d3d11_filter_chain_create>(lib.sym("libra_d3d11_filter_chain_create"));
        frame_ = reinterpret_cast<PFN_libra_d3d11_filter_chain_frame>(lib.sym("libra_d3d11_filter_chain_frame"));
        set_param_ =
            reinterpret_cast<PFN_libra_d3d11_filter_chain_set_param>(lib.sym("libra_d3d11_filter_chain_set_param"));
        free_ = reinterpret_cast<PFN_libra_d3d11_filter_chain_free>(lib.sym("libra_d3d11_filter_chain_free"));
        if (!create_ || !frame_ || !set_param_ || !free_)
            throw std::runtime_error("librashader at " + lib.path + " lacks the D3D11 runtime");
        void *preset = lib.open_preset(preset_path, params_);
        libra_shader_preset_t p = static_cast<libra_shader_preset_t>(preset);
        auto *device = static_cast<ID3D11Device *>(dev.native_device());
        if (libra_error_t e = create_(&p, device, nullptr, &chain_)) {
            lib.free_preset(p);
            throw std::runtime_error("librashader d3d11_filter_chain_create(" + preset_path + "): " + lib.take_error(e));
        }
        // create consumes the preset on success.
    }
    ~D3D11Chain() override {
        if (chain_) free_(&chain_);
    }

    void frame(const gpu::Texture &in, gpu::Texture &out, uint64_t frame_count, bool clear_history) override {
        ID3D11ShaderResourceView *srv = gpu::d3d11::srv(in);
        ID3D11RenderTargetView *rtv = gpu::d3d11::rtv(out);
        if (!srv || !rtv) throw std::runtime_error("librashader frame: input needs an SRV and output a render target");
        libra_viewport_t vp{0.f, 0.f, uint32_t(out.desc().width), uint32_t(out.desc().height)};
        frame_d3d11_opt_t opt{};
        opt.version = LIBRASHADER_CURRENT_VERSION;
        opt.clear_history = true;
        opt.frame_direction = 1;
        opt.total_subframes = 1;
        opt.current_subframe = 1;
        opt.frames_per_second = 1.f;
        auto *ctx = static_cast<ID3D11DeviceContext *>(dev_.native_context());
        if (libra_error_t e = frame_(&chain_, ctx, size_t(frame_count), srv, rtv, &vp, nullptr,
                                     clear_history ? &opt : nullptr))
            throw std::runtime_error("librashader d3d11_filter_chain_frame: " + lib_.take_error(e));
    }

    void set_param(const std::string &name, float v) override {
        if (libra_error_t e = set_param_(&chain_, name.c_str(), v)) lib_.take_error(e);  // unknown name: ignore
    }

private:
    Library &lib_;
    gpu::Device &dev_;
    libra_d3d11_filter_chain_t chain_ = nullptr;
    PFN_libra_d3d11_filter_chain_create create_ = nullptr;
    PFN_libra_d3d11_filter_chain_frame frame_ = nullptr;
    PFN_libra_d3d11_filter_chain_set_param set_param_ = nullptr;
    PFN_libra_d3d11_filter_chain_free free_ = nullptr;
};

}  // namespace

std::unique_ptr<Chain> create_d3d11_chain(Library &lib, gpu::Device &dev, const std::string &preset_path) {
    return std::make_unique<D3D11Chain>(lib, dev, preset_path);
}

}  // namespace ntscrt::crt
