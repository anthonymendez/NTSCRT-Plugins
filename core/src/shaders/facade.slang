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
