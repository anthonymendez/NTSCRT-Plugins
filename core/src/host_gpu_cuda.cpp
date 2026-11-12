// host_gpu_cuda.cpp — Bridge for CUDA hosts (OpenFX kOfxImageEffectPropCudaEnabled,
// DaVinci Resolve on NVIDIA GPUs, Windows) and the core's Direct3D 11 device.
//
// No CUDA toolkit needed: the driver API is loaded from nvcuda.dll (part of every
// NVIDIA driver, already loaded by a CUDA host) with the prototypes declared here
// from the CUDA 12 driver API reference (cuda.h, cudaD3D11.h). Interop: the core's
// D3D11 textures are registered with the host's CUDA context
// (cuGraphicsD3D11RegisterResource) and copied to / from the host's device memory
// with cuMemcpy2DAsync on the host's stream, between cuGraphicsMapResources and
// cuGraphicsUnmapResources, which order the copies against the D3D11 work:
//   map:   graphics work issued before it completes before later CUDA work in the stream;
//   unmap: CUDA work issued in the stream before it completes before later graphics work.
// Registration needs the D3D11 device on the CUDA device's adapter (same LUID).
#include "host_gpu.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ntscrt::hostgpu {
namespace {

// ---- CUDA driver API subset (cuda.h, CUDA 12; CUDAAPI = __stdcall) ----------
using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;  // CUdeviceptr_v2 (64-bit)
using CUcontext = struct CUctx_st *;
using CUstream = struct CUstream_st *;
using CUarray = struct CUarray_st *;
using CUgraphicsResource = struct CUgraphicsResource_st *;
constexpr CUresult CUDA_SUCCESS = 0;
enum CUmemorytype { CU_MEMORYTYPE_HOST = 1, CU_MEMORYTYPE_DEVICE = 2, CU_MEMORYTYPE_ARRAY = 3 };
constexpr unsigned CU_GRAPHICS_REGISTER_FLAGS_NONE = 0;
constexpr unsigned CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY = 1;
constexpr unsigned CU_GRAPHICS_MAP_RESOURCE_FLAGS_WRITE_DISCARD = 2;
struct CUDA_MEMCPY2D {  // CUDA_MEMCPY2D_st (v2)
    size_t srcXInBytes, srcY;
    CUmemorytype srcMemoryType;
    const void *srcHost;
    CUdeviceptr srcDevice;
    CUarray srcArray;
    size_t srcPitch;
    size_t dstXInBytes, dstY;
    CUmemorytype dstMemoryType;
    void *dstHost;
    CUdeviceptr dstDevice;
    CUarray dstArray;
    size_t dstPitch;
    size_t WidthInBytes, Height;
};

struct Cuda {
    HMODULE dll = nullptr;
    CUresult(__stdcall *Init)(unsigned);
    CUresult(__stdcall *GetErrorString)(CUresult, const char **);
    CUresult(__stdcall *StreamGetCtx)(CUstream, CUcontext *);
    CUresult(__stdcall *CtxGetCurrent)(CUcontext *);
    CUresult(__stdcall *CtxPushCurrent)(CUcontext);
    CUresult(__stdcall *CtxPopCurrent)(CUcontext *);
    CUresult(__stdcall *CtxGetDevice)(CUdevice *);
    CUresult(__stdcall *DeviceGetLuid)(char *, unsigned *, CUdevice);
    CUresult(__stdcall *DeviceGetName)(char *, int, CUdevice);
    CUresult(__stdcall *GraphicsD3D11RegisterResource)(CUgraphicsResource *, ID3D11Resource *, unsigned);
    CUresult(__stdcall *GraphicsUnregisterResource)(CUgraphicsResource);
    CUresult(__stdcall *GraphicsResourceSetMapFlags)(CUgraphicsResource, unsigned);
    CUresult(__stdcall *GraphicsMapResources)(unsigned, CUgraphicsResource *, CUstream);
    CUresult(__stdcall *GraphicsUnmapResources)(unsigned, CUgraphicsResource *, CUstream);
    CUresult(__stdcall *GraphicsSubResourceGetMappedArray)(CUarray *, CUgraphicsResource, unsigned, unsigned);
    CUresult(__stdcall *Memcpy2DAsync)(const CUDA_MEMCPY2D *, CUstream);
    CUresult(__stdcall *StreamSynchronize)(CUstream);
    CUresult(__stdcall *MemAllocHost)(void **, size_t);
    CUresult(__stdcall *MemFreeHost)(void *);
    std::string error;  // why it couldn't load

