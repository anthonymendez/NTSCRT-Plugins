// metal_device.mm — Metal HAL backend (macOS). Compiled with -fobjc-arc.
//
// Own MTLDevice/queue by default, or the host's (FxPlug). Work is encoded on
// one "current" command buffer: the device's own (committed by finish()) or
// a host buffer adopted with set_external_command_buffer() (the host commits
// it; finish() then only ends our use of it). Kernels: the embedded MSL of a
// shader file is compiled once with newLibraryWithSource.
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "../backend.h"
#include "metal_interop.h"

namespace ntscrt::gpu {
namespace {

MTLPixelFormat mtl_format(Format f) {
    switch (f) {
    case Format::BGRA8: return MTLPixelFormatBGRA8Unorm;
    case Format::RGBA8: return MTLPixelFormatRGBA8Unorm;
    case Format::RGBA16F: return MTLPixelFormatRGBA16Float;
    case Format::RGBA32F: return MTLPixelFormatRGBA32Float;
    case Format::R32F: return MTLPixelFormatR32Float;
    case Format::RG32F: return MTLPixelFormatRG32Float;
    }
    return MTLPixelFormatInvalid;
}

bool from_mtl(MTLPixelFormat p, Format &f) {
    switch (p) {
    case MTLPixelFormatBGRA8Unorm: f = Format::BGRA8; return true;
    case MTLPixelFormatRGBA8Unorm: f = Format::RGBA8; return true;
    case MTLPixelFormatRGBA16Float: f = Format::RGBA16F; return true;
    case MTLPixelFormatRGBA32Float: f = Format::RGBA32F; return true;
    case MTLPixelFormatR32Float: f = Format::R32F; return true;
    case MTLPixelFormatRG32Float: f = Format::RG32F; return true;
    default: return false;
    }
}

std::string ns(NSError *e) {
    return e ? std::string([[e localizedDescription] UTF8String] ?: "unknown error") : std::string("unknown error");
}

class MtlTexture final : public Texture {
public:
    TextureDesc d;
    id<MTLTexture> tex = nil;
    const TextureDesc &desc() const override { return d; }
    void *native() const override { return (__bridge void *)tex; }
};

class MtlBuffer final : public Buffer {
public:
    size_t bytes = 0;
    id<MTLBuffer> buf = nil;
    size_t size() const override { return bytes; }
    void *native() const override { return (__bridge void *)buf; }
};

class MtlKernel final : public Kernel {
public:
    id<MTLComputePipelineState> pso = nil;
};

class MetalDevice final : public Device {
public:
    MetalDevice(id<MTLDevice> dev, id<MTLCommandQueue> queue) : dev_(dev), queue_(queue) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
        linear_ = [dev_ newSamplerStateWithDescriptor:sd];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        point_ = [dev_ newSamplerStateWithDescriptor:sd];
        if (!linear_ || !point_) throw std::runtime_error("Metal: cannot create samplers");
    }

    ~MetalDevice() override {
        if (!external_ && cb_) {
            [cb_ commit];
            [cb_ waitUntilCompleted];
        }
    }

    Backend backend() const override { return Backend::Metal; }
    id<MTLCommandQueue> queue() const { return queue_; }

    std::shared_ptr<Texture> create_texture(const TextureDesc &d) override {
        if (d.width <= 0 || d.height <= 0) throw std::runtime_error("create_texture: empty size");
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_format(d.format)
                                                                                      width:NSUInteger(d.width)
                                                                                     height:NSUInteger(d.height)
                                                                                  mipmapped:NO];
        MTLTextureUsage u = MTLTextureUsageShaderRead;
        if (d.shader_write) u |= MTLTextureUsageShaderWrite;
        if (d.render_target) u |= MTLTextureUsageRenderTarget;
        td.usage = u;
        td.storageMode = MTLStorageModePrivate;
        auto t = std::make_shared<MtlTexture>();
        t->d = d;
        t->tex = [dev_ newTextureWithDescriptor:td];
        if (!t->tex) throw std::runtime_error("Metal: texture allocation failed");
        return t;
    }

    std::shared_ptr<Buffer> create_buffer(size_t bytes, const void *init) override {
        auto b = std::make_shared<MtlBuffer>();
        b->bytes = bytes;
        b->buf = [dev_ newBufferWithLength:std::max<size_t>(bytes, 16) options:MTLResourceStorageModePrivate];
        if (!b->buf) throw std::runtime_error("Metal: buffer allocation failed");
        if (init) upload(*b, init, bytes, 0);
        return b;
    }

