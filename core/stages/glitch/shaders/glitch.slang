// glitch.slang — the glitch stage's GPU passes, ported from the inline MSL in
// upstream Sources/CrtCore/GlitchStage.swift (NTSCRT 2b3013d). Owner: agent B.
// CPU reference with the same math: core/stages/glitch/glitch_kernels.cpp —
// keep the two in lockstep.
//
// 1. glitch_gate    (one thread per row): what the TV's burst gate sees.
// 2. glitch_chroma  (one thread): the color reference loop down the rows.
// 3. glitch_compose (one thread per pixel): the screen.
//
// Bindings (gpu.h convention):
//   b0  GlitchU            all kernels
//   t0  src (chain input)  gate, compose
//   t8  rows               gate, compose
//   t9  color (read)       compose
//   t10 dropouts           compose
//   u0  dst                compose
//   u8  gate               gate (write), chroma (read)
//   u9  color (write)      chroma
// Written in the HLSL subset of Slang (also compiles with fxc cs_5_1).

// Note: `line` is a reserved word in HLSL, hence `ln`.
#ifdef __SLANG__
#define COMPUTE_ENTRY [shader("compute")]
#else
#define COMPUTE_ENTRY
#endif

struct GlitchU {
    uint width, activeLines, vbiLines, totalLines;
    uint preEqEnd, vsyncEnd, postEqEnd, ccEnabled;
    uint ccBits, fieldIndex, seed, dropoutCount;
    uint docEnabled, tapsI, tapsQ, colorOnInit;
    float usPerPixel, lineUS, ghostLevel, ghostDelay;
    float ghostCos, ghostSin, brightness, pictureGain;
    float burstAmp, chromaAlpha, accGamma, killerKappa;
    float phaseInit, gainInit, killerInit, gateNoise;
};
struct Row {
    float u0; int fieldLine; float lenPrev; float lenCur; float lenNext;
    float noiseIRE; float humIRE; float burstScale;
    float tapeLoss; float gateShift; float pad1; float pad2;
};
struct Dropout { int fieldLine; float uStart; float uLength; float pad; };

ConstantBuffer<GlitchU> U : register(b0);
Texture2D<float4> src : register(t0);
StructuredBuffer<Row> rows : register(t8);
StructuredBuffer<float4> colorIn : register(t9);
StructuredBuffer<Dropout> drops : register(t10);
RWTexture2D<float4> dst : register(u0);
RWStructuredBuffer<float2> gateBuf : register(u8);
RWStructuredBuffer<float4> colorOut : register(u9);

static const float SYNC_END = 4.7;
static const float BURST_S = 5.3;
static const float BURST_E = 7.8;
static const float GATE_S = 4.55;           // 4 µs gate around the burst
static const float GATE_E = 8.55;
static const float ACT_S = 9.4;
static const float ACT_L = 52.656;
static const float EQ_PULSE = 2.3;
static const float BROAD = 27.1;
static const float2 BURST_DIR = float2(0.5446390350, -0.8386705679);

// ---- color space (NTSC Y'IQ on the gamma-encoded picture) ----
float3 rgb2yiq(float3 c) {
    return float3(0.299 * c.r + 0.587 * c.g + 0.114 * c.b,
                  0.596 * c.r - 0.274 * c.g - 0.322 * c.b,
                  0.211 * c.r - 0.523 * c.g + 0.312 * c.b);
}
float3 yiq2rgb(float3 y) {
    return float3(y.x + 0.9561706854 * y.y + 0.6214325663 * y.z,
                  y.x - 0.2726886023 * y.y - 0.6468132370 * y.z,
                  y.x - 1.1037440822 * y.y + 1.7006230947 * y.z);
}
float ire2y(float ire) { return (ire - 7.5) / 92.5; }

