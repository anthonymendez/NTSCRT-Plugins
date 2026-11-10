// howl_cpu.h — CPU reference of the Screen Loop kernels (shaders/howlaround.slang:
// camera, blur, mean, resample), line for line, on float RGBA images whose
// stored values are quantized to 8 bits like the GPU's unorm textures. Used by
// tests and as the semantic spec for the Slang port.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "howl_counter.h"
#include "howl_math.h"

namespace ntscrt::howl {

// Logical RGBA, 0..1, top row first.
struct ImageF {
    int width = 0, height = 0;
    std::vector<float> px;  // width*height*4
    ImageF() = default;
    ImageF(int w, int h) : width(w), height(h), px(size_t(w) * h * 4, 0.f) {}
    float *at(int x, int y) { return px.data() + (size_t(y) * width + x) * 4; }
    const float *at(int x, int y) const { return px.data() + (size_t(y) * width + x) * 4; }
    static ImageF from_bgra8(const uint8_t *bgra, int w, int h);  // tightly packed
    void to_bgra8(uint8_t *bgra) const;
    bool operator==(const ImageF &o) const { return width == o.width && height == o.height && px == o.px; }
};

// unorm8 store: what writing `x` to an 8-bit texture keeps.
inline float quantize8(float x) {
    x = x < 0 ? 0 : (x > 1 ? 1 : x);
    return float(int(x * 255.f + 0.5f)) / 255.f;
}

// Bilinear sample, clamp to edge, normalized coords (the kernels' linear sampler).
void sample_linear(const ImageF &img, float u, float v, float out[4]);

// howl_camera: what the camera sees. `tv` may be null (a dark TV);
// `counter` (premultiplied) may be null. Output is the scene's size.
ImageF camera_ref(const ImageF &scene, const ImageF *tv, const ImageF *counter, const CameraUniforms &u);
// howl_blur: one separable Gaussian pass.
ImageF blur_ref(const ImageF &src, const BlurUniforms &u);
// howl_resample: bilinear resize (the TV picture when no CRT hook is set).
ImageF resample_ref(const ImageF &src, int w, int h);
// Mean luma (0.299 R + 0.587 G + 0.114 B) — upstream MPSImageStatisticsMean + luma().
double mean_luma(const ImageF &img);

ImageF counter_image(const CounterBitmap &c);

}  // namespace ntscrt::howl
