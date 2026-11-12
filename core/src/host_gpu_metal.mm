// host_gpu_metal.mm — Bridge for Metal hosts (OpenFX kOfxImageEffectPropMetalEnabled,
// DaVinci Resolve on macOS): the host's images are id<MTLBuffer>s on the queue's
// device, which must be the core's Metal device (Apple silicon: the one GPU).
// Blits between the host's buffers and the core's textures run on the core's
// queue (Device::native_context(): its current command buffer). Ordering: before
// the first blit an empty command buffer committed on the host's queue is waited
// for (a queue runs its command buffers in commit order, so the host's work
// producing the source is done); export / write end with Device::finish(), so the
// host's later work sees the result. Compiled with ARC (core/CMakeLists.txt).
#include "host_gpu.h"

#import <Metal/Metal.h>

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace ntscrt::hostgpu {
namespace {

size_t bpp(ntscrt_pixel_format f) {
    return f == NTSCRT_RGBA32F ? 16 : (f == NTSCRT_RGBA16F || f == NTSCRT_RGBA16) ? 8 : 4;
}

class MetalBridge final : public Bridge {
public:
    explicit MetalBridge(id<MTLCommandQueue> q) : queue_(q), device_(q.device) {}

    bool rebind(const ntscrt_host_gpu &host) override {
        if (host.api != NTSCRT_HOST_GPU_METAL || !host.queue) return false;
        id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)host.queue;
        if (q.device != device_) return false;
        queue_ = q;
        return true;
    }
    std::string describe() const override { return std::string("Metal: ") + device_.name.UTF8String; }

    std::string check(gpu::Device &dev) override {
        if (dev.backend() != gpu::Backend::Metal) return "the core's GPU device isn't Metal";
        id<MTLDevice> ours = (__bridge id<MTLDevice>)dev.native_device();
        if (ours != device_)
            return std::string("the core's Metal device (") + ours.name.UTF8String + ") isn't the host's (" +
                   device_.name.UTF8String + ")";
        return {};
    }

    std::shared_ptr<gpu::Texture> import(gpu::Device &dev, const ntscrt_gpu_image &img) override {
        Slot &s = slot(dev, img, kIn);
        wait_host();
        std::lock_guard<std::recursive_mutex> lock(dev.context_mutex());
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)dev.native_context();
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:buffer(img)
                 sourceOffset:img.offset
            sourceBytesPerRow:NSUInteger(img.row_bytes)
          sourceBytesPerImage:NSUInteger(img.row_bytes) * NSUInteger(img.height)
                   sourceSize:MTLSizeMake(NSUInteger(img.width), NSUInteger(img.height), 1)
                    toTexture:(__bridge id<MTLTexture>)s.tex->native()
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        return s.tex;
    }

    std::shared_ptr<gpu::Texture> target(gpu::Device &dev, const ntscrt_gpu_image &img) override {
        return slot(dev, img, kOut).tex;
    }

    void export_(gpu::Device &dev, const ntscrt_gpu_image &img) override {
        Slot &s = slot(dev, img, kOut);
        {
            std::lock_guard<std::recursive_mutex> lock(dev.context_mutex());
            id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)dev.native_context();
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:(__bridge id<MTLTexture>)s.tex->native()
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(NSUInteger(img.width), NSUInteger(img.height), 1)
                                toBuffer:buffer(img)
                       destinationOffset:img.offset
                  destinationBytesPerRow:NSUInteger(img.row_bytes)
                destinationBytesPerImage:NSUInteger(img.row_bytes) * NSUInteger(img.height)];
            [blit endEncoding];
        }
        dev.finish();  // done before the host's queue goes on with the frame
    }

    void read(const ntscrt_gpu_image &img, void *cpu, ptrdiff_t row_bytes) override {
        const size_t tight = size_t(img.width) * bpp(img.format);
        wait_host();
        id<MTLBuffer> staging = [device_ newBufferWithLength:tight * size_t(img.height)
                                                     options:MTLResourceStorageModeShared];
        if (!staging) throw std::runtime_error("Metal: staging buffer allocation failed");
        id<MTLCommandBuffer> cb = [own_queue() commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        id<MTLBuffer> src = buffer(img);
        for (int y = 0; y < img.height; ++y)
            [blit copyFromBuffer:src
                    sourceOffset:img.offset + size_t(y) * size_t(img.row_bytes)
                        toBuffer:staging
               destinationOffset:size_t(y) * tight
                            size:tight];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status == MTLCommandBufferStatusError) throw std::runtime_error("Metal: reading the host's frame failed");
        const auto *p = static_cast<const uint8_t *>(staging.contents);
        for (int y = 0; y < img.height; ++y) {
            const int my = img.bottom_up ? img.height - 1 - y : y;
            std::memcpy(static_cast<uint8_t *>(cpu) + ptrdiff_t(y) * row_bytes, p + tight * size_t(my), tight);
        }
    }

    void write(const void *cpu, ptrdiff_t row_bytes, const ntscrt_gpu_image &img) override {
        const size_t tight = size_t(img.width) * bpp(img.format);
        wait_host();
        id<MTLBuffer> staging = [device_ newBufferWithLength:tight * size_t(img.height)
                                                     options:MTLResourceStorageModeShared];
        if (!staging) throw std::runtime_error("Metal: staging buffer allocation failed");
        auto *p = static_cast<uint8_t *>(staging.contents);
        for (int y = 0; y < img.height; ++y) {
            const int my = img.bottom_up ? img.height - 1 - y : y;
            std::memcpy(p + tight * size_t(my), static_cast<const uint8_t *>(cpu) + ptrdiff_t(y) * row_bytes, tight);
        }
        id<MTLCommandBuffer> cb = [own_queue() commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        id<MTLBuffer> dst = buffer(img);
        for (int y = 0; y < img.height; ++y)
            [blit copyFromBuffer:staging
                    sourceOffset:size_t(y) * tight
                        toBuffer:dst
               destinationOffset:img.offset + size_t(y) * size_t(img.row_bytes)
                            size:tight];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status == MTLCommandBufferStatusError) throw std::runtime_error("Metal: writing the host's frame failed");
    }

