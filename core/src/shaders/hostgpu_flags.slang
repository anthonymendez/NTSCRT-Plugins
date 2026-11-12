// hostgpu_flags/row_flags: per frame row (top-down), 1 when some pixel of the
// host's source frame is transparent (alpha at or below 0.5/255). Clip mode
// then needs to_bgra8's hole filling, which runs on the CPU. Same frame
// mapping as hostgpu_in.slang (one thread per row; flags is frame_h x 1).
struct InParams {
    uint frame_w, frame_h, src_w, src_h;
    int src_x, src_y, bottom_up, mode;
    uint cx, cy, cw, ch;
    uint premult, pad0, pad1, pad2;
};
ConstantBuffer<InParams> P : register(b0);
Texture2D<float4> host_src : register(t0);
RWTexture2D<float> flags : register(u0);

[shader("compute")]
[numthreads(64, 1, 1)]
void row_flags(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.frame_h) return;
    int my = P.bottom_up != 0 ? int(P.frame_h) - 1 - int(id.x) : int(id.x);
    int sy = my - P.src_y;
    float f = 0.0;
    // Rows outside the source, or a source narrower than the frame: padding.
    if (sy < 0 || sy >= int(P.src_h) || P.src_x > 0 || P.src_x + int(P.src_w) < int(P.frame_w)) {
        f = 1.0;
    } else {
        for (uint x = 0; x < P.frame_w; ++x) {
            float a = host_src.Load(int3(int(x) - P.src_x, sy, 0)).a;
            if (!(a > 0.5 / 255.0)) {
                f = 1.0;
                break;
            }
        }
    }
    flags[uint2(id.x, 0)] = f;
}
