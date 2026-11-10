// HAL built-ins. gpu_builtin/copy: Device::copy() between textures whose
// formats differ (CopyResource needs identical formats). Copies the region
// both textures cover.
struct CopyParams { uint2 size; uint2 pad; };
ConstantBuffer<CopyParams> P : register(b0);
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);

[shader("compute")]
[numthreads(8, 8, 1)]
void copy(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.size.x || id.y >= P.size.y) return;
    dst[id.xy] = src.Load(int3(id.xy, 0));
}