private:
    enum Role { kIn, kOut };
    struct Slot {
        int w = 0, h = 0;
        gpu::Format f = gpu::Format::RGBA32F;
        std::shared_ptr<gpu::Texture> tex;
        void *dev = nullptr;
    };

    static id<MTLBuffer> buffer(const ntscrt_gpu_image &img) { return (__bridge id<MTLBuffer>)img.data; }

    Slot &slot(gpu::Device &dev, const ntscrt_gpu_image &img, Role role) {
        gpu::Format f;
        if (!hal_format(img.format, f)) throw std::runtime_error("unsupported host GPU pixel format");
        if (img.offset % bpp(img.format) || size_t(img.row_bytes) % bpp(img.format))
            throw std::runtime_error("host Metal buffer rows aren't aligned to whole pixels");
        Slot &s = slots_[role];
        if (s.tex && s.w == img.width && s.h == img.height && s.f == f && s.dev == &dev) return s;
        s = Slot{img.width, img.height, f, dev.create_texture({img.width, img.height, f, role == kOut, false}), &dev};
        return s;
    }

    // The host's work queued so far (the source frame) is complete.
    void wait_host() {
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        if (!cb) throw std::runtime_error("Metal: no command buffer on the host's queue");
        [cb commit];
        [cb waitUntilCompleted];
    }

    // For CPU copies: a queue of our own on the host's device.
    id<MTLCommandQueue> own_queue() {
        if (!own_) own_ = [device_ newCommandQueue];
        if (!own_) throw std::runtime_error("Metal: cannot create a command queue");
        return own_;
    }

    id<MTLCommandQueue> queue_;
    id<MTLDevice> device_;
    id<MTLCommandQueue> own_ = nil;
    Slot slots_[2];
};

}  // namespace

std::unique_ptr<Bridge> make_metal_bridge(void *queue, std::string &err) {
    if (!queue) {
        err = "the host gave no Metal command queue";
        return nullptr;
    }
    @autoreleasepool {
        id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)queue;
        if (!q.device) {
            err = "the host's Metal command queue has no device";
            return nullptr;
        }
        return std::make_unique<MetalBridge>(q);
    }
}

}  // namespace ntscrt::hostgpu
