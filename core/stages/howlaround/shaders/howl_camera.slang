// howl_camera.slang — kernel "howl_camera/camera". Port of upstream
// Howlaround.swift howl_camera (inline MSL). CPU reference: howl_cpu.cpp camera_ref.
//
// What the camera sees at each pixel: the room (the scene), or — where the
// ray hits the TV's screen — the TV's picture, as bright and as tinted as the
// camera sees it; then the mixer's luma key, the exposure, the counter.

struct CameraParams {
    float4 axisX, axisY, normal, center, balance, counterRect;
    float4 params;   // aspect, zoom, brightness, contrast
    float4 params2;  // hue cos, hue sin, exposure, TV in view
    float4 key;      // key level, invert
};

ConstantBuffer<CameraParams> u : register(b0);
Texture2D<float4> scene : register(t0);
Texture2D<float4> tv : register(t1);
Texture2D<float4> counter : register(t2);   // premultiplied; bound to tv when unused
RWTexture2D<float4> outTex : register(u0);
SamplerState linearSampler : register(s0);

// Clamp to the outer texel centres: identical to clamp-to-edge addressing
// whatever address mode the HAL's linear sampler uses.
float4 sampleClamped(Texture2D<float4> t, float2 uv) {
    uint w, h;
    t.GetDimensions(w, h);
    float2 lo = 0.5 / float2(w, h);
    return t.SampleLevel(linearSampler, clamp(uv, lo, 1.0 - lo), 0);
}

[numthreads(16, 16, 1)]
void camera(uint3 id : SV_DispatchThreadID) {
    uint W, H;
    outTex.GetDimensions(W, H);
    uint2 gid = id.xy;
    if (gid.x >= W || gid.y >= H) return;
    float3 src = scene.Load(int3(gid, 0)).rgb;
    float3 c = src;
    float aspect = u.params.x, zoom = u.params.y;
    float2 uv = (float2(gid) + 0.5) / float2(W, H);
    if (u.params2.w > 0.5) {
        float3 d = float3((uv.x - 0.5) * aspect, uv.y - 0.5, 1.0);
        float3 n = u.normal.xyz;
        float denom = dot(d, n);
        if (abs(denom) > 1e-6) {
            float t = dot(u.center.xyz, n) / denom;
            if (t > 0) {
                float3 p = d * t - u.center.xyz;
                float sx = dot(p, u.axisX.xyz) / (zoom * aspect) + 0.5;
                float sy = dot(p, u.axisY.xyz) / zoom + 0.5;
                // Anti-aliased screen edge, about a pixel wide.
                float ex = min(sx, 1.0 - sx) * zoom * float(W);
                float ey = min(sy, 1.0 - sy) * zoom * float(H);
                float cover = saturate(min(ex, ey) + 0.5);
                if (cover > 0.0) {
                    float3 s = sampleClamped(tv, float2(sx, sy)).rgb;
                    s *= u.params.z;
                    s = (s - 0.5) * u.params.w + 0.5;
                    s *= u.balance.rgb;
                    float y = dot(s, float3(0.299, 0.587, 0.114));
                    float i = dot(s, float3(0.596, -0.274, -0.322));
                    float q = dot(s, float3(0.211, -0.523, 0.312));
                    float i2 = i * u.params2.x - q * u.params2.y;
                    float q2 = i * u.params2.y + q * u.params2.x;
                    s = float3(y + 0.956 * i2 + 0.621 * q2,
                               y - 0.272 * i2 - 0.647 * q2,
                               y - 1.106 * i2 + 1.703 * q2);
                    c = lerp(c, saturate(s), cover);
                }
            }
        }
    }
    // The mixer's luma key: your picture over the camera's wherever it's
    // brighter than the key level (darker, inverted), with a soft edge.
    if (u.key.x > 0.0) {
        float y = dot(src, float3(0.299, 0.587, 0.114));
        if (u.key.y > 0.5) y = 1.0 - y;
        float level = 1.0 - u.key.x;
        c = lerp(c, src, smoothstep(level - 0.06, level + 0.06, y));
    }
    c *= u.params2.z;
    float4 r = u.counterRect;
    if (r.z > r.x && uv.x >= r.x && uv.x <= r.z && uv.y >= r.y && uv.y <= r.w) {
        float4 t = sampleClamped(counter, (uv - r.xy) / (r.zw - r.xy));
        c = c * (1.0 - t.a) + t.rgb;
    }
    outTex[gid] = float4(saturate(c), 1.0);
}
