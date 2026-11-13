// facade/alpha_merge: final output step of ntscrt_render_metal. RGB from the
// pipeline result (opaque BGRA8, or RGBA16F for 16-bit / float destinations,
// clamped to 0..1 like an 8-bit target; upstream ignores alpha), alpha from the
// host's source frame, combined per the look's alpha mode (same maths as
// merge_alpha in pixel_convert.h), written in the destination's format.
//   mode 0 Clip:  alpha = source alpha
//   mode 1 Spill: alpha = max(source alpha, brightest channel)
//   premult: destination wants premultiplied RGB
//   use_alpha 0: source alpha not aligned with the result (counts as 1 for
//                Clip, 0 for Spill)
struct MergeParams { uint2 size; uint use_alpha; uint mode; uint premult; uint3 pad; };
ConstantBuffer<MergeParams> P : register(b0);
Texture2D<float4> rgb : register(t0);
Texture2D<float4> alpha_src : register(t1);
RWTexture2D<float4> dst : register(u0);

[shader("compute")]
[numthreads(8, 8, 1)]
void alpha_merge(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.size.x || id.y >= P.size.y) return;
    float3 c = saturate(rgb.Load(int3(id.xy, 0)).rgb);
    float a = P.use_alpha != 0 ? saturate(alpha_src.Load(int3(id.xy, 0)).a) : (P.mode != 0 ? 0.0 : 1.0);
    if (P.mode != 0) {
        a = max(a, max(c.r, max(c.g, c.b)));
        if (P.premult == 0) c = a > 0.0 ? min(c / a, float3(1.0, 1.0, 1.0)) : float3(0.0, 0.0, 0.0);
    } else if (P.premult != 0) {
        c *= a;
    }
    dst[id.xy] = float4(c, a);
}

// facade/pack_rgb: the CPU path's readback of an RGBA16F result (ntscrt_render_cpu) without
// its alpha, which nothing reads (the host's alpha comes from the source): 6 bytes a pixel
// instead of 8. size = the result's w x h; dst (RGBA16F, ceil(3w / 4) x h): texel j of a row
// holds that row's halves 4j .. 4j + 3 of r0 g0 b0 r1 g1 b1 ... (0 past the end). Each value
// is a half read as a float and written as a half again: the same bits (NaN aside, which the
// CPU conversion turns into 0 either way; tests/core/test_bit_depth.cpp checks every half).
[shader("compute")]
[numthreads(8, 8, 1)]
void pack_rgb(uint3 id : SV_DispatchThreadID)
{
    const uint pw = (P.size.x * 3u + 3u) / 4u;
    if (id.x >= pw || id.y >= P.size.y) return;
    // Texel j starts at half 4j: pixel 4j / 3, channel j % 3. Static swizzles only: an
    // indexed component (v[c]) may compile to a dot product with a one-hot vector, which
    // turns a neighbouring NaN or infinity into NaN.
    const uint ph = id.x % 3u, p = (4u * id.x) / 3u;
    const float4 a = rgb.Load(int3(int(p), int(id.y), 0));
    const float4 b = p + 1u < P.size.x ? rgb.Load(int3(int(p + 1u), int(id.y), 0)) : float4(0.0, 0.0, 0.0, 0.0);
    float4 o;
    if (ph == 0u) o = float4(a.r, a.g, a.b, b.r);
    else if (ph == 1u) o = float4(a.g, a.b, b.r, b.g);
    else o = float4(a.b, b.r, b.g, b.b);
    dst[id.xy] = o;
}
