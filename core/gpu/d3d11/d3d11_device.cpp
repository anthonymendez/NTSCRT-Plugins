// d3d11_device.cpp — Direct3D 11 HAL backend (Windows).
//
// Own device by default (hardware, then WARP so it also runs headless / in
// CI; NTSCRT_D3D11_WARP=1 forces WARP), or the host's ID3D11Device.
// Kernels: embedded HLSL compiled on first use with D3DCompile, loaded from
// d3dcompiler_47.dll at runtime (ships with Windows 10+).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "../backend.h"
#include "d3d11_interop.h"

using Microsoft::WRL::ComPtr;

namespace ntscrt::gpu {
namespace {

std::string hr_msg(const char *what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}
void check(HRESULT hr, const char *what) {
    if (FAILED(hr)) throw std::runtime_error(hr_msg(what, hr));
}

DXGI_FORMAT dxgi(Format f) {
    switch (f) {
    case Format::BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case Format::R32F: return DXGI_FORMAT_R32_FLOAT;
    case Format::RG32F: return DXGI_FORMAT_R32G32_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

bool from_dxgi(DXGI_FORMAT d, Format &f) {
    switch (d) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: f = Format::BGRA8; return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS: f = Format::RGBA8; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: f = Format::RGBA16F; return true;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: f = Format::RGBA32F; return true;
    case DXGI_FORMAT_R32_FLOAT: f = Format::R32F; return true;
    case DXGI_FORMAT_R32G32_FLOAT: f = Format::RG32F; return true;
    default: return false;
    }
}

class D3DTexture final : public Texture {
public:
    TextureDesc d;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11RenderTargetView> rtv;
    // Storage format. Differs from dxgi(d.format) only for BGRA8 textures
    // with shader_write on GPUs without typed BGRA8 UAV stores: they are
    // stored as RGBA8 (shaders see the same float4), and upload/download
    // swap R and B so callers still get BGRA bytes.
    DXGI_FORMAT phys = DXGI_FORMAT_UNKNOWN;
    bool swap_rb() const { return d.format == Format::BGRA8 && phys == DXGI_FORMAT_R8G8B8A8_UNORM; }
    const TextureDesc &desc() const override { return d; }
    void *native() const override { return tex.Get(); }
};

// D3D11 structured buffers need their element stride at creation, which
// gpu::Buffer doesn't carry: the stride comes from the reflection of the
// first kernel the buffer is bound to. Until then the contents live in
// `shadow`. Rebinding with another stride recreates it (contents copied).
class D3D11Device;
class D3DBuffer final : public Buffer {
public:
    size_t bytes = 0;
    uint32_t stride = 0;
    std::vector<uint8_t> shadow;
    ComPtr<ID3D11Buffer> buf;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    D3D11Device *owner = nullptr;
    size_t size() const override { return bytes; }
    void *native() const override;  // creates it with stride 4 if still unbound
};

class D3DKernel final : public Kernel {
public:
    ComPtr<ID3D11ComputeShader> cs;
};

using Lock = std::lock_guard<std::recursive_mutex>;

class D3D11Device final : public Device {
public:
    D3D11Device(ComPtr<ID3D11Device> dev, bool warp) : dev_(std::move(dev)), warp_(warp) {
        dev_->GetImmediateContext(&ctx_);
        D3D11_SAMPLER_DESC sd{};
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        check(dev_->CreateSamplerState(&sd, &linear_), "CreateSamplerState");
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        check(dev_->CreateSamplerState(&sd, &point_), "CreateSamplerState");
        give_query(take_query());
        // D3D11.4 fence for finish(), when the runtime and driver have it.
        ComPtr<ID3D11Device5> dev5;
        ComPtr<ID3D11DeviceContext4> ctx4;
        if (SUCCEEDED(dev_.As(&dev5)) && SUCCEEDED(ctx_.As(&ctx4)) &&
            SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) &&
            !std::getenv("NTSCRT_D3D11_NO_FENCE"))
            ctx4_ = ctx4;
        else
            fence_.Reset();
    }

    bool warp() const { return warp_; }
    Backend backend() const override { return Backend::D3D11; }

    std::shared_ptr<Texture> create_texture(const TextureDesc &d) override {
        if (d.width <= 0 || d.height <= 0) throw std::runtime_error("create_texture: empty size");
        auto t = std::make_shared<D3DTexture>();
        t->d = d;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(d.width);
        td.Height = UINT(d.height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dxgi(d.format);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (d.shader_write) {
            if (d.format == Format::BGRA8 && !typed_uav(DXGI_FORMAT_B8G8R8A8_UNORM)) td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (!typed_uav(td.Format))
                throw std::runtime_error(std::string("this GPU has no typed UAV support for ") + format_name(d.format));
            td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        }
        t->phys = td.Format;
        if (d.render_target) td.BindFlags |= D3D11_BIND_RENDER_TARGET;
        check(dev_->CreateTexture2D(&td, nullptr, &t->tex), "CreateTexture2D");
        make_views(*t);
        return t;
    }

    std::shared_ptr<Buffer> create_buffer(size_t bytes, const void *init) override {
        auto b = std::make_shared<D3DBuffer>();
        b->bytes = bytes;
        b->owner = this;
        b->shadow.assign(std::max<size_t>(bytes, 1), 0);
        if (init) std::memcpy(b->shadow.data(), init, bytes);
        return b;
    }

    void upload(Texture &t, const void *data, ptrdiff_t row_bytes) override {
        auto &dt = as(t);
        Lock lock(context_mutex());
        const int w = dt.d.width, h = dt.d.height, bpp = bytes_per_pixel(dt.d.format);
        const size_t tight = size_t(w) * bpp;
        if (row_bytes > 0 && !dt.swap_rb()) {
            ctx_->UpdateSubresource(dt.tex.Get(), 0, nullptr, data, UINT(row_bytes), 0);
            return;
        }
        // Bottom-up / zero stride / swizzled storage: repack top-down.
        std::vector<uint8_t> tmp(tight * h);
        const auto *src = static_cast<const uint8_t *>(data);
        ptrdiff_t stride = row_bytes ? row_bytes : ptrdiff_t(tight);
        for (int y = 0; y < h; ++y) std::memcpy(tmp.data() + tight * y, src + stride * y, tight);
        if (dt.swap_rb())
            for (size_t i = 0; i < tmp.size(); i += 4) std::swap(tmp[i], tmp[i + 2]);
        ctx_->UpdateSubresource(dt.tex.Get(), 0, nullptr, tmp.data(), UINT(tight), 0);
    }

    // Copies into a staging texture of its own, waits for that copy (and
    // everything before it) without holding the context lock, so threads
    // sharing the device keep submitting meanwhile, then maps it.
    void download(const Texture &t, void *data, ptrdiff_t row_bytes) override {
        auto &dt = as(t);
        const int w = dt.d.width, h = dt.d.height, bpp = bytes_per_pixel(dt.d.format);
        const size_t tight = size_t(w) * bpp;
        Staging st = take_staging(dt);
        ID3D11Texture2D *staging = st.tex.Get();
        {
            Lock lock(context_mutex());
            ctx_->CopyResource(staging, dt.tex.Get());
        }
        try {
            finish();
        } catch (...) {
            give_staging(std::move(st));
            throw;
        }
        Lock lock(context_mutex());
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr)) {
            give_staging(std::move(st));
            check(hr, "Map(staging)");
        }
        auto *dst = static_cast<uint8_t *>(data);
        ptrdiff_t stride = row_bytes ? row_bytes : ptrdiff_t(tight);
        for (int y = 0; y < h; ++y) {
            uint8_t *row = dst + stride * y;
            std::memcpy(row, static_cast<const uint8_t *>(m.pData) + size_t(m.RowPitch) * y, tight);
            if (dt.swap_rb())
                for (size_t i = 0; i < tight; i += 4) std::swap(row[i], row[i + 2]);
        }
        ctx_->Unmap(staging, 0);
        give_staging(std::move(st));
    }

