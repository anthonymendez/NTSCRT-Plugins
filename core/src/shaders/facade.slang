// facade/alpha_merge: final output step of ntscrt_render_metal. RGB from the
// pipeline result, alpha from the host's source frame (the pipeline is opaque
// 8-bit BGRA; upstream ignores alpha), written in the destination's format.
struct MergeParams { uint2 size; uint use_alpha; uint pad; };
ConstantBuffer<MergeParams> P : register(b0);
Texture2D<float4> rgb : register(t0);
Texture2D<float4> alpha_src : register(t1);
RWTexture2D<float4> dst : register(u0);

[shader("compute")]
[numthreads(8, 8, 1)]
void alpha_merge(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.size.x || id.y >= P.size.y) return;
    float4 c = rgb.Load(int3(id.xy, 0));
    float a = P.use_alpha != 0 ? alpha_src.Load(int3(id.xy, 0)).a : 1.0;
    dst[id.xy] = float4(c.rgb, a);
}
