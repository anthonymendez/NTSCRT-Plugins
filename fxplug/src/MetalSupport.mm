#import "MetalSupport.h"

static const char *const kCopyShader = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; };
vertex VOut ntscrt_fx_vs(uint vid [[vertex_id]]) {
    // full-screen triangle
    float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
    VOut o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o;
}
struct CopyArgs { int2 offset; int flip; int srcHeight; };
fragment float4 ntscrt_fx_fs(VOut in [[stage_in]],
                             texture2d<float, access::read> src [[texture(0)]],
                             constant CopyArgs &a [[buffer(0)]]) {
    int2 d = int2(in.pos.xy);
    int x = d.x + a.offset.x;
    int y = d.y + a.offset.y;
    if (a.flip != 0) y = a.srcHeight - 1 - y;
    x = clamp(x, 0, int(src.get_width()) - 1);
    y = clamp(y, 0, int(src.get_height()) - 1);
    return src.read(uint2(x, y));
}
)MSL";

struct CopyArgs { int ox, oy, flip, srcHeight; };

@implementation NTSCRTMetal

+ (nullable id<MTLDevice>)deviceForRegistryID:(uint64_t)registryID {
    static NSMutableDictionary<NSNumber *, id<MTLDevice>> *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        cache = [NSMutableDictionary dictionary];
        for (id<MTLDevice> d in MTLCopyAllDevices()) cache[@(d.registryID)] = d;
    });
    @synchronized(cache) {
        id<MTLDevice> d = cache[@(registryID)];
        if (!d) {  // hot-plugged eGPU: refresh once
            for (id<MTLDevice> x in MTLCopyAllDevices()) cache[@(x.registryID)] = x;
            d = cache[@(registryID)];
        }
        return d ?: MTLCreateSystemDefaultDevice();
    }
}

+ (nullable id<MTLTexture>)scratchTextureForDevice:(id<MTLDevice>)device
                                            format:(MTLPixelFormat)format
                                             width:(NSUInteger)width
                                            height:(NSUInteger)height {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    return [device newTextureWithDescriptor:d];
}

+ (nullable id<MTLRenderPipelineState>)pipelineForDevice:(id<MTLDevice>)device format:(MTLPixelFormat)format {
    static NSMutableDictionary<NSString *, id<MTLRenderPipelineState>> *cache;
    static NSMutableDictionary<NSNumber *, id<MTLLibrary>> *libs;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ cache = [NSMutableDictionary dictionary]; libs = [NSMutableDictionary dictionary]; });
    NSString *key = [NSString stringWithFormat:@"%llu/%lu", device.registryID, (unsigned long)format];
    @synchronized(cache) {
        id<MTLRenderPipelineState> p = cache[key];
        if (p) return p;
        id<MTLLibrary> lib = libs[@(device.registryID)];
        NSError *err = nil;
        if (!lib) {
            lib = [device newLibraryWithSource:@(kCopyShader) options:nil error:&err];
            if (!lib) { NSLog(@"[NTSCRT] copy shader compile failed: %@", err); return nil; }
            libs[@(device.registryID)] = lib;
        }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"ntscrt_fx_vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"ntscrt_fx_fs"];
        rd.colorAttachments[0].pixelFormat = format;
        p = [device newRenderPipelineStateWithDescriptor:rd error:&err];
        if (!p) { NSLog(@"[NTSCRT] copy pipeline failed: %@", err); return nil; }
        cache[key] = p;
        return p;
    }
}

+ (BOOL)encodeCopyFrom:(id<MTLTexture>)src
                    to:(id<MTLTexture>)dst
               offsetX:(int)offsetX
               offsetY:(int)offsetY
                 flipY:(BOOL)flipY
         commandBuffer:(id<MTLCommandBuffer>)commandBuffer {
    id<MTLRenderPipelineState> pso = [self pipelineForDevice:dst.device format:dst.pixelFormat];
    if (!pso) return NO;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:rp];
    if (!enc) return NO;
    CopyArgs a{offsetX, offsetY, flipY ? 1 : 0, int(src.height)};
    [enc setRenderPipelineState:pso];
    [enc setFragmentTexture:src atIndex:0];
    [enc setFragmentBytes:&a length:sizeof a atIndex:0];
    [enc setViewport:(MTLViewport){0, 0, double(dst.width), double(dst.height), 0, 1}];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    return YES;
}

@end
