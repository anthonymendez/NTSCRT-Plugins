// crt_libra.h — librashader, loaded at runtime (librashader_ld.h style, like
// upstream Sources/CrtAppBridge/LibrashaderBridge.m): dlopen/LoadLibrary of
// an explicit path, every symbol resolved by name, ABI version checked.
// Private to core/stages/crt.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../gpu/gpu.h"

namespace ntscrt::crt {

// The preset file can't be read or parsed (vs. a chain / GPU failure).
struct PresetError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ShaderParam {
    std::string name;
    float initial = 0, minimum = 0, maximum = 1, step = 0;
};

// One compiled preset on the HAL device.
class Chain {
public:
    virtual ~Chain() = default;
    // Render `in` into all of `out` (render_target texture). Throws on error.
    virtual void frame(const gpu::Texture &in, gpu::Texture &out, uint64_t frame_count, bool clear_history) = 0;
    virtual void set_param(const std::string &name, float v) = 0;
    const std::vector<ShaderParam> &params() const { return params_; }

protected:
    std::vector<ShaderParam> params_;
};

class Library {
public:
    // Loads librashader from the first existing candidate: $NTSCRT_LIBRASHADER,
    // <resource_dir>/librashader.<ext>, <resource_dir>/../Frameworks/ (macOS
    // bundles), the directory of the module holding this code. nullptr +
    // `err` if not found or the ABI doesn't match. Cached per process.
    static Library *load(const std::string &resource_dir, std::string &err);

    // Creates a chain for `preset_path` on `dev` (D3D11 or Metal runtime).
    std::unique_ptr<Chain> create_chain(gpu::Device &dev, const std::string &preset_path);

    void *sym(const char *name) const;
    std::string path;

    // Turns a libra_error_t into a message and frees it.
    std::string take_error(void *err) const;

    // Opens the preset and returns it (libra_shader_preset_t) with its params.
    void *open_preset(const std::string &preset_path, std::vector<ShaderParam> &params);
    void free_preset(void *preset);

private:
    void *handle_ = nullptr;
};

// Backend-specific chain factories (crt_libra_d3d11.cpp / crt_libra_metal.mm).
std::unique_ptr<Chain> create_d3d11_chain(Library &lib, gpu::Device &dev, const std::string &preset_path);
std::unique_ptr<Chain> create_metal_chain(Library &lib, gpu::Device &dev, const std::string &preset_path);

}  // namespace ntscrt::crt
