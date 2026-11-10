// crt_place/place: draw `src` at (offset, src size) into dst, opaque black
// elsewhere (the letterbox bars).
struct PlaceParams { int2 offset; uint2 dst_size; };
ConstantBuffer<PlaceParams> P : register(b0);
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);

[shader("compute")]
[numthreads(8, 8, 1)]
void place(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.dst_size.x || id.y >= P.dst_size.y) return;
    uint w, h;
    src.GetDimensions(w, h);
    int2 p = int2(id.xy) - P.offset;
    bool inside = p.x >= 0 && p.y >= 0 && p.x < int(w) && p.y < int(h);
    dst[id.xy] = inside ? src.Load(int3(p, 0)) : float4(0, 0, 0, 1);
}
