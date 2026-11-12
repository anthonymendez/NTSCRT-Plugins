// ntsc_gpu.slang — the GPU port of ntsc-rs's effect (phases 1-3 of 4).
// Design, plane format and tables: docs/dev/NTSC-GPU.md. Driver:
// core/stages/ntsc/gpu/ntsc_gpu.cpp. Reference: vendor/ntsc-rs/crates/ntscrs/src
// (ntsc.rs, filter.rs, yiq_fielding.rs, shift.rs, noise/); the plan and tables
// come from rust/ntscrs-capi/src/gpu_plan.rs.
//
// The signal: Y, I, Q in three R32F planes (u0..u2) of width x planeRows
// (ntsc-rs's YIQ buffer layout: for the interleaved fields, one field's rows
// then the other's), plus a scratch plane (u3, ntsc-rs's `scratch`). They are
// read and written as RWTexture2D<float> (R32F: typed UAV loads are allowed).
//
// Arithmetic follows ntsc-rs operation by operation, with ntsc-rs's rounding:
// where ntsc-rs uses an explicit fused multiply-add (fearless_simd mul_add:
// FMA on AVX2 / NEON), the shader uses mad() (fused on NVIDIA / AMD / Apple
// GPUs); everywhere else values are `precise` so the compiler neither fuses
// nor reorders (Rust never contracts a * b + c on its own).
//
// Bindings (gpu.h convention):
//   b0  NtscGpuU (all)              t0  source picture (BGRA8, from_bgra8)
//   t8  rows (NtscGpuRow per plane row)   t9  outRows (uint2 per output row)
//   t10 noiseRows   t11 shifts   t12 speckleRows   t13 speckles   t14 speckleValues
//   u0  Y   u1  I   u2  Q   u3  S (scratch)   u4  output picture (to_bgra8)

struct NtscGpuU {
    uint width, planeRows, outHeight, planes;
    uint mode, pad0, pad1, pad2;
    float c16, inv255, rotSin, rotCos;  // 16/255 and 1/255 as ntsc-rs's f32 constants
    float4 num[3];                      // per plane (Y, I, Q): row filter coefficients
    float4 den[3];
    float4 zi[3];
    int4 fp[3];                         // len, delay, initial (0 zero, 1 first sample), unused
    float4 fiv;                         // constant initial value per plane
    float4 fx;                          // per pass: noise (frequency, -, gain), shifts (horizontal shift)
    int4 ix;                            // per pass: x octaves / vertical offset / longest speckle, y SIMD lanes,
                                        // z flag, w first row (passes dispatched over a run of rows)
};
struct Row {
    uint src_row, field_row, field_first, field_rows;
    uint xi, flags;
    float phase_sin, phase_cos;
};

ConstantBuffer<NtscGpuU> U : register(b0);
Texture2D<float4> srcPic : register(t0);
StructuredBuffer<Row> rows : register(t8);
StructuredBuffer<uint2> outRows : register(t9);
// Phase 2-3 tables (NtscGpuRowShift / NtscGpuSpeckle layouts, ntscrs_capi.h;
// NoiseRow is the driver's: the row seed's Fbm seed and offset, the intensity).
struct NoiseRow {
    int seed;
    float offset;
    float intensity;  // already * 0.25 (video_noise_line)
    uint active;
};
struct RowShift {
    float shift;
    int boundary;    // 0 constant 0, 1 extend, -1 row untouched
    int copy_start;  // head switching's mid line: pixels from here on are shifted; -1 whole row
    float intensity;
};
struct Speckle {
    uint row;
    int x0;
    uint count;
    uint value_offset;
};
StructuredBuffer<NoiseRow> noiseRows : register(t10);
StructuredBuffer<RowShift> shifts : register(t11);
StructuredBuffer<uint2> speckleRows : register(t12);  // per plane row: first event, event count
StructuredBuffer<Speckle> speckles : register(t13);
StructuredBuffer<float> speckleValues : register(t14);
RWTexture2D<float> pY : register(u0);
RWTexture2D<float> pI : register(u1);
RWTexture2D<float> pQ : register(u2);
RWTexture2D<float> pS : register(u3);
RWTexture2D<float4> outPic : register(u4);

