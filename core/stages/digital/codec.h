// codec.h — port of upstream Sources/CrtCodec/DigitalCodec.swift.
//
// A small, real block-transform video codec in the MPEG/H.263/DV family:
// BT.601 Y'CbCr with subsampled color, 8x8 DCT blocks against the JPEG
// reference tables, motion-compensated prediction between keyframes, and
// separate encoder/decoder references (their divergence is what packet-loss
// smearing and datamoshing are). Deterministic regardless of thread count.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ntscrt::digital {

enum class Chroma : int { Full = 0, HalfWide = 1, HalfBoth = 2, QuarterWide = 3 };

inline int chroma_fx(Chroma c) { return c == Chroma::Full ? 1 : c == Chroma::QuarterWide ? 4 : 2; }
inline int chroma_fy(Chroma c) { return c == Chroma::HalfBoth ? 2 : 1; }

// One plane of samples on the 0..255 scale, padded to whole blocks.
struct Plane {
    int width = 0, height = 0;
    std::vector<float> data;
    Plane() = default;
    Plane(int w, int h, float fill) : width(w), height(h), data(size_t(w) * h, fill) {}
    float *p() { return data.data(); }
    const float *p() const { return data.data(); }
    float clamped(int x, int y) const {
        int yy = y < 0 ? 0 : (y > height - 1 ? height - 1 : y);
        int xx = x < 0 ? 0 : (x > width - 1 ? width - 1 : x);
        return data.data()[size_t(yy) * width + xx];
    }
};

struct Geometry {
    int width = 0, height = 0;
    Chroma chroma = Chroma::HalfBoth;
    int lumaW() const { return (width + 15) / 16 * 16; }
    int lumaH() const { return (height + 15) / 16 * 16; }
    int sx() const { return chroma_fx(chroma); }
    int sy() const { return chroma_fy(chroma); }
    int chromaW() const { return (lumaW() / sx() + 7) / 8 * 8; }
    int chromaH() const { return (lumaH() / sy() + 7) / 8 * 8; }
    int mbCols() const { return lumaW() / 16; }
    int mbRows() const { return lumaH() / 16; }
    int mbCount() const { return mbCols() * mbRows(); }
    auto operator<=>(const Geometry &) const = default;
};

// A picture: brightness and two color-difference planes.
struct Picture {
    Plane y, cb, cr;
    explicit Picture(const Geometry &g)
        : y(g.lumaW(), g.lumaH(), 0), cb(g.chromaW(), g.chromaH(), 128), cr(g.chromaW(), g.chromaH(), 128) {}
    Plane &plane(int i) { return i == 0 ? y : i == 1 ? cb : cr; }
    const Plane &plane(int i) const { return i == 0 ? y : i == 1 ? cb : cr; }
    size_t byteCount() const { return (size_t(y.width) * y.height + 2 * size_t(cb.width) * cb.height) * sizeof(float); }
};
using PicturePtr = std::shared_ptr<Picture>;

enum class FrameKind { IntraOnly, Key, DroppedKey, Predicted };

struct MotionVector {
    int32_t dx = 0, dy = 0;
    bool operator==(const MotionVector &) const = default;
};

// The codec's state between frames.
struct CodecState {
    Geometry geometry;
    PicturePtr encoder;  // what a perfect decoder would hold
    PicturePtr decoder;  // what this decoder actually holds (may alias encoder)
    std::vector<MotionVector> vectors;
    bool diverged = false;

    explicit CodecState(const Geometry &g) : geometry(g), vectors(size_t(g.mbCount())) {}
    std::shared_ptr<CodecState> copy() const;
};

// SplitMix64 (upstream SplitMix).
struct SplitMix {
    uint64_t state;
    explicit SplitMix(uint64_t seed) : state(seed) {}
    uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double unit() { return double(next() >> 11) / double(uint64_t(1) << 53); }
};

namespace codec {

extern const float kLumaTable[64];
extern const float kChromaTable[64];
const float *basis();  // orthonormal 8-point DCT-II, C[u*8+x]

float tableScale(double quality);

struct Steps {
    std::array<float, 64> luma, chroma;
    float inter;
    Steps(double quality, float squeeze);
};

void forwardDCT(const float *input, int stride, float *out, float *tmp, const float *c);
void inverseDCT(const float *coef, float *out, int stride, float *tmp, const float *c);

void codeIntra(const Picture &source, Picture &recon, const Steps &steps);

struct Motion {
    std::vector<MotionVector> vectors;
    float error;
};
Motion searchMotion(const Picture &source, const Picture &reference, const std::vector<MotionVector> &previous,
                    const Geometry &g);
void compensate(const Picture &reference, const std::vector<MotionVector> &vectors, const Geometry &g,
                Picture &prediction);
void codeResidual(const Picture &source, const Picture &prediction, float step, Picture &residual);
void reconstruct(const Picture &prediction, const Picture &residual, const std::vector<bool> *lost,
                 const Geometry &g, Picture &recon);

// `seedMix` = 0 reproduces upstream exactly; anything else varies the pattern.
std::optional<std::vector<bool>> lostBlocks(double amount, int index, const Geometry &g, uint64_t seedMix);
bool dropsKey(int keyIndex, double amount, uint64_t seedMix);

float squeeze(float complexity, float typical, double strictness);
float detail(const Picture &p, const Geometry &g);

struct FrameSettings {
    double quality, fixedBitrate, dataLoss;
};
struct StepResult {
    PicturePtr picture;  // the decoder's reference (not a copy)
    float step;          // quantizer step for the deblocking filter
};
StepResult step(CodecState &state, const PicturePtr &source, FrameKind kind, int index, const FrameSettings &s,
                uint64_t seedMix);

void copyBlocks(const Picture &source, Picture &target, const std::vector<bool> &mask, const Geometry &g);
void deblock(Picture &p, double strength, float step);

}  // namespace codec
}  // namespace ntscrt::digital