    static const Cuda &get() {
        static const Cuda c = [] {
            Cuda c{};
            // Already loaded by a CUDA host; LoadLibrary only takes a reference.
            c.dll = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!c.dll) {
                c.error = "nvcuda.dll not found (no NVIDIA driver)";
                return c;
            }
            bool ok = true;
            auto load = [&](auto &fn, const char *name) {
                fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(c.dll, name));
                if (!fn) {
                    ok = false;
                    c.error += std::string(c.error.empty() ? "nvcuda.dll lacks " : ", ") + name;
                }
            };
            load(c.Init, "cuInit");
            load(c.GetErrorString, "cuGetErrorString");
            load(c.StreamGetCtx, "cuStreamGetCtx");
            load(c.CtxGetCurrent, "cuCtxGetCurrent");
            load(c.CtxPushCurrent, "cuCtxPushCurrent_v2");
            load(c.CtxPopCurrent, "cuCtxPopCurrent_v2");
            load(c.CtxGetDevice, "cuCtxGetDevice");
            load(c.DeviceGetLuid, "cuDeviceGetLuid");
            load(c.DeviceGetName, "cuDeviceGetName");
            load(c.GraphicsD3D11RegisterResource, "cuGraphicsD3D11RegisterResource");
            load(c.GraphicsUnregisterResource, "cuGraphicsUnregisterResource");
            load(c.GraphicsResourceSetMapFlags, "cuGraphicsResourceSetMapFlags_v2");
            load(c.GraphicsMapResources, "cuGraphicsMapResources");
            load(c.GraphicsUnmapResources, "cuGraphicsUnmapResources");
            load(c.GraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray");
            load(c.Memcpy2DAsync, "cuMemcpy2DAsync_v2");
            load(c.StreamSynchronize, "cuStreamSynchronize");
            load(c.MemAllocHost, "cuMemAllocHost_v2");
            load(c.MemFreeHost, "cuMemFreeHost");
            if (ok && c.Init(0) != CUDA_SUCCESS) {
                ok = false;
                c.error = "cuInit failed";
            }
            if (!ok) c.dll = nullptr;  // (the reference is kept: the host holds the DLL anyway)
            return c;
        }();
        return c;
    }
};

void cu_check(CUresult r, const char *what) {
    if (r == CUDA_SUCCESS) return;
    const char *s = nullptr;
    Cuda::get().GetErrorString(r, &s);
    throw std::runtime_error(std::string(what) + " failed: CUDA error " + std::to_string(r) + (s ? std::string(" (") + s + ")" : ""));
}

std::string luid_text(const LUID &l) {
    char b[32];
    std::snprintf(b, sizeof b, "%08lx:%08lx", static_cast<unsigned long>(l.HighPart), l.LowPart);
    return b;
}

class CudaBridge final : public Bridge {
public:
    explicit CudaBridge(CUstream s) : stream_(s) {
        const Cuda &cu = Cuda::get();
        if (stream_) cu_check(cu.StreamGetCtx(stream_, &ctx_), "cuStreamGetCtx");
        else cu_check(cu.CtxGetCurrent(&ctx_), "cuCtxGetCurrent");
        if (!ctx_) throw std::runtime_error("no CUDA context (the host's stream has none and none is current)");
        Push p(ctx_);
        cu_check(cu.CtxGetDevice(&cudev_), "cuCtxGetDevice");
        unsigned mask = 0;
        cu_check(cu.DeviceGetLuid(reinterpret_cast<char *>(&luid_), &mask, cudev_), "cuDeviceGetLuid");
        char name[128] = {};
        cu.DeviceGetName(name, int(sizeof name) - 1, cudev_);
        name_ = name;
    }
    ~CudaBridge() override {
        try {
            Push p(ctx_);
            for (auto &s : slots_)
                if (s.res) Cuda::get().GraphicsUnregisterResource(s.res);
            if (pinned_) Cuda::get().MemFreeHost(pinned_);
        } catch (...) {
        }
    }

    bool rebind(const ntscrt_host_gpu &host) override {
        if (host.api != NTSCRT_HOST_GPU_CUDA) return false;
        auto s = static_cast<CUstream>(host.queue);
        if (s == stream_) return true;
        CUcontext c = nullptr;
        const Cuda &cu = Cuda::get();
        if ((s ? cu.StreamGetCtx(s, &c) : cu.CtxGetCurrent(&c)) != CUDA_SUCCESS || c != ctx_) return false;
        stream_ = s;
        return true;
    }
    std::string describe() const override { return "CUDA: " + name_; }

