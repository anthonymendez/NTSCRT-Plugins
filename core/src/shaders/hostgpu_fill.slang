// hostgpu_fill.slang — Clip mode's hole filling for host GPU frames: fill_transparent
// (core/src/pixel_convert.cpp) as kernels, so a frame with transparent pixels stays on
// the GPU (path=gpu-direct). Driver: core/src/host_gpu_fill.cpp. Design and
// measurements: docs/dev/HOST-GPU.md ("Hole filling on the GPU").
//
// The CPU code, step by step, and the kernel doing it:
//   the 8-bit picture to_bgra8 makes (convert_in), each pixel's weight 1 when its
//   alpha is above 0.5/255, else 0
//   pull: the mip levels, each pixel the weighted mean of a 2 x 2 block of the level
//         below (edge pixels clamped: counted twice on odd sizes), weight min(1, sum)
//                                                       -> fill_down0 (from the source), fill_down
//   push: coarse to fine, a pixel of weight < 1 takes the bilinear sample of the
//         (finished) coarser level                       -> fill_up, one dispatch per level
//   level 0: only transparent pixels change, to the bilinear sample of level 1,
//            rounded to 8 bits                            -> convert_fill (convert_in + that)
// Nothing to fill from (every pixel transparent): the picture stays as convert_in
// makes it (the top level's weight is 0).
//
// Exactness: the weights are 0 or 1 at every level (min(1, a whole number)), so the
// sums are sums of 8-bit values / 255 and of their means, the divisions by the weight
// sum divide by 1, 2, 3 or 4, and the push only copies. The CPU (MSVC /fp:precise,
// SSE2: nothing fused) rounds each product and sum to float, in a fixed order; so does
// every value here: `precise`, and through A.mul / A.add (IArith, as the Digital video
// kernels: FloatArith where the probe says the device rounds a precise float product
// and sum like the CPU and doesn't fuse them, DoubleArith in the *_x kernels else).
// /2 and /4 are exact multiplications; /3 is spelled out (div3: the float nearest
// s / 3, picked by exact residuals), as GPU division isn't correctly rounded. The
// 8-bit values come from the CPU's own table of k / 255.f (unorm8), not a GPU division.
//
// Bindings (gpu.h convention):
//   b0  FillParams (InParams of hostgpu_in.slang, then the level sizes and the probe)
//   fill_down0:   t0 host source                         t8 unorm8   u0 level 1 (RGBA32F)
//   fill_down:    t1 the level below (RGBA32F)                       u0 the level (RGBA32F)
//   fill_up:      t1 the level's pull result  t2 the finished coarser level  u0 the finished level
//   convert_fill: t0 host source  t2 finished level 1  t3 the top level (1 x 1)  u0 picture (BGRA8)
//   fill_probe:   u0 (RGBA32F, 1 x 1)
struct FillParams {
    uint frame_w, frame_h, src_w, src_h;
    int src_x, src_y, bottom_up, mode;
    uint cx, cy, cw, ch;
    uint premult, pad0, pad1, pad2;
    uint in_w, in_h, out_w, out_h;  // fine and coarse level sizes (fill_up: this level, the coarser one)
    float4 probe;                   // fill_probe only: x, y, z, w
    float4 probe2;                  // fill_probe only: 1, v
};
ConstantBuffer<FillParams> P : register(b0);
Texture2D<float4> host_src : register(t0);
Texture2D<float4> lv_in : register(t1);
Texture2D<float4> lv_coarse : register(t2);
Texture2D<float4> lv_top : register(t3);
StructuredBuffer<float> unorm8 : register(t8);
RWTexture2D<float4> outTex : register(u0);

static const float kTransparent = 0.5 / 255.0;

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

// ---- the frame and its conversion: as hostgpu_in.slang (convert_in), Clip mode ----
float4 frame_px(uint fx, uint fy)
{
    int my = P.bottom_up != 0 ? int(P.frame_h) - 1 - int(fy) : int(fy);
    int sx = int(fx) - P.src_x, sy = my - P.src_y;
    if (sx < 0 || sy < 0 || sx >= int(P.src_w) || sy >= int(P.src_h)) return float4(0.0, 0.0, 0.0, 0.0);
    return host_src.Load(int3(sx, sy, 0));
}
float unit_alpha(float a) { return a > 0.0 ? (a < 1.0 ? a : 1.0) : 0.0; }
// The byte uint8_t(double(v * 255.0f) + 0.5) (0 / 255 outside (0, 1), NaN -> 0):
// hostgpu_in.slang's q8, as the byte.
uint q8i(float v)
{
    if (!(v > 0.0)) return 0u;
    if (v >= 1.0) return 255u;
    precise float p = v * 255.0;
    float f = floor(p);
    return uint(f) + (p >= f + 0.5 ? 1u : 0u);
}
float q8(float v) { return float(q8i(v)) / 255.0; }  // for a UNORM8 texel (exact: round(q8 * 255) = q8i)
float3 div(float3 c, float a)
{
    precise float3 q = c / a;
    precise float3 e = mad(-q, float3(a, a, a), c);
    return q + e / a;
}
// The straight colour convert_in gives frame pixel (fx, fy), as its bytes, and its alpha.
uint3 converted(uint fx, uint fy, out float a)
{
    float4 v = frame_px(fx, fy);
    a = unit_alpha(v.a);
    float3 c = v.rgb;
    if (P.premult != 0 && a > kTransparent && a < 1.0) c = div(c, a);
    return uint3(q8i(c.r), q8i(c.g), q8i(c.b));
}