// ---- fused multiply-add flavours ----
// ntsc-rs's SIMD code rounds a * b + c once (fearless_simd mul_add = FMA).
// mad() is that on GPUs that fuse it (NVIDIA, AMD, Apple: the driver probes
// it, NtscGpu::Impl::probe); on devices that don't (WARP) the *_x kernels
// compute it in double precision instead: the product of two floats is exact
// in a double, the sum is rounded to double and then to float (a double
// rounding that differs from a true FMA only when the double lands exactly
// halfway between two floats: practically never). Metal: fma() is fused.
interface IFma {
    static float fma3(float a, float b, float c);
};
// The result is `precise`: a mad() whose result isn't may be split into a
// multiply and an add by the driver (measured on NVIDIA: shift_row's blend).
struct MadFma : IFma {
    static float fma3(float a, float b, float c)
    {
        precise float r = mad(a, b, c);
        return r;
    }
};
struct ExactFma : IFma {
    static float fma3(float a, float b, float c)
    {
        __target_switch
        {
        case hlsl:
            precise double r = double(a) * double(b) + double(c);
            return float(r);
        default:
            return fma(a, b, c);
        }
    }
};

// ---- conversion in: yiq_fielding.rs set_from_strided_buffer (u8 Bgrx) ----
// to_norm: i32 -> f32 times (1 / 255); rgb_to_yiq: fma(r, c0, fma(g, c1, b * c2)).
void from_bgra8_impl<F : IFma>(uint3 id)
{
    if (id.x >= U.width || id.y >= U.planeRows) return;
    float4 px = srcPic.Load(int3(int(id.x), int(rows[id.y].src_row), 0));
    // The UNORM value back to its byte, then ntsc-rs's normalization.
    precise float r = float(uint(round(px.r * 255.0))) * U.inv255;
    precise float g = float(uint(round(px.g * 255.0))) * U.inv255;
    precise float b = float(uint(round(px.b * 255.0))) * U.inv255;
    precise float b0 = b * 0.114, b1 = b * -0.3213, b2 = b * 0.3112;
    precise float y = F.fma3(r, 0.299, F.fma3(g, 0.587, b0));
    precise float i = F.fma3(r, 0.5959, F.fma3(g, -0.2746, b1));
    precise float q = F.fma3(r, 0.2115, F.fma3(g, -0.5227, b2));
    pY[id.xy] = y;
    pI[id.xy] = i;
    pQ[id.xy] = q;
}
[shader("compute")]
[numthreads(8, 8, 1)]
void from_bgra8(uint3 id : SV_DispatchThreadID) { from_bgra8_impl<MadFma>(id); }
[shader("compute")]
[numthreads(8, 8, 1)]
void from_bgra8_x(uint3 id : SV_DispatchThreadID) { from_bgra8_impl<ExactFma>(id); }

// ---- conversion out: write_to_strided_buffer with DeinterlaceMode::Bob ----
// yiq_to_rgb: fma(q, c2, fma(i, c1, y)); from_norm: (v * 255).min(255).max(0), truncated.
float to_byte(float v)
{
    precise float t = v * 255.0;
    t = min(t, 255.0);
    t = max(t, 0.0);
    return float(int(t));
}

void to_bgra8_impl<F : IFma>(uint3 id)
{
    if (id.x >= U.width || id.y >= U.outHeight) return;
    uint2 o = outRows[id.y];
    precise float y = pY[uint2(id.x, o.x)];
    precise float i = pI[uint2(id.x, o.x)];
    precise float q = pQ[uint2(id.x, o.x)];
    if (o.x != o.y) {
        // (upper_pixel + lower_pixel) * 0.5, upper = row b
        y = (pY[uint2(id.x, o.y)] + y) * 0.5;
        i = (pI[uint2(id.x, o.y)] + i) * 0.5;
        q = (pQ[uint2(id.x, o.y)] + q) * 0.5;
    }
    precise float r = F.fma3(q, 0.619, F.fma3(i, 0.956, y));
    precise float g = F.fma3(q, -0.647, F.fma3(i, -0.272, y));
    precise float b = F.fma3(q, 1.703, F.fma3(i, -1.106, y));
    outPic[id.xy] = float4(to_byte(r), to_byte(g), to_byte(b), 255.0) / 255.0;
}
[shader("compute")]
[numthreads(8, 8, 1)]
void to_bgra8(uint3 id : SV_DispatchThreadID) { to_bgra8_impl<MadFma>(id); }
[shader("compute")]
[numthreads(8, 8, 1)]
void to_bgra8_x(uint3 id : SV_DispatchThreadID) { to_bgra8_impl<ExactFma>(id); }

