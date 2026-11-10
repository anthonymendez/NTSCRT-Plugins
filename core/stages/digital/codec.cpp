// codec.cpp — port of upstream Sources/CrtCodec/DigitalCodec.swift (2b3013d).
// Arithmetic follows the Swift source operation for operation, in single
// precision, so results match upstream as closely as the platform's libm
// allows. Hot loops use raw pointers (fast in Debug as well).
#include "codec.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "parallel.h"

namespace ntscrt::digital {

static inline float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

std::shared_ptr<CodecState> CodecState::copy() const {
    auto s = std::make_shared<CodecState>(geometry);
    s->encoder = encoder ? std::make_shared<Picture>(*encoder) : nullptr;
    s->decoder = decoder == encoder ? s->encoder : (decoder ? std::make_shared<Picture>(*decoder) : nullptr);
    s->vectors = vectors;
    s->diverged = diverged;
    return s;
}

namespace codec {

// JPEG reference tables (ITU T.81 Annex K), natural order.
const float kLumaTable[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};
const float kChromaTable[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
    99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

const float *basis() {
    static const std::array<float, 64> c = [] {
        std::array<float, 64> b{};
        const double pi = 3.14159265358979323846;
        for (int u = 0; u < 8; ++u) {
            double a = u == 0 ? std::sqrt(1.0 / 8) : std::sqrt(2.0 / 8);
            for (int x = 0; x < 8; ++x) b[u * 8 + x] = float(a * std::cos(double(2 * x + 1) * double(u) * pi / 16));
        }
        return b;
    }();
    return c.data();
}

float tableScale(double quality) {
    double q = 2 + 93 * std::min(1.0, std::max(0.0, quality));
    return float(q < 50 ? 5000 / q : 200 - 2 * q) / 100;
}

Steps::Steps(double quality, float squeeze) {
    float s = tableScale(quality) * squeeze;
    for (int k = 0; k < 64; ++k) {
        luma[k] = std::max(1.0f, std::round(kLumaTable[k] * s));
        chroma[k] = std::max(1.0f, std::round(kChromaTable[k] * s));
    }
    inter = std::max(1.0f, 16 * s);
}

void forwardDCT(const float *input, int stride, float *out, float *tmp, const float *c) {
    for (int y = 0; y < 8; ++y) {
        const float *row = input + y * stride;
        for (int u = 0; u < 8; ++u) {
            const float *cu = c + u * 8;
            float s = 0;
            for (int x = 0; x < 8; ++x) s += cu[x] * row[x];
            tmp[y * 8 + u] = s;
        }
    }
    for (int v = 0; v < 8; ++v) {
        const float *cv = c + v * 8;
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int y = 0; y < 8; ++y) s += cv[y] * tmp[y * 8 + u];
            out[v * 8 + u] = s;
        }
    }
}

void inverseDCT(const float *coef, float *out, int stride, float *tmp, const float *c) {
    for (int y = 0; y < 8; ++y) {
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int v = 0; v < 8; ++v) s += c[v * 8 + y] * coef[v * 8 + u];
            tmp[y * 8 + u] = s;
        }
    }
    for (int y = 0; y < 8; ++y) {
        float *row = out + y * stride;
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int u = 0; u < 8; ++u) s += c[u * 8 + x] * tmp[y * 8 + u];
            row[x] = s;
        }
    }
}

// ---- keyframes ---------------------------------------------------------------

void codeIntra(const Picture &source, Picture &recon, const Steps &steps) {
    const float *c = basis();
    for (int i = 0; i < 3; ++i) {
        const Plane &src = source.plane(i);
        Plane &dst = recon.plane(i);
        const float *t = i == 0 ? steps.luma.data() : steps.chroma.data();
        const int w = src.width;
        const float *sp = src.p();
        float *dp = dst.p();
        parallel_for(src.height / 8, [&](int ty) {
            float scratch[192];
            float *block = scratch, *coef = scratch + 64, *tmp = scratch + 128;
            for (int tx = 0; tx < w / 8; ++tx) {
                size_t origin = size_t(ty) * 8 * w + size_t(tx) * 8;
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x) block[y * 8 + x] = sp[origin + size_t(y) * w + x] - 128;
                forwardDCT(block, 8, coef, tmp, c);
                for (int k = 0; k < 64; ++k) coef[k] = std::round(coef[k] / t[k]) * t[k];
                inverseDCT(coef, block, 8, tmp, c);
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        dp[origin + size_t(y) * w + x] = clampf(std::round(block[y * 8 + x] + 128), 0, 255);
            }
        });
    }
}