    void upload(Buffer &b, const void *data, size_t bytes, size_t offset) override {
        auto &db = static_cast<D3DBuffer &>(b);
        if (offset + bytes > db.bytes) throw std::runtime_error("buffer upload out of range");
        Lock lock(context_mutex());
        if (!db.buf) {
            std::memcpy(db.shadow.data() + offset, data, bytes);
            return;
        }
        D3D11_BOX box{UINT(offset), 0, 0, UINT(offset + bytes), 1, 1};
        ctx_->UpdateSubresource(db.buf.Get(), 0, &box, data, 0, 0);
    }

    // Compiled once per device: with a shared device, once per process.
    Kernel *kernel(std::string_view name) override {
        std::string key(name);
        std::lock_guard<std::mutex> lock(kern_mu_);
        auto it = kernels_.find(key);
        if (it != kernels_.end()) return it->second.get();
        const ShaderFile *file = nullptr;
        const ShaderEntry *e = find_shader(name, &file);
        if (!e) throw std::runtime_error("unknown kernel '" + key + "' (not registered with ntscrt_add_shaders?)");
        auto k = std::make_unique<D3DKernel>();
        k->entry = e;
        k->file = file;
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = compile()(e->hlsl, std::strlen(e->hlsl), key.c_str(), nullptr, nullptr, e->name, "cs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) {
            std::string msg = "D3DCompile " + key + ": ";
            msg += errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                          : hr_msg("D3DCompile", hr);
            throw std::runtime_error(msg);
        }
        check(dev_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &k->cs),
              "CreateComputeShader");
        Kernel *raw = k.get();
        kernels_[key] = std::move(k);
        return raw;
    }