// ---- s / 3 rounded to nearest, as the CPU divides (s >= 0) ----
// s - 3q exactly, for q within a few units in the last place of s / 3: 2q + q as a sum
// and its rounding error (TwoSum), s minus the sum (exact: they're within a factor 2),
// minus the error (exact: the result is a small multiple of q's last place).
float residual3<A : IArith>(float s, float q)
{
    precise float a = A.add(q, q);
    precise float t = A.add(a, q);
    precise float bv = A.add(t, -a);
    precise float av = A.add(t, -bv);
    precise float e = A.add(A.add(a, -av), A.add(q, -bv));
    return A.add(A.add(s, -t), -e);
}
float div3<A : IArith>(float s)
{
    if (!(s > 0.0)) return 0.0;
    // s * float(1/3): within 2 units in the last place of s / 3; the nearest of the 5
    // floats around it (no ties: s / 3 is never halfway between two floats).
    precise float q0 = A.mul(s, 0.3333333432674408);
    float best = q0, best_r = abs(residual3<A>(s, q0));
    [unroll] for (int k = -2; k <= 2; ++k) {
        if (k == 0) continue;
        const float q = asfloat(uint(int(asuint(q0)) + k));
        const float r = abs(residual3<A>(s, q));
        if (r < best_r) {
            best = q;
            best_r = r;
        }
    }
    return best;
}
// s / n for the weight sums n of a 2 x 2 block (1..4).
float mean<A : IArith>(float s, float n)
{
    if (n == 1.0) return s;
    if (n == 2.0) return A.mul(s, 0.5);
    if (n == 4.0) return A.mul(s, 0.25);
    return div3<A>(s);
}

// ---- pull: one level from the one below ----
// A pixel of the level below as fill_transparent reads it: r, g, b (straight), weight.
float4 below<A : IArith>(bool first, int x, int y)
{
    if (!first) return lv_in.Load(int3(x, y, 0));
    float a;
    const uint3 b = converted(uint(x), uint(y), a);
    if (!(a > kTransparent)) return float4(0.0, 0.0, 0.0, 0.0);  // weight 0 (its colour counts 0 times)
    return float4(unorm8[b.r], unorm8[b.g], unorm8[b.b], 1.0);
}
void down_impl<A : IArith>(uint3 id, bool first)
{
    if (id.x >= P.out_w || id.y >= P.out_h) return;
    precise float4 s = float4(0.0, 0.0, 0.0, 0.0);
    [unroll] for (int dy = 0; dy < 2; ++dy)
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            const int sx = min(2 * int(id.x) + dx, int(P.in_w) - 1), sy = min(2 * int(id.y) + dy, int(P.in_h) - 1);
            const float4 v = below<A>(first, sx, sy);
            s.r = A.add(s.r, A.mul(v.r, v.a));
            s.g = A.add(s.g, A.mul(v.g, v.a));
            s.b = A.add(s.b, A.mul(v.b, v.a));
            s.a = A.add(s.a, v.a);
        }
    float4 o = float4(0.0, 0.0, 0.0, min(1.0, s.a));
    if (s.a > 0.0) o.rgb = float3(mean<A>(s.r, s.a), mean<A>(s.g, s.a), mean<A>(s.b, s.a));
    outTex[id.xy] = o;
}
[shader("compute")]
[numthreads(8, 8, 1)]
void fill_down0(uint3 id : SV_DispatchThreadID) { down_impl<FloatArith>(id, true); }
[shader("compute")]
[numthreads(8, 8, 1)]
void fill_down0_x(uint3 id : SV_DispatchThreadID) { down_impl<DoubleArith>(id, true); }
[shader("compute")]
[numthreads(8, 8, 1)]
void fill_down(uint3 id : SV_DispatchThreadID) { down_impl<FloatArith>(id, false); }
[shader("compute")]
[numthreads(8, 8, 1)]
void fill_down_x(uint3 id : SV_DispatchThreadID) { down_impl<DoubleArith>(id, false); }