    void upload(Texture &t, const void *data, ptrdiff_t row_bytes) override {
        auto &mt = as(t);
        Lock lock(context_mutex());
        const int w = mt.d.width, h = mt.d.height;
        const size_t tight = size_t(w) * bytes_per_pixel(mt.d.format);
        id<MTLBuffer> staging = [dev_ newBufferWithLength:tight * h options:MTLResourceStorageModeShared];
        auto *dst = static_cast<uint8_t *>([staging contents]);
        const auto *src = static_cast<const uint8_t *>(data);
        ptrdiff_t stride = row_bytes ? row_bytes : ptrdiff_t(tight);
        for (int y = 0; y < h; ++y) std::memcpy(dst + tight * y, src + stride * y, tight);
        id<MTLBlitCommandEncoder> blit = [cmd() blitCommandEncoder];
        [blit copyFromBuffer:staging
                 sourceOffset:0
            sourceBytesPerRow:tight
          sourceBytesPerImage:tight * h
                   sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                    toTexture:mt.tex
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];  // the command buffer retains `staging`
    }

    void download(const Texture &t, void *data, ptrdiff_t row_bytes) override {
        auto &mt = as(t);
        const int w = mt.d.width, h = mt.d.height;
        const size_t tight = size_t(w) * bytes_per_pixel(mt.d.format);
        id<MTLBuffer> staging = [dev_ newBufferWithLength:tight * h options:MTLResourceStorageModeShared];
        std::unique_lock<std::recursive_mutex> lock(context_mutex());
        if (external_) throw std::runtime_error("Metal: download while a host command buffer is adopted");
        id<MTLBlitCommandEncoder> blit = [cmd() blitCommandEncoder];
        [blit copyFromTexture:mt.tex
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                            toBuffer:staging
                   destinationOffset:0
              destinationBytesPerRow:tight
            destinationBytesPerImage:tight * h];
        [blit endEncoding];
        id<MTLCommandBuffer> cb = take_current();
        lock.unlock();  // other threads encode into a new buffer meanwhile
        wait(cb);
        const auto *src = static_cast<const uint8_t *>([staging contents]);
        auto *dst = static_cast<uint8_t *>(data);
        ptrdiff_t stride = row_bytes ? row_bytes : ptrdiff_t(tight);
        for (int y = 0; y < h; ++y) std::memcpy(dst + stride * y, src + tight * y, tight);
    }