    void dispatch(Kernel *kk, const Bindings &b, int wx, int wy, int wz) override {
        if (!kk) throw std::runtime_error("dispatch: null kernel");
        auto *k = static_cast<D3DKernel *>(kk);
        const ShaderEntry &e = *k->entry;
        if (wx <= 0 || wy <= 0 || wz <= 0) return;
        if (b.textures.size() > 8 || b.buffers.size() > 8 || b.out_textures.size() > 8 || b.out_buffers.size() > 8)
            throw std::runtime_error("dispatch: at most 8 bindings per class");

        ID3D11ShaderResourceView *srvs[16] = {};
        ID3D11UnorderedAccessView *uavs[16] = {};
        for (size_t i = 0; i < b.textures.size(); ++i)
            if (b.textures[i]) srvs[i] = as(*b.textures[i]).srv.Get();
        for (size_t i = 0; i < b.out_textures.size(); ++i)
            if (b.out_textures[i]) {
                auto &t = as(*b.out_textures[i]);
                if (!t.uav) throw std::runtime_error("dispatch: output texture lacks shader_write");
                uavs[i] = t.uav.Get();
            }
        for (size_t i = 0; i < b.buffers.size(); ++i)
            if (b.buffers[i]) {
                auto &db = const_cast<D3DBuffer &>(static_cast<const D3DBuffer &>(*b.buffers[i]));
                ensure_stride(db, e.buffer_strides[i] ? e.buffer_strides[i] : (db.stride ? db.stride : 4));
                srvs[8 + i] = db.srv.Get();
            }
        for (size_t i = 0; i < b.out_buffers.size(); ++i)
            if (b.out_buffers[i]) {
                auto &db = static_cast<D3DBuffer &>(*b.out_buffers[i]);
                ensure_stride(db, e.buffer_strides[8 + i] ? e.buffer_strides[8 + i] : (db.stride ? db.stride : 4));
                uavs[8 + i] = db.uav.Get();
            }

        Lock lock(context_mutex());  // the constant buffer and the bind-dispatch-unbind sequence
        ID3D11Buffer *cb = nullptr;
        if (b.constants && b.constants_size) {
            size_t need = std::max<size_t>(b.constants_size, e.cb_size);
            need = (need + 15) / 16 * 16;
            if (cb_size_ < need) {
                cb_.Reset();
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = UINT(need);
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                check(dev_->CreateBuffer(&bd, nullptr, &cb_), "CreateBuffer(constants)");
                cb_size_ = need;
            }
            D3D11_MAPPED_SUBRESOURCE m{};
            check(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map(constants)");
            std::memset(m.pData, 0, cb_size_);
            std::memcpy(m.pData, b.constants, b.constants_size);
            ctx_->Unmap(cb_.Get(), 0);
            cb = cb_.Get();
        }

        ID3D11SamplerState *samplers[2] = {linear_.Get(), point_.Get()};
        ctx_->CSSetShader(k->cs.Get(), nullptr, 0);
        ctx_->CSSetConstantBuffers(0, 1, &cb);
        ctx_->CSSetSamplers(0, 2, samplers);
        // UAVs first: a resource last bound as UAV must leave UAV slots before
        // its SRV binds, and vice versa.
        ctx_->CSSetShaderResources(0, 16, null_srvs_);
        ctx_->CSSetUnorderedAccessViews(0, 16, uavs, nullptr);
        ctx_->CSSetShaderResources(0, 16, srvs);
        auto groups = [](int work, uint32_t threads) { return UINT((uint32_t(work) + threads - 1) / threads); };
        ctx_->Dispatch(groups(wx, e.threads[0]), groups(wy, e.threads[1]), groups(wz, e.threads[2]));
        ctx_->CSSetShaderResources(0, 16, null_srvs_);
        ctx_->CSSetUnorderedAccessViews(0, 16, null_uavs_, nullptr);
    }

    void copy(const Texture &src, Texture &dst) override {
        auto &s = as(src);
        auto &d = as(dst);
        if (s.phys == d.phys) {
            Lock lock(context_mutex());
            if (s.d.width == d.d.width && s.d.height == d.d.height) {
                ctx_->CopyResource(d.tex.Get(), s.tex.Get());
            } else {
                D3D11_BOX box{0, 0, 0, UINT(std::min(s.d.width, d.d.width)), UINT(std::min(s.d.height, d.d.height)), 1};
                ctx_->CopySubresourceRegion(d.tex.Get(), 0, 0, 0, 0, s.tex.Get(), 0, &box);
            }
            return;
        }
        struct { uint32_t w, h, pad0, pad1; } p{uint32_t(std::min(s.d.width, d.d.width)),
                                                uint32_t(std::min(s.d.height, d.d.height)), 0, 0};
        Bindings b;
        b.constants = &p;
        b.constants_size = sizeof p;
        b.textures = {&src};
        b.out_textures = {&dst};
        dispatch(kernel("gpu_builtin/copy"), b, int(p.w), int(p.h), 1);
    }

    // Waits for everything submitted so far. With D3D11.4 fences: signal a
    // fence value of its own and block on an event until the GPU reaches it
    // (threads sharing the device each wait for their own point). Otherwise
    // an event query of its own, polled with backoff (yield, then sleep).
    // The context lock is held only to submit / poll, never while waiting.
    void finish() override {
        if (fence_) {
            uint64_t v;
            {
                Lock lock(context_mutex());
                v = ++fence_value_;
                ctx4_->Signal(fence_.Get(), v);
                ctx_->Flush();
            }
            if (fence_->GetCompletedValue() < v) {
                HANDLE ev = wait_event();
                check(fence_->SetEventOnCompletion(v, ev), "ID3D11Fence::SetEventOnCompletion");
                // (re-checked: a registration left from an earlier, abandoned
                // wait may set the event early)
                while (fence_->GetCompletedValue() < v && dev_->GetDeviceRemovedReason() == S_OK)
                    WaitForSingleObject(ev, 200);
            }
            if (dev_->GetDeviceRemovedReason() != S_OK)
                throw std::runtime_error(hr_msg("D3D11 device removed", dev_->GetDeviceRemovedReason()));
            return;
        }
        ComPtr<ID3D11Query> q = take_query();
        {
            Lock lock(context_mutex());
            ctx_->End(q.Get());
            ctx_->Flush();
        }
        for (int polls = 0;; ++polls) {
            BOOL done = FALSE;
            HRESULT hr;
            {
                Lock lock(context_mutex());
                // (not DONOTFLUSH: some drivers then never finish the query
                // while other threads keep submitting)
                hr = ctx_->GetData(q.Get(), &done, sizeof done, 0);
            }
            if (hr == S_OK && done) break;
            if (FAILED(hr) || dev_->GetDeviceRemovedReason() != S_OK) {
                HRESULT why = dev_->GetDeviceRemovedReason();
                throw std::runtime_error(hr_msg("D3D11 device removed", why != S_OK ? why : hr));
            }
            if (polls < 64) SwitchToThread();
            else if (polls < 256) Sleep(0);
            else Sleep(1);
        }
        give_query(std::move(q));
    }

    bool lost() const override { return dev_->GetDeviceRemovedReason() != S_OK; }

    void *native_device() const override { return dev_.Get(); }
    void *native_context() override { return ctx_.Get(); }

    std::shared_ptr<Texture> wrap_native_texture(void *native) override {
        if (!native) throw std::runtime_error("wrap_native_texture: null");
        auto t = std::make_shared<D3DTexture>();
        t->tex = static_cast<ID3D11Texture2D *>(native);  // ComPtr AddRefs
        D3D11_TEXTURE2D_DESC td{};
        t->tex->GetDesc(&td);
        if (!from_dxgi(td.Format, t->d.format))
            throw std::runtime_error("wrap_native_texture: unsupported DXGI format " + std::to_string(td.Format));
        t->phys = td.Format;
        t->d.width = int(td.Width);
        t->d.height = int(td.Height);
        t->d.shader_write = (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
        t->d.render_target = (td.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
        make_views(*t, (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0);
        return t;
    }

private:
    static D3DTexture &as(const Texture &t) {
        auto *p = dynamic_cast<const D3DTexture *>(&t);
        if (!p) throw std::runtime_error("texture belongs to another backend");
        return const_cast<D3DTexture &>(*p);
    }

    void make_views(D3DTexture &t, bool srv = true) {
        DXGI_FORMAT f = t.phys;
        if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) f = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) f = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (srv) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = f;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            check(dev_->CreateShaderResourceView(t.tex.Get(), &sd, &t.srv), "CreateShaderResourceView");
        }
        if (t.d.shader_write) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = f;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            check(dev_->CreateUnorderedAccessView(t.tex.Get(), &ud, &t.uav), "CreateUnorderedAccessView");
        }
        if (t.d.render_target) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = f;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            check(dev_->CreateRenderTargetView(t.tex.Get(), &rd, &t.rtv), "CreateRenderTargetView");
        }
    }

    bool typed_uav(DXGI_FORMAT f) {
        UINT support = 0;
        if (std::getenv("NTSCRT_D3D11_NO_BGRA_UAV") && f == DXGI_FORMAT_B8G8R8A8_UNORM) return false;  // testing
        return SUCCEEDED(dev_->CheckFormatSupport(f, &support)) &&
               (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW);
    }