// ---- deterministic noise ----
uint hash4(uint a, uint b, uint c, uint d) {
    uint h = a * 0x8DA6B343u ^ b * 0xD8163841u ^ c * 0xCB1AB31Fu ^ d * 0x165667B1u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
float uni(uint a, uint b, uint c, uint d) {
    return (float(hash4(a, b, c, d) >> 8) + 0.5) * (1.0 / 16777216.0);
}
float gauss(uint a, uint b, uint c, uint d) {
    float u1 = uni(a, b, c, d);
    float u2 = uni(a ^ 0x5bd1e995u, b, c, d + 7u);
    return sqrt(-2.0 * log(u1)) * cos(6.2831853 * u2);
}

// ---- where in the signal a moment of a TV line falls ----
struct Loc { int ln; float u; };
Loc locate(Row rw, float u, uint L) {
    int ln = rw.fieldLine;
    if (u < 0.0) { u += rw.lenPrev; ln -= 1; }
    else if (u >= rw.lenCur) {
        u -= rw.lenCur; ln += 1;
        if (u >= rw.lenNext) { u -= rw.lenNext; ln += 1; }
    }
    int iL = int(L);
    ln = ((ln % iL) + iL) % iL;
    Loc l;
    l.ln = ln;
    l.u = u;
    return l;
}

// Is (line, u) inside a dropout? Reports where that dropout starts.
bool inDropout(uint count, int ln, float u, inout float start) {
    for (uint i = 0; i < count; i++) {
        Dropout d = drops[i];
        if (d.fieldLine == ln && u >= d.uStart && u < d.uStart + d.uLength) {
            start = d.uStart;
            return true;
        }
    }
    return false;
}

// Line 21: 7 cycles of clock run-in at 503.5 kHz, then 19 bits.
float captionIRE(float u, uint bits) {
    float t = u - 10.5;
    const float period = 1.986;
    if (t < 0.0) return 0.0;
    if (t < 7.0 * period) return 25.0 - 25.0 * cos(6.2831853 * t / period);
    float b = (t - 7.0 * period) / period;
    int k = int(floor(b));
    if (k < 0 || k >= 19) return 0.0;
    float on = float((bits >> uint(k)) & 1u);
    float prev = k > 0 ? float((bits >> uint(k - 1)) & 1u) : 0.0;
    float edge = smoothstep(0.0, 0.25, b - float(k));
    return 50.0 * lerp(prev, on, edge);
}

// What the signal carries at (line, u): Y'IQ on the decoder's scale, and the
// raw texel + exactness flag when it is a picture pixel.
struct Sig { float3 yiq; float3 rgb; bool picture; bool exact; };
float3 texel(int x, int y) { return src.Load(int3(x, y, 0)).rgb; }
Sig signalAt(int ln, float u) {
    Sig s;
    s.picture = false; s.exact = false; s.rgb = float3(0.0, 0.0, 0.0); s.yiq = float3(0.0, 0.0, 0.0);
    int vbi = int(U.vbiLines);
    float halfLine = U.lineUS * 0.5;
    float ire = 0.0;                                   // blanking
    if (ln < vbi) {
        if (ln < int(U.preEqEnd) || (ln >= int(U.vsyncEnd) && ln < int(U.postEqEnd))) {
            if (u < EQ_PULSE || (u >= halfLine && u < halfLine + EQ_PULSE)) ire = -40.0;
        } else if (ln < int(U.vsyncEnd)) {
            if (u < BROAD || (u >= halfLine && u < halfLine + BROAD)) ire = -40.0;
        } else {
            if (u < SYNC_END) ire = -40.0;
            else if (ln == vbi - 1 && U.ccEnabled != 0u && u >= ACT_S && u < ACT_S + ACT_L)
                ire = captionIRE(u, U.ccBits);
        }
        s.yiq = float3(ire2y(ire), 0.0, 0.0);
        return s;
    }
    if (u < SYNC_END) { s.yiq = float3(ire2y(-40.0), 0.0, 0.0); return s; }
    if (u < ACT_S || u >= ACT_S + ACT_L) { s.yiq = float3(ire2y(0.0), 0.0, 0.0); return s; }
    int row = ln - vbi;
    float xs = (u - ACT_S) / U.usPerPixel - 0.5;
    float x0 = floor(xs);
    float f = xs - x0;
    int w = int(U.width);
    int ix = int(x0);
    if (f < 1e-3 || f > 1.0 - 1e-3) {
        int xi = clamp(f < 0.5 ? ix : ix + 1, 0, w - 1);
        s.rgb = texel(xi, row);
        s.exact = true;
    } else {
        float3 a = texel(clamp(ix, 0, w - 1), row);
        float3 b = texel(clamp(ix + 1, 0, w - 1), row);
        s.rgb = lerp(a, b, f);
    }
    s.picture = true;
    s.yiq = rgb2yiq(s.rgb);
    return s;
}

// ---- pass 1: the burst gate ----
COMPUTE_ENTRY
[numthreads(64, 1, 1)]
void glitch_gate(uint3 tid : SV_DispatchThreadID)
{
    uint y = tid.x;
    if (y >= U.activeLines) return;
    Row rw = rows[y];
    float2 g = float2(0.0, 0.0);
    const int N = 16;
    for (int i = 0; i < N; i++) {
        float u = rw.u0 + rw.gateShift + GATE_S + (float(i) + 0.5) / float(N) * (GATE_E - GATE_S);
        Loc l = locate(rw, u, U.totalLines);
        if (l.ln >= int(U.postEqEnd) && l.u >= BURST_S && l.u < BURST_E) {
            g += BURST_DIR * U.burstAmp * rw.burstScale;
        } else if (l.ln >= int(U.vbiLines) && l.u >= ACT_S && l.u < ACT_S + ACT_L) {
            Sig s = signalAt(l.ln, l.u);
            g += s.yiq.yz * U.pictureGain;
        }
    }
    // Normalized so a centered gate (10 of its 16 samples on the burst)
    // reads the burst at full amplitude.
    g *= (GATE_E - GATE_S) / ((BURST_E - BURST_S) * float(N));
    float n = rw.noiseIRE / 92.5 * U.gateNoise;
    if (n > 0.0) {
        g += float2(gauss(U.seed, U.fieldIndex, y, 101u), gauss(U.seed, U.fieldIndex, y, 102u)) * n;
    }
    gateBuf[y] = g;
}

// ---- pass 2: the color reference loop, top to bottom ----
COMPUTE_ENTRY
[numthreads(1, 1, 1)]
void glitch_chroma(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x != 0u) return;
    float phase = U.phaseInit, gain = U.gainInit, killer = U.killerInit;
    bool on = U.colorOnInit != 0u;
    float b0 = 0.2162162162;                           // 20 IRE burst
    for (uint y = 0; y < U.activeLines; y++) {
        float2 g = gateBuf[y];
        float mag = length(g);
        float c = cos(-phase), s = sin(-phase);
        float2 r = float2(g.x * c - g.y * s, g.x * s + g.y * c);
        float d = dot(r, BURST_DIR);
        float x = BURST_DIR.x * r.y - BURST_DIR.y * r.x;
        if (mag > 0.0) phase += U.chromaAlpha * min(1.0, mag / b0) * atan2(x, d);
        float c2 = cos(-phase), s2 = sin(-phase);
        float inPhase = dot(float2(g.x * c2 - g.y * s2, g.x * s2 + g.y * c2), BURST_DIR);
        gain += U.accGamma * (b0 / max(inPhase, 0.25 * b0) - gain);
        gain = clamp(gain, 0.3, 3.0);
        killer += U.killerKappa * (clamp(inPhase / b0, -1.0, 1.0) - killer);
        if (on && killer < 0.3) on = false;
        else if (!on && killer > 0.5) on = true;
        float ph = abs(phase) < 1e-4 ? 0.0 : phase;
        float gn = abs(gain - 1.0) < 1e-4 ? 1.0 : gain;
        colorOut[y] = float4(cos(-ph), sin(-ph), on ? gn : 0.0, 0.0);
    }
}

