// Port of upstream Sources/CrtCore/Downscaler.swift (inline MSL).
//
// nearest and area are single passes. nearest+ (nearestaa), bilinear,
// bicubic and lanczos are proper decimation filters whose support scales
// with the downscale ratio, run as two separable passes: _h (sw x sh) ->
// (dw x sh), then _v (dw x sh) -> (dw x dh).
struct Dims { uint sw, sh, dw, dh; };
ConstantBuffer<Dims> d : register(b0);
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState pointSampler : register(s1);

// ---------- nearest ----------
[shader("compute")]
[numthreads(8, 8, 1)]
void nearest(uint3 gid : SV_DispatchThreadID)
{
    if (gid.x >= d.dw || gid.y >= d.dh) return;
    float2 uv = (float2(gid.xy) + 0.5) / float2(d.dw, d.dh);
    dst[gid.xy] = src.SampleLevel(pointSampler, uv, 0);
}

// ---------- weight functions ----------
// t is in filter space (source distance / downscale ratio): tent +-1,
// Mitchell +-2, lanczos3 +-3, scaled back up by the ratio in the passes.
float w_bilinear(float t) { return max(0.0, 1.0 - abs(t)); }

// Stabilized nearest: narrow Gaussian (sigma 0.35 output pixels).
float w_nearestaa(float t) { return exp(t * t * -4.0816); }

float w_bicubic(float t)  // Mitchell-Netravali B=C=1/3
{
    float x = abs(t);
    const float B = 1.0 / 3.0, C = 1.0 / 3.0;
    float x2 = x * x, x3 = x2 * x;
    if (x < 1.0)
        return ((12.0 - 9.0 * B - 6.0 * C) * x3 + (-18.0 + 12.0 * B + 6.0 * C) * x2 + (6.0 - 2.0 * B)) * (1.0 / 6.0);
    if (x < 2.0)
        return ((-B - 6.0 * C) * x3 + (6.0 * B + 30.0 * C) * x2 + (-12.0 * B - 48.0 * C) * x + (8.0 * B + 24.0 * C)) * (1.0 / 6.0);
    return 0.0;
}

float sinc_(float x)
{
    if (abs(x) < 1e-6) return 1.0;
    float xp = x * 3.14159265358979323846;
    return sin(xp) / xp;
}
float w_lanczos(float t)
{
    if (abs(t) >= 3.0) return 0.0;
    return sinc_(t) * sinc_(t / 3.0);
}

float weight(int kind, float t)
{
    if (kind == 0) return w_nearestaa(t);
    if (kind == 1) return w_bilinear(t);
    if (kind == 2) return w_bicubic(t);
    return w_lanczos(t);
}

// ---------- separable ratio-scaled passes ----------
void pass_h(uint2 gid, int kind, float radius)
{
    if (gid.x >= d.dw || gid.y >= d.sh) return;
    float scale = max(1.0, float(d.sw) / float(d.dw));
    float center = (float(gid.x) + 0.5) * float(d.sw) / float(d.dw) - 0.5;
    float support = radius * scale;
    int lo = int(ceil(center - support));
    int hi = int(floor(center + support));
    float4 acc = 0;
    float wsum = 0.0;
    for (int x = lo; x <= hi; x++) {
        float w = weight(kind, (float(x) - center) / scale);
        int cx = clamp(x, 0, int(d.sw) - 1);
        acc += src.Load(int3(cx, gid.y, 0)) * w;
        wsum += w;
    }
    dst[gid] = acc / max(wsum, 1e-6);
}

void pass_v(uint2 gid, int kind, float radius)
{
    if (gid.x >= d.dw || gid.y >= d.dh) return;
    float scale = max(1.0, float(d.sh) / float(d.dh));
    float center = (float(gid.y) + 0.5) * float(d.sh) / float(d.dh) - 0.5;
    float support = radius * scale;
    int lo = int(ceil(center - support));
    int hi = int(floor(center + support));
    float4 acc = 0;
    float wsum = 0.0;
    for (int y = lo; y <= hi; y++) {
        float w = weight(kind, (float(y) - center) / scale);
        int cy = clamp(y, 0, int(d.sh) - 1);
        acc += src.Load(int3(gid.x, cy, 0)) * w;
        wsum += w;
    }
    dst[gid] = acc / max(wsum, 1e-6);
}

[shader("compute")][numthreads(8, 8, 1)] void nearestaa_h(uint3 g : SV_DispatchThreadID) { pass_h(g.xy, 0, 1.0); }
[shader("compute")][numthreads(8, 8, 1)] void nearestaa_v(uint3 g : SV_DispatchThreadID) { pass_v(g.xy, 0, 1.0); }
[shader("compute")][numthreads(8, 8, 1)] void bilinear_h(uint3 g : SV_DispatchThreadID) { pass_h(g.xy, 1, 1.0); }
[shader("compute")][numthreads(8, 8, 1)] void bilinear_v(uint3 g : SV_DispatchThreadID) { pass_v(g.xy, 1, 1.0); }
[shader("compute")][numthreads(8, 8, 1)] void bicubic_h(uint3 g : SV_DispatchThreadID) { pass_h(g.xy, 2, 2.0); }
[shader("compute")][numthreads(8, 8, 1)] void bicubic_v(uint3 g : SV_DispatchThreadID) { pass_v(g.xy, 2, 2.0); }
[shader("compute")][numthreads(8, 8, 1)] void lanczos_h(uint3 g : SV_DispatchThreadID) { pass_h(g.xy, 3, 3.0); }
[shader("compute")][numthreads(8, 8, 1)] void lanczos_v(uint3 g : SV_DispatchThreadID) { pass_v(g.xy, 3, 3.0); }

// ---------- area (box average) ----------
[shader("compute")]
[numthreads(8, 8, 1)]
void area(uint3 gid : SV_DispatchThreadID)
{
    if (gid.x >= d.dw || gid.y >= d.dh) return;
    float sx0 = float(gid.x) * float(d.sw) / float(d.dw);
    float sx1 = float(gid.x + 1) * float(d.sw) / float(d.dw);
    float sy0 = float(gid.y) * float(d.sh) / float(d.dh);
    float sy1 = float(gid.y + 1) * float(d.sh) / float(d.dh);
    int x0 = int(floor(sx0)), x1 = int(ceil(sx1));
    int y0 = int(floor(sy0)), y1 = int(ceil(sy1));
    float4 acc = 0;
    float wsum = 0.0;
    for (int y = y0; y < y1; y++) {
        float fy = clamp(min(float(y) + 1.0, sy1) - max(float(y), sy0), 0.0, 1.0);
        int cy = clamp(y, 0, int(d.sh) - 1);
        for (int x = x0; x < x1; x++) {
            float fx = clamp(min(float(x) + 1.0, sx1) - max(float(x), sx0), 0.0, 1.0);
            int cx = clamp(x, 0, int(d.sw) - 1);
            float w = fx * fy;
            acc += src.Load(int3(cx, cy, 0)) * w;
            wsum += w;
        }
    }
    dst[gid.xy] = acc / max(wsum, 1e-6);
}
