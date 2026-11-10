// gpu.h — minimal GPU HAL. CONTRACT (Wave 0). Owner: agent D (backends).
// Users: downscale, glitch compose, CRT (librashader), howlaround, bypass.
//
// All custom kernels are COMPUTE shaders written once in Slang
// (core/**/shaders/*.slang) and registered with the CMake function
// ntscrt_add_shaders(). The build emits HLSL + MSL text that is embedded and
// compiled at runtime (D3DCompile / newLibraryWithSource), mirroring upstream's
// inline-MSL approach.
//
// Binding convention in Slang (keep exact so both backends map identically):
//   ConstantBuffer<Params>  : register(b0)          (one per kernel, optional)
//   Texture2D  inputs       : register(t0..t7)
//   StructuredBuffer inputs : register(t8..t15)
//   RWTexture2D outputs     : register(u0..u7)
//   RWStructuredBuffer      : register(u8..u15)
//   SamplerState linear     : register(s0), point : register(s1)
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace ntscrt::gpu {

enum class Backend { D3D11, Metal };
enum class Format { BGRA8, RGBA8, RGBA16F, RGBA32F, R32F, RG32F };

struct TextureDesc {
    int width = 0, height = 0;
    Format format = Format::BGRA8;
    bool shader_write = true;    // UAV / MTLTextureUsageShaderWrite
    bool render_target = false;  // librashader output
};

class Texture {
public:
    virtual ~Texture() = default;
    virtual const TextureDesc &desc() const = 0;
    // ID3D11Texture2D* or id<MTLTexture> (bridged, not retained).
    virtual void *native() const = 0;
};

class Buffer {
public:
    virtual ~Buffer() = default;
    virtual size_t size() const = 0;
    virtual void *native() const = 0;  // ID3D11Buffer* / id<MTLBuffer>
};

class Kernel;  // opaque compiled compute pipeline

struct Bindings {
    const void *constants = nullptr;  // b0, copied at dispatch
    size_t constants_size = 0;
    std::vector<const Texture *> textures;      // t0..
    std::vector<const Buffer *> buffers;        // t8..
    std::vector<Texture *> out_textures;        // u0..
    std::vector<Buffer *> out_buffers;          // u8..
};

class Device {
public:
    // native_device/native_queue: see ntscrt_create_info. NULL = own device.
    static std::unique_ptr<Device> create(Backend backend, void *native_device, void *native_queue);
    virtual ~Device() = default;

    virtual Backend backend() const = 0;
    virtual std::shared_ptr<Texture> create_texture(const TextureDesc &d) = 0;
    virtual std::shared_ptr<Buffer> create_buffer(size_t bytes, const void *init = nullptr) = 0;

    virtual void upload(Texture &t, const void *data, ptrdiff_t row_bytes) = 0;
    virtual void download(const Texture &t, void *data, ptrdiff_t row_bytes) = 0;
    virtual void upload(Buffer &b, const void *data, size_t bytes, size_t offset = 0) = 0;

    // `name` = "<shader file stem>/<entry point>", e.g. "downscale/area_h".
    virtual Kernel *kernel(std::string_view name) = 0;
    // Threadgroup size comes from [numthreads] in the shader; groups = ceil(work/threads).
    virtual void dispatch(Kernel *k, const Bindings &b, int work_x, int work_y, int work_z = 1) = 0;
    virtual void copy(const Texture &src, Texture &dst) = 0;

    // Submit queued work and block until complete.
    virtual void finish() = 0;

    // For librashader: ID3D11Device* / ID3D11DeviceContext*, or id<MTLDevice> /
    // current id<MTLCommandBuffer> (begins one if needed).
    virtual void *native_device() const = 0;
    virtual void *native_context() = 0;
    // Metal only: adopt a host command buffer for the duration of a render.
    virtual void set_external_command_buffer(void *cmd) { (void)cmd; }
    // Wrap a host texture without copying (Metal FxPlug path).
    virtual std::shared_ptr<Texture> wrap_native_texture(void *native) = 0;
};

}  // namespace ntscrt::gpu
