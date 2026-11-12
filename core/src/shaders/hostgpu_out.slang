// hostgpu_out/merge_out: last step of ntscrt_render_host_gpu. The CRT result
// (top-down, frame size; opaque BGRA8 or RGBA16F) + the source's alpha -> the
// host's destination texture in its format and memory row order. Maths of
// merge_alpha (pixel_convert.h) and facade/alpha_merge:
//   mode 0 Clip:  alpha = source alpha (premult: RGB times alpha)
//   mode 1 Spill: alpha = max(source alpha, brightest channel) (straight: RGB / alpha)
//   use_alpha 0:  source alpha not aligned with the result (crop): 1 for Clip, 0 for Spill
// The source sits in the frame as in hostgpu_in.slang; frame pixels outside it
// have alpha 0.
struct OutParams {
    uint dst_w, dst_h, src_w, src_h;
    int src_x, src_y, bottom_up, mode;
    uint use_alpha, premult, pad0, pad1;
};
ConstantBuffer<OutParams> Q : register(b0);
Texture2D<float4> rgb : register(t0);
Texture2D<float4> host_src : register(t1);
RWTexture2D<float4> host_dst : register(u0);

[shader("compute")]
[numthreads(8, 8, 1)]
void merge_out(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Q.dst_w || id.y >= Q.dst_h) return;
    uint fy = Q.bottom_up != 0 ? Q.dst_h - 1 - id.y : id.y;
    float3 c = saturate(rgb.Load(int3(id.x, fy, 0)).rgb);
    float a;
    if (Q.use_alpha != 0) {
        int sx = int(id.x) - Q.src_x, sy = int(id.y) - Q.src_y;
        float s = (sx < 0 || sy < 0 || sx >= int(Q.src_w) || sy >= int(Q.src_h))
                      ? 0.0
                      : host_src.Load(int3(sx, sy, 0)).a;
        a = s > 0.0 ? (s < 1.0 ? s : 1.0) : 0.0;
    } else {
        a = Q.mode != 0 ? 0.0 : 1.0;
    }
    if (Q.mode != 0) {
        a = max(a, max(c.r, max(c.g, c.b)));
        if (Q.premult == 0) c = a > 0.0 ? min(c / a, float3(1.0, 1.0, 1.0)) : float3(0.0, 0.0, 0.0);
    } else if (Q.premult != 0) {
        c *= a;
    }
    host_dst[id.xy] = float4(c, a);
}
