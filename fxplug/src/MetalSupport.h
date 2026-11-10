// MetalSupport.h — device lookup, scratch textures and a copy/flip pass for
// the FxPlug renderer (host textures come from IOSurfaces on the host's GPU).
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

@interface NTSCRTMetal : NSObject
/// The MTLDevice with this registryID (FxImageTile.deviceRegistryID), or the default device.
+ (nullable id<MTLDevice>)deviceForRegistryID:(uint64_t)registryID;

/// Private-storage texture usable as ntscrt input/output (read, write, render target).
+ (nullable id<MTLTexture>)scratchTextureForDevice:(id<MTLDevice>)device
                                            format:(MTLPixelFormat)format
                                             width:(NSUInteger)width
                                            height:(NSUInteger)height;

/// Encodes dst(x, y) = src(x + offsetX, flipY ? src.height-1-(y+offsetY) : y+offsetY) as a
/// render pass (only needs MTLTextureUsageRenderTarget on dst, which host outputs have).
+ (BOOL)encodeCopyFrom:(id<MTLTexture>)src
                    to:(id<MTLTexture>)dst
               offsetX:(int)offsetX
               offsetY:(int)offsetY
                 flipY:(BOOL)flipY
         commandBuffer:(id<MTLCommandBuffer>)commandBuffer;
@end

NS_ASSUME_NONNULL_END