// ---- the bilinear sample of the coarser level (cw x ch) at pixel (x, y) of the finer ----
float3 sample_coarse<A : IArith>(uint cw, uint ch, uint x, uint y)
{
    precise float fx = min(max(A.add(A.mul(A.add(float(x), 0.5), 0.5), -0.5), 0.0), float(cw - 1u));
    precise float fy = min(max(A.add(A.mul(A.add(float(y), 0.5), 0.5), -0.5), 0.0), float(ch - 1u));
    const int x0 = int(fx), y0 = int(fy);
    const int x1 = min(x0 + 1, int(cw) - 1), y1 = min(y0 + 1, int(ch) - 1);
    precise float tx = A.add(fx, -float(x0)), ty = A.add(fy, -float(y0));
    precise float ux = A.add(1.0, -tx), uy = A.add(1.0, -ty);
    const float3 c00 = lv_coarse.Load(int3(x0, y0, 0)).rgb, c10 = lv_coarse.Load(int3(x1, y0, 0)).rgb;
    const float3 c01 = lv_coarse.Load(int3(x0, y1, 0)).rgb, c11 = lv_coarse.Load(int3(x1, y1, 0)).rgb;
    float3 o;
    [unroll] for (int k = 0; k < 3; ++k) {
        // (c00 * (1 - tx) + c10 * tx) * (1 - ty) + (c01 * (1 - tx) + c11 * tx) * ty
        precise float top = A.add(A.mul(c00[k], ux), A.mul(c10[k], tx));
        precise float bot = A.add(A.mul(c01[k], ux), A.mul(c11[k], tx));
        o[k] = A.add(A.mul(top, uy), A.mul(bot, ty));
    }
    return o;
}

// ---- push: a level finished from the finished coarser one ----
void up_impl<A : IArith>(uint3 id)
{
    if (id.x >= P.in_w || id.y >= P.in_h) return;
    const float4 p = lv_in.Load(int3(int(id.x), int(id.y), 0));
    if (p.a >= 1.0) {
        outTex[id.xy] = p;
        return;
    }
    const float3 u = sample_coarse<A>(P.out_w, P.out_h, id.x, id.y);
    // p * w + u * (1 - w), w = p.a
    precise float uw = A.add(1.0, -p.a);
    outTex[id.xy] = float4(A.add(A.mul(p.r, p.a), A.mul(u.r, uw)), A.add(A.mul(p.g, p.a), A.mul(u.g, uw)),
                           A.add(A.mul(p.b, p.a), A.mul(u.b, uw)), 1.0);
}
[shader("compute")]
[numthreads(8, 8, 1)]
void fill_up(uint3 id : SV_DispatchThreadID) { up_impl<FloatArith>(id); }
[shader("compute")]
[numthreads(8, 8, 1)]
void fill_up_x(uint3 id : SV_DispatchThreadID) { up_impl<DoubleArith>(id); }

// ---- level 0: convert_in, transparent pixels from the finished level 1 ----
// Writes the frame cut to cx, cy, cw, ch (the look's crop) like convert_in; the fill
// is the whole frame's (the CPU path fills, then crops). out_w x out_h: level 1's size.
void convert_fill_impl<A : IArith>(uint3 id)
{
    if (id.x >= P.cw || id.y >= P.ch) return;
    const uint fx = P.cx + id.x, fy = P.cy + id.y;
    float a;
    uint3 b = converted(fx, fy, a);
    float3 c = float3(float(b.r), float(b.g), float(b.b)) / 255.0;
    if (!(a > kTransparent) && lv_top.Load(int3(0, 0, 0)).a > 0.0) {
        const float3 u = sample_coarse<A>(P.out_w, P.out_h, fx, fy);
        c = float3(q8(u.r), q8(u.g), q8(u.b));
    }
    outTex[id.xy] = float4(c, 1.0);
}
[shader("compute")]
[numthreads(8, 8, 1)]
void convert_fill(uint3 id : SV_DispatchThreadID) { convert_fill_impl<FloatArith>(id); }
[shader("compute")]
[numthreads(8, 8, 1)]
void convert_fill_x(uint3 id : SV_DispatchThreadID) { convert_fill_impl<DoubleArith>(id); }

// ---- probe: does this device round a precise product and sum like the CPU? ----
// As digital_gpu.slang's probe_round: x*x - z (0 when the product is rounded to nearest
// even and not fused into the subtraction), x*y - w (0 when it rounds to nearest, not
// toward zero), (1 + v) - 1 (2^-23 when sums round to nearest); the constants come
// from the constant buffer, so nothing folds.
void probe_impl<A : IArith>()
{
    const float x = P.probe.x, y = P.probe.y, z = P.probe.z, w = P.probe.w;
    outTex[uint2(0, 0)] = float4(A.add(A.mul(x, x), -z), A.add(A.mul(x, y), -w),
                                 A.add(A.add(P.probe2.x, P.probe2.y), -P.probe2.x), 1.0);
}
[shader("compute")]
[numthreads(1, 1, 1)]
void fill_probe(uint3 id : SV_DispatchThreadID) { probe_impl<FloatArith>(); }
[shader("compute")]
[numthreads(1, 1, 1)]
void fill_probe_x(uint3 id : SV_DispatchThreadID) { probe_impl<DoubleArith>(); }