// ---- prediction ----------------------------------------------------------------

static inline float sad(const Plane &src, const Plane &ref, int bx, int by, int dx, int dy, float limit) {
    const int w = src.width;
    const int rx = bx + dx, ry = by + dy;
    float total = 0;
    const float *sp = src.p(), *rp = ref.p();
    if (rx >= 0 && ry >= 0 && rx + 16 <= ref.width && ry + 16 <= ref.height) {
        for (int y = 0; y < 16; ++y) {
            const float *s = sp + size_t(by + y) * w + bx;
            const float *r = rp + size_t(ry + y) * w + rx;
            for (int x = 0; x < 16; ++x) total += std::fabs(s[x] - r[x]);
            if (total >= limit) return total;
        }
    } else {
        for (int y = 0; y < 16; ++y) {
            const float *s = sp + size_t(by + y) * w + bx;
            for (int x = 0; x < 16; ++x) total += std::fabs(s[x] - ref.clamped(rx + x, ry + y));
            if (total >= limit) return total;
        }
    }
    return total;
}

static constexpr int kSearchRange = 24;

Motion searchMotion(const Picture &source, const Picture &reference, const std::vector<MotionVector> &previous,
                    const Geometry &g) {
    const int cols = g.mbCols(), rows = g.mbRows();
    std::vector<MotionVector> vectors(size_t(g.mbCount()));
    std::vector<float> rowError(size_t(rows), 0);
    const Plane &src = source.y, &ref = reference.y;
    MotionVector *out = vectors.data();
    float *err = rowError.data();
    const MotionVector *prev = previous.data();
    const float inf = std::numeric_limits<float>::infinity();
    parallel_for(rows, [&](int my) {
        MotionVector left;
        float sum = 0;
        for (int mx = 0; mx < cols; ++mx) {
            const int bx = mx * 16, by = my * 16;
            const int r = kSearchRange;
            auto cost = [&](int vx, int vy, float limit) -> float {
                if (!(std::abs(vx) <= r && std::abs(vy) <= r)) return inf;
                // A small bias keeps still areas still.
                float bias = (vx == 0 && vy == 0) ? -48.0f : 0.0f;
                return sad(src, ref, bx, by, vx, vy, limit - bias) + bias;
            };
            int bestX = 0, bestY = 0;
            float bestCost = cost(0, 0, inf);
            const MotionVector cands[2] = {prev[size_t(my) * cols + mx], left};
            for (const MotionVector &cand : cands) {
                int vx = int(cand.dx), vy = int(cand.dy);
                if (vx == bestX && vy == bestY) continue;
                float cst = cost(vx, vy, bestCost);
                if (cst < bestCost) {
                    bestCost = cst;
                    bestX = vx;
                    bestY = vy;
                }
            }
            int step = 8;
            while (step >= 1) {
                bool moved = true;
                int guardCount = 0;
                while (moved && guardCount < 8) {
                    moved = false;
                    guardCount += 1;
                    const int d[4][2] = {{step, 0}, {-step, 0}, {0, step}, {0, -step}};
                    for (const auto &dd : d) {
                        int vx = bestX + dd[0], vy = bestY + dd[1];
                        float cst = cost(vx, vy, bestCost);
                        if (cst < bestCost) {
                            bestCost = cst;
                            bestX = vx;
                            bestY = vy;
                            moved = true;
                        }
                    }
                }
                step /= 2;
            }
            MotionVector mv{int32_t(bestX), int32_t(bestY)};
            out[size_t(my) * cols + mx] = mv;
            left = mv;
            sum += sad(src, ref, bx, by, bestX, bestY, inf);
        }
        err[my] = sum;
    });
    float total = 0;
    for (float e : rowError) total += e;
    return {std::move(vectors), total / float(g.lumaW() * g.lumaH())};
}

