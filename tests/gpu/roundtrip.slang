// HAL test kernels (tests/gpu/test_gpu.cpp).
struct Params { float4 gain; uint2 size; uint2 pad; };
ConstantBuffer<Params> P : register(b0);
Texture2D<float4> src : register(t0);
StructuredBuffer<float4> offsets : register(t8);   // stride 16, one per row
StructuredBuffer<float4> rowsum_in : register(t9); // written by `rowsum`
RWTexture2D<float4> dst : register(u0);
RWStructuredBuffer<float4> rowsum : register(u8);
SamplerState lin : register(s0);
SamplerState pt : register(s1);

// dst = src * gain + offsets[y]
[shader("compute")]
[numthreads(8, 8, 1)]
void scale(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.size.x || id.y >= P.size.y) return;
    dst[id.xy] = src.Load(int3(id.xy, 0)) * P.gain + offsets[id.y];
}

// rowsum[y] = sum of row y (UAV buffer write).
[shader("compute")]
[numthreads(64, 1, 1)]
void sum_rows(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.size.y) return;
    float4 s = 0;
    for (uint x = 0; x < P.size.x; ++x) s += src.Load(int3(x, id.x, 0));
    rowsum[id.x] = s;
}

// dst = rowsum_in[y] / width (the UAV buffer from sum_rows, now as SRV).
[shader("compute")]
[numthreads(8, 8, 1)]
void row_mean(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.size.x || id.y >= P.size.y) return;
    dst[id.xy] = rowsum_in[id.y] / float(P.size.x);
}

// Point vs. linear sampler at a texel boundary.
[shader("compute")]
[numthreads(8, 8, 1)]
void samplers(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.size.x || id.y >= P.size.y) return;
    float2 uv = float2(1.0, 0.5) / float2(2.0, 1.0);  // exactly between texel 0 and 1 of a 2x1 texture
    dst[id.xy] = id.x == 0 ? src.SampleLevel(pt, float2(0.25, 0.5), 0) : src.SampleLevel(lin, uv, 0);
}