// ---- pass 3: compose the screen ----
COMPUTE_ENTRY
[numthreads(8, 8, 1)]
void glitch_compose(uint3 tid : SV_DispatchThreadID)
{
    uint2 gid = tid.xy;
    if (gid.x >= U.width || gid.y >= U.activeLines) return;
    Row rw = rows[gid.y];
    float4 rc = colorIn[gid.y];
    float u = rw.u0 + ACT_S + (float(gid.x) + 0.5) * U.usPerPixel;
    Loc l = locate(rw, u, U.totalLines);

    // Oxide dropout. Without compensation the FM demodulator throws a white
    // streak with a dark tail. With it, the deck replays the stretch from its
    // one-line delay — a tall flaw repeats one line down several (a frozen
    // smear). The detector and switch react ~0.7 µs late: each dropout still
    // flashes a tick.
    int sigLine = l.ln;
    bool streak = false;
    float tail = 0.0;
    float dStart = 0.0;
    if (inDropout(U.dropoutCount, l.ln, l.u, dStart)) {
        if (U.docEnabled != 0u && l.u - dStart >= 0.7) {
            int srcLine = l.ln - 1;
            float s2 = 0.0;
            for (int k = 0; k < 6 && srcLine > int(U.vbiLines)
                 && inDropout(U.dropoutCount, srcLine, l.u, s2); k++) srcLine -= 1;
            sigLine = max(int(U.vbiLines), srcLine);
        } else {
            streak = true;
        }
    } else if (U.docEnabled == 0u) {
        for (uint i = 0; i < U.dropoutCount; i++) {
            Dropout d = drops[i];
            if (d.fieldLine == l.ln && l.u >= d.uStart + d.uLength
                && l.u < d.uStart + d.uLength + 1.5) {
                tail = max(tail, 1.0 - (l.u - d.uStart - d.uLength) / 1.5);
            }
        }
    }

    Sig s = signalAt(sigLine, l.u);
    bool plain = s.picture && s.exact && sigLine == l.ln && !streak && tail == 0.0
        && U.ghostLevel == 0.0 && rw.noiseIRE == 0.0 && rw.humIRE == 0.0 && rw.tapeLoss == 0.0
        && U.pictureGain == 1.0 && U.brightness == 0.0
        && rc.x == 1.0 && rc.y == 0.0 && rc.z == 1.0;
    if (plain) { dst[gid] = float4(s.rgb, 1.0); return; }

    float3 yiq = s.yiq;
    if (streak) yiq = float3(1.0, 0.0, 0.0);
    if (tail > 0.0) yiq.x = lerp(yiq.x, -0.08, tail * 0.6);

    // Multipath: the same signal again, later, possibly inverted.
    if (U.ghostLevel != 0.0) {
        float ug = l.u - U.ghostDelay;
        int gl = l.ln;
        if (ug < 0.0) { ug += U.lineUS; gl = (gl - 1 + int(U.totalLines)) % int(U.totalLines); }
        Sig g = signalAt(gl, ug);
        float2 iq = float2(g.yiq.y * U.ghostCos - g.yiq.z * U.ghostSin,
                           g.yiq.y * U.ghostSin + g.yiq.z * U.ghostCos);
        // The ghost adds the signal's variation about blanking.
        yiq.x += U.ghostLevel * (g.yiq.x - ire2y(0.0));
        yiq.yz += U.ghostLevel * iq;
    }

    // Tape signal loss: past the FM threshold the demodulator outputs streaks
    // instead of picture, and the deck's color killer drops chroma.
    if (rw.tapeLoss > 0.0) {
        float seg = 0.5 + 1.6 * uni(U.seed, U.fieldIndex, gid.y, 201u);
        float pos = l.u + uni(U.seed, U.fieldIndex, gid.y, 202u) * seg;
        uint run = uint(max(0.0, pos) / seg);
        float g = gauss(U.seed ^ 0xF00Du, U.fieldIndex, run, gid.y);
        float sparkle = uni(U.seed ^ 0xBEEFu, U.fieldIndex, run, gid.y) > 0.93 ? 0.6 : 0.0;
        float st = clamp(0.35 + 0.32 * g + sparkle, -0.08, 1.05);
        yiq.x = lerp(yiq.x, st, rw.tapeLoss);
        yiq.yz *= 1.0 - rw.tapeLoss;
    }

    // AGC running out of gain: everything shrinks toward blanking.
    if (U.pictureGain != 1.0) {
        float ire = 7.5 + 92.5 * yiq.x;
        yiq.x = ire2y(ire * U.pictureGain);
        yiq.yz *= U.pictureGain;
    }

    yiq.x += rw.humIRE / 92.5;

    // Snow with the decoder's spectrum: luma near-white, chroma low-passed to
    // the I and Q bandwidths.
    if (rw.noiseIRE > 0.0) {
        float n = rw.noiseIRE / 92.5;
        yiq.x += gauss(U.seed, U.fieldIndex, gid.x, gid.y * 3u) * n;
        float ni = 0.0, nq = 0.0;
        for (uint k = 0; k < U.tapsI; k++)
            ni += gauss(U.seed ^ 0x1234u, U.fieldIndex, gid.x + k, gid.y * 3u + 1u);
        for (uint k2 = 0; k2 < U.tapsQ; k2++)
            nq += gauss(U.seed ^ 0x4321u, U.fieldIndex, gid.x + k2, gid.y * 3u + 2u);
        yiq.y += ni / sqrt(float(U.tapsI)) * n * 0.35;
        yiq.z += nq / sqrt(float(U.tapsQ)) * n * 0.25;
    }

    // Decode chroma against the row's reference: a phase error rotates it,
    // ACC scales it, the killer removes it.
    float2 iqd = float2(yiq.y * rc.x - yiq.z * rc.y, yiq.y * rc.y + yiq.z * rc.x) * rc.z;
    yiq.y = iqd.x; yiq.z = iqd.y;

    // Brightness lowers the tube's cutoff: blanking (0 IRE) and sync (−40)
    // become visible as the black level drops below them.
    if (U.brightness > 0.0) {
        float cutoff = 7.5 - 47.5 * U.brightness;
        float ire = 7.5 + 92.5 * yiq.x;
        yiq.x = (ire - cutoff) / (100.0 - cutoff);
        yiq.yz *= 92.5 / (100.0 - cutoff);
    }

    float3 rgb = clamp(yiq2rgb(yiq), 0.0, 1.0);
    dst[gid] = float4(rgb, 1.0);
}
