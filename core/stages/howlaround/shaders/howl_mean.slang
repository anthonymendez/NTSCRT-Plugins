// howl_mean.slang — kernel "howl_mean/tiles". Replaces upstream's
// MPSImageStatisticsMean (the camcorder's exposure meter): each thread sums
// the luma (0.299 R + 0.587 G + 0.114 B) of one 16x16 tile of the camera's
// picture (x) and of the scene (y) into an RG32F texture of
// ceil(W/16) x ceil(H/16) texels; the CPU reads it back and adds the tiles up
// in a fixed order (deterministic). Mean of luma == luma of mean (linear).

Texture2D<float4> cameraTex : register(t0);
Texture2D<float4> sceneTex : register(t1);
RWTexture2D<float2> sums : register(u0);

static const uint TILE = 16;

[numthreads(8, 8, 1)]
void tiles(uint3 id : SV_DispatchThreadID) {
    uint tw, th;
    sums.GetDimensions(tw, th);
    if (id.x >= tw || id.y >= th) return;
    uint W, H;
    cameraTex.GetDimensions(W, H);
    float3 lw = float3(0.299, 0.587, 0.114);
    float2 acc = 0.0;
    uint x0 = id.x * TILE, y0 = id.y * TILE;
    for (uint y = y0; y < min(y0 + TILE, H); y++) {
        for (uint x = x0; x < min(x0 + TILE, W); x++) {
            acc.x += dot(cameraTex.Load(int3(x, y, 0)).rgb, lw);
            acc.y += dot(sceneTex.Load(int3(x, y, 0)).rgb, lw);
        }
    }
    sums[id.xy] = acc;
}