    std::string check(gpu::Device &dev) override {
        if (dev.backend() != gpu::Backend::D3D11) return "the core's GPU device isn't Direct3D 11";
        auto *d3d = static_cast<ID3D11Device *>(dev.native_device());
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> ad;
        DXGI_ADAPTER_DESC desc{};
        if (!d3d || FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&ad)) ||
            FAILED(ad->GetDesc(&desc)))
            return "can't read the Direct3D 11 device's adapter";
        if (std::memcmp(&desc.AdapterLuid, &luid_, sizeof(LUID)) != 0) {
            char an[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, an, int(sizeof an), nullptr, nullptr);
            return "the core's Direct3D 11 device is on another GPU (" + std::string(an) + ", LUID " +
                   luid_text(desc.AdapterLuid) + ") than the host's CUDA device (" + name_ + ", LUID " +
                   luid_text(luid_) + ")";
        }
        return {};
    }

    std::shared_ptr<gpu::Texture> import(gpu::Device &dev, const ntscrt_gpu_image &img) override {
        Slot &s = slot(dev, img, kIn);
        flush(dev);  // the D3D11 reads of the last frame's copy are queued before the map waits for them
        const Cuda &cu = Cuda::get();
        Push p(ctx_);
        cu_check(cu.GraphicsMapResources(1, &s.res, stream_), "cuGraphicsMapResources");
        try {
            CUarray arr = nullptr;
            cu_check(cu.GraphicsSubResourceGetMappedArray(&arr, s.res, 0, 0), "cuGraphicsSubResourceGetMappedArray");
            CUDA_MEMCPY2D m{};
            m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            m.srcDevice = device_ptr(img);
            m.srcPitch = size_t(img.row_bytes);
            m.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            m.dstArray = arr;
            m.WidthInBytes = size_t(img.width) * bpp(img.format);
            m.Height = size_t(img.height);
            cu_check(cu.Memcpy2DAsync(&m, stream_), "cuMemcpy2DAsync (host frame -> texture)");
        } catch (...) {
            cu.GraphicsUnmapResources(1, &s.res, stream_);
            throw;
        }
        cu_check(cu.GraphicsUnmapResources(1, &s.res, stream_), "cuGraphicsUnmapResources");
        return s.tex;
    }

    std::shared_ptr<gpu::Texture> target(gpu::Device &dev, const ntscrt_gpu_image &img) override {
        return slot(dev, img, kOut).tex;
    }

    void export_(gpu::Device &dev, const ntscrt_gpu_image &img) override {
        Slot &s = slot(dev, img, kOut);
        flush(dev);  // the kernels writing the target must be submitted before the map waits for them
        const Cuda &cu = Cuda::get();
        Push p(ctx_);
        cu_check(cu.GraphicsMapResources(1, &s.res, stream_), "cuGraphicsMapResources");
        try {
            CUarray arr = nullptr;
            cu_check(cu.GraphicsSubResourceGetMappedArray(&arr, s.res, 0, 0), "cuGraphicsSubResourceGetMappedArray");
            CUDA_MEMCPY2D m{};
            m.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            m.srcArray = arr;
            m.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            m.dstDevice = device_ptr(img);
            m.dstPitch = size_t(img.row_bytes);
            m.WidthInBytes = size_t(img.width) * bpp(img.format);
            m.Height = size_t(img.height);
            cu_check(cu.Memcpy2DAsync(&m, stream_), "cuMemcpy2DAsync (texture -> host frame)");
        } catch (...) {
            cu.GraphicsUnmapResources(1, &s.res, stream_);
            throw;
        }
        cu_check(cu.GraphicsUnmapResources(1, &s.res, stream_), "cuGraphicsUnmapResources");
        if (!stream_) cu_check(cu.StreamSynchronize(stream_), "cuStreamSynchronize");
    }

    void read(const ntscrt_gpu_image &img, void *cpu, ptrdiff_t row_bytes) override {
        const size_t tight = size_t(img.width) * bpp(img.format);
        const Cuda &cu = Cuda::get();
        Push p(ctx_);
        uint8_t *stage = staging(tight * size_t(img.height));
        CUDA_MEMCPY2D m{};
        m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        m.srcDevice = device_ptr(img);
        m.srcPitch = size_t(img.row_bytes);
        m.dstMemoryType = CU_MEMORYTYPE_HOST;
        m.dstHost = stage;
        m.dstPitch = tight;
        m.WidthInBytes = tight;
        m.Height = size_t(img.height);
        cu_check(cu.Memcpy2DAsync(&m, stream_), "cuMemcpy2DAsync (host frame -> CPU)");
        cu_check(cu.StreamSynchronize(stream_), "cuStreamSynchronize");
        for (int y = 0; y < img.height; ++y) {
            const int my = img.bottom_up ? img.height - 1 - y : y;
            std::memcpy(static_cast<uint8_t *>(cpu) + ptrdiff_t(y) * row_bytes, stage + tight * size_t(my), tight);
        }
    }

    void write(const void *cpu, ptrdiff_t row_bytes, const ntscrt_gpu_image &img) override {
        const size_t tight = size_t(img.width) * bpp(img.format);
        const Cuda &cu = Cuda::get();
        Push p(ctx_);
        uint8_t *stage = staging(tight * size_t(img.height));
        for (int y = 0; y < img.height; ++y) {
            const int my = img.bottom_up ? img.height - 1 - y : y;
            std::memcpy(stage + tight * size_t(my), static_cast<const uint8_t *>(cpu) + ptrdiff_t(y) * row_bytes, tight);
        }
        CUDA_MEMCPY2D m{};
        m.srcMemoryType = CU_MEMORYTYPE_HOST;
        m.srcHost = stage;
        m.srcPitch = tight;
        m.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        m.dstDevice = device_ptr(img);
        m.dstPitch = size_t(img.row_bytes);
        m.WidthInBytes = tight;
        m.Height = size_t(img.height);
        cu_check(cu.Memcpy2DAsync(&m, stream_), "cuMemcpy2DAsync (CPU -> host frame)");
        cu_check(cu.StreamSynchronize(stream_), "cuStreamSynchronize");  // the staging buffer is reused
    }

