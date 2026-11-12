// gpu_route.cpp — see gpu_route.h.
#include "gpu_route.h"

#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace ntscrt {

std::string gpu_route_env_text(const char *var) {
    const char *e = std::getenv(var);
    return e ? e : "";
}

GpuRouteEnv gpu_route_env(const char *var) {
    const std::string e = gpu_route_env_text(var);
    if (e == "0") return GpuRouteEnv::Off;
    if (e == "force") return GpuRouteEnv::Force;
    if (e.empty() || e == "auto") return GpuRouteEnv::Auto;
    return GpuRouteEnv::On;
}

std::string gpu_route_env_label(const char *var, GpuRouteEnv env) {
    switch (env) {
    case GpuRouteEnv::Auto: return "auto";
    case GpuRouteEnv::Force: return std::string(var) + "=force";
    case GpuRouteEnv::Off: return std::string(var) + "=0";
    default: return std::string(var) + "=1";
    }
}

#ifdef _WIN32
namespace {
bool adapter_desc(gpu::Device &dev, DXGI_ADAPTER_DESC &desc) {
    if (dev.backend() != gpu::Backend::D3D11) return false;
    auto *d = static_cast<ID3D11Device *>(dev.native_device());
    if (!d) return false;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
    return SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&ad)) &&
           SUCCEEDED(ad->GetDesc(&desc));
}
}  // namespace
#endif

std::string gpu_adapter_description(gpu::Device &dev) {
#ifdef _WIN32
    DXGI_ADAPTER_DESC desc{};
    if (!adapter_desc(dev, desc)) return "";
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, int(sizeof buf), nullptr, nullptr);
    return buf;
#else
    (void)dev;
    return "";
#endif
}

bool gpu_is_software(gpu::Device &dev) {
#ifdef _WIN32
    DXGI_ADAPTER_DESC desc{};
    // Microsoft Basic Render Driver (WARP): vendor 0x1414, device 0x8c.
    return adapter_desc(dev, desc) && desc.VendorId == 0x1414 && desc.DeviceId == 0x8c;
#else
    (void)dev;
    return false;
#endif
}

}  // namespace ntscrt