public:
    // (Re)create a structured buffer with `stride`, keeping its contents.
    void ensure_stride(D3DBuffer &b, uint32_t stride) {
        Lock lock(context_mutex());
        if (b.buf && b.stride == stride) return;
        size_t elems = (std::max<size_t>(b.bytes, 1) + stride - 1) / stride;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(elems * stride);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = stride;
        std::vector<uint8_t> init;
        D3D11_SUBRESOURCE_DATA sd{};
        if (!b.buf) {
            init = std::move(b.shadow);
            init.resize(bd.ByteWidth, 0);
            sd.pSysMem = init.data();
        }
        ComPtr<ID3D11Buffer> nb;
        check(dev_->CreateBuffer(&bd, b.buf ? nullptr : &sd, &nb), "CreateBuffer(structured)");
        if (b.buf) {
            D3D11_BOX box{0, 0, 0, UINT(std::min<size_t>(b.bytes, bd.ByteWidth)), 1, 1};
            ctx_->CopySubresourceRegion(nb.Get(), 0, 0, 0, 0, b.buf.Get(), 0, &box);
        }
        b.buf = nb;
        b.stride = stride;
        b.srv.Reset();
        b.uav.Reset();
        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = DXGI_FORMAT_UNKNOWN;
        svd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        svd.Buffer.FirstElement = 0;
        svd.Buffer.NumElements = UINT(elems);
        check(dev_->CreateShaderResourceView(b.buf.Get(), &svd, &b.srv), "CreateShaderResourceView(buffer)");
        D3D11_UNORDERED_ACCESS_VIEW_DESC uvd{};
        uvd.Format = DXGI_FORMAT_UNKNOWN;
        uvd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uvd.Buffer.NumElements = UINT(elems);
        check(dev_->CreateUnorderedAccessView(b.buf.Get(), &uvd, &b.uav), "CreateUnorderedAccessView(buffer)");
    }