private:
    enum Role { kIn, kOut };
    struct Slot {
        int w = 0, h = 0;
        gpu::Format f = gpu::Format::RGBA32F;
        std::shared_ptr<gpu::Texture> tex;
        CUgraphicsResource res = nullptr;
        void *dev = nullptr;  // the HAL device the texture belongs to
    };

    // Pushes the host's context for a scope (the host's own stays current after).
    struct Push {
        explicit Push(CUcontext c) { cu_check(Cuda::get().CtxPushCurrent(c), "cuCtxPushCurrent"); }
        ~Push() {
            CUcontext dummy = nullptr;
            Cuda::get().CtxPopCurrent(&dummy);
        }
    };

    static size_t bpp(ntscrt_pixel_format f) {
        return f == NTSCRT_RGBA32F ? 16 : (f == NTSCRT_RGBA16F || f == NTSCRT_RGBA16) ? 8 : 4;
    }
    static CUdeviceptr device_ptr(const ntscrt_gpu_image &img) {
        return CUdeviceptr(reinterpret_cast<uintptr_t>(img.data)) + img.offset;
    }

    // The registered texture for `role`, (re)made for the image's size and format.
    Slot &slot(gpu::Device &dev, const ntscrt_gpu_image &img, Role role) {
        gpu::Format f;
        if (!hal_format(img.format, f)) throw std::runtime_error("unsupported host GPU pixel format");
        Slot &s = slots_[role];
        if (s.tex && s.w == img.width && s.h == img.height && s.f == f && s.dev == &dev) return s;
        Push p(ctx_);
        if (s.res) Cuda::get().GraphicsUnregisterResource(s.res);
        s = Slot{};
        auto t = dev.create_texture({img.width, img.height, f, role == kOut, false});
        CUgraphicsResource r = nullptr;
        cu_check(Cuda::get().GraphicsD3D11RegisterResource(&r, static_cast<ID3D11Resource *>(t->native()),
                                                        CU_GRAPHICS_REGISTER_FLAGS_NONE),
              "cuGraphicsD3D11RegisterResource");
        Cuda::get().GraphicsResourceSetMapFlags(
            r, role == kIn ? CU_GRAPHICS_MAP_RESOURCE_FLAGS_WRITE_DISCARD : CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
        s.w = img.width;
        s.h = img.height;
        s.f = f;
        s.tex = std::move(t);
        s.res = r;
        s.dev = &dev;
        return s;
    }

    static void flush(gpu::Device &dev) {
        std::lock_guard<std::recursive_mutex> lock(dev.context_mutex());
        static_cast<ID3D11DeviceContext *>(dev.native_context())->Flush();
    }

    uint8_t *staging(size_t bytes) {
        if (pinned_bytes_ < bytes) {
            if (pinned_) Cuda::get().MemFreeHost(pinned_);
            pinned_ = nullptr;
            pinned_bytes_ = 0;
            cu_check(Cuda::get().MemAllocHost(&pinned_, bytes), "cuMemAllocHost");
            pinned_bytes_ = bytes;
        }
        return static_cast<uint8_t *>(pinned_);
    }

    CUstream stream_ = nullptr;
    CUcontext ctx_ = nullptr;
    CUdevice cudev_ = 0;
    LUID luid_{};
    std::string name_;
    Slot slots_[2];
    void *pinned_ = nullptr;
    size_t pinned_bytes_ = 0;
};

}  // namespace

std::unique_ptr<Bridge> make_cuda_bridge(void *stream, std::string &err) {
    const Cuda &cu = Cuda::get();
    if (!cu.dll) {
        err = cu.error;
        return nullptr;
    }
    try {
        return std::make_unique<CudaBridge>(static_cast<CUstream>(stream));
    } catch (const std::exception &e) {
        err = e.what();
        return nullptr;
    }
}

bool cuda_available() { return Cuda::get().dll != nullptr; }

}  // namespace ntscrt::hostgpu
