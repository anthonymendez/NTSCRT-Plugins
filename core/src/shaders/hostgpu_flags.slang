// hostgpu_flags/row_flags: 1 where some pixel of the host's source frame is
// transparent (alpha at or below 0.5/255; frame pixels the source doesn't cover
// count as transparent), per run of 64 pixels of a frame row (top-down):
// flags is ceil(frame_w / 64) x frame_h. Clip mode then needs to_bgra8's hole
// filling, which runs on the CPU. Same frame mapping as hostgpu_in.slang.
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
[numthreads(8, 8, 1)]
void row_flags(uint3 id : SV_DispatchThreadID)
{
    const uint run = 64;
    if (id.x * run >= P.frame_w || id.y >= P.frame_h) return;
    int my = P.bottom_up != 0 ? int(P.frame_h) - 1 - int(id.y) : int(id.y);
    int sy = my - P.src_y;
    float f = 0.0;
    uint x1 = min(P.frame_w, id.x * run + run);
    for (uint x = id.x * run; x < x1; ++x) {
        int sx = int(x) - P.src_x;
        if (sy < 0 || sy >= int(P.src_h) || sx < 0 || sx >= int(P.src_w) ||
            !(host_src.Load(int3(sx, sy, 0)).a > 0.5 / 255.0)) {
            f = 1.0;
            break;
        }
    }
    flags[id.xy] = f;
}
