// fake_cuda_host.h — tests acting as a CUDA host (Windows, NVIDIA driver): the
// driver API calls a host makes to hand images over in device memory, loaded from
// nvcuda.dll (prototypes from cuda.h, CUDA 12). start() fails without a driver /
// device: tests skip then. Used by tests/hostgpu and tests/ofx (mock host).
#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <type_traits>
#include <vector>

namespace fakecuda {
struct FakeCudaHost {
    using CUresult = int;
    using CUdeviceptr = unsigned long long;
    HMODULE dll = nullptr;
    CUresult(__stdcall *Init)(unsigned) = nullptr;
    CUresult(__stdcall *DeviceGet)(int *, int) = nullptr;
    CUresult(__stdcall *PrimaryCtxRetain)(void **, int) = nullptr;
    CUresult(__stdcall *CtxPushCurrent)(void *) = nullptr;
    CUresult(__stdcall *CtxPopCurrent)(void **) = nullptr;
    CUresult(__stdcall *StreamCreate)(void **, unsigned) = nullptr;
    CUresult(__stdcall *StreamSynchronize)(void *) = nullptr;
    CUresult(__stdcall *MemAlloc)(CUdeviceptr *, size_t) = nullptr;
    CUresult(__stdcall *MemFree)(CUdeviceptr) = nullptr;
    CUresult(__stdcall *MemcpyHtoD)(CUdeviceptr, const void *, size_t) = nullptr;
    CUresult(__stdcall *MemcpyDtoH)(void *, CUdeviceptr, size_t) = nullptr;
    void *ctx = nullptr, *stream = nullptr;
    std::string why;

    bool start() {
        dll = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) return (why = "no nvcuda.dll"), false;
        auto load = [&](auto &fn, const char *n) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll, n));
            return fn != nullptr;
        };
        if (!(load(Init, "cuInit") && load(DeviceGet, "cuDeviceGet") &&
              load(PrimaryCtxRetain, "cuDevicePrimaryCtxRetain") && load(CtxPushCurrent, "cuCtxPushCurrent_v2") &&
              load(CtxPopCurrent, "cuCtxPopCurrent_v2") && load(StreamCreate, "cuStreamCreate") &&
              load(StreamSynchronize, "cuStreamSynchronize") && load(MemAlloc, "cuMemAlloc_v2") &&
              load(MemFree, "cuMemFree_v2") && load(MemcpyHtoD, "cuMemcpyHtoD_v2") &&
              load(MemcpyDtoH, "cuMemcpyDtoH_v2")))
            return (why = "nvcuda.dll lacks an entry point"), false;
        int dev = 0;
        if (Init(0) != 0 || DeviceGet(&dev, 0) != 0) return (why = "no CUDA device"), false;
        if (PrimaryCtxRetain(&ctx, dev) != 0 || CtxPushCurrent(ctx) != 0) return (why = "no CUDA context"), false;
        if (StreamCreate(&stream, 1 /* CU_STREAM_NON_BLOCKING */) != 0) return (why = "no CUDA stream"), false;
        return true;
    }
    // Device copy of `bytes` bytes (0 on failure).
    CUdeviceptr upload(const void *data, size_t bytes) {
        CUdeviceptr p = 0;
        if (MemAlloc(&p, bytes) != 0) return 0;
        if (MemcpyHtoD(p, data, bytes) != 0) {
            MemFree(p);
            return 0;
        }
        return p;
    }
    // Waits for the host stream (the plug-in's work on it), then copies back.
    bool download(CUdeviceptr p, void *data, size_t bytes) {
        return StreamSynchronize(stream) == 0 && MemcpyDtoH(data, p, bytes) == 0;
    }
};
}  // namespace fakecuda
#endif
