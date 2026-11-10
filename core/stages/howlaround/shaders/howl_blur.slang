// howl_blur.slang — kernel "howl_blur/blur". Port of upstream howl_blur:
// one separable Gaussian pass of the camera's defocus. CPU reference: blur_ref.

struct BlurParams {
    float2 dir;
    float sigma;
    int radius;
};

ConstantBuffer<BlurParams> u : register(b0);
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);

[numthreads(16, 16, 1)]
void blur(uint3 id : SV_DispatchThreadID) {
    uint W, H;
    dst.GetDimensions(W, H);
    uint2 gid = id.xy;
    if (gid.x >= W || gid.y >= H) return;
    float4 acc = 0.0;
    float wsum = 0.0;
    int2 stepv = int2(u.dir);
    for (int k = -u.radius; k <= u.radius; k++) {
        float w = exp(-0.5 * float(k * k) / (u.sigma * u.sigma));
        int2 p = clamp(int2(gid) + stepv * k, int2(0, 0), int2(int(W) - 1, int(H) - 1));
        acc += src.Load(int3(p, 0)) * w;
        wsum += w;
    }
    dst[gid] = acc / wsum;
}
