// digital_gpu.slang — the full-size pictures of the Digital video stage on the GPU
// (docs/dev/DIGITAL-GPU.md). Driver: core/stages/digital/gpu/digital_gpu.cpp.
// Reference, line by line: core/stages/digital/renderer.cpp
//   scale_h + scale_v = Renderer::scale (scaleLanczos, the aliasing, One field)
//   upscale           = Renderer::upscale
// The codec between them stays on the CPU (capture-size pictures only).
//
// Exactness: the CPU code rounds every product and every sum to float, one by
// one, in a fixed order (renderer.cpp builds with /fp:precise /
// -ffp-contract=off). Every value here is `precise` (no fusion into mad, no
// reordering) and goes through A.mul / A.add: FloatArith where the device
// rounds a precise product and a precise sum like the CPU (the driver's probe),
// DoubleArith (the *_x kernels) where it doesn't: a float product is exact in
// a double and a double sum rounded to float is the float sum (53 >= 2 * 24 + 2
// bits), so they reproduce float arithmetic exactly. Nothing that involves
// sin(), a division or doubles on the CPU side is computed here: the Lanczos
// taps, the aliasing coordinates, the 3x3 mean's division by 9 and the upscale
// maps come as tables (Renderer::scalePlan / ninths / upscaleMaps). Rounding:
// std::round (half away from zero) is spelled out (HLSL round() is half to even).
//
// Bindings (gpu.h convention):
//   b0  DigitalU
//   scale_h:  t0 picture (BGRA8, W x H)   t8 spanX  t9 weightX            u0 mid (RGBA32F, cw x H)
//   scale_v:  t0 picture  t1 mid  t8 spanY  t9 weightY  t10 aliasRow  t11 aliasCol  t12 ninths
//                                                                       u0 capture (BGRA8, cw x ch)
//   upscale:  t0 shown (BGRA8, cw x ch)   t13 mapX  t14 mapY            u0 out (BGRA8, W x H)
//   probe_round: u0 (RGBA32F, 1 x 1)
// Channels: the shader's float4 is (R, G, B, A) whatever the texture's byte
// order; the CPU's BGRA bytes 0, 1, 2, 3 are .b, .g, .r, .a. Each channel is
// filtered alone, so only the aliasing's per-channel columns care (aliasCol is
// blue, green, red).

struct DigitalU {
    uint W, H, cw, ch;
    uint tapsX, tapsY, flags, pad0;
    float mix, boost, pad1, pad2;
    float4 probe;  // probe_round only
};
static const uint kCopy = 1u;     // same size: the picture itself
static const uint kAlias = 2u;    // aliasing on (mix > 0)
static const uint kBoost = 4u;    // aliasing above 50% (boost > 0)
static const uint kFields = 8u;   // One field

ConstantBuffer<DigitalU> U : register(b0);
Texture2D<float4> pic : register(t0);
Texture2D<float4> mid : register(t1);
StructuredBuffer<int2> span : register(t8);
StructuredBuffer<float> weight : register(t9);
StructuredBuffer<int> aliasRow : register(t10);
StructuredBuffer<int4> aliasCol : register(t11);
StructuredBuffer<float> ninths : register(t12);
StructuredBuffer<int> mapX : register(t13);
StructuredBuffer<int> mapY : register(t14);
RWTexture2D<float4> outTex : register(u0);  // mid, the capture picture, the upscaled picture, the probe

interface IArith {
    static float mul(float a, float b);
    static float add(float a, float b);
};
struct FloatArith : IArith {
    static float mul(float a, float b)
    {
        precise float r = a * b;
        return r;
    }
    static float add(float a, float b)
    {
        precise float r = a + b;
        return r;
    }
};
struct DoubleArith : IArith {
    static float mul(float a, float b)
    {
        __target_switch
        {
        case hlsl:
            precise double r = double(a) * double(b);
            return float(r);
        default:
            precise float f = a * b;
            return f;
        }
    }
    static float add(float a, float b)
    {
        __target_switch
        {
        case hlsl:
            precise double r = double(a) + double(b);
            return float(r);
        default:
            precise float f = a + b;
            return f;
        }
    }
};

// The byte a UNORM8 texel holds, as a float (exact).
float4 bytes(float4 v)
{
    return float4(uint4(round(v * 255.0)));
}
// std::round, then clamped to 0..255 (the CPU's clampf(std::round(a), 0, 255)):
// negative values round to <= 0 and clamp to 0 either way.
float round_byte(float a)
{
    if (!(a > 0.0)) return 0.0;
    precise float f = floor(a);
    precise float frac = a - f;  // exact (a and f share the exponent range)
    if (frac >= 0.5) f = f + 1.0;
    return min(f, 255.0);
}

// ---- probe: does this device round a precise product and sum like the CPU? ----
// u0 (RGBA32F, 1 x 1): x*x - z (x = 1 + 2^-12, z = 1 + 2^-11: 0 when the product is
// rounded to nearest even and not fused into the subtraction), x*y - w (y = 1 +
// 2^-12 + 2^-23, w = 1 + 2^-11 + 2^-22: 0 when the product rounds to nearest,
// not toward zero), (1 + v) - 1 (v = 2^-24 + 2^-30: 2^-23 when sums round to
// nearest). The constants come from the constant buffer, so nothing folds.
void probe_impl<A : IArith>()
{
    const float x = U.probe.x, y = U.probe.y, z = U.probe.z, w = U.probe.w;
    outTex[uint2(0, 0)] = float4(A.add(A.mul(x, x), -z), A.add(A.mul(x, y), -w), A.add(A.add(U.mix, U.boost), -U.mix), 1.0);
}
[shader("compute")]
[numthreads(1, 1, 1)]
void probe_round(uint3 id : SV_DispatchThreadID) { probe_impl<FloatArith>(); }
[shader("compute")]
[numthreads(1, 1, 1)]
void probe_round_x(uint3 id : SV_DispatchThreadID) { probe_impl<DoubleArith>(); }

