// STUB — NOT Apple's FxPlug SDK. Declares only what fxplug/src uses, so CI can syntax-check
// the Objective-C++ (`fxplug/tests/stubs/syntax_check.sh`) on runners without the SDK, which
// can't be downloaded anonymously. Never linked, never shipped; real builds use
// /Library/Developer/Frameworks/FxPlug.framework.
//
// Every declaration below was copied from Apple's FxPlug 4 reference
// (https://developer.apple.com/documentation/professional_video_applications/, the
// Objective-C declarations of each symbol's page, checked 2026-11-11). Names, selectors and
// types follow the docs; NUMERIC VALUES of enums and the strings behind the property-key
// macros are placeholders (the docs don't give them), so nothing here may be used to decide
// behaviour. When the real headers disagree, they win: fix the stub.
//
// Known doc ambiguity: the docs render addPushButtonWithName:...selector: as `(SEL *)`;
// Apple's sample code passes @selector(...), so the stub declares SEL (a build with the real
// SDK settles it).
#pragma once

#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

// ---- PluginManager: PROAPIAccessing --------------------------------------------------
@protocol PROAPIAccessing <NSObject>
- (nullable id)apiForProtocol:(Protocol *)apiProtocol;
@end

// ---- basic types -------------------------------------------------------------------------
typedef struct FxRect {
    SInt32 left;
    SInt32 bottom;
    SInt32 right;
    SInt32 top;
} FxRect;

typedef NSUInteger FxQuality;
enum { kFxQuality_LOW = 0, kFxQuality_MEDIUM = 1, kFxQuality_HIGH = 2 };  // values: placeholders

typedef NSUInteger FxImageOrigin;
enum { kFxImageOrigin_BOTTOM_LEFT = 0, kFxImageOrigin_TOP_LEFT = 1 };  // values: placeholders

typedef NSUInteger FxImageColorInfo;
enum { kFxImageColorInfo_RGB_LINEAR = 0, kFxImageColorInfo_RGB_GAMMA_VIDEO = 1 };  // values: placeholders

typedef NSUInteger FxPixelTransformSupport;
enum { kFxPixelTransform_Scale = 0, kFxPixelTransform_ScaleTranslate = 1, kFxPixelTransform_Full = 2 };  // placeholders

typedef enum FxImageTileRequestSource {
    kFxImageTileRequestSourceNone = 0,
    kFxImageTileRequestSourceEffectClip = 1,
    kFxImageTileRequestSourceParameter = 2,
    kFxImageTileRequestSourceOutput = 3,
} FxImageTileRequestSource;  // values: placeholders

typedef UInt32 FxParameterFlags;
enum {  // values: placeholders
    kFxParameterFlag_DEFAULT = 0,
    kFxParameterFlag_NOT_ANIMATABLE = 1 << 1,
    kFxParameterFlag_HIDDEN = 1 << 2,
    kFxParameterFlag_DISABLED = 1 << 3,
    kFxParameterFlag_COLLAPSED = 1 << 4,
};

typedef NSInteger FxError;
enum {  // values: placeholders
    kFxError_Success = 0,
    kFxError_InvalidParameter = 100002,
    kFxError_APIUnavailable = 100004,
    kFxError_ThirdPartyDeveloperStart = 1000000,
};
extern NSString *FxPlugErrorDomain;

// Property keys (#define in the SDK; the string values here are placeholders).
#define kFxPropertyKey_MayRemapTime @"STUB_MayRemapTime"
#define kFxPropertyKey_NeedsFullBuffer @"STUB_NeedsFullBuffer"
#define kFxPropertyKey_VariesWhenParamsAreStatic @"STUB_VariesWhenParamsAreStatic"
#define kFxPropertyKey_ChangesOutputSize @"STUB_ChangesOutputSize"
#define kFxPropertyKey_DesiredProcessingColorInfo @"STUB_DesiredProcessingColorInfo"
#define kFxPropertyKey_PixelTransformSupport @"STUB_PixelTransformSupport"

