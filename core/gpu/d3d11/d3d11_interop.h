// d3d11_interop.h — views of HAL textures for code that talks to D3D11
// directly (librashader). Windows only.
#pragma once

#include <d3d11.h>

#include "../gpu.h"

namespace ntscrt::gpu::d3d11 {

// nullptr if `t` isn't a D3D11 HAL texture or lacks that view.
ID3D11ShaderResourceView *srv(const Texture &t);
ID3D11RenderTargetView *rtv(const Texture &t);  // TextureDesc::render_target
bool using_warp(const Device &d);

}  // namespace ntscrt::gpu::d3d11
