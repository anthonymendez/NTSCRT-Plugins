// ntsc_gpu.slang — the GPU port of ntsc-rs's effect (phase 1 of 4).
// Design, plane format and tables: docs/dev/NTSC-GPU.md. Driver:
// core/stages/ntsc/gpu/ntsc_gpu.cpp. Reference: vendor/ntsc-rs/crates/ntscrs/src
// (ntsc.rs, filter.rs, yiq_fielding.rs); the plan and tables come from
// rust/ntscrs-capi/src/gpu_plan.rs.
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
struct MadFma : IFma {
    static float fma3(float a, float b, float c) { return mad(a, b, c); }
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