void compensate(const Picture &reference, const std::vector<MotionVector> &vectors, const Geometry &g,
                Picture &prediction) {
    const int cols = g.mbCols(), rows = g.mbRows();
    for (int i = 0; i < 3; ++i) {
        const Plane &ref = reference.plane(i);
        Plane &dst = prediction.plane(i);
        const int fx = i == 0 ? 1 : g.sx(), fy = i == 0 ? 1 : g.sy();
        const int bw = 16 / fx, bh = 16 / fy;
        const int w = dst.width, h = dst.height;
        float *dp = dst.p();
        const MotionVector *vp = vectors.data();
        parallel_for(rows, [&](int my) {
            for (int mx = 0; mx < cols; ++mx) {
                MotionVector v = vp[size_t(my) * cols + mx];
                int dx = int(std::round(float(v.dx) / float(fx)));
                int dy = int(std::round(float(v.dy) / float(fy)));
                int x0 = mx * bw, y0 = my * bh;
                for (int y = y0; y < std::min(h, y0 + bh); ++y)
                    for (int x = x0; x < std::min(w, x0 + bw); ++x) dp[size_t(y) * w + x] = ref.clamped(x + dx, y + dy);
            }
        });
        // Color planes padded past the last macroblock follow the edge.
        if (bw * cols < w || bh * rows < h) {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (x >= bw * cols || y >= bh * rows)
                        dp[size_t(y) * w + x] = dst.clamped(std::min(x, bw * cols - 1), std::min(y, bh * rows - 1));
        }
    }
}

void codeResidual(const Picture &source, const Picture &prediction, float step, Picture &residual) {
    const float *c = basis();
    for (int i = 0; i < 3; ++i) {
        const Plane &src = source.plane(i), &pred = prediction.plane(i);
        Plane &dst = residual.plane(i);
        const int w = src.width;
        const float *sp = src.p(), *pp = pred.p();
        float *dp = dst.p();
        parallel_for(src.height / 8, [&](int ty) {
            float scratch[192];
            float *block = scratch, *coef = scratch + 64, *tmp = scratch + 128;
            for (int tx = 0; tx < w / 8; ++tx) {
                size_t origin = size_t(ty) * 8 * w + size_t(tx) * 8;
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x) {
                        size_t k = origin + size_t(y) * w + x;
                        block[y * 8 + x] = sp[k] - pp[k];
                    }
                forwardDCT(block, 8, coef, tmp, c);
                bool any = false;
                for (int k = 0; k < 64; ++k) {
                    // A dead zone: small changes round to nothing.
                    float q = coef[k] / step;
                    float level = q < 0 ? -std::floor(-q + 0.2f) : std::floor(q + 0.2f);
                    coef[k] = level * step;
                    if (level != 0) any = true;
                }
                if (any) {
                    inverseDCT(coef, dp + origin, w, tmp, c);
                } else {
                    for (int y = 0; y < 8; ++y) std::fill_n(dp + origin + size_t(y) * w, 8, 0.0f);
                }
            }
        });
    }
}

void reconstruct(const Picture &prediction, const Picture &residual, const std::vector<bool> *lost,
                 const Geometry &g, Picture &recon) {
    const int cols = g.mbCols(), rows = g.mbRows();
    std::vector<uint8_t> lostMask;
    if (lost) lostMask.assign(lost->begin(), lost->end());
    const uint8_t *lm = lost ? lostMask.data() : nullptr;
    for (int i = 0; i < 3; ++i) {
        const Plane &pred = prediction.plane(i), &res = residual.plane(i);
        Plane &dst = recon.plane(i);
        const int fx = i == 0 ? 1 : g.sx(), fy = i == 0 ? 1 : g.sy();
        const int w = dst.width;
        const float *pp = pred.p(), *rp = res.p();
        float *dp = dst.p();
        parallel_for(dst.height, [&](int y) {
            int my = std::min(rows - 1, y * fy / 16);
            for (int x = 0; x < w; ++x) {
                size_t k = size_t(y) * w + x;
                float v = pp[k];
                int mx = std::min(cols - 1, x * fx / 16);
                if (!(lm && lm[size_t(my) * cols + mx])) v += rp[k];
                dp[k] = clampf(std::round(v), 0, 255);
            }
        });
    }
}