// ---- the row IIR filter: filter.rs filter_signal_in_place_fixed_size_simdx4 ----
// One thread per (row, plane), serial along the row, the transposed direct
// form II of ntsc-rs's SIMD path (up to 4 coefficients):
//   y      = fma(num0, x, z0)
//   z[k]   = fma(-den[k], y, fma(num[k+1], x, z[k+1]))   (z[3] = 0)
// Reads sample min(i, width - 1), writes output i - delay (in place: a sample
// is always read before anything is written over it).
#define FILTER_CHUNK 16

void filter_row<F : IFma>(RWTexture2D<float> T, uint row, uint p)
{
    const int W = int(U.width);
    const int D = U.fp[p].y;
    const int total = W + D;
    const float4 n = U.num[p];
    const float4 d = U.den[p];
    float v = 0.0;
    if (U.fp[p].z == 1) v = T[uint2(0, row)];
    else if (U.fp[p].z == 2) v = U.fiv[p];
    precise float z0 = U.zi[p].x * v;
    precise float z1 = U.zi[p].y * v;
    precise float z2 = U.zi[p].z * v;
    // Software-pipelined: the next 16 samples are loaded while this 16 are
    // filtered, so the memory latency overlaps the serial arithmetic (one thread
    // per row leaves the GPU mostly idle; latency, not math, is the cost).
    // Safe in place: the next chunk's samples are never written before they're
    // read (writes go to i - delay <= i). Written out because Slang emits loops
    // as for(;;) + break, which fxc cannot unroll.
#define NTSC_LOAD(k, b) cur##k = T[uint2(uint(min((b) + k, W - 1)), row)];
#define NTSC_NEXT(k) const float nxt##k = T[uint2(uint(min(base + FILTER_CHUNK + k, W - 1)), row)];
#define NTSC_STEP(k)                                                   \
    if (base + k < total) {                                            \
        precise float y = F.fma3(n.x, cur##k, z0);                     \
        precise float n3 = n.w * cur##k;                               \
        z0 = F.fma3(-d.x, y, F.fma3(n.y, cur##k, z1));                 \
        z1 = F.fma3(-d.y, y, F.fma3(n.z, cur##k, z2));                 \
        z2 = F.fma3(-d.z, y, n3);                                      \
        if (base + k >= D) T[uint2(uint(base + k - D), row)] = y;      \
    }
#define NTSC_SHIFT(k) cur##k = nxt##k;
    float cur0, cur1, cur2, cur3, cur4, cur5, cur6, cur7, cur8, cur9, cur10, cur11, cur12, cur13, cur14, cur15;
    NTSC_LOAD(0, 0) NTSC_LOAD(1, 0) NTSC_LOAD(2, 0) NTSC_LOAD(3, 0)
    NTSC_LOAD(4, 0) NTSC_LOAD(5, 0) NTSC_LOAD(6, 0) NTSC_LOAD(7, 0)
    NTSC_LOAD(8, 0) NTSC_LOAD(9, 0) NTSC_LOAD(10, 0) NTSC_LOAD(11, 0)
    NTSC_LOAD(12, 0) NTSC_LOAD(13, 0) NTSC_LOAD(14, 0) NTSC_LOAD(15, 0)
    [loop] for (int base = 0; base < total; base += FILTER_CHUNK) {
        NTSC_NEXT(0) NTSC_NEXT(1) NTSC_NEXT(2) NTSC_NEXT(3)
        NTSC_NEXT(4) NTSC_NEXT(5) NTSC_NEXT(6) NTSC_NEXT(7)
        NTSC_NEXT(8) NTSC_NEXT(9) NTSC_NEXT(10) NTSC_NEXT(11)
        NTSC_NEXT(12) NTSC_NEXT(13) NTSC_NEXT(14) NTSC_NEXT(15)
        NTSC_STEP(0) NTSC_STEP(1) NTSC_STEP(2) NTSC_STEP(3)
        NTSC_STEP(4) NTSC_STEP(5) NTSC_STEP(6) NTSC_STEP(7)
        NTSC_STEP(8) NTSC_STEP(9) NTSC_STEP(10) NTSC_STEP(11)
        NTSC_STEP(12) NTSC_STEP(13) NTSC_STEP(14) NTSC_STEP(15)
        NTSC_SHIFT(0) NTSC_SHIFT(1) NTSC_SHIFT(2) NTSC_SHIFT(3)
        NTSC_SHIFT(4) NTSC_SHIFT(5) NTSC_SHIFT(6) NTSC_SHIFT(7)
        NTSC_SHIFT(8) NTSC_SHIFT(9) NTSC_SHIFT(10) NTSC_SHIFT(11)
        NTSC_SHIFT(12) NTSC_SHIFT(13) NTSC_SHIFT(14) NTSC_SHIFT(15)
    }
#undef NTSC_NEXT
#undef NTSC_SHIFT
#undef NTSC_LOAD
#undef NTSC_STEP
}

void row_filter_impl<F : IFma>(uint3 id)
{
    if (id.x >= U.planeRows || id.y > 2u) return;
    if ((U.planes & (1u << id.y)) == 0u || U.fp[id.y].x == 0) return;
    // luma_into_chroma with a comb on a picture with some 1-row fields: the notch only on those
    if (U.ix.z == 1 && rows[id.x].field_rows != 1u) return;
    if (id.y == 0u) filter_row<F>(pY, id.x, 0u);
    else if (id.y == 1u) filter_row<F>(pI, id.x, 1u);
    else filter_row<F>(pQ, id.x, 2u);
}
[shader("compute")]
[numthreads(64, 1, 1)]
void row_filter(uint3 id : SV_DispatchThreadID) { row_filter_impl<MadFma>(id); }
[shader("compute")]
[numthreads(64, 1, 1)]
void row_filter_x(uint3 id : SV_DispatchThreadID) { row_filter_impl<ExactFma>(id); }

// Whether mad() is fused here: mad(a, a, c) with a = 1 + 2^-13, c = -(1 + 2^-12)
// is 2^-26 when fused and 0 when the product is rounded first.
[shader("compute")]
[numthreads(1, 1, 1)]
void probe_fma(uint3 id : SV_DispatchThreadID)
{
    pY[uint2(0, 0)] = mad(U.rotSin, U.rotSin, U.rotCos);
}

// ---- input luma box filter: ntsc.rs luma_filter, LumaLowpass::Box ----
[shader("compute")]
[numthreads(64, 1, 1)]
void luma_box(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= U.planeRows) return;
    const uint row = id.x;
    const int W = int(U.width);
    if (W < 2) return;
    // FixedQueue [16/255, 16/255, y0, y1]; sum = 0 + q0 + q1 + q2 + q3
    precise float q0 = U.c16, q1 = U.c16, q2 = pY[uint2(0, row)], q3 = pY[uint2(1, row)];
    precise float sum = ((q0 + q1) + q2) + q3;
    [loop] for (int index = 0; index < W - 2; index++) {
        const float c = pY[uint2(uint(index + 2), row)];
        sum -= q0;
        q0 = q1; q1 = q2; q2 = q3; q3 = c;
        sum += c;
        pY[uint2(uint(index), row)] = sum * 0.25;
    }
    const float last = pY[uint2(uint(W - 1), row)];
    for (int index = W - 2; index < W; index++) {
        sum -= q0;
        q0 = q1; q1 = q2; q2 = q3; q3 = last;
        sum += last;
        pY[uint2(uint(index), row)] = sum * 0.25;
    }
}

// ---- chroma_into_luma (ntsc.rs 295-324) ----
static const float I_MULT[4] = { 1.0, 0.0, -1.0, 0.0 };
static const float Q_MULT[4] = { 0.0, 1.0, 0.0, -1.0 };
static const float I_MULT_INV[4] = { -1.0, 0.0, 1.0, 0.0 };
static const float Q_MULT_INV[4] = { 0.0, -1.0, 0.0, 1.0 };

[shader("compute")]
[numthreads(8, 8, 1)]
void chroma_into_luma(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= U.width || id.y >= U.planeRows) return;
    const uint phase = (id.x + (rows[id.y].xi & 3u)) & 3u;
    precise float m = pI[id.xy] * I_MULT[phase] + pQ[id.xy] * Q_MULT[phase];
    pY[id.xy] = pY[id.xy] + m;
}

// ---- luma_into_chroma (ntsc.rs 446-574): the luma estimate from the modulated
// signal (copied to the scratch plane), then the chroma demodulation ----
[shader("compute")]
[numthreads(8, 8, 1)]
void demod_luma(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= U.width || id.y >= U.planeRows) return;
    const uint x = id.x, y = id.y;
    const Row r = rows[y];
    if (U.ix.z == 1 && r.field_rows == 1u) return;  // a 1-row field: notch (row_filter), not the comb
    if (U.mode == 0u) {
        // luma_into_chroma_line_box: [m[x-1] or 16/255, m[x], m[x+1], m[x+2]] summed * 0.25
        const uint w1 = U.width - 1u;
        precise float a = x > 0u ? pS[uint2(x - 1u, y)] : U.c16;
        precise float s = ((a + pS[uint2(x, y)]) + pS[uint2(min(x + 1u, w1), y)]) + pS[uint2(min(x + 2u, w1), y)];
        pY[id.xy] = s * 0.25;
    } else if (U.mode == 2u) {
        // one-line comb: (top + bottom) * 0.5, line 0 takes line 1 as its top
        const uint top = r.field_row == 0u ? r.field_first + 1u : y - 1u;
        precise float s = pS[uint2(x, top)] + pS[uint2(x, y)];
        pY[id.xy] = s * 0.5;
    } else if (U.mode == 3u) {
        // two-line comb: cur * 0.5 + prev * 0.25 + next * 0.25, edges reflected
        const uint prev = r.field_row == 0u ? r.field_first + 1u : y - 1u;
        const uint next = r.field_row == r.field_rows - 1u ? r.field_first + r.field_rows - 2u : y + 1u;
        precise float s = (pS[uint2(x, y)] * 0.5 + pS[uint2(x, prev)] * 0.25) + pS[uint2(x, next)] * 0.25;
        pY[id.xy] = s;
    }
}

// demodulate_chroma_line: chroma = y - modulated; centre + right / 2 + left / 2.
// Each product is exact (multipliers 0, +-1, +-0.5) and at most two terms are
// non-zero, so the SIMD path's order (centre, left, right) and the scalar one's
// (centre, right, left) give the same sums.
[shader("compute")]
[numthreads(8, 8, 1)]
void demodulate(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= U.width || id.y >= U.planeRows) return;
    const uint x = id.x, y = id.y;
    const uint xi = rows[y].xi;
    const uint oc = (x + xi) & 3u;
    precise float cc = pY[uint2(x, y)] - pS[uint2(x, y)];
    precise float i = cc * I_MULT_INV[oc];
    precise float q = cc * Q_MULT_INV[oc];
    if (x + 1u < U.width) {
        const uint o = (x + 1u + xi) & 3u;
        precise float cr = pY[uint2(x + 1u, y)] - pS[uint2(x + 1u, y)];
        i += cr * I_MULT_INV[o] * 0.5;
        q += cr * Q_MULT_INV[o] * 0.5;
    }
    if (x > 0u) {
        const uint o = (x - 1u + xi) & 3u;
        precise float cl = pY[uint2(x - 1u, y)] - pS[uint2(x - 1u, y)];
        i += cl * I_MULT_INV[o] * 0.5;
        q += cl * Q_MULT_INV[o] * 0.5;
    }
    pI[id.xy] = i;
    pQ[id.xy] = q;
}

// ---- chroma_phase_offset_line (ntsc.rs 676-690): mode 0 rotSin/rotCos, mode 1 the row's ----
[shader("compute")]
[numthreads(8, 8, 1)]
void rotate_chroma(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= U.width || id.y >= U.planeRows) return;
    float s = U.rotSin, c = U.rotCos;
    if (U.mode == 1u) {
        s = rows[id.y].phase_sin;
        c = rows[id.y].phase_cos;
    }
    precise float i = pI[id.xy];
    precise float q = pQ[id.xy];
    precise float ri = (i * c) - (q * s);
    precise float rq = (i * s) + (q * c);
    pI[id.xy] = ri;
    pQ[id.xy] = rq;
}