// ---- images ----------------------------------------------------------------------------------
@interface FxImageTile : NSObject
@property (nonatomic, readonly) FxRect imagePixelBounds;
@property (nonatomic, readonly) FxRect tilePixelBounds;
@property (nonatomic, readonly, nullable) IOSurface *ioSurface;
@property (nonatomic, readonly) uint64_t deviceRegistryID;
@property (nonatomic, readonly) FxImageOrigin imageOrigin;
@property (nonatomic, copy, readonly, nullable) NSError *requestError;
@property (nonatomic, readonly) double scaleX;
@property (nonatomic, readonly) double scaleY;
@property (nonatomic, readonly) double pixelAspect;
- (nullable id<MTLTexture>)metalTextureForDevice:(id<MTLDevice>)metalDevice;
@end

@interface FxImageTileRequest : NSObject
- (id)initWithSource:(FxImageTileRequestSource)requestSource
                time:(CMTime)requestTime
      includeFilters:(BOOL)includeFilters
         parameterID:(UInt32)parameterID;
@end

// ---- the effect protocol ----------------------------------------------------------------
@protocol FxTileableEffect <NSObject>
@optional  // the stub doesn't check which methods are required
- (nullable id)initWithAPIManager:(id<PROAPIAccessing>)apiManager;
- (BOOL)properties:(NSDictionary *_Nonnull *_Nonnull)properties error:(NSError **)error;
- (BOOL)addParametersWithError:(NSError **)error;
- (BOOL)parameterChanged:(UInt32)paramID atTime:(CMTime)time error:(NSError **)error;
- (BOOL)pluginState:(NSData *_Nullable *_Nullable)pluginState
             atTime:(CMTime)renderTime
            quality:(FxQuality)qualityLevel
              error:(NSError **)error;
- (BOOL)scheduleInputs:(NSArray<FxImageTileRequest *> *_Nullable *_Nullable)inputImageRequests
       withPluginState:(NSData *)pluginState
                atTime:(CMTime)renderTime
                 error:(NSError **)error;
- (BOOL)destinationImageRect:(FxRect *)destinationImageRect
                sourceImages:(NSArray<FxImageTile *> *)sourceImages
            destinationImage:(FxImageTile *)destinationImage
                 pluginState:(NSData *)pluginState
                      atTime:(CMTime)renderTime
                       error:(NSError **)outError;
- (BOOL)sourceTileRect:(FxRect *)sourceTileRect
      sourceImageIndex:(NSUInteger)sourceImageIndex
          sourceImages:(NSArray<FxImageTile *> *)sourceImages
   destinationTileRect:(FxRect)destinationTileRect
      destinationImage:(FxImageTile *)destinationImage
           pluginState:(NSData *)pluginState
                atTime:(CMTime)renderTime
                 error:(NSError **)outError;
- (BOOL)renderDestinationImage:(FxImageTile *)destinationImage
                  sourceImages:(NSArray<FxImageTile *> *)sourceImages
                   pluginState:(NSData *)pluginState
                        atTime:(CMTime)renderTime
                         error:(NSError **)outError;
@end

// ---- host APIs --------------------------------------------------------------------------------
@protocol FxParameterCreationAPI_v5 <NSObject>
- (BOOL)addToggleButtonWithName:(NSString *)name parameterID:(UInt32)parameterID defaultValue:(BOOL)defaultValue
                 parameterFlags:(FxParameterFlags)flags;
- (BOOL)addIntSliderWithName:(NSString *)name parameterID:(UInt32)parameterID defaultValue:(int)defaultValue
                parameterMin:(int)min parameterMax:(int)max sliderMin:(int)sliderMin sliderMax:(int)sliderMax
                       delta:(int)sliderDelta parameterFlags:(FxParameterFlags)flags;
- (BOOL)addFloatSliderWithName:(NSString *)name parameterID:(UInt32)parameterID defaultValue:(double)defaultValue
                  parameterMin:(double)min parameterMax:(double)max sliderMin:(double)sliderMin
                     sliderMax:(double)sliderMax delta:(double)sliderDelta parameterFlags:(FxParameterFlags)flags;