// ---- errors ----------------------------------------------------------------------

std::optional<std::vector<bool>> lostBlocks(double amount, int index, const Geometry &g, uint64_t seedMix) {
    if (!(amount > 0)) return std::nullopt;
    const int count = g.mbCount(), cols = g.mbCols();
    std::vector<bool> lost(size_t(count), false);
    SplitMix rng((uint64_t(int64_t(index)) * 0x9E3779B9ull ^ 0x10C5ull) ^ seedMix);
    double meanRun = 1 + double(cols) * 2 / 3;
    double start = std::min(1.0, amount * 0.3 / meanRun);
    int i = 0;
    bool any = false;
    while (i < count) {
        if (rng.unit() < start) {
            double u = rng.unit();
            int run = 1 + int(u * u * double(2 * cols));
            for (int j = i; j < std::min(count, i + run); ++j) lost[size_t(j)] = true;
            any = true;
            i += run;
        } else {
            i += 1;
        }
    }
    if (!any) return std::nullopt;
    return lost;
}

bool dropsKey(int keyIndex, double amount, uint64_t seedMix) {
    if (!(amount > 0) || keyIndex <= 0) return false;
    SplitMix rng((uint64_t(keyIndex) * 0xD1B54A32D192ED03ull ^ 0xDA7Aull) ^ seedMix);
    return rng.unit() < amount;
}

// ---- complexity (the fixed-bitrate squeeze) --------------------------------------

float squeeze(float complexity, float typical, double strictness) {
    if (!(strictness > 0)) return 1;
    float ratio = std::max(0.05f, complexity / typical);
    return std::min(6.0f, std::max(0.5f, std::pow(ratio, float(strictness))));
}

float detail(const Picture &p, const Geometry &g) {
    const int w = p.y.width, gw = g.width, gh = g.height;
    std::vector<float> rows(size_t(gh), 0);
    float *out = rows.data();
    const float *yp = p.y.p();
    parallel_for(gh, [&](int y) {
        float s = 0;
        const float *row = yp + size_t(y) * w;
        const float *below = yp + size_t(std::min(gh - 1, y + 1)) * w;
        for (int x = 0; x < gw - 1; ++x) s += std::fabs(row[x + 1] - row[x]) + std::fabs(below[x] - row[x]);
        out[y] = s;
    });
    float total = 0;
    for (float r : rows) total += r;
    return total / float(gw * gh);
}

// ---- one frame -----------------------------------------------------------------------