// ---- chroma_loss (ntsc.rs 1088-1115): rows chosen on the CPU ----
[shader("compute")]
[numthreads(8, 8, 1)]
void chroma_loss(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= U.width || id.y >= U.planeRows) return;
    if ((rows[id.y].flags & 1u) == 0u) return;
    pI[id.xy] = 0.0;
    pQ[id.xy] = 0.0;
}

// ---- chroma_vert_blend (ntsc.rs 1118-1147): each chroma row averaged with the
// one above it in its field (the field's first row with 0), from a copy of the
// plane in the scratch plane: mode 0 writes I, mode 1 Q ----
[shader("compute")]
[numthreads(8, 8, 1)]
void vert_blend(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= U.width || id.y >= U.planeRows) return;
    precise float prev = rows[id.y].field_row == 0u ? 0.0 : pS[uint2(id.x, id.y - 1u)];
    precise float c = pS[id.xy];
    precise float r = (prev + c) * 0.5;
    if (U.mode == 0u) pI[id.xy] = r;
    else pQ[id.xy] = r;
}

// ---- phase 2: gradient noise (noise.rs, noise/simplex.rs, grid.rs, hash.rs) ----
// video_noise_line: add_noise_1d with Fbm(Simplex1d), per row seed and offset
// (ntsc_gpu_row_seeds). ntsc-rs evaluates it N lanes at a time (N = the SIMD
// width, 8 on AVX2): lane i of chunk k samples the coordinate
//   c = offset + i, then c += N, k times (each addition rounded),
// then Fbm scales it (x * frequency, x * 2 per octave). Every operation below is
// ntsc-rs's, fused where it uses mul_add, `precise` elsewhere.