private:

    ComPtr<ID3D11Query> take_query() {
        {
            std::lock_guard<std::mutex> lock(query_mu_);
            if (!queries_.empty()) {
                ComPtr<ID3D11Query> q = std::move(queries_.back());
                queries_.pop_back();
                return q;
            }
        }
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        ComPtr<ID3D11Query> q;
        check(dev_->CreateQuery(&qd, &q), "CreateQuery");
        return q;
    }
    void give_query(ComPtr<ID3D11Query> q) {
        std::lock_guard<std::mutex> lock(query_mu_);
        if (queries_.size() < 16) queries_.push_back(std::move(q));
    }

    // Staging textures are checked out for one download (another thread's
    // download may not reuse one between its copy and its map).
    struct Staging {
        std::tuple<int, int, int> key;
        ComPtr<ID3D11Texture2D> tex;
    };
    Staging take_staging(const D3DTexture &t) {
        auto key = std::make_tuple(t.d.width, t.d.height, int(t.phys));
        {
            std::lock_guard<std::mutex> lock(staging_mu_);
            auto it = staging_.find(key);
            if (it != staging_.end()) {
                Staging s{key, std::move(it->second)};
                staging_.erase(it);
                return s;
            }
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(t.d.width);
        td.Height = UINT(t.d.height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = t.phys;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Staging s{key, nullptr};
        check(dev_->CreateTexture2D(&td, nullptr, &s.tex), "CreateTexture2D(staging)");
        return s;
    }
    void give_staging(Staging s) {
        std::lock_guard<std::mutex> lock(staging_mu_);
        if (staging_.size() >= 16) staging_.clear();  // sizes keep changing: start over
        staging_.emplace(s.key, std::move(s.tex));
    }

    static pD3DCompile compile() {
        static pD3DCompile fn = [] {
            HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
            auto f = m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
            return f;
        }();
        if (!fn) throw std::runtime_error("d3dcompiler_47.dll (D3DCompile) is not available");
        return fn;
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11SamplerState> linear_, point_;
    ComPtr<ID3D11DeviceContext4> ctx4_;
    ComPtr<ID3D11Fence> fence_;
    uint64_t fence_value_ = 0;  // last signalled (context lock)
    // One auto-reset event per waiting thread, kept for the thread's life.
    static HANDLE wait_event() {
        struct Ev {
            HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            ~Ev() { if (h) CloseHandle(h); }
        };
        thread_local Ev e;
        if (!e.h) throw std::runtime_error("CreateEvent failed");
        return e.h;
    }
    std::mutex query_mu_;
    std::vector<ComPtr<ID3D11Query>> queries_;  // idle event queries for finish()
    std::mutex kern_mu_;                         // kernels_
    ComPtr<ID3D11Buffer> cb_;
    size_t cb_size_ = 0;
    bool warp_ = false;
    std::unordered_map<std::string, std::unique_ptr<D3DKernel>> kernels_;
    std::mutex staging_mu_;
    std::multimap<std::tuple<int, int, int>, ComPtr<ID3D11Texture2D>> staging_;  // idle ones
    ID3D11ShaderResourceView *null_srvs_[16] = {};
    ID3D11UnorderedAccessView *null_uavs_[16] = {};
};

void *D3DBuffer::native() const {
    auto *self = const_cast<D3DBuffer *>(this);
    if (!buf && owner) owner->ensure_stride(*self, 4);
    return buf.Get();
}

}  // namespace

std::unique_ptr<Device> create_d3d11_device(void *native_device, std::string &err) {
    try {
        if (native_device) {
            ComPtr<ID3D11Device> dev(static_cast<ID3D11Device *>(native_device));
            if (dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
                err = "host ID3D11Device is below feature level 11_0";
                return nullptr;
            }
            return std::make_unique<D3D11Device>(dev, false);
        }
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const char *force = std::getenv("NTSCRT_D3D11_WARP");
        bool want_warp = force && *force && *force != '0';
        HRESULT hw = E_FAIL;
        ComPtr<ID3D11Device> dev;
        if (!want_warp) {
            for (int attempt = 0; attempt < 2 && FAILED(hw); ++attempt)  // 11_1 unknown on old runtimes
                hw = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + attempt,
                                       UINT(std::size(levels)) - attempt, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
            if (SUCCEEDED(hw)) return std::make_unique<D3D11Device>(dev, false);
        }
        HRESULT wp = E_FAIL;
        for (int attempt = 0; attempt < 2 && FAILED(wp); ++attempt)
            wp = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels + attempt,
                                   UINT(std::size(levels)) - attempt, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
        if (SUCCEEDED(wp)) return std::make_unique<D3D11Device>(dev, true);
        err = "no Direct3D 11 device (feature level 11_0): " + hr_msg("hardware D3D11CreateDevice", hw) + "; " +
              hr_msg("WARP D3D11CreateDevice", wp);
        return nullptr;
    } catch (const std::exception &e) {
        err = e.what();
        return nullptr;
    }
}

namespace d3d11 {
ID3D11ShaderResourceView *srv(const Texture &t) {
    auto *p = dynamic_cast<const D3DTexture *>(&t);
    return p ? p->srv.Get() : nullptr;
}
ID3D11RenderTargetView *rtv(const Texture &t) {
    auto *p = dynamic_cast<const D3DTexture *>(&t);
    return p ? p->rtv.Get() : nullptr;
}
bool using_warp(const Device &d) {
    auto *p = dynamic_cast<const D3D11Device *>(&d);
    return p && p->warp();
}
}  // namespace d3d11

}  // namespace ntscrt::gpu
