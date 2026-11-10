// howl_resample.slang — kernel "howl_resample/resample": bilinear resize.
// The TV's picture when the façade has not lent Howlaround its chain hooks
// (the TV then shows the camera's picture directly). CPU reference: resample_ref.

Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState linearSampler : register(s0);

[numthreads(16, 16, 1)]
void resample(uint3 id : SV_DispatchThreadID) {
    uint W, H, sw, sh;
    dst.GetDimensions(W, H);
    if (id.x >= W || id.y >= H) return;
    src.GetDimensions(sw, sh);
    float2 lo = 0.5 / float2(sw, sh);
    float2 uv = (float2(id.xy) + 0.5) / float2(W, H);
    dst[id.xy] = src.SampleLevel(linearSampler, clamp(uv, lo, 1.0 - lo), 0);
}
