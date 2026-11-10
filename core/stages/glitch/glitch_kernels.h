// glitch_kernels.h — the glitch stage's three GPU passes (upstream
// GlitchStage.swift inline MSL: glitch_gate, glitch_chroma, glitch_compose),
// their shared data layout, and a CPU reference implementation with the same
// math (float32, same formulas, same order) as shaders/glitch.slang.
// Owner: agent B.
#pragma once

#include <cstdint>
#include <vector>

#include "../../src/internal.h"
#include "receiver.h"

namespace ntscrt::glitch {

// Mirrors `GlitchU` in the shader (ConstantBuffer at b0): 4-byte scalars only,
// 32 of them = 8 x 16-byte rows, so HLSL cbuffer packing, MSL and C++ agree.
struct GlitchU {
    uint32_t width, activeLines, vbiLines, totalLines;
    uint32_t preEqEnd, vsyncEnd, postEqEnd, ccEnabled;
    uint32_t ccBits, fieldIndex, seed, dropoutCount;
    uint32_t docEnabled, tapsI, tapsQ, colorOnInit;
    float usPerPixel, lineUS, ghostLevel, ghostDelay;
    float ghostCos, ghostSin, brightness, pictureGain;
    float burstAmp, chromaAlpha, accGamma, killerKappa;
    float phaseInit, gainInit, killerInit, gateNoise;
};
static_assert(sizeof(GlitchU) == 128);

// Upstream's fixed GPU noise seed (GlitchStage.encode).
constexpr uint32_t kDefaultNoiseSeed = 0x9E3779B9u;

// Uniforms for drawing `plan` onto a `width`-pixel-wide chain input
// (upstream GlitchStage.encode).
GlitchU make_uniforms(const GlitchFieldPlan &plan, int width, uint32_t noiseSeed = kDefaultNoiseSeed);

// Dropout buffer contents as uploaded (never empty: one inert entry when there
// are none, as upstream does).
std::vector<GlitchDropout> dropout_buffer(const GlitchFieldPlan &plan);

// ---- CPU reference kernels (same layout as the GPU buffers) ----------------------------
// gate: 2 floats per row; color: 4 floats per row. Texture = BGRA8, read as
// unorm rgb, written as round(clamp(rgb)*255) with alpha 255.
void cpu_gate(const GlitchU &U, const GlitchRow *rows, const ImageBGRA8 &src, float *gate);
void cpu_chroma(const GlitchU &U, const float *gate, float *color);
void cpu_compose(const GlitchU &U, const GlitchRow *rows, const float *color, const GlitchDropout *drops,
                 const ImageBGRA8 &src, ImageBGRA8 &dst);

// All three passes. `dst` gets src's size. Returns false (dst = src) when the
// plan doesn't fit the image (upstream's guard).
bool cpu_render_plan(const GlitchFieldPlan &plan, const ImageBGRA8 &src, ImageBGRA8 &dst,
                     uint32_t noiseSeed = kDefaultNoiseSeed);

}  // namespace ntscrt::glitch
