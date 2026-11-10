// glitch_kernels.cpp — CPU reference of shaders/glitch.slang (port of the MSL in
// upstream GlitchStage.swift). Keep the two in lockstep: same constants, same
// float32 formulas in the same order.
#include "glitch_kernels.h"

#include <algorithm>
#include <cmath>

namespace ntscrt::glitch {

namespace {

struct f2 {
    float x, y;
};
struct f3 {
    float x, y, z;
};
inline f3 operator+(f3 a, f3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

constexpr float SYNC_END = 4.7f;
constexpr float BURST_S = 5.3f;
constexpr float BURST_E = 7.8f;
constexpr float GATE_S = 4.55f;
constexpr float GATE_E = 8.55f;
constexpr float ACT_S = 9.4f;
constexpr float ACT_L = 52.656f;
constexpr float EQ_PULSE = 2.3f;
constexpr float BROAD = 27.1f;
constexpr f2 BURST_DIR = {0.5446390350f, -0.8386705679f};

inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstepf(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline f3 rgb2yiq(f3 c) {
    return {0.299f * c.x + 0.587f * c.y + 0.114f * c.z, 0.596f * c.x - 0.274f * c.y - 0.322f * c.z,
            0.211f * c.x - 0.523f * c.y + 0.312f * c.z};
}
inline f3 yiq2rgb(f3 y) {
    return {y.x + 0.9561706854f * y.y + 0.6214325663f * y.z, y.x - 0.2726886023f * y.y - 0.6468132370f * y.z,
            y.x - 1.1037440822f * y.y + 1.7006230947f * y.z};
}
inline float ire2y(float ire) { return (ire - 7.5f) / 92.5f; }

inline uint32_t hash4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t h = a * 0x8DA6B343u ^ b * 0xD8163841u ^ c * 0xCB1AB31Fu ^ d * 0x165667B1u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
inline float uni(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (float(hash4(a, b, c, d) >> 8) + 0.5f) * (1.0f / 16777216.0f);
}
inline float gauss(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    float u1 = uni(a, b, c, d);
    float u2 = uni(a ^ 0x5bd1e995u, b, c, d + 7u);
    return std::sqrt(-2.0f * std::log(u1)) * std::cos(6.2831853f * u2);
}

struct Loc {
    int line;
    float u;
};
inline Loc locate(const GlitchRow &rw, float u, uint32_t L) {
    int line = rw.fieldLine;
    if (u < 0.0f) {
        u += rw.lenPrev;
        line -= 1;
    } else if (u >= rw.lenCur) {
        u -= rw.lenCur;
        line += 1;
        if (u >= rw.lenNext) {
            u -= rw.lenNext;
            line += 1;
        }
    }
    int iL = int(L);
    line = ((line % iL) + iL) % iL;
    return {line, u};
}

inline bool inDropout(const GlitchDropout *drops, uint32_t count, int line, float u, float &start) {
    for (uint32_t i = 0; i < count; i++) {
        const GlitchDropout &d = drops[i];
        if (d.fieldLine == line && u >= d.uStart && u < d.uStart + d.uLength) {
            start = d.uStart;
            return true;
        }
    }
    return false;
}

inline float captionIRE(float u, uint32_t bits) {
    float t = u - 10.5f;
    const float period = 1.986f;
    if (t < 0.0f) return 0.0f;
    if (t < 7.0f * period) return 25.0f - 25.0f * std::cos(6.2831853f * t / period);
    float b = (t - 7.0f * period) / period;
    int k = int(std::floor(b));
    if (k < 0 || k >= 19) return 0.0f;
    float on = float((bits >> uint32_t(k)) & 1u);
    float prev = k > 0 ? float((bits >> uint32_t(k - 1)) & 1u) : 0.0f;
    float edge = smoothstepf(0.0f, 0.25f, b - float(k));
    return 50.0f * mixf(prev, on, edge);
}

inline f3 texel(const ImageBGRA8 &src, int x, int y) {
    const uint8_t *p = src.row(y) + size_t(x) * 4;
    return {float(p[2]) / 255.0f, float(p[1]) / 255.0f, float(p[0]) / 255.0f};
}
inline void write_px(ImageBGRA8 &dst, int x, int y, f3 c) {
    uint8_t *p = dst.row(y) + size_t(x) * 4;
    auto q = [](float v) { return uint8_t(std::lround(clampf(v, 0.0f, 1.0f) * 255.0f)); };
    p[0] = q(c.z);
    p[1] = q(c.y);
    p[2] = q(c.x);
    p[3] = 255;
}

struct Sig {
    f3 yiq;
    f3 rgb;
    bool picture;
    bool exact;
};
inline Sig signalAt(const ImageBGRA8 &src, const GlitchU &U, int line, float u) {
    Sig s;
    s.picture = false;
    s.exact = false;
    s.rgb = {0.0f, 0.0f, 0.0f};
    int vbi = int(U.vbiLines);
    float halfLine = U.lineUS * 0.5f;
    float ire = 0.0f;
    if (line < vbi) {
        if (line < int(U.preEqEnd) || (line >= int(U.vsyncEnd) && line < int(U.postEqEnd))) {
            if (u < EQ_PULSE || (u >= halfLine && u < halfLine + EQ_PULSE)) ire = -40.0f;
        } else if (line < int(U.vsyncEnd)) {
            if (u < BROAD || (u >= halfLine && u < halfLine + BROAD)) ire = -40.0f;
        } else {
            if (u < SYNC_END) ire = -40.0f;
            else if (line == vbi - 1 && U.ccEnabled != 0u && u >= ACT_S && u < ACT_S + ACT_L)
                ire = captionIRE(u, U.ccBits);
        }
        s.yiq = {ire2y(ire), 0.0f, 0.0f};
        return s;
    }
    if (u < SYNC_END) {
        s.yiq = {ire2y(-40.0f), 0.0f, 0.0f};
        return s;
    }
    if (u < ACT_S || u >= ACT_S + ACT_L) {
        s.yiq = {ire2y(0.0f), 0.0f, 0.0f};
        return s;
    }
    int row = line - vbi;
    float xs = (u - ACT_S) / U.usPerPixel - 0.5f;
    float x0 = std::floor(xs);
    float f = xs - x0;
    int w = int(U.width);
    int ix = int(x0);
    if (f < 1e-3f || f > 1.0f - 1e-3f) {
        int xi = std::clamp(f < 0.5f ? ix : ix + 1, 0, w - 1);
        s.rgb = texel(src, xi, row);
        s.exact = true;
    } else {
        f3 a = texel(src, std::clamp(ix, 0, w - 1), row);
        f3 b = texel(src, std::clamp(ix + 1, 0, w - 1), row);
        s.rgb = {mixf(a.x, b.x, f), mixf(a.y, b.y, f), mixf(a.z, b.z, f)};
    }
    s.picture = true;
    s.yiq = rgb2yiq(s.rgb);
    return s;
}

}  // namespace

GlitchU make_uniforms(const GlitchFieldPlan &plan, int width, uint32_t noiseSeed) {
    const ReceiverRaster &raster = plan.raster;
    const GlitchSettings &s = plan.settings;
    Knobs knobs(s, raster);
    double usPerPixel = NTSCTiming::activeLength / double(width);
    double ghostDelay = s[kGhostDelay];
    double ghostAngle = -2 * 3.14159265358979323846 * NTSCTiming::subcarrierMHz * ghostDelay;
    auto snap = [](float v, float target) { return std::abs(v - target) < 1e-6f ? target : v; };

    GlitchU u{};
    u.width = uint32_t(width);
    u.activeLines = uint32_t(raster.activeLines);
    u.vbiLines = uint32_t(raster.vbiLines);
    u.totalLines = uint32_t(raster.totalLines());
    u.preEqEnd = uint32_t(raster.preEqEnd);
    u.vsyncEnd = uint32_t(raster.vsyncEnd);
    u.postEqEnd = uint32_t(raster.postEqEnd);
    u.ccEnabled = s.flag(kClosedCaptions) ? 1u : 0u;
    u.ccBits = plan.ccBits;
    u.fieldIndex = plan.fieldIndex;
    u.seed = noiseSeed;
    u.dropoutCount = uint32_t(std::min<size_t>(plan.dropouts.size(), 64));
    u.docEnabled = s.flag(kDropoutCompensation) ? 1u : 0u;
    u.tapsI = uint32_t(std::max<long long>(1, std::llround(0.38 / usPerPixel)));
    u.tapsQ = uint32_t(std::max<long long>(1, std::llround(0.83 / usPerPixel)));
    u.colorOnInit = plan.colorOn ? 1u : 0u;
    u.usPerPixel = float(usPerPixel);
    u.lineUS = float(NTSCTiming::line);
    u.ghostLevel = float(s[kGhostLevel]);
    u.ghostDelay = float(ghostDelay);
    u.ghostCos = float(std::cos(ghostAngle));
    u.ghostSin = float(std::sin(ghostAngle));
    u.brightness = float(std::min(1.0, std::max(0.0, s[kBrightness])));
    u.pictureGain = snap(plan.pictureGain, 1.0f);
    u.burstAmp = float(NTSCTiming::burstAmplitude * knobs.agc);
    u.chromaAlpha = float(knobs.chromaAlpha);
    u.accGamma = float(knobs.accGamma);
    u.killerKappa = float(knobs.killerKappa);
    u.phaseInit = snap(plan.chromaPhase, 0.0f);
    u.gainInit = snap(plan.chromaGain, 1.0f);
    u.killerInit = snap(plan.killer, 1.0f);
    u.gateNoise = 0.45f;
    return u;
}

std::vector<GlitchDropout> dropout_buffer(const GlitchFieldPlan &plan) {
    std::vector<GlitchDropout> d(plan.dropouts.begin(),
                                 plan.dropouts.begin() + std::min<size_t>(plan.dropouts.size(), 64));
    if (d.empty()) {
        GlitchDropout none;
        none.fieldLine = -1;
        d.push_back(none);
    }
    return d;
}

// ---- pass 1: the burst gate ----
void cpu_gate(const GlitchU &U, const GlitchRow *rows, const ImageBGRA8 &src, float *gate) {
    for (uint32_t y = 0; y < U.activeLines; y++) {
        const GlitchRow &rw = rows[y];
        f2 g = {0.0f, 0.0f};
        const int N = 16;
        for (int i = 0; i < N; i++) {
            float u = rw.u0 + rw.gateShift + GATE_S + (float(i) + 0.5f) / float(N) * (GATE_E - GATE_S);
            Loc l = locate(rw, u, U.totalLines);
            if (l.line >= int(U.postEqEnd) && l.u >= BURST_S && l.u < BURST_E) {
                g.x += BURST_DIR.x * U.burstAmp * rw.burstScale;
                g.y += BURST_DIR.y * U.burstAmp * rw.burstScale;
            } else if (l.line >= int(U.vbiLines) && l.u >= ACT_S && l.u < ACT_S + ACT_L) {
                Sig s = signalAt(src, U, l.line, l.u);
                g.x += s.yiq.y * U.pictureGain;
                g.y += s.yiq.z * U.pictureGain;
            }
        }
        float norm = (GATE_E - GATE_S) / ((BURST_E - BURST_S) * float(N));
        g.x *= norm;
        g.y *= norm;
        float n = rw.noiseIRE / 92.5f * U.gateNoise;
        if (n > 0.0f) {
            g.x += gauss(U.seed, U.fieldIndex, y, 101u) * n;
            g.y += gauss(U.seed, U.fieldIndex, y, 102u) * n;
        }
        gate[y * 2 + 0] = g.x;
        gate[y * 2 + 1] = g.y;
    }
}

// ---- pass 2: the color reference loop, top to bottom ----
void cpu_chroma(const GlitchU &U, const float *gate, float *color) {
    float phase = U.phaseInit, gain = U.gainInit, killer = U.killerInit;
    bool on = U.colorOnInit != 0u;
    const float b0 = 0.2162162162f;
    for (uint32_t y = 0; y < U.activeLines; y++) {
        f2 g = {gate[y * 2], gate[y * 2 + 1]};
        float mag = std::sqrt(g.x * g.x + g.y * g.y);
        float c = std::cos(-phase), s = std::sin(-phase);
        f2 r = {g.x * c - g.y * s, g.x * s + g.y * c};
        float d = r.x * BURST_DIR.x + r.y * BURST_DIR.y;
        float x = BURST_DIR.x * r.y - BURST_DIR.y * r.x;
        if (mag > 0.0f) phase += U.chromaAlpha * std::min(1.0f, mag / b0) * std::atan2(x, d);
        float c2 = std::cos(-phase), s2 = std::sin(-phase);
        f2 r2 = {g.x * c2 - g.y * s2, g.x * s2 + g.y * c2};
        float inPhase = r2.x * BURST_DIR.x + r2.y * BURST_DIR.y;
        gain += U.accGamma * (b0 / std::max(inPhase, 0.25f * b0) - gain);
        gain = clampf(gain, 0.3f, 3.0f);
        killer += U.killerKappa * (clampf(inPhase / b0, -1.0f, 1.0f) - killer);
        if (on && killer < 0.3f) on = false;
        else if (!on && killer > 0.5f) on = true;
        float ph = std::fabs(phase) < 1e-4f ? 0.0f : phase;
        float gn = std::fabs(gain - 1.0f) < 1e-4f ? 1.0f : gain;
        color[y * 4 + 0] = std::cos(-ph);
        color[y * 4 + 1] = std::sin(-ph);
        color[y * 4 + 2] = on ? gn : 0.0f;
        color[y * 4 + 3] = 0.0f;
    }
}

// ---- pass 3: compose the screen ----
void cpu_compose(const GlitchU &U, const GlitchRow *rows, const float *color, const GlitchDropout *drops,
                 const ImageBGRA8 &src, ImageBGRA8 &dst) {
    for (uint32_t gy = 0; gy < U.activeLines; gy++) {
        const GlitchRow &rw = rows[gy];
        const float rc[4] = {color[gy * 4], color[gy * 4 + 1], color[gy * 4 + 2], color[gy * 4 + 3]};
        for (uint32_t gx = 0; gx < U.width; gx++) {
            float u = rw.u0 + ACT_S + (float(gx) + 0.5f) * U.usPerPixel;
            Loc l = locate(rw, u, U.totalLines);

            int sigLine = l.line;
            bool streak = false;
            float tail = 0.0f;
            float dStart = 0.0f;
            if (inDropout(drops, U.dropoutCount, l.line, l.u, dStart)) {
                if (U.docEnabled != 0u && l.u - dStart >= 0.7f) {
                    int srcLine = l.line - 1;
                    float s2 = 0.0f;
                    for (int k = 0; k < 6 && srcLine > int(U.vbiLines) &&
                                    inDropout(drops, U.dropoutCount, srcLine, l.u, s2);
                         k++)
                        srcLine -= 1;
                    sigLine = std::max(int(U.vbiLines), srcLine);
                } else {
                    streak = true;
                }
            } else if (U.docEnabled == 0u) {
                for (uint32_t i = 0; i < U.dropoutCount; i++) {
                    const GlitchDropout &d = drops[i];
                    if (d.fieldLine == l.line && l.u >= d.uStart + d.uLength && l.u < d.uStart + d.uLength + 1.5f) {
                        tail = std::max(tail, 1.0f - (l.u - d.uStart - d.uLength) / 1.5f);
                    }
                }
            }

            Sig s = signalAt(src, U, sigLine, l.u);
            bool plain = s.picture && s.exact && sigLine == l.line && !streak && tail == 0.0f &&
                         U.ghostLevel == 0.0f && rw.noiseIRE == 0.0f && rw.humIRE == 0.0f && rw.tapeLoss == 0.0f &&
                         U.pictureGain == 1.0f && U.brightness == 0.0f && rc[0] == 1.0f && rc[1] == 0.0f &&
                         rc[2] == 1.0f;
            if (plain) {
                write_px(dst, int(gx), int(gy), s.rgb);
                continue;
            }

            f3 yiq = s.yiq;
            if (streak) yiq = {1.0f, 0.0f, 0.0f};
            if (tail > 0.0f) yiq.x = mixf(yiq.x, -0.08f, tail * 0.6f);

            if (U.ghostLevel != 0.0f) {
                float ug = l.u - U.ghostDelay;
                int gl = l.line;
                if (ug < 0.0f) {
                    ug += U.lineUS;
                    gl = (gl - 1 + int(U.totalLines)) % int(U.totalLines);
                }
                Sig g = signalAt(src, U, gl, ug);
                f2 iq = {g.yiq.y * U.ghostCos - g.yiq.z * U.ghostSin, g.yiq.y * U.ghostSin + g.yiq.z * U.ghostCos};
                yiq.x += U.ghostLevel * (g.yiq.x - ire2y(0.0f));
                yiq.y += U.ghostLevel * iq.x;
                yiq.z += U.ghostLevel * iq.y;
            }

            if (rw.tapeLoss > 0.0f) {
                float seg = 0.5f + 1.6f * uni(U.seed, U.fieldIndex, gy, 201u);
                float pos = l.u + uni(U.seed, U.fieldIndex, gy, 202u) * seg;
                uint32_t run = uint32_t(std::max(0.0f, pos) / seg);
                float g = gauss(U.seed ^ 0xF00Du, U.fieldIndex, run, gy);
                float sparkle = uni(U.seed ^ 0xBEEFu, U.fieldIndex, run, gy) > 0.93f ? 0.6f : 0.0f;
                float st = clampf(0.35f + 0.32f * g + sparkle, -0.08f, 1.05f);
                yiq.x = mixf(yiq.x, st, rw.tapeLoss);
                yiq.y *= 1.0f - rw.tapeLoss;
                yiq.z *= 1.0f - rw.tapeLoss;
            }

            if (U.pictureGain != 1.0f) {
                float ire = 7.5f + 92.5f * yiq.x;
                yiq.x = ire2y(ire * U.pictureGain);
                yiq.y *= U.pictureGain;
                yiq.z *= U.pictureGain;
            }

            yiq.x += rw.humIRE / 92.5f;

            if (rw.noiseIRE > 0.0f) {
                float n = rw.noiseIRE / 92.5f;
                yiq.x += gauss(U.seed, U.fieldIndex, gx, gy * 3u) * n;
                float ni = 0.0f, nq = 0.0f;
                for (uint32_t k = 0; k < U.tapsI; k++) ni += gauss(U.seed ^ 0x1234u, U.fieldIndex, gx + k, gy * 3u + 1u);
                for (uint32_t k = 0; k < U.tapsQ; k++) nq += gauss(U.seed ^ 0x4321u, U.fieldIndex, gx + k, gy * 3u + 2u);
                yiq.y += ni / std::sqrt(float(U.tapsI)) * n * 0.35f;
                yiq.z += nq / std::sqrt(float(U.tapsQ)) * n * 0.25f;
            }

            f2 iq = {(yiq.y * rc[0] - yiq.z * rc[1]) * rc[2], (yiq.y * rc[1] + yiq.z * rc[0]) * rc[2]};
            yiq.y = iq.x;
            yiq.z = iq.y;

            if (U.brightness > 0.0f) {
                float cutoff = 7.5f - 47.5f * U.brightness;
                float ire = 7.5f + 92.5f * yiq.x;
                yiq.x = (ire - cutoff) / (100.0f - cutoff);
                yiq.y *= 92.5f / (100.0f - cutoff);
                yiq.z *= 92.5f / (100.0f - cutoff);
            }

            write_px(dst, int(gx), int(gy), yiq2rgb(yiq));
        }
    }
}

bool cpu_render_plan(const GlitchFieldPlan &plan, const ImageBGRA8 &src, ImageBGRA8 &dst, uint32_t noiseSeed) {
    if (int(plan.rows.size()) != src.height || plan.rows.empty() || src.width <= 0) {
        dst = src;
        return false;
    }
    GlitchU U = make_uniforms(plan, src.width, noiseSeed);
    std::vector<float> gate(plan.rows.size() * 2), color(plan.rows.size() * 4);
    std::vector<GlitchDropout> drops = dropout_buffer(plan);
    cpu_gate(U, plan.rows.data(), src, gate.data());
    cpu_chroma(U, gate.data(), color.data());
    ImageBGRA8 out;
    out.resize(src.width, src.height);
    cpu_compose(U, plan.rows.data(), color.data(), drops.data(), src, out);
    dst = std::move(out);
    return true;
}

}  // namespace ntscrt::glitch