// The k-fold rounded sum c + N + N + ... of the SIMD loop, without k steps:
// within a binade [2^e, 2^(e+1)) adding N (a power of two at least the ulp,
// for coordinates below 2^26) is exact, so the steps up to the next power of
// two are one exact multiply-add; only the step that crosses into the next
// binade rounds, and it is the same addition ntsc-rs does.
float chain_add(float x, float c, uint k)
{
    [loop] for (int guard = 0; guard < 96 && k > 0u; ++guard) {
        if (!(x >= c)) {
            precise float y = x + c;
            x = y;
            k -= 1u;
            continue;
        }
        const float lo = asfloat(asuint(x) & 0x7F800000u);  // 2^e <= x
        precise float d = lo * 2.0 - x;                     // exact (Sterbenz)
        const uint steps = uint(ceil(d * (1.0 / c)));        // additions to reach 2^(e+1)
        const uint m = min(steps - 1u, k);
        precise float mc = float(m) * c;
        precise float y = x + mc;  // exact: stays below 2^(e+1)
        x = y;
        k -= m;
        if (k > 0u) {
            precise float z = x + c;  // the crossing step: rounds like ntsc-rs's
            x = z;
            k -= 1u;
        }
    }
    return x;
}

// hash.rs hash_1d (ESGTSA)
uint hash_1d(uint v)
{
    v = (v ^ 2747636419u) * 2654435769u;
    v = (v ^ (v >> 16)) * 2654435769u;
    v = (v ^ (v >> 16)) * 2654435769u;
    return v;
}