- (BOOL)addPopupMenuWithName:(NSString *)name parameterID:(UInt32)parameterID defaultValue:(UInt32)defaultValue
                 menuEntries:(NSArray *)entries parameterFlags:(FxParameterFlags)flags;
- (BOOL)addPushButtonWithName:(NSString *)name parameterID:(UInt32)parameterID selector:(SEL)selector
               parameterFlags:(FxParameterFlags)flags;
- (BOOL)addColorParameterWithName:(NSString *)name parameterID:(UInt32)parameterID defaultRed:(double)red
                     defaultGreen:(double)green defaultBlue:(double)blue parameterFlags:(FxParameterFlags)flags;
- (BOOL)addStringParameterWithName:(NSString *)name parameterID:(UInt32)parameterID defaultValue:(NSString *)defaultValue
                    parameterFlags:(FxParameterFlags)flags;
- (BOOL)startParameterSubGroup:(NSString *)name parameterID:(UInt32)parameterID parameterFlags:(FxParameterFlags)flags;
- (BOOL)endParameterSubGroup;
@end

@protocol FxParameterRetrievalAPI_v6 <NSObject>
- (BOOL)getBoolValue:(BOOL *)value fromParameter:(UInt32)parameterID atTime:(CMTime)time;
- (BOOL)getIntValue:(int *)value fromParameter:(UInt32)parameterID atTime:(CMTime)time;
- (BOOL)getFloatValue:(double *)value fromParameter:(UInt32)parameterID atTime:(CMTime)time;
- (BOOL)getRedValue:(double *)red greenValue:(double *)green blueValue:(double *)blue
      fromParameter:(UInt32)parameterID atTime:(CMTime)time;
- (BOOL)getStringParameterValue:(NSString *_Nullable *_Nullable)string fromParameter:(UInt32)parameterID;
- (BOOL)getParameterFlags:(FxParameterFlags *)flags fromParameter:(UInt32)parameterID;
@end

@protocol FxParameterSettingAPI_v5 <NSObject>
- (BOOL)setBoolValue:(BOOL)value toParameter:(UInt32)parameterID atTime:(CMTime)time;
- (BOOL)setIntValue:(int)value toParameter:(UInt32)parameterID atTime:(CMTime)time;
- (BOOL)setFloatValue:(double)value toParameter:(UInt32)parameterID atTime:(CMTime)time;
- (BOOL)setRedValue:(double)red greenValue:(double)green blueValue:(double)blue
        toParameter:(UInt32)parameterID atTime:(CMTime)time;
- (BOOL)setStringParameterValue:(NSString *)string toParameter:(UInt32)parameterID;
- (BOOL)setParameterFlags:(FxParameterFlags)flags toParameter:(UInt32)parameterID;
@end

@protocol FxCustomParameterActionAPI_v4 <NSObject>
- (void)startAction:(id)sender;
- (void)endAction:(id)sender;
- (CMTime)currentTime;
@end

@protocol FxTimingAPI_v4 <NSObject>
- (void)timelineTime:(CMTime *)timelineTime fromInputTime:(CMTime)time;
- (void)inputTime:(CMTime *)inputTime fromTimelineTime:(CMTime)time;
- (void)startTimeForEffect:(CMTime *)startTime;
- (void)durationTimeForEffect:(CMTime *)duration;
- (void)frameDuration:(CMTime *)duration;
- (NSUInteger)timelineFpsNumeratorForEffect:(id<FxTileableEffect>)effect;
- (NSUInteger)timelineFpsDenominatorForEffect:(id<FxTileableEffect>)effect;
@end

@protocol FxKeyframeAPI_v3 <NSObject>
- (nullable NSError *)keyframeCount:(NSUInteger *)count forParameter:(NSUInteger)parameterID andChannel:(NSUInteger)channelIndex;
- (nullable NSError *)channelCount:(NSUInteger *)count forParameter:(NSUInteger)parameterID;
@end

// ---- the XPC service principal ------------------------------------------------------------------
@interface FxPrincipal : NSObject
+ (void)startServicePrincipal;
@end

NS_ASSUME_NONNULL_END
