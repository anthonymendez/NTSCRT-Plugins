// d3d11_interop.h — views of HAL textures for code that talks to D3D11
// directly (librashader). Windows only.
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string_view>
#include <vector>

#include "../gpu.h"

namespace ntscrt::gpu::d3d11 {

// nullptr if `t` isn't a D3D11 HAL texture or lacks that view.
ID3D11ShaderResourceView *srv(const Texture &t);
ID3D11RenderTargetView *rtv(const Texture &t);  // TextureDesc::render_target
bool using_warp(const Device &d);
// The bytecode D3DCompile makes of kernel `name` ("<stem>/<entry>") here and now:
// what the device compiles when the build left no bytecode (tests compare it with
// the build's). Throws if unknown or the compiler fails.
std::vector<uint8_t> runtime_kernel_bytecode(std::string_view name);

}  // namespace ntscrt::gpu::d3d11