// simplex.rs gradient_1d: (h & 7) + 1 with copysign(v, mask): negative when h & 8 == 0
float gradient_1d(uint hash)
{
    const uint h = hash >> 28;
    const float v = float((h & 7u) + 1u);
    return (h & 8u) == 0u ? -v : v;
}

// simplex.rs simplex_1d with grid.rs Grid<1> for Simplex
float simplex_1d<F : IFma>(float x, int seed)
{
    precise float fi = floor(x);
    const int i0 = int(fi);
    const int i1 = i0 + 1;
    precise float x0 = x - fi;
    precise float x1 = x0 - 1.0;
    const uint gi0 = hash_1d(uint(i0 ^ seed));
    const uint gi1 = hash_1d(uint(i1 ^ seed));
    precise float t0 = F.fma3(-x0, x0, 1.0);
    precise float t20 = t0 * t0;
    precise float t40 = t20 * t20;
    const float gx0 = gradient_1d(gi0);
    precise float t1 = F.fma3(-x1, x1, 1.0);
    precise float t21 = t1 * t1;
    precise float t41 = t21 * t21;
    const float gx1 = gradient_1d(gi1);
    precise float a = t40 * gx0;
    precise float b0 = t41 * gx1;
    precise float b = b0 * x1;
    precise float r = F.fma3(a, x0, b);
    return r;
}

