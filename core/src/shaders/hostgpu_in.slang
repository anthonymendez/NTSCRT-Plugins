// hostgpu_in/convert_in: first step of ntscrt_render_host_gpu (core/src/host_gpu.cpp).
// The host's image arrives in a texture of its own format and memory row order
// (OpenFX: the bottom row first), possibly smaller than the output frame and
// placed in it at (src_x, src_y) of the frame's memory order; frame pixels it
// doesn't cover are transparent black (what the CPU path pads with).
//
// Writes the frame, cut to the rect cx,cy,cw,ch (top-down: the look's crop),
// as the pipeline's 8-bit picture, the way to_bgra8 (pixel_convert.cpp) makes it:
//   Clip (mode 0):  straight colour: premultiplied sources divided by alpha when
//                   it's strictly between 0.5/255 and 1;
//   Spill (mode 1): the element over black: straight sources times alpha;
// clamped to 0..1 (NaN = 0), rounded half up to 8 bits, opaque.
struct InParams {
    uint frame_w, frame_h, src_w, src_h;
    int src_x, src_y, bottom_up, mode;
    uint cx, cy, cw, ch;
    uint premult, pad0, pad1, pad2;
};
ConstantBuffer<InParams> P : register(b0);
Texture2D<float4> host_src : register(t0);
RWTexture2D<float4> pic : register(u0);

float4 frame_px(uint fx, uint fy)
{
    int my = P.bottom_up != 0 ? int(P.frame_h) - 1 - int(fy) : int(fy);
    int sx = int(fx) - P.src_x, sy = my - P.src_y;
    if (sx < 0 || sy < 0 || sx >= int(P.src_w) || sy >= int(P.src_h)) return float4(0.0, 0.0, 0.0, 0.0);
    return host_src.Load(int3(sx, sy, 0));
}

float unit_alpha(float a) { return a > 0.0 ? (a < 1.0 ? a : 1.0) : 0.0; }

// uint8_t(double(v * 255.0f) + 0.5) for v in (0, 1), else 0 / 255 (NaN -> 0):
// floor(p) + (frac(p) >= 0.5) is that rounding exactly, without doubles.
float q8(float v)
{
    if (!(v > 0.0)) return 0.0;
    if (v >= 1.0) return 1.0;
    float p = v * 255.0;
    float f = floor(p);
    return (f + ((p - f) >= 0.5 ? 1.0 : 0.0)) / 255.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void convert_in(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.cw || id.y >= P.ch) return;
    float4 v = frame_px(P.cx + id.x, P.cy + id.y);
    float a = unit_alpha(v.a);
    float3 c = v.rgb;
    const float kTransparent = 0.5 / 255.0;
    if (P.mode == 0) {
        if (P.premult != 0 && a > kTransparent && a < 1.0) c = c / a;
    } else if (P.premult == 0) {
        c = c * a;
    }
    pic[id.xy] = float4(q8(c.r), q8(c.g), q8(c.b), 1.0);
}