StepResult step(CodecState &state, const PicturePtr &source, FrameKind kind, int index, const FrameSettings &s,
                uint64_t seedMix) {
    const Geometry g = state.geometry;
    auto lost = lostBlocks(s.dataLoss, index, g, seedMix);

    switch (kind) {
    case FrameKind::IntraOnly:
    case FrameKind::Key:
    case FrameKind::DroppedKey: {
        float sq = squeeze(detail(*source, g), 14, s.fixedBitrate);
        Steps steps(s.quality, sq);
        PicturePtr previous = state.encoder;
        auto recon = std::make_shared<Picture>(g);
        codeIntra(*source, *recon, steps);
        state.encoder = recon;
        state.vectors.assign(size_t(g.mbCount()), MotionVector{});
        if (kind == FrameKind::IntraOnly) {
            // Each frame stands alone; a lost block is patched from the
            // previous frame (DV's error concealment).
            if (lost && previous) {
                auto shown = std::make_shared<Picture>(*recon);
                copyBlocks(*previous, *shown, *lost, g);
                state.decoder = shown;
                state.diverged = true;
            } else {
                state.decoder = recon;
                state.diverged = false;
            }
        } else if (kind == FrameKind::Key) {
            // Keyframes arrive whole (streams protect them).
            state.decoder = recon;
            state.diverged = false;
        } else {
            // Never arrives: the decoder holds its old picture.
            if (!state.decoder) state.decoder = std::make_shared<Picture>(*recon);
            state.diverged = state.decoder != recon;
        }
        return {state.decoder, steps.inter};
    }
    case FrameKind::Predicted: {
        if (!state.encoder) return step(state, source, FrameKind::Key, index, s, seedMix);
        PicturePtr encRef = state.encoder;
        Motion motion = searchMotion(*source, *encRef, state.vectors, g);
        float sq = squeeze(motion.error, 3, s.fixedBitrate);
        Steps steps(s.quality, sq);
        Picture prediction(g);
        compensate(*encRef, motion.vectors, g, prediction);
        Picture residual(g);
        codeResidual(*source, prediction, steps.inter, residual);
        auto encRecon = std::make_shared<Picture>(g);
        reconstruct(prediction, residual, nullptr, g, *encRecon);

        if (!state.diverged && !lost) {
            state.decoder = encRecon;
        } else {
            // The decoder moves its own picture: the encoder's vectors,
            // except where lost — there it guesses from the block above and
            // gets no difference at all.
            std::vector<MotionVector> vectors = motion.vectors;
            if (lost) {
                const int cols = g.mbCols();
                for (int i = 0; i < g.mbCount(); ++i) {
                    if (!(*lost)[size_t(i)]) continue;
                    int above = i - cols;
                    vectors[size_t(i)] =
                        above >= 0 && !(*lost)[size_t(above)] ? vectors[size_t(above)] : MotionVector{};
                }
            }
            PicturePtr decRef = state.decoder ? state.decoder : encRef;
            Picture decPrediction(g);
            compensate(*decRef, vectors, g, decPrediction);
            auto decRecon = std::make_shared<Picture>(g);
            reconstruct(decPrediction, residual, lost ? &*lost : nullptr, g, *decRecon);
            state.decoder = decRecon;
            state.diverged = true;
        }
        state.encoder = encRecon;
        state.vectors = std::move(motion.vectors);
        return {state.decoder, steps.inter};
    }
    }
    return {state.decoder, 1};
}

void copyBlocks(const Picture &source, Picture &target, const std::vector<bool> &mask, const Geometry &g) {
    const int cols = g.mbCols(), rows = g.mbRows();
    for (int i = 0; i < 3; ++i) {
        const Plane &src = source.plane(i);
        Plane &dst = target.plane(i);
        const int fx = i == 0 ? 1 : g.sx(), fy = i == 0 ? 1 : g.sy();
        const int w = dst.width;
        for (int y = 0; y < dst.height; ++y) {
            int my = std::min(rows - 1, y * fy / 16);
            for (int x = 0; x < w; ++x)
                if (mask[size_t(my) * cols + std::min(cols - 1, x * fx / 16)])
                    dst.data[size_t(y) * w + x] = src.data[size_t(y) * w + x];
        }
    }
}

// ---- deblocking (the decoder's display filter) ----------------------------------------

void deblock(Picture &p, double strength, float step) {
    if (!(strength > 0)) return;
    const float w8 = float(strength);
    const float threshold = float(strength) * (4 + 0.9f * step);
    for (int i = 0; i < 3; ++i) {
        Plane &plane = p.plane(i);
        const int w = plane.width, h = plane.height;
        float *d = plane.p();
        auto filter = [&](size_t a, size_t b, size_t c, size_t e) {
            float p1 = d[a], p0 = d[b], q0 = d[c], q1 = d[e];
            float delta = q0 - p0;
            if (!(std::fabs(delta) < threshold && std::fabs(p1 - p0) < threshold / 2 &&
                  std::fabs(q1 - q0) < threshold / 2))
                return;
            d[b] = p0 + delta * 0.375f * w8;
            d[c] = q0 - delta * 0.375f * w8;
            d[a] = p1 + delta * 0.125f * w8;
            d[e] = q1 - delta * 0.125f * w8;
        };
        for (int y = 0; y < h; ++y)
            for (int x = 8; x < w; x += 8) {
                size_t k = size_t(y) * w + x;
                filter(k - 2, k - 1, k, k + 1);
            }
        for (int y = 8; y < h; y += 8)
            for (int x = 0; x < w; ++x) {
                size_t k = size_t(y) * w + x;
                filter(k - 2 * size_t(w), k - size_t(w), k, k + size_t(w));
            }
    }
}

}  // namespace codec
}  // namespace ntscrt::digital