// noise.rs Fbm::generate (lacunarity 2)
float fbm_1d<F : IFma>(float x, int seed, float frequency, int octaves, float gain)
{
    precise float pt = x * frequency;
    precise float result = simplex_1d<F>(pt, seed);
    precise float amplitude = gain;
    [loop] for (int o = 1; o < octaves; ++o) {
        pt = pt * 2.0;
        result = F.fma3(simplex_1d<F>(pt, seed), amplitude, result);
        amplitude = amplitude * gain;
    }
    return result;
}

float plane_load(uint p, uint2 at)
{
    if (p == 0u) return pY[at];
    if (p == 1u) return pI[at];
    return pQ[at];
}
void plane_store(uint p, uint2 at, float v)
{
    if (p == 0u) pY[at] = v;
    else if (p == 1u) pI[at] = v;
    else pQ[at] = v;
}
uint plane_of(uint bits) { return (bits & 1u) != 0u ? 0u : ((bits & 2u) != 0u ? 1u : 2u); }

// add_noise_1d: row + noise * intensity (fx.x frequency, fx.z gain, ix.x octaves, ix.y lanes)
void noise_add_impl<F : IFma>(uint3 id)
{
    id.y += uint(U.ix.w);
    if (id.x >= U.width || id.y >= U.planeRows) return;
    const NoiseRow nr = noiseRows[id.y];
    if (nr.active == 0u) return;
    const uint n = uint(U.ix.y);
    precise float c0 = nr.offset + float(id.x % n);
    const float c = chain_add(c0, float(n), id.x / n);
    const float noise = fbm_1d<F>(c, nr.seed, U.fx.x, U.ix.x, U.fx.z);
    const uint p = plane_of(U.planes);
    precise float add = noise * nr.intensity;
    precise float v = plane_load(p, id.xy) + add;
    plane_store(p, id.xy, v);
}
[shader("compute")]
[numthreads(64, 1, 1)]
void noise_add(uint3 id : SV_DispatchThreadID) { noise_add_impl<MadFma>(id); }
[shader("compute")]
[numthreads(64, 1, 1)]
void noise_add_x(uint3 id : SV_DispatchThreadID) { noise_add_impl<ExactFma>(id); }

// ---- phases 2-3: speckles (row_speckles' transients, head switching's transient) ----
// The values come from the CPU; a pixel adds those of the transients that
// cover it in the row's event order, i.e. ntsc-rs's order where transients
// overlap. A row's events start in non-decreasing x0 and are at most ix.x
// pixels long, so only those starting in (x - ix.x, x] can cover x.
[shader("compute")]
[numthreads(64, 1, 1)]
void add_speckles(uint3 id : SV_DispatchThreadID)
{
    const uint y = id.y + uint(U.ix.w);
    if (id.x >= U.width || y >= U.planeRows) return;
    const uint2 se = speckleRows[y];
    if (se.y == 0u) return;
    const int x = int(id.x);
    const int lo = x - U.ix.x + 1;
    uint a = se.x, b = se.x + se.y;
    [loop] while (a < b) {
        const uint m = (a + b) / 2u;
        if (speckles[m].x0 < lo) a = m + 1u;
        else b = m;
    }
    precise float v = pY[uint2(id.x, y)];
    bool any = false;
    [loop] for (uint e = a; e < se.x + se.y; ++e) {
        const Speckle sp = speckles[e];
        if (sp.x0 > x) break;
        if (x < sp.x0 + int(sp.count)) {
            v = v + speckleValues[sp.value_offset + uint(x - sp.x0)];
            any = true;
        }
    }
    if (any) pY[uint2(id.x, y)] = v;
}

