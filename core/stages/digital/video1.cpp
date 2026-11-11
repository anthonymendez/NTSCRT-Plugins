// video1.cpp — port of upstream Sources/CrtCodec/Video1Codec.swift (v0.20.0).
// Same arithmetic and order as upstream (SIMD3<Float> sums done per channel).
#include "video1.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "parallel.h"

namespace ntscrt::digital::video1 {

namespace {

constexpr double kRootWorst = 50, kRootBest = 1;

inline RGB operator+(RGB a, RGB b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline RGB operator-(RGB a, RGB b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline RGB operator/(RGB a, float d) { return {a.x / d, a.y / d, a.z / d}; }

inline float luma(RGB c) { return 0.299f * c.x + 0.587f * c.y + 0.114f * c.z; }
inline float distance(RGB a, RGB b) {
    RGB d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}
inline RGB quantize(RGB c) { return {q555(c.x), q555(c.y), q555(c.z)}; }

// RGB of a picture pixel (full-resolution color: Video 1 has no subsampling).
inline RGB rgb(const Picture &p, size_t i) {
    float y = p.y.data[i], cb = p.cb.data[i] - 128, cr = p.cr.data[i] - 128;
    return {y + 1.402f * cr, y - 0.344136f * cb - 0.714136f * cr, y + 1.772f * cb};
}

inline void store(RGB c, Picture &p, size_t i) {
    p.y.data[i] = 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
    p.cb.data[i] = 128 - 0.168736f * c.x - 0.331264f * c.y + 0.5f * c.z;
    p.cr.data[i] = 128 + 0.5f * c.x - 0.418688f * c.y - 0.081312f * c.z;
}

// Two 15-bit colors for the members of `pixels`: split at the mean
// brightness, then one pass of reassigning to the nearer color. Writes the
// coded colors into `out`; returns the squared error.
template <size_t N>
float twoColors(const std::array<RGB, 16> &pixels, const std::array<int, N> &members, std::array<RGB, 16> &out) {
    float mean = 0;
    for (int i : members) mean += luma(pixels[size_t(i)]);
    mean /= float(N);
    std::array<bool, N> hi{};
    for (size_t k = 0; k < N; ++k) hi[k] = luma(pixels[size_t(members[k])]) > mean;
    auto centers = [&](RGB &a, RGB &b) {
        RGB sa{}, sb{};
        float na = 0, nb = 0;
        for (size_t k = 0; k < N; ++k) {
            const RGB &p = pixels[size_t(members[k])];
            if (hi[k]) {
                sb = sb + p;
                nb += 1;
            } else {
                sa = sa + p;
                na += 1;
            }
        }
        if (!(na > 0) || !(nb > 0)) return false;
        a = sa / na;
        b = sb / nb;
        return true;
    };
    RGB a, b;
    if (!centers(a, b)) {
        // All one brightness: one color, twice.
        RGB m{};
        for (int i : members) m = m + pixels[size_t(i)];
        RGB c = quantize(m / float(N));
        float e = 0;
        for (int i : members) {
            out[size_t(i)] = c;
            e += distance(pixels[size_t(i)], c);
        }
        return e;
    }
    for (size_t k = 0; k < N; ++k) {
        const RGB &p = pixels[size_t(members[k])];
        hi[k] = distance(p, b) < distance(p, a);
    }
    RGB a2, b2;
    if (centers(a2, b2)) {
        a = a2;
        b = b2;
    }
    const RGB qa = quantize(a), qb = quantize(b);
    float e = 0;
    for (int i : members) {
        const RGB &p = pixels[size_t(i)];
        RGB c = distance(p, qb) < distance(p, qa) ? qb : qa;
        out[size_t(i)] = c;
        e += distance(p, c);
    }
    return e;
}

constexpr std::array<int, 16> kAll = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr std::array<std::array<int, 4>, 4> kQuarters = {
    {{0, 1, 4, 5}, {2, 3, 6, 7}, {8, 9, 12, 13}, {10, 11, 14, 15}}};

// Mean absolute luma change against the reference (for the fixed-bitrate squeeze).
float change(const Picture &a, const Picture &b, const Geometry &g) {
    std::vector<float> rows(size_t(g.height), 0);
    parallel_for(g.height, [&](int y) {
        float s = 0;
        for (int x = 0; x < g.width; ++x)
            s += std::fabs(a.y.data[size_t(y) * a.y.width + x] - b.y.data[size_t(y) * b.y.width + x]);
        rows[size_t(y)] = s;
    });
    float sum = 0;
    for (float r : rows) sum += r;
    return sum / float(g.width * g.height);
}

}  // namespace

float threshold(double quality, float squeeze) {
    double q = std::min(1.0, std::max(0.0, quality));
    double r = kRootWorst + (kRootBest - kRootWorst) * q;
    return float(r * r) * squeeze * squeeze;
}

float q555(float v) { return std::round(std::min(255.0f, std::max(0.0f, v)) * 31 / 255) * 255 / 31; }

Coded codeBlock(const std::array<RGB, 16> &src, const std::array<RGB, 16> *reference, float limit) {
    if (reference) {
        float e = 0;
        for (size_t i = 0; i < 16; ++i) e += distance(src[i], (*reference)[i]);
        if (e <= limit) return {Mode::Skip, *reference};
    }
    RGB mean{};
    for (const RGB &p : src) mean = mean + p;
    mean = mean / 16;
    const RGB fill = quantize(mean);
    float fillError = 0;
    for (const RGB &p : src) fillError += distance(p, fill);
    Coded filled{Mode::Fill, {}};
    filled.pixels.fill(fill);
    if (fillError <= limit) return filled;
    Coded two{Mode::Two, src};
    float twoError = twoColors(src, kAll, two.pixels);
    Coded eight{Mode::Eight, src};
    float eightError = 0;
    for (const auto &q : kQuarters) eightError += twoColors(src, q, eight.pixels);
    float good = std::max(limit, eightError * 1.05f + 1e-3f);
    if (fillError <= good) return filled;
    if (twoError <= good) return two;
    return eight;
}

PicturePtr step(CodecState &state, const PicturePtr &source, FrameKind kind, int index,
                const codec::FrameSettings &s, uint64_t seedMix) {
    const Geometry g = state.geometry;
    const auto lost = codec::lostBlocks(s.dataLoss, index, g, seedMix);
    const bool predicted = kind == FrameKind::Predicted && state.encoder != nullptr;
    const PicturePtr encRef = predicted ? state.encoder : nullptr;
    const float complexity = predicted ? change(*source, *encRef, g) : codec::detail(*source, g);
    const float squeeze = codec::squeeze(complexity, predicted ? 3.0f : 14.0f, s.fixedBitrate);
    const float limit = threshold(s.quality, squeeze) * 48;

    const PicturePtr previous = state.encoder;
    auto enc = std::make_shared<Picture>(g);
    // The decoder starts from what it had; coded blocks overwrite it.
    auto dec = std::make_shared<Picture>(g);
    const PicturePtr decBase = state.decoder ? state.decoder : previous;
    const PicturePtr base = kind == FrameKind::IntraOnly ? previous : decBase;
    const int w = g.lumaW(), cols = g.lumaW() / 4;
    bool anyLoss = false;
    if (lost) anyLoss = std::find(lost->begin(), lost->end(), true) != lost->end();
    const Picture &src = *source;
    parallel_for(g.lumaH() / 4, [&](int by) {
        std::array<RGB, 16> px{}, ref{};
        for (int bx = 0; bx < cols; ++bx) {
            for (int i = 0; i < 16; ++i) {
                size_t k = size_t(by * 4 + i / 4) * w + bx * 4 + i % 4;
                px[size_t(i)] = rgb(src, k);
                if (encRef) ref[size_t(i)] = rgb(*encRef, k);
            }
            const Coded coded = codeBlock(px, encRef ? &ref : nullptr, limit);
            const int mb = (by * 4 / 16) * g.mbCols() + bx * 4 / 16;
            const bool blockLost = lost && (*lost)[size_t(mb)];
            for (int i = 0; i < 16; ++i) {
                size_t k = size_t(by * 4 + i / 4) * w + bx * 4 + i % 4;
                if (coded.mode == Mode::Skip && encRef) {
                    // Copied exactly, never re-converted: a block can be
                    // skipped for hundreds of frames.
                    enc->y.data[k] = encRef->y.data[k];
                    enc->cb.data[k] = encRef->cb.data[k];
                    enc->cr.data[k] = encRef->cr.data[k];
                } else {
                    store(coded.pixels[size_t(i)], *enc, k);
                }
                if ((blockLost || coded.mode == Mode::Skip) && base) {
                    // Nothing arrived: whatever the decoder had stays.
                    dec->y.data[k] = base->y.data[k];
                    dec->cb.data[k] = base->cb.data[k];
                    dec->cr.data[k] = base->cr.data[k];
                } else {
                    store(coded.pixels[size_t(i)], *dec, k);
                }
            }
        }
    });
    state.encoder = enc;
    state.vectors.assign(size_t(g.mbCount()), MotionVector{});
    switch (kind) {
    case FrameKind::Key:
    case FrameKind::DroppedKey:
        // Keyframes arrive whole (streams protect them).
        state.decoder = enc;
        state.diverged = false;
        break;
    case FrameKind::IntraOnly:
        state.decoder = anyLoss && previous ? dec : enc;
        state.diverged = state.decoder != enc;
        break;
    case FrameKind::Predicted:
        if (!state.diverged && !anyLoss) {
            state.decoder = enc;
        } else {
            state.decoder = dec;
            state.diverged = true;
        }
        break;
    }
    return state.decoder;
}

}  // namespace ntscrt::digital::video1