// ---- scaleLanczos, first pass: rows -> cw columns, float (mid) ----
void scale_h_impl<A : IArith>(uint3 id)
{
    if (id.x >= U.cw || id.y >= U.H) return;
    const int2 s = span[id.x];
    const uint base = id.x * U.tapsX;
    precise float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    for (int j = 0; j < s.y; ++j) {
        const int sx = min(int(U.W) - 1, max(0, s.x + j));
        const float4 p = bytes(pic.Load(int3(sx, int(id.y), 0)));
        const float wj = weight[base + uint(j)];
        acc.x = A.add(acc.x, A.mul(wj, p.x));
        acc.y = A.add(acc.y, A.mul(wj, p.y));
        acc.z = A.add(acc.z, A.mul(wj, p.z));
        acc.w = A.add(acc.w, A.mul(wj, p.w));
    }
    outTex[id.xy] = acc;
}
[shader("compute")]
[numthreads(8, 8, 1)]
void scale_h(uint3 id : SV_DispatchThreadID) { scale_h_impl<FloatArith>(id); }
[shader("compute")]
[numthreads(8, 8, 1)]
void scale_h_x(uint3 id : SV_DispatchThreadID) { scale_h_impl<DoubleArith>(id); }

// ---- scale's aliasing: one channel at source pixel sx of row rows[1], its fine detail boosted ----
float alias_read<A : IArith>(int sx, int c, int3 rows)
{
    const float v = bytes(pic.Load(int3(sx, rows.y, 0)))[c];
    if ((U.flags & kBoost) == 0u) return v;
    // sum of the 3 x 3 neighbourhood: whole numbers, exact in any order
    float sum = 0.0;
    [unroll] for (int r = 0; r < 3; ++r) {
        const int ry = r == 0 ? rows.x : (r == 1 ? rows.y : rows.z);
        [unroll] for (int dx = -1; dx <= 1; ++dx)
            sum += bytes(pic.Load(int3(min(int(U.W) - 1, max(0, sx + dx)), ry, 0)))[c];
    }
    // clampf(v + boost * (v - sum / 9), 0, 255)
    precise float d = A.add(v, -ninths[uint(sum)]);
    precise float e = A.add(v, A.mul(U.boost, d));
    return min(255.0, max(0.0, e));
}

// ---- scaleLanczos, second pass (or the copy), then the aliasing, then One field ----
void scale_v_impl<A : IArith>(uint3 id)
{
    if (id.x >= U.cw || id.y >= U.ch) return;
    // One field: odd rows repeat the (finished) row above.
    const uint y = (U.flags & kFields) != 0u ? (id.y & ~1u) : id.y;
    float4 o;
    if ((U.flags & kCopy) != 0u) {
        o = bytes(pic.Load(int3(int(id.x), int(y), 0)));
    } else {
        const int2 s = span[y];
        const uint base = y * U.tapsY;
        precise float4 acc = float4(0.0, 0.0, 0.0, 0.0);
        for (int j = 0; j < s.y; ++j) {
            const int sy = min(int(U.H) - 1, max(0, s.x + j));
            const float4 m = mid.Load(int3(int(id.x), sy, 0));
            const float wj = weight[base + uint(j)];
            acc.x = A.add(acc.x, A.mul(wj, m.x));
            acc.y = A.add(acc.y, A.mul(wj, m.y));
            acc.z = A.add(acc.z, A.mul(wj, m.z));
            acc.w = A.add(acc.w, A.mul(wj, m.w));
        }
        o = float4(round_byte(acc.x), round_byte(acc.y), round_byte(acc.z), round_byte(acc.w));
    }
    if ((U.flags & kAlias) != 0u) {
        const int sy = aliasRow[y];
        const int3 rows = int3(max(0, sy - 1), sy, min(int(U.H) - 1, sy + 1));
        const int4 cols = aliasCol[id.x];  // blue, green, red
        const float pb = alias_read<A>(cols.x, 2, rows);
        const float pg = alias_read<A>(cols.y, 1, rows);
        const float pr = alias_read<A>(cols.z, 0, rows);
        // uint8_t(std::round(o + (p - o) * mix)): o and p in 0..255, so is the result
        o.z = round_byte(A.add(o.z, A.mul(A.add(pb, -o.z), U.mix)));
        o.y = round_byte(A.add(o.y, A.mul(A.add(pg, -o.y), U.mix)));
        o.x = round_byte(A.add(o.x, A.mul(A.add(pr, -o.x), U.mix)));
    }
    outTex[id.xy] = o / 255.0;
}
[shader("compute")]
[numthreads(8, 8, 1)]
void scale_v(uint3 id : SV_DispatchThreadID) { scale_v_impl<FloatArith>(id); }
[shader("compute")]
[numthreads(8, 8, 1)]
void scale_v_x(uint3 id : SV_DispatchThreadID) { scale_v_impl<DoubleArith>(id); }

// ---- Renderer::upscale: nearest capture pixel, by the CPU's maps ----
[shader("compute")]
[numthreads(8, 8, 1)]
void upscale(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= U.W || id.y >= U.H) return;
    outTex[id.xy] = pic.Load(int3(mapX[id.x], mapY[id.y], 0));
}