// ---- phase 3: shift.rs shift_row / shift_row_to ----
// Output pixel x of a row shifted by s (read from row `srcRow` of the scratch
// copy): ntsc-rs's per-pixel formula, which differs between its SIMD blocks,
// (left - right).mul_add(frac, right), and its scalar edges, left * frac +
// right * (1 - frac); the blocks are simd_safe_range's (ix.y = their width).
float shifted<F : IFma>(uint x, uint srcRow, float s, int boundary)
{
    const int W = int(U.width);
    const int N = U.ix.y;
    precise float ts = trunc(s);
    const float tsc = clamp(ts, -float(W + 4), float(W + 4));  // beyond the row every read is the boundary
    const int si = int(tsc) - (s < 0.0 ? 1 : 0);
    precise float fr = s - ts;  // f32::fract
    precise float frac = s < 0.0 ? 1.0 - abs(fr) : fr;
    float bv = 0.0;
    if (boundary == 1) bv = si >= 0 ? pS[uint2(0u, srcRow)] : pS[uint2(uint(W - 1), srcRow)];
    const int li = int(x) - si - 1;
    const int ri = li + 1;
    const float L = (li >= 0 && li < W) ? pS[uint2(uint(li), srcRow)] : bv;
    const float R = (ri >= 0 && ri < W) ? pS[uint2(uint(ri), srcRow)] : bv;
    bool simd = false;
    if (N <= W) {
        const int safe_start = max(si + 1, 0);
        const int upper = W - N + min(si, 0);
        if (upper >= safe_start) {
            const int end = safe_start + ((upper - safe_start) / N + 1) * N;
            simd = int(x) >= safe_start && int(x) < end;
        }
    }
    if (simd) {
        precise float d = L - R;
        return F.fma3(d, frac, R);
    }
    precise float a = L * frac;
    precise float omf = 1.0 - frac;
    precise float b = R * omf;
    precise float r = a + b;
    return r;
}

// Row shifts of head switching / tracking noise / edge wave (the row table
// `shifts`) on the plane in `planes`, which the driver copied to the scratch plane.
void shift_rows_impl<F : IFma>(uint3 id)
{
    id.y += uint(U.ix.w);
    if (id.x >= U.width || id.y >= U.planeRows) return;
    const RowShift rs = shifts[id.y];
    if (rs.boundary < 0) return;
    if (rs.copy_start >= 0 && int(id.x) < rs.copy_start) return;  // the mid line: before it, unshifted
    plane_store(plane_of(U.planes), id.xy, shifted<F>(id.x, id.y, rs.shift, rs.boundary));
}
[shader("compute")]
[numthreads(64, 1, 1)]
void shift_rows(uint3 id : SV_DispatchThreadID) { shift_rows_impl<MadFma>(id); }
[shader("compute")]
[numthreads(64, 1, 1)]
void shift_rows_x(uint3 id : SV_DispatchThreadID) { shift_rows_impl<ExactFma>(id); }

// chroma_delay (ntsc.rs 969-1051) on the plane in `planes` (copied to the
// scratch plane first): fx.x horizontal shift, ix.x vertical offset in rows,
// ix.z = 1: rows copied (the horizontal shift is 0). Rows move within their
// field; rows with no source are 0. For a negative offset ntsc-rs reads
// `(offset - 1) * width` into the slice that starts after the destination
// row, i.e. row dst + offset: no off-by-one.
void chroma_delay_impl<F : IFma>(uint3 id)
{
    if (id.x >= U.width || id.y >= U.planeRows) return;
    const Row r = rows[id.y];
    const int fr = int(r.field_row), H = int(r.field_rows), off = U.ix.x;
    int src = fr;
    bool zero = false;
    if (off > 0) {
        if (fr < off) zero = true;
        else src = fr - off;
    } else if (off < 0) {
        const int o = -off;
        if (fr >= max(H, o) - o) zero = true;
        else src = fr + o;
    }
    const uint p = plane_of(U.planes);
    if (zero) {
        plane_store(p, id.xy, 0.0);
        return;
    }
    const uint srcRow = r.field_first + uint(src);
    if (off != 0 && U.ix.z == 1) plane_store(p, id.xy, pS[uint2(id.x, srcRow)]);
    else plane_store(p, id.xy, shifted<F>(id.x, srcRow, U.fx.x, 0));
}
[shader("compute")]
[numthreads(64, 1, 1)]
void chroma_delay(uint3 id : SV_DispatchThreadID) { chroma_delay_impl<MadFma>(id); }
[shader("compute")]
[numthreads(64, 1, 1)]
void chroma_delay_x(uint3 id : SV_DispatchThreadID) { chroma_delay_impl<ExactFma>(id); }