    void upload(Buffer &b, const void *data, size_t bytes, size_t offset) override {
        auto &mb = static_cast<MtlBuffer &>(b);
        if (offset + bytes > mb.bytes) throw std::runtime_error("buffer upload out of range");
        if (!bytes) return;
        id<MTLBuffer> staging = [dev_ newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
        Lock lock(context_mutex());
        id<MTLBlitCommandEncoder> blit = [cmd() blitCommandEncoder];
        [blit copyFromBuffer:staging sourceOffset:0 toBuffer:mb.buf destinationOffset:offset size:bytes];
        [blit endEncoding];
    }

    // Compiled once per device: with a shared device, once per process.
    Kernel *kernel(std::string_view name) override {
        std::string key(name);
        std::lock_guard<std::mutex> klock(kern_mu_);
        auto it = kernels_.find(key);
        if (it != kernels_.end()) return it->second.get();
        const ShaderFile *file = nullptr;
        const ShaderEntry *e = find_shader(name, &file);
        if (!e) throw std::runtime_error("unknown kernel '" + key + "' (not registered with ntscrt_add_shaders?)");
        id<MTLLibrary> lib = library(file);
        id<MTLFunction> fn = [lib newFunctionWithName:@(e->name)];
        if (!fn) throw std::runtime_error("Metal: entry point missing in library: " + key);
        NSError *err = nil;
        auto k = std::make_unique<MtlKernel>();
        k->entry = e;
        k->file = file;
        k->pso = [dev_ newComputePipelineStateWithFunction:fn error:&err];
        if (!k->pso) throw std::runtime_error("Metal pipeline " + key + ": " + ns(err));
        if (NSUInteger(e->threads[0]) * e->threads[1] * e->threads[2] > k->pso.maxTotalThreadsPerThreadgroup)
            throw std::runtime_error("Metal: [numthreads] of " + key + " exceeds the pipeline limit");
        Kernel *raw = k.get();
        kernels_[key] = std::move(k);
        return raw;
    }

    void dispatch(Kernel *kk, const Bindings &b, int wx, int wy, int wz) override {
        if (!kk) throw std::runtime_error("dispatch: null kernel");
        auto *k = static_cast<MtlKernel *>(kk);
        const ShaderEntry &e = *k->entry;
        if (wx <= 0 || wy <= 0 || wz <= 0) return;
        if (b.textures.size() > 8 || b.buffers.size() > 8 || b.out_textures.size() > 8 || b.out_buffers.size() > 8)
            throw std::runtime_error("dispatch: at most 8 bindings per class");
        Lock lock(context_mutex());
        id<MTLComputeCommandEncoder> enc = [cmd() computeCommandEncoder];
        [enc setComputePipelineState:k->pso];
        // Indices: see cmake/Shaders.cmake (derived from the D3D registers).
        for (size_t i = 0; i < b.textures.size(); ++i)
            if (b.textures[i]) [enc setTexture:as(*b.textures[i]).tex atIndex:i];
        for (size_t i = 0; i < b.out_textures.size(); ++i)
            if (b.out_textures[i]) [enc setTexture:as(*b.out_textures[i]).tex atIndex:8 + i];
        for (size_t i = 0; i < b.buffers.size(); ++i)
            if (b.buffers[i]) [enc setBuffer:static_cast<const MtlBuffer *>(b.buffers[i])->buf offset:0 atIndex:8 + i];
        for (size_t i = 0; i < b.out_buffers.size(); ++i)
            if (b.out_buffers[i]) [enc setBuffer:static_cast<MtlBuffer *>(b.out_buffers[i])->buf offset:0 atIndex:16 + i];
        if (b.constants && b.constants_size) {
            size_t need = (std::max<size_t>(b.constants_size, e.cb_size) + 15) / 16 * 16;
            std::vector<uint8_t> cb(need, 0);
            std::memcpy(cb.data(), b.constants, b.constants_size);
            if (need <= 4096) {
                [enc setBytes:cb.data() length:need atIndex:0];
            } else {
                id<MTLBuffer> tmp = [dev_ newBufferWithBytes:cb.data() length:need options:MTLResourceStorageModeShared];
                [enc setBuffer:tmp offset:0 atIndex:0];
            }
        }
        [enc setSamplerState:linear_ atIndex:0];
        [enc setSamplerState:point_ atIndex:1];
        auto groups = [](int work, uint32_t threads) { return NSUInteger((uint32_t(work) + threads - 1) / threads); };
        [enc dispatchThreadgroups:MTLSizeMake(groups(wx, e.threads[0]), groups(wy, e.threads[1]), groups(wz, e.threads[2]))
            threadsPerThreadgroup:MTLSizeMake(e.threads[0], e.threads[1], e.threads[2])];
        [enc endEncoding];
    }

    void copy(const Texture &src, Texture &dst) override {
        auto &s = as(src);
        auto &d = as(dst);
        const int w = std::min(s.d.width, d.d.width), h = std::min(s.d.height, d.d.height);
        if (s.d.format == d.d.format) {
            Lock lock(context_mutex());
            id<MTLBlitCommandEncoder> blit = [cmd() blitCommandEncoder];
            [blit copyFromTexture:s.tex
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                        toTexture:d.tex
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            return;
        }
        struct { uint32_t w, h, pad0, pad1; } p{uint32_t(w), uint32_t(h), 0, 0};
        Bindings b;
        b.constants = &p;
        b.constants_size = sizeof p;
        b.textures = {&src};
        b.out_textures = {&dst};
        dispatch(kernel("gpu_builtin/copy"), b, w, h, 1);
    }

    // Commits the current buffer (it may hold other threads' work too: one
    // queue, so their order is kept) and waits for it without the lock.
    void finish() override {
        id<MTLCommandBuffer> cb = nil;
        {
            Lock lock(context_mutex());
            if (external_) return;  // the host commits its own command buffer
            cb = take_current();
        }
        wait(cb);
    }

    bool lost() const override { return lost_; }

    void *native_device() const override { return (__bridge void *)dev_; }
    // Callers hold context_mutex() while they encode into it.
    void *native_context() override {
        Lock lock(context_mutex());
        return (__bridge void *)cmd();
    }

    void set_external_command_buffer(void *c) override {
        Lock lock(context_mutex());
        if (c && cb_) {  // keep our earlier work ahead of the host's buffer
            [cb_ commit];
            cb_ = nil;
        }
        external_ = (__bridge id<MTLCommandBuffer>)c;
    }

    std::shared_ptr<Texture> wrap_native_texture(void *native) override {
        if (!native) throw std::runtime_error("wrap_native_texture: null");
        auto t = std::make_shared<MtlTexture>();
        t->tex = (__bridge id<MTLTexture>)native;
        if (!from_mtl(t->tex.pixelFormat, t->d.format))
            throw std::runtime_error("wrap_native_texture: unsupported MTLPixelFormat " +
                                     std::to_string(int(t->tex.pixelFormat)));
        t->d.width = int(t->tex.width);
        t->d.height = int(t->tex.height);
        t->d.shader_write = (t->tex.usage & MTLTextureUsageShaderWrite) != 0;
        t->d.render_target = (t->tex.usage & MTLTextureUsageRenderTarget) != 0;
        return t;
    }

private:
    static MtlTexture &as(const Texture &t) {
        auto *p = dynamic_cast<const MtlTexture *>(&t);
        if (!p) throw std::runtime_error("texture belongs to another backend");
        return const_cast<MtlTexture &>(*p);
    }

    using Lock = std::lock_guard<std::recursive_mutex>;

    // Under context_mutex(): the current own buffer, committed (nil if none).
    id<MTLCommandBuffer> take_current() {
        id<MTLCommandBuffer> cb = cb_;
        cb_ = nil;
        if (cb) [cb commit];
        return cb;
    }
    void wait(id<MTLCommandBuffer> cb) {
        if (!cb) return;
        [cb waitUntilCompleted];
        if (cb.status == MTLCommandBufferStatusError) {
            lost_ = true;  // a hang / GPU reset leaves the queue unusable: the owner makes a new device
            throw std::runtime_error("Metal command buffer failed: " + ns(cb.error));
        }
    }

    // Under context_mutex().
    id<MTLCommandBuffer> cmd() {
        if (external_) return external_;
        if (!cb_) {
            cb_ = [queue_ commandBuffer];
            if (!cb_) throw std::runtime_error("Metal: cannot create a command buffer");
        }
        return cb_;
    }

    id<MTLLibrary> library(const ShaderFile *f) {
        auto it = libs_.find(f->stem);
        if (it != libs_.end()) return it->second;
        NSError *err = nil;
        MTLCompileOptions *opts = [MTLCompileOptions new];
        id<MTLLibrary> lib = [dev_ newLibraryWithSource:@(f->msl) options:opts error:&err];
        if (!lib) throw std::runtime_error(std::string("Metal compile ") + f->stem + ": " + ns(err));
        libs_[f->stem] = lib;
        return lib;
    }

    id<MTLDevice> dev_;
    id<MTLCommandQueue> queue_;
    id<MTLSamplerState> linear_, point_;
    id<MTLCommandBuffer> cb_ = nil;
    id<MTLCommandBuffer> external_ = nil;
    std::atomic<bool> lost_{false};  // a command buffer failed (GPU hang / reset); any thread sets it
    std::unordered_map<std::string, id<MTLLibrary>> libs_;
    std::unordered_map<std::string, std::unique_ptr<MtlKernel>> kernels_;
    std::mutex kern_mu_;  // libs_, kernels_
};

}  // namespace

std::unique_ptr<Device> create_metal_device(void *native_device, void *native_queue, std::string &err) {
    @autoreleasepool {
        try {
            id<MTLDevice> dev = native_device ? (__bridge id<MTLDevice>)native_device : MTLCreateSystemDefaultDevice();
            if (!dev) {
                err = "no Metal device available";
                return nullptr;
            }
            id<MTLCommandQueue> q = native_queue ? (__bridge id<MTLCommandQueue>)native_queue : [dev newCommandQueue];
            if (!q) {
                err = "cannot create a Metal command queue";
                return nullptr;
            }
            return std::make_unique<MetalDevice>(dev, q);
        } catch (const std::exception &e) {
            err = e.what();
            return nullptr;
        }
    }
}

namespace metal {
void *command_queue(Device &d) {
    auto *p = dynamic_cast<MetalDevice *>(&d);
    return p ? (__bridge void *)p->queue() : nullptr;
}
}  // namespace metal

}  // namespace ntscrt::gpu
