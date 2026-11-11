// NTSCRTPlugIn — FxPlug 4 FxTileableEffect over the ntscrt C ABI.
//
// Flow per frame (FxPlug 4):
//   pluginState:atTime:   read every param at renderTime (and at earlier frames when a
//                         temporal stage is on and something is keyframed, cached per
//                         parameter generation), build look/feedback JSON -> RenderState blob
//   scheduleInputs:       request the effect source at renderTime and the earlier frames the
//                         stages can read (ntscrt_source_frames_needed, capped by History Frames)
//   renderDestinationImage: render with ntscrt_render_metal on the host's MTLDevice
//                         (ntscrt_render_cpu through the IOSurfaces as fallback); earlier
//                         source frames reach the core through fetch_source.
// Params are created at runtime from ntscrt_param_schema_json() via ntscrt_fx::Model, the FxPlug
// side of the host param model the OpenFX plug-in shares (hostparams/ntscrt_host/params.h).
#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <FxPlug/FxPlugSDK.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <ntscrt/ntscrt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "param_model.h"
#import "MetalSupport.h"

using namespace ntscrt_fx;

// Past looks are read from the host for at most this many earlier frames per render
// (each one is a full sweep over every parameter through XPC); older frames use
// look_at == NULL, i.e. "same as the current frame".
static const int kMaxPastLooks = 16;
static const int kMaxHistory = 64;

// ---------------------------------------------------------------------------
// Process-wide shared state
// ---------------------------------------------------------------------------
namespace {

// ---- diagnostic log ------------------------------------------------------------
// <Library>/Logs/NTSCRT/ntscrt-fxplug.log, where <Library> is the renderer's sandbox
// container (~/Library/Containers/<renderer bundle id>/Data/Library). Same content as the
// Resolve plug-in's ntscrt-ofx.log: load, failures, the first renders, and every frame while
// "Debug Logging" is on. Load lines and failures also go to the system log ([NTSCRT], Console.app).
const FileLog &Log() {
    static const FileLog *log = [] {
        NSURL *lib = [[NSFileManager defaultManager] URLsForDirectory:NSLibraryDirectory inDomains:NSUserDomainMask].firstObject;
        NSURL *file = [[lib URLByAppendingPathComponent:@"Logs/NTSCRT" isDirectory:YES] URLByAppendingPathComponent:@"ntscrt-fxplug.log"];
        const char *fs = file ? file.fileSystemRepresentation : nullptr;
        return new FileLog(fs ? std::string(fs) : std::string());
    }();
    return *log;
}
void LogLine(const std::string &msg, bool also_nslog = true) {
    Log().write(msg);
    if (also_nslog) NSLog(@"[NTSCRT] %s", msg.c_str());
}

struct Shared {
    Model model;
    std::vector<host::Preset> menu;   // look Preset menu (Model::look_menu: bundled looks, then Digital formats)
    std::vector<host::Preset> loops;  // bundled Screen Loop presets (Model::loop_menu)
    std::string resource_dir;
};

std::string ReadFile(NSString *path) {
    if (!path) return {};
    std::ifstream f(path.fileSystemRepresentation, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const Shared &GetShared() {
    static Shared *s = [] {
        auto *sh = new Shared;
        NSBundle *b = [NSBundle bundleForClass:NSClassFromString(@"NTSCRTPlugIn")] ?: NSBundle.mainBundle;
        sh->resource_dir = b.resourcePath.fileSystemRepresentation ?: "";
        json ids = json::parse(ReadFile([b pathForResource:@"param_ids" ofType:@"json"]), nullptr, false);
        if (ids.is_discarded()) ids = json::object();
        json schema = json::parse(ntscrt_param_schema_json(), nullptr, false);
        if (schema.is_discarded()) schema = json::object();
        std::vector<std::string> warnings;
        sh->model = Model::from_schema(schema, ids, &warnings);
        {
            NSString *ver = [b objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
            NSString *proc = NSProcessInfo.processInfo.processName ?: @"?";
            NSString *os = NSProcessInfo.processInfo.operatingSystemVersionString ?: @"?";
            LogLine(std::string("NTSCRT ") + ver.UTF8String + " (FxPlug) loaded; resources " + sh->resource_dir +
                    "; process " + proc.UTF8String + "; macOS " + os.UTF8String + "; log " + Log().path());
        }
        for (const auto &w : warnings) LogLine("schema: " + w);

        // Bundled presets need an instance; it never renders. Same menu order as the OpenFX plug-in.
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_METAL, nullptr, nullptr, sh->resource_dir.c_str()};
        ntscrt_instance *meta = nullptr;
        if (ntscrt_create(&ci, &meta) == NTSCRT_OK && meta) {
            const json arr = json::parse(ntscrt_bundled_presets_json(meta), nullptr, false);
            sh->menu = sh->model.look_menu(arr.is_array() ? arr : json::array());
            sh->loops = sh->model.loop_menu(arr.is_array() ? arr : json::array());
            ntscrt_destroy(meta);
        }
        return sh;
    }();
    return *s;
}

// Render log bookkeeping (process-wide: one renderer process serves all the plug-in's instances).
std::atomic<long> gRenders{0};
std::atomic<size_t> gLastDebugLookHash{0};  // debug logging: the full look only when it changes

// Render calls whose status still wrote a complete (possibly degraded) frame: the CRT shader
// skipped (ERR_MISSING_RESOURCE / ERR_GPU) or an earlier source frame missing. ntscrt.h.
inline bool FrameWritten(ntscrt_status s) {
    return s == NTSCRT_OK || s == NTSCRT_ERR_GPU || s == NTSCRT_ERR_MISSING_RESOURCE || s == NTSCRT_ERR_SOURCE_UNAVAILABLE;
}

// One ntscrt instance renders one frame at a time; FxPlug renders on several threads.
struct Slot {
    ntscrt_instance *inst = nullptr;
    __strong id<MTLDevice> device;
    __strong id<MTLCommandQueue> queue;
    uint32_t pending_flush = 0;                     // caches to drop before the next render (InstanceState::m)
    std::unordered_map<int64_t, size_t> look_hash;  // frame -> hash of looks seen when rendered
    ~Slot() {
        if (inst) ntscrt_destroy(inst);
    }
};

// The whole frame a tiled render produced, so the frame's other tiles copy from it instead of
// rendering the frame again (FxPlug may split the output into tiles despite NeedsFullBuffer).
struct WholeFrame {
    bool valid = false;
    int64_t frame = 0;
    size_t look = 0;
    uint64_t device = 0;
    int width = 0, height = 0;
    ntscrt_pixel_format format = NTSCRT_RGBA8;
    std::vector<uint8_t> cpu;  // CPU path: top row first, width * bpp per row
    __strong id<MTLTexture> tex;  // Metal path: top row first
    bool matches(int64_t f, size_t l, uint64_t d, int w, int h, ntscrt_pixel_format fmt) const {
        return valid && frame == f && look == l && device == d && width == w && height == h && format == fmt;
    }
};

// State of one effect instance (one NTSCRTPlugIn object): its own ntscrt instances (render
// history and caches belong to one clip), its parameter generation and look cache.
struct InstanceState {
    std::mutex m;  // slots, free, last_warning
    std::vector<std::unique_ptr<Slot>> slots;
    std::vector<Slot *> free;
    std::string last_warning;  // degraded-frame message logged
    std::atomic<uint64_t> generation{1};  // bumped by every look-changing parameter change

    // pluginState: looks of other frames, valid for one generation and keyframe layout.
    std::mutex look_m;
    uint64_t look_gen = 0, keys_sig = 0, keys_sig_gen = 0;
    int keys_checks = 0;
    std::map<std::pair<int64_t, int32_t>, std::string> looks;  // CMTime (value, timescale) -> look JSON

    std::mutex tile_m;  // held while a tiled render produces / copies a whole frame
    WholeFrame whole;

    Slot *acquire(id<MTLDevice> device) {
        {
            std::lock_guard<std::mutex> l(m);
            for (size_t i = 0; i < free.size(); ++i) {
                if (free[i]->device.registryID == device.registryID) {
                    Slot *s = free[i];
                    free.erase(free.begin() + ptrdiff_t(i));
                    return s;
                }
            }
        }
        auto s = std::make_unique<Slot>();
        s->device = device;
        s->queue = [device newCommandQueue];
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_METAL, (__bridge void *)device,
                              (__bridge void *)s->queue, GetShared().resource_dir.c_str()};
        if (!s->queue || ntscrt_create(&ci, &s->inst) != NTSCRT_OK || !s->inst) return nullptr;
        std::lock_guard<std::mutex> l(m);
        slots.push_back(std::move(s));
        return slots.back().get();
    }
    void release(Slot *s) {
        std::lock_guard<std::mutex> l(m);
        free.push_back(s);
    }
    // Every slot drops `mask` before its next render (a slot may be rendering right now).
    void flush(uint32_t mask) {
        if (!mask) return;
        {
            std::lock_guard<std::mutex> l(m);
            for (auto &s : slots) s->pending_flush |= mask;
        }
        std::lock_guard<std::mutex> t(tile_m);
        whole.valid = false;
    }
    uint32_t take_pending(Slot *s) {
        std::lock_guard<std::mutex> l(m);
        const uint32_t p = s->pending_flush;
        s->pending_flush = 0;
        return p;
    }
};

// ---- IOSurface <-> ntscrt_image --------------------------------------------
// C-style casts: these SDK option types may be enums, which C++ won't build from ints implicitly.
inline IOSurfaceLockOptions LockOpts(bool read_only) { return (IOSurfaceLockOptions)(read_only ? kIOSurfaceLockReadOnly : 0u); }
inline FxParameterFlags PF(uint32_t v) { return (FxParameterFlags)v; }

bool FormatForSurface(IOSurfaceRef s, ntscrt_pixel_format *out) {
    switch (IOSurfaceGetPixelFormat(s)) {
    case kCVPixelFormatType_32BGRA: *out = NTSCRT_BGRA8; return true;
    case kCVPixelFormatType_32RGBA: *out = NTSCRT_RGBA8; return true;
    case kCVPixelFormatType_64RGBAHalf: *out = NTSCRT_RGBA16F; return true;
    case kCVPixelFormatType_128RGBAFloat: *out = NTSCRT_RGBA32F; return true;
    default: return false;
    }
}

// Locked view of an IOSurface as an ntscrt_image (top row first; bottom-up surfaces get a
// pointer to the last row and negative row_bytes, as the ABI specifies).
struct SurfaceView {
    IOSurfaceRef surface = nullptr;
    bool read_only = true;
    ntscrt_image img{};
    bool lock(IOSurfaceRef s, bool ro, bool bottom_up) {
        ntscrt_pixel_format f;
        if (!s || !FormatForSurface(s, &f)) return false;
        if (IOSurfaceLock(s, LockOpts(ro), nullptr) != kIOReturnSuccess) return false;
        surface = (IOSurfaceRef)CFRetain(s);
        read_only = ro;
        const auto w = int32_t(IOSurfaceGetWidth(s)), h = int32_t(IOSurfaceGetHeight(s));
        const auto rb = ptrdiff_t(IOSurfaceGetBytesPerRow(s));
        auto *base = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(s));
        img = {w, h, bottom_up ? -rb : rb, f, bottom_up ? base + rb * (h - 1) : base};
        return true;
    }
    ~SurfaceView() {
        if (surface) {
            IOSurfaceUnlock(surface, LockOpts(read_only), nullptr);
            CFRelease(surface);
        }
    }
};

// Whether host images are stored bottom row first. FxPlug reports the image origin per
// tile; NTSCRT_FXPLUG_FLIP=0/1 overrides (unverified on real hosts - see README).
bool BottomUp(FxImageTile *tile) {
    static int env = [] {
        const char *e = getenv("NTSCRT_FXPLUG_FLIP");
        return e ? atoi(e) : -1;
    }();
    if (env >= 0) return env != 0;
    return tile.imageOrigin == kFxImageOrigin_BOTTOM_LEFT;
}

// ---- render callbacks --------------------------------------------------------
struct RenderContext {
    NSArray<FxImageTile *> *sources;
    const RenderState *state;
    std::map<const ntscrt_image *, std::unique_ptr<SurfaceView>> live;
};

const ntscrt_image *FetchSource(void *user, int64_t frame_index) {
    auto *c = static_cast<RenderContext *>(user);
    const int64_t k = c->state->frame - frame_index;  // sourceImages[k] is frame - k
    if (k < 0 || k >= int64_t(c->sources.count) || k > c->state->history) return nullptr;
    FxImageTile *t = c->sources[NSUInteger(k)];
    if (t.requestError || !t.ioSurface) return nullptr;
    auto v = std::make_unique<SurfaceView>();
    if (!v->lock((__bridge IOSurfaceRef)t.ioSurface, true, BottomUp(t))) return nullptr;
    const ntscrt_image *key = &v->img;
    c->live[key] = std::move(v);
    return key;
}

void ReleaseSource(void *user, const ntscrt_image *img) {
    static_cast<RenderContext *>(user)->live.erase(img);
}

const char *LookAt(void *user, int64_t frame_index) {
    auto *c = static_cast<RenderContext *>(user);
    auto it = c->state->past_looks.find(frame_index);
    return it == c->state->past_looks.end() ? nullptr : it->second.c_str();
}

NSError *MakeError(NSInteger code, NSString *msg) {
    LogLine(std::string("error: ") + (msg.UTF8String ?: "?"));
    return [NSError errorWithDomain:FxPlugErrorDomain code:code userInfo:@{NSLocalizedDescriptionKey : msg}];
}

}  // namespace

// ---------------------------------------------------------------------------
@interface NTSCRTPlugIn : NSObject <FxTileableEffect>
@end

@implementation NTSCRTPlugIn {
    id<PROAPIAccessing> _apiManager;
    BOOL _applying;  // suppress re-entrant parameterChanged while we set values
    // This effect instance's renderers, caches and generation. Apple doesn't state that one
    // plug-in object serves exactly one effect instance, but addParameters / properties are
    // documented per instance; two objects never share render history this way.
    std::shared_ptr<InstanceState> _state;
}

- (nullable instancetype)initWithAPIManager:(id<PROAPIAccessing>)newApiManager {
    self = [super init];
    if (self) {
        _apiManager = newApiManager;
        _state = std::make_shared<InstanceState>();
        (void)GetShared();
    }
    return self;
}

- (BOOL)properties:(NSDictionary *_Nonnull *_Nonnull)properties error:(NSError **)error {
    (void)error;
    *properties = @{
        // Temporal stages (Digital video, Screen Loop) read source frames before the output
        // time (scheduleInputs); Apple: "may try to sample its input image(s) at any time
        // other than the output time" (default YES).
        kFxPropertyKey_MayRemapTime : @YES,
        // Only scaling is handled (render scale = FxImageTile.scaleX); Apple's documented
        // default is ScaleTranslate, so say it.
        kFxPropertyKey_PixelTransformSupport : @(kFxPixelTransform_Scale),
        // whole-frame processing: NTSC/CRT stages are not tileable
        kFxPropertyKey_NeedsFullBuffer : @YES,
        // noise / glitches animate with time even when params are static
        kFxPropertyKey_VariesWhenParamsAreStatic : @YES,
        kFxPropertyKey_ChangesOutputSize : @NO,
        // gamma-encoded video RGB, like the upstream app's 8-bit sRGB pipeline
        kFxPropertyKey_DesiredProcessingColorInfo : @(kFxImageColorInfo_RGB_GAMMA_VIDEO),
    };
    return YES;
}

// ---- parameters -----------------------------------------------------------
- (BOOL)addParametersWithError:(NSError **)error {
    id<FxParameterCreationAPI_v5> api = [_apiManager apiForProtocol:@protocol(FxParameterCreationAPI_v5)];
    if (!api) {
        if (error) *error = MakeError(kFxError_APIUnavailable, @"FxParameterCreationAPI_v5 unavailable");
        return NO;
    }
    const Shared &sh = GetShared();
    const Model &m = sh.model;
    BOOL ok = YES;
    const FxParameterFlags fixed = PF(uint32_t(kFxParameterFlag_DEFAULT) | uint32_t(kFxParameterFlag_NOT_ANIMATABLE));

    ok &= [api startParameterSubGroup:@"NTSCRT" parameterID:kTopGroup parameterFlags:PF(kFxParameterFlag_DEFAULT)];
    ok &= [api startParameterSubGroup:@"Preset & timing" parameterID:kExtrasGroup parameterFlags:PF(kFxParameterFlag_DEFAULT)];
    NSMutableArray *looks = [NSMutableArray arrayWithObject:@"(Custom)"];
    for (const auto &p : sh.menu) [looks addObject:@(p.name.c_str())];
    ok &= [api addPopupMenuWithName:@"Preset" parameterID:kPresetMenu defaultValue:0 menuEntries:looks parameterFlags:fixed];
    ok &= [api addPushButtonWithName:@"Load Preset…" parameterID:kLoadPreset selector:@selector(loadPresetPressed) parameterFlags:PF(kFxParameterFlag_DEFAULT)];
    ok &= [api addPushButtonWithName:@"Save Preset…" parameterID:kSavePreset selector:@selector(savePresetPressed) parameterFlags:PF(kFxParameterFlag_DEFAULT)];
    NSMutableArray *loops = [NSMutableArray arrayWithObjects:@"Off", @"(Custom)", nil];
    for (const auto &p : sh.loops) [loops addObject:@(p.name.c_str())];
    ok &= [api addPopupMenuWithName:@"Screen Loop" parameterID:kScreenLoopMenu defaultValue:0 menuEntries:loops parameterFlags:fixed];
    ok &= [api addIntSliderWithName:@"Seed" parameterID:kSeed defaultValue:0 parameterMin:host::kSeedMin
                       parameterMax:host::kSeedMax sliderMin:host::kSeedMin sliderMax:host::kSeedDisplayMax delta:1
                     parameterFlags:fixed];
    ok &= [api addIntSliderWithName:@"History Frames" parameterID:kHistoryFrames defaultValue:8 parameterMin:0
                       parameterMax:kMaxHistory sliderMin:0 sliderMax:32 delta:1 parameterFlags:fixed];
    // Diagnostics: never part of the look or of presets.
    ok &= [api addToggleButtonWithName:@"Debug Logging" parameterID:kDebugLogging defaultValue:NO parameterFlags:fixed];
    ok &= [api addPushButtonWithName:@"Open Logs Folder" parameterID:kOpenLogs selector:@selector(openLogsPressed)
                      parameterFlags:PF(kFxParameterFlag_DEFAULT)];
    ok &= [api endParameterSubGroup];

    for (const UiItem &it : m.layout) {
        if (it.type == UiItem::StartGroup) {
            const Group &g = m.groups[it.index];
            ok &= [api startParameterSubGroup:@(g.label.c_str()) parameterID:g.host_id
                               parameterFlags:PF(uint32_t(kFxParameterFlag_DEFAULT) | uint32_t(kFxParameterFlag_COLLAPSED))];
            continue;
        }
        if (it.type == UiItem::EndGroup) { ok &= [api endParameterSubGroup]; continue; }
        const Param &p = m.params[it.index];
        NSString *name = @(p.label.c_str());
        uint32_t bits = uint32_t(kFxParameterFlag_DEFAULT);
        if (!p.animatable) bits |= uint32_t(kFxParameterFlag_NOT_ANIMATABLE);
        const FxParameterFlags flags = PF(bits);
        const json d = m.default_host_value(p);
        switch (p.type) {
        case Kind::Bool:
            ok &= [api addToggleButtonWithName:name parameterID:p.host_id defaultValue:d.get<bool>() parameterFlags:flags];
            break;
        case Kind::Int:
            ok &= [api addIntSliderWithName:name parameterID:p.host_id defaultValue:int(d.get<int64_t>())
                               parameterMin:int(p.ui_min) parameterMax:int(p.ui_max) sliderMin:int(p.slider_min)
                                  sliderMax:int(p.slider_max) delta:std::max(1, int(p.ui_step)) parameterFlags:flags];
            break;
        case Kind::Float:
            ok &= [api addFloatSliderWithName:name parameterID:p.host_id defaultValue:d.get<double>()
                                 parameterMin:p.ui_min parameterMax:p.ui_max sliderMin:p.slider_min
                                    sliderMax:p.slider_max delta:p.ui_step parameterFlags:flags];
            break;
        case Kind::Choice: {
            NSMutableArray *entries = [NSMutableArray array];
            for (const auto &o : p.options) [entries addObject:@(o.label.c_str())];
            ok &= [api addPopupMenuWithName:name parameterID:p.host_id defaultValue:UInt32(d.get<int64_t>())
                                menuEntries:entries parameterFlags:flags];  // keyable when the schema says so
            break;
        }
        case Kind::Color:
            ok &= [api addColorParameterWithName:name parameterID:p.host_id defaultRed:d[0].get<double>()
                                    defaultGreen:d[1].get<double>() defaultBlue:d[2].get<double>() parameterFlags:flags];
            break;
        }
    }
    ok &= [api endParameterSubGroup];  // NTSCRT

    // Unused since 0.5.0 (the look is built from the params alone, as in the OpenFX plug-in);
    // kept so the parameter IDs stay as they were.
    const FxParameterFlags hidden = PF(uint32_t(kFxParameterFlag_HIDDEN) | uint32_t(kFxParameterFlag_NOT_ANIMATABLE));
    ok &= [api addStringParameterWithName:@"Base Look" parameterID:kBaseLook defaultValue:@"{}" parameterFlags:hidden];
    ok &= [api addStringParameterWithName:@"Base Screen Loop" parameterID:kBaseFeedback defaultValue:@"{}" parameterFlags:hidden];
    if (!ok && error) *error = MakeError(kFxError_InvalidParameter, @"Unable to create NTSCRT parameters");
    return ok;
}

// ---- value access -----------------------------------------------------------
static json GetHostValue(id<FxParameterRetrievalAPI_v6> api, const Model &m, const Param &p, CMTime t) {
    switch (p.type) {
    case Kind::Bool: {
        BOOL b = NO;
        if ([api getBoolValue:&b fromParameter:p.host_id atTime:t]) return bool(b);
        break;
    }
    case Kind::Int:
    case Kind::Choice: {
        int v = 0;
        if ([api getIntValue:&v fromParameter:p.host_id atTime:t]) return int64_t(v);
        break;
    }
    case Kind::Float: {
        double v = 0;
        if ([api getFloatValue:&v fromParameter:p.host_id atTime:t]) return v;
        break;
    }
    case Kind::Color: {
        double r = 0, g = 0, b = 0;
        if ([api getRedValue:&r greenValue:&g blueValue:&b fromParameter:p.host_id atTime:t]) return json::array({r, g, b});
        break;
    }
    }
    return m.default_host_value(p);
}

static BOOL SetHostValue(id<FxParameterSettingAPI_v5> api, const Param &p, const json &v, CMTime t) {
    switch (p.type) {
    case Kind::Bool: return [api setBoolValue:host::truthy(v) toParameter:p.host_id atTime:t];
    case Kind::Int:
    case Kind::Choice: return [api setIntValue:int(v.get<int64_t>()) toParameter:p.host_id atTime:t];
    case Kind::Float: return [api setFloatValue:v.get<double>() toParameter:p.host_id atTime:t];
    case Kind::Color:
        return [api setRedValue:v[0].get<double>() greenValue:v[1].get<double>() blueValue:v[2].get<double>()
                    toParameter:p.host_id atTime:t];
    }
    return NO;
}

static int GetInt(id<FxParameterRetrievalAPI_v6> api, UInt32 pid, CMTime t, int fallback) {
    int v = fallback;
    return [api getIntValue:&v fromParameter:pid atTime:t] ? v : fallback;
}

// The look and feedback at time t (the shared model; Screen Loop off = "enabled": false).
static host::Built BuildAt(id<FxParameterRetrievalAPI_v6> api, CMTime t) {
    const Model &m = GetShared().model;
    return m.build([&](const Param &p) { return GetHostValue(api, m, p, t); });
}

// Grey out / hide params per enabled_if / visible_if at time t.
- (void)updateParameterStatesAtTime:(CMTime)t {
    id<FxParameterRetrievalAPI_v6> get = [_apiManager apiForProtocol:@protocol(FxParameterRetrievalAPI_v6)];
    id<FxParameterSettingAPI_v5> set = [_apiManager apiForProtocol:@protocol(FxParameterSettingAPI_v5)];
    if (!get || !set) return;
    const Model &m = GetShared().model;
    auto value = [&](const Param &p) { return GetHostValue(get, m, p, t); };
    for (const Param &p : m.params) {
        if (!p.enabled_if && !p.visible_if) continue;
        FxParameterFlags flags = PF(0);
        if (![get getParameterFlags:&flags fromParameter:p.host_id]) continue;
        const uint32_t have = uint32_t(flags);
        uint32_t want = have & ~(uint32_t(kFxParameterFlag_DISABLED) | uint32_t(kFxParameterFlag_HIDDEN));
        if (!m.is_enabled(p, value)) want |= uint32_t(kFxParameterFlag_DISABLED);
        if (!m.is_visible(p, value)) want |= uint32_t(kFxParameterFlag_HIDDEN);
        if (want != have) [set setParameterFlags:PF(want) toParameter:p.host_id];
    }
}

// The effect's timing: host frame rate, one frame, its first frame and length in frames
// (0 = unknown), from FxTimingAPI_v4. Keyframe times use startTimeForEffect's base, which is
// assumed to be the one set*Value:atTime: takes (unverified, see README).
- (void)effectFps:(double *)fps frame:(CMTime *)frameDur start:(CMTime *)start frames:(int64_t *)frames {
    *fps = 30.0;
    *frameDur = CMTimeMake(1, 30);
    *start = kCMTimeZero;
    *frames = 0;
    id<FxTimingAPI_v4> timing = [_apiManager apiForProtocol:@protocol(FxTimingAPI_v4)];
    if (!timing) return;
    const NSUInteger num = [timing timelineFpsNumeratorForEffect:self], den = [timing timelineFpsDenominatorForEffect:self];
    if (num > 0 && den > 0) {
        *fps = double(num) / double(den);
        *frameDur = CMTimeMake(int64_t(den), int32_t(num));
    }
    [timing startTimeForEffect:start];
    CMTime dur = kCMTimeZero;
    [timing durationTimeForEffect:&dur];
    if (CMTIME_IS_NUMERIC(dur) && CMTimeGetSeconds(dur) > 0)
        *frames = int64_t(std::llround(CMTimeGetSeconds(dur) * *fps));
}

// Applies a preset (bundled or a loaded file) as the shared model plans it (Model::plan_preset,
// the same plan the OpenFX plug-in follows): a look preset replaces the look, so every look
// parameter presets set loses its keyframes; an animated one (upstream timeline keys) then gets
// host keyframes from the effect's first frame, one pass after another until the effect ends (at
// most kMaxKeyedFrames). A Screen Loop file (or a look file's "screen_loop") sets the Screen Loop
// parameters and turns Screen Loop on unless it says "enabled": false. FxKeyframeAPI_v3's
// addKeyframe carries no value: a keyframe is added at the time, then the value is set there.
- (void)applyPreset:(const json &)preset atTime:(CMTime)t {
    id<FxParameterSettingAPI_v5> set = [_apiManager apiForProtocol:@protocol(FxParameterSettingAPI_v5)];
    if (!set) return;
    id<FxKeyframeAPI_v3> keys = [_apiManager apiForProtocol:@protocol(FxKeyframeAPI_v3)];
    const Model &m = GetShared().model;
    auto channels = [&](const Param &p) -> NSUInteger {
        NSUInteger c = p.type == Kind::Color ? 3 : 1;
        if (keys) {
            NSUInteger n = 0;
            if ([keys channelCount:&n forParameter:p.host_id] == nil && n > 0) c = n;
        }
        return c;
    };
    double fps = 30;
    CMTime fd = CMTimeMake(1, 30), start = kCMTimeZero;
    int64_t frames = 0;
    [self effectFps:&fps frame:&fd start:&start frames:&frames];
    const HostPlan plan = m.plan_preset(preset, fps, frames);
    const BOOL was = _applying;
    _applying = YES;
    size_t keyed = 0;
    for (const auto &[id, entries] : plan.look) {
        const Param *p = m.find_host(id);
        if (!p || entries.empty()) continue;
        if (keys && p->animatable)
            for (NSUInteger c = 0, n = channels(*p); c < n; ++c) [keys removeAllKeyframesForParameter:p->host_id andChannel:c];
        if (entries.size() == 1 || !keys) {  // holds still (or no keyframe API): a value at the first frame's
            SetHostValue(set, *p, entries.front().second, t);
            continue;
        }
        const bool stepped = p->type == Kind::Bool || p->type == Kind::Choice;
        const NSUInteger nch = channels(*p);
        for (const auto &[n, v] : entries) {
            const CMTime tk = CMTimeAdd(start, CMTimeMultiply(fd, int32_t(n)));
            FxKeyframe k{};
            k.version = kFxKeyframe_CurrentVersion;
            k.time = tk;
            k.segmentStyle = stepped ? kFxKeyframeSegmentStyle_Constant : kFxKeyframeSegmentStyle_Linear;
            for (NSUInteger c = 0; c < nch; ++c) [keys addKeyframe:&k toParameter:p->host_id andChannel:c];
            SetHostValue(set, *p, v, tk);
            ++keyed;
        }
    }
    for (const auto &[id, v] : plan.feedback)
        if (const Param *p = m.find_host(id)) SetHostValue(set, *p, v, t);
    _applying = was;
    if (plan.animated()) {
        std::ostringstream o;
        o << "animated preset: " << plan.pass << " frames per pass, " << plan.frames << " frames keyed, " << keyed
          << " keyframes";
        if (!keys) o << " (FxKeyframeAPI_v3 unavailable: first frame's values set instead)";
        LogLine(o.str(), !keys);
    }
    _state->generation.fetch_add(1);
    _state->flush(plan.replaces_look ? (NTSCRT_CACHE_ALL & ~NTSCRT_CACHE_SOURCE) : NTSCRT_CACHE_SCREENLOOP);
}

// Writes host values (a Digital video format, a choice option's "applies") at time t.
- (void)applyHostValues:(const std::map<uint32_t, json> &)vals atTime:(CMTime)t {
    if (vals.empty()) return;
    id<FxParameterSettingAPI_v5> set = [_apiManager apiForProtocol:@protocol(FxParameterSettingAPI_v5)];
    if (!set) return;
    const Model &m = GetShared().model;
    const BOOL was = _applying;
    _applying = YES;
    for (const auto &[id, v] : vals)
        if (const Param *p = m.find_host(id)) SetHostValue(set, *p, v, t);
    _applying = was;
    _state->generation.fetch_add(1);
    _state->flush(NTSCRT_CACHE_ALL & ~NTSCRT_CACHE_SOURCE);
}

// A user edit of schema parameter `pid`: a choice option with "applies" writes its values
// (Horizontal resolution sets the Width); a hand edit of a value a chosen option set demotes
// that choice to the option that sets nothing ("Custom"). Model::edit_effects, as in OpenFX.
- (void)applyChoiceEffectsOf:(UInt32)pid atTime:(CMTime)t {
    id<FxParameterRetrievalAPI_v6> get = [_apiManager apiForProtocol:@protocol(FxParameterRetrievalAPI_v6)];
    if (!get) return;
    const Model &m = GetShared().model;
    const Param *edited = m.find_host(pid);
    if (!edited) return;
    [self applyHostValues:m.edit_effects(*edited, [&](const Param &p) { return GetHostValue(get, m, p, t); }) atTime:t];
}

// Does host parameter `pid` have keyframes (FxKeyframeAPI_v3, channel 0)? Unknown = yes.
- (BOOL)parameterIsKeyed:(UInt32)pid {
    id<FxKeyframeAPI_v3> keys = [_apiManager apiForProtocol:@protocol(FxKeyframeAPI_v3)];
    if (!keys) return YES;
    NSUInteger n = 0;
    if ([keys keyframeCount:&n forParameter:pid andChannel:0] != nil) return YES;
    return n > 0;
}

// Hash of the keyframe counts of every animatable parameter (0 = nothing keyframed anywhere,
// so every frame has the current look). ~0 when the keyframe API is unavailable / errors.
- (uint64_t)keyframeSignature {
    id<FxKeyframeAPI_v3> keys = [_apiManager apiForProtocol:@protocol(FxKeyframeAPI_v3)];
    if (!keys) return ~uint64_t(0);
    uint64_t sig = 0;
    for (const Param &p : GetShared().model.params) {
        if (!p.animatable) continue;  // created NOT_ANIMATABLE
        NSUInteger n = 0;
        if ([keys keyframeCount:&n forParameter:p.host_id andChannel:0] != nil) return ~uint64_t(0);
        if (n) sig = (sig ^ (uint64_t(p.host_id) << 32 | uint32_t(n))) * 0x100000001B3ull + 1;
    }
    return sig;
}

- (BOOL)parameterChanged:(UInt32)paramID atTime:(CMTime)time error:(NSError **)error {
    (void)error;
    if (paramID == kDebugLogging || paramID == kOpenLogs) {  // diagnostics: the look is unchanged
        if (paramID == kDebugLogging) {
            id<FxParameterRetrievalAPI_v6> g = [_apiManager apiForProtocol:@protocol(FxParameterRetrievalAPI_v6)];
            BOOL on = NO;
            if (g) [g getBoolValue:&on fromParameter:kDebugLogging atTime:time];
            LogLine(std::string("debug logging ") + (on ? "on" : "off"), false);
            gLastDebugLookHash.store(0);
        }
        return YES;
    }
    // Drop only the caches this parameter can affect (the OpenFX plug-in does the same).
    const uint32_t mask = GetShared().model.cache_mask(paramID, [self parameterIsKeyed:paramID]);
    if (mask) {
        _state->generation.fetch_add(1);
        _state->flush(mask);
    }
    if (_applying) return YES;
    id<FxParameterRetrievalAPI_v6> get = [_apiManager apiForProtocol:@protocol(FxParameterRetrievalAPI_v6)];
    const Shared &sh = GetShared();
    if (get && paramID == kPresetMenu) {
        int idx = GetInt(get, kPresetMenu, time, 0);
        if (idx >= 1 && size_t(idx) <= sh.menu.size()) {
            const host::Preset &mp = sh.menu[size_t(idx - 1)];
            if (mp.format) [self applyHostValues:sh.model.applies_host_values(mp.data) atTime:time];  // the rest of the look stays
            else [self applyPreset:mp.data atTime:time];
        }
    } else if (get && paramID == kScreenLoopMenu) {
        // Off / (Custom) switch Screen Loop (the "Screen Loop" checkbox, schema screenloop.enabled);
        // a preset sets the Screen Loop parameters and turns it on.
        int idx = GetInt(get, kScreenLoopMenu, time, 0);
        if (idx == kScreenLoopOff || idx == kScreenLoopCustom)
            [self applyHostValues:sh.model.screen_loop_switch(idx == kScreenLoopCustom) atTime:time];
        else if (idx >= kFirstScreenLoopPreset && size_t(idx - kFirstScreenLoopPreset) < sh.loops.size())
            [self applyPreset:sh.loops[size_t(idx - kFirstScreenLoopPreset)].data atTime:time];
    } else if (sh.model.find_host(paramID)) {
        [self applyChoiceEffectsOf:paramID atTime:time];
    }
    [self updateParameterStatesAtTime:time];
    return YES;
}

// ---- Load / Save preset buttons -----------------------------------------------
- (void)loadPresetPressed {
    id<FxCustomParameterActionAPI_v4> action = [_apiManager apiForProtocol:@protocol(FxCustomParameterActionAPI_v4)];
    __weak NTSCRTPlugIn *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.allowedContentTypes = @[ UTTypeJSON ];
        panel.message = @"Choose an NTSCRT look or Screen Loop preset (.json)";
        if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
        NSData *data = [NSData dataWithContentsOfURL:panel.URL];
        json j = data ? json::parse(std::string((const char *)data.bytes, data.length), nullptr, false) : json();
        NTSCRTPlugIn *strongSelf = weakSelf;
        if (!strongSelf || !j.is_object()) {
            NSAlert *a = [NSAlert new];
            a.messageText = @"Not a valid NTSCRT preset";
            [a runModal];
            return;
        }
        const bool fb = host::is_feedback_preset(j);
        const bool with_loop = fb || (j.contains("screen_loop") && host::is_feedback_preset(j["screen_loop"]));
        [action startAction:strongSelf];
        CMTime t = [action currentTime];
        [strongSelf applyPreset:j atTime:t];  // a look file's "screen_loop" included
        id<FxParameterSettingAPI_v5> set = [strongSelf->_apiManager apiForProtocol:@protocol(FxParameterSettingAPI_v5)];
        strongSelf->_applying = YES;
        if (!fb) [set setIntValue:0 toParameter:kPresetMenu atTime:t];  // "(Custom)"
        if (with_loop) {
            const json loop = fb ? j : j["screen_loop"];
            const bool on = !loop.contains("enabled") || host::truthy(loop["enabled"]);
            [set setIntValue:(on ? kScreenLoopCustom : kScreenLoopOff) toParameter:kScreenLoopMenu atTime:t];
        }
        strongSelf->_applying = NO;
        [strongSelf updateParameterStatesAtTime:t];
        [action endAction:strongSelf];
    });
}

- (void)savePresetPressed {
    id<FxCustomParameterActionAPI_v4> action = [_apiManager apiForProtocol:@protocol(FxCustomParameterActionAPI_v4)];
    id<FxParameterRetrievalAPI_v6> get = [_apiManager apiForProtocol:@protocol(FxParameterRetrievalAPI_v6)];
    if (!action || !get) return;
    // Host called us: the parameter APIs are valid right now.
    const host::Built b = BuildAt(get, [action currentTime]);
    // Upstream's look file with our "screen_loop" (Load applies it), and while the loop is on an
    // upstream Screen Loop file beside it: what the OpenFX plug-in saves.
    const std::string look = host::saved_preset(b.look, b.feedback).dump(2);
    const auto side = host::saved_screen_loop(b.feedback);
    const std::string loop = side ? side->dump(2) : std::string();
    dispatch_async(dispatch_get_main_queue(), ^{
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.allowedContentTypes = @[ UTTypeJSON ];
        panel.nameFieldStringValue = @"My NTSCRT look.json";
        if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
        [[NSData dataWithBytes:look.data() length:look.size()] writeToURL:panel.URL atomically:YES];
        if (!loop.empty()) {
            NSString *base = panel.URL.URLByDeletingPathExtension.lastPathComponent;
            NSURL *u = [[panel.URL URLByDeletingLastPathComponent]
                URLByAppendingPathComponent:[base stringByAppendingString:@" (Screen Loop).json"]];
            // Outside the user-selected file, so this may be refused by the sandbox; the look is saved regardless.
            [[NSData dataWithBytes:loop.data() length:loop.size()] writeToURL:u atomically:YES];
        }
    });
}

// ---- Open Logs Folder button ------------------------------------------------------
- (void)openLogsPressed {
    const std::string folder = Log().folder();
    if (folder.empty()) {
        LogLine("open logs folder: no Library folder");
        return;
    }
    NSURL *dir = [NSURL fileURLWithFileSystemRepresentation:folder.c_str() isDirectory:YES relativeToURL:nil];
    [[NSFileManager defaultManager] createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil];
    LogLine("open logs folder: " + folder, false);
    dispatch_async(dispatch_get_main_queue(), ^{
        NSURL *file = [dir URLByAppendingPathComponent:@"ntscrt-fxplug.log"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:file.path]) {
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ file ]];
        } else if (![[NSWorkspace sharedWorkspace] openURL:dir]) {
            // The sandbox may refuse: show where the log is instead.
            NSAlert *a = [NSAlert new];
            a.messageText = @"NTSCRT log folder";
            a.informativeText = dir.path ?: @"";
            [a runModal];
        }
    });
}

// ---- timing / state -----------------------------------------------------------
- (BOOL)pluginState:(NSData **)pluginState atTime:(CMTime)renderTime quality:(FxQuality)qualityLevel error:(NSError **)error {
    (void)qualityLevel;
    id<FxTimingAPI_v4> timing = [_apiManager apiForProtocol:@protocol(FxTimingAPI_v4)];
    id<FxParameterRetrievalAPI_v6> get = [_apiManager apiForProtocol:@protocol(FxParameterRetrievalAPI_v6)];
    if (!timing || !get) {
        if (error) *error = MakeError(kFxError_APIUnavailable, @"FxTimingAPI_v4 / FxParameterRetrievalAPI_v6 unavailable");
        return NO;
    }
    RenderState st;
    const NSInteger num = [timing timelineFpsNumeratorForEffect:self];
    const NSInteger den = [timing timelineFpsDenominatorForEffect:self];
    CMTime frameDur = (num > 0 && den > 0) ? CMTimeMake(den, int32_t(num)) : CMTimeMake(1, 30);
    st.fps = (num > 0 && den > 0) ? double(num) / double(den) : 30.0;
    st.frame_duration_value = frameDur.value;
    st.frame_duration_timescale = frameDur.timescale;

    CMTime tl = kCMTimeZero, start = kCMTimeZero, startTL = kCMTimeZero;
    [timing timelineTime:&tl fromInputTime:renderTime];
    [timing startTimeForEffect:&start];
    [timing timelineTime:&startTL fromInputTime:start];
    CMTime offset = CMTimeSubtract(tl, startTL);
    st.frame = std::max<int64_t>(0, frame_from_offset(offset.value, offset.timescale, frameDur.value, frameDur.timescale));

    InstanceState &S = *_state;
    const uint64_t gen = S.generation.load();
    const host::Built now = BuildAt(get, renderTime);
    st.look = now.look.dump();
    st.feedback = now.feedback.is_null() ? std::string() : now.feedback.dump();
    st.seed = host::render_seed(GetInt(get, kSeed, renderTime, 0));
    BOOL debug = NO;
    st.debug = [get getBoolValue:&debug fromParameter:kDebugLogging atTime:renderTime] && debug;

    // Earlier source frames: only what the stages that read them (Digital video back to its
    // keyframe, Screen Loop's window) can fetch, capped by "History Frames". Glitch and the
    // CRT read only looks, so they schedule nothing extra.
    {
        ntscrt_render_args ra{};
        ra.frame_index = st.frame;
        ra.fps = st.fps;
        ra.look_json = st.look.c_str();
        ra.feedback_json = st.feedback.empty() ? nullptr : st.feedback.c_str();
        ra.seed = st.seed;
        int64_t first = st.frame;
        if (ntscrt_source_frames_needed(&ra, &first) != NTSCRT_OK) first = st.frame;
        const int want = std::clamp(GetInt(get, kHistoryFrames, renderTime, 8), 0, kMaxHistory);
        st.history = int(std::clamp<int64_t>(st.frame - first, 0, want));
    }

    // Looks of earlier frames (Glitch replays its receiver, Digital its stream): only when some
    // parameter is keyframed (otherwise every frame has this look and look_at answers NULL),
    // and each built once per parameter generation (playback reuses the previous frames').
    if (host::temporal_stages_on(now.look, now.feedback) && st.frame > 0) {
        uint64_t sig = 0;
        bool recheck = false;
        {
            std::lock_guard<std::mutex> l(S.look_m);
            // Keyframe layout: re-read after any parameter change and every 24 frames (an edit
            // the host doesn't report through parameterChanged).
            recheck = S.keys_sig_gen != gen || ++S.keys_checks >= 24;
            if (recheck) S.keys_checks = 0;
            sig = S.keys_sig;
        }
        if (recheck) {
            sig = [self keyframeSignature];  // a sweep over XPC: outside the lock
            std::lock_guard<std::mutex> l(S.look_m);
            S.keys_sig = sig;
            S.keys_sig_gen = gen;
        }
        if (sig != 0) {
            auto key = [](CMTime t) { return std::make_pair(int64_t(t.value), int32_t(t.timescale)); };
            {
                std::lock_guard<std::mutex> l(S.look_m);
                if (S.look_gen != gen || S.keys_sig != sig || S.looks.size() > 1024) {
                    S.looks.clear();
                    S.look_gen = gen;
                }
                S.looks[key(renderTime)] = st.look;
            }
            for (int k = 1; k <= std::min<int64_t>(kMaxPastLooks, st.frame); ++k) {
                const CMTime tk = CMTimeSubtract(renderTime, CMTimeMultiply(frameDur, k));
                std::string lk;
                {
                    std::lock_guard<std::mutex> l(S.look_m);
                    auto it = S.looks.find(key(tk));
                    if (it != S.looks.end()) lk = it->second;
                }
                if (lk.empty()) {
                    lk = BuildAt(get, tk).look.dump();
                    std::lock_guard<std::mutex> l(S.look_m);
                    if (S.look_gen == gen) S.looks[key(tk)] = lk;
                }
                if (lk != st.look) st.past_looks[st.frame - k] = std::move(lk);
            }
        }
    }
    if (st.debug) {
        std::ostringstream o;
        o << "debug state frame=" << st.frame << " renderTime=" << renderTime.value << '/' << renderTime.timescale
          << " timeline=" << tl.value << '/' << tl.timescale << " effectStart=" << start.value << '/' << start.timescale
          << " history=" << st.history << " past_looks=" << st.past_looks.size();
        LogLine(o.str(), false);
    }
    const std::string bytes = st.serialize();
    *pluginState = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    return YES;
}

static std::optional<RenderState> ParseState(NSData *d) {
    if (!d) return std::nullopt;
    return RenderState::parse(std::string((const char *)d.bytes, d.length));
}

- (BOOL)scheduleInputs:(NSArray<FxImageTileRequest *> *_Nullable *_Nullable)inputImageRequests
       withPluginState:(NSData *)pluginState
                atTime:(CMTime)renderTime
                 error:(NSError **)error {
    (void)error;
    auto st = ParseState(pluginState);
    const int history = st ? st->history : 0;
    CMTime fd = st ? CMTimeMake(st->frame_duration_value, st->frame_duration_timescale) : CMTimeMake(1, 30);
    NSMutableArray<FxImageTileRequest *> *reqs = [NSMutableArray arrayWithCapacity:NSUInteger(history + 1)];
    // sourceImages[k] = effect source at renderTime - k frames (k = 0 is the current frame)
    for (int k = 0; k <= history; ++k) {
        CMTime t = k == 0 ? renderTime : CMTimeSubtract(renderTime, CMTimeMultiply(fd, k));
        [reqs addObject:[[FxImageTileRequest alloc] initWithSource:kFxImageTileRequestSourceEffectClip
                                                              time:t
                                                    includeFilters:YES
                                                       parameterID:0]];
    }
    if (inputImageRequests) *inputImageRequests = reqs;
    return YES;
}

- (BOOL)destinationImageRect:(FxRect *)destinationImageRect
                sourceImages:(NSArray<FxImageTile *> *)sourceImages
            destinationImage:(FxImageTile *)destinationImage
                 pluginState:(NSData *)pluginState
                      atTime:(CMTime)renderTime
                       error:(NSError **)outError {
    (void)destinationImage; (void)pluginState; (void)renderTime;
    if (sourceImages.count < 1) {
        if (outError) *outError = MakeError(kFxError_InvalidParameter, @"no source image");
        return NO;
    }
    *destinationImageRect = sourceImages[0].imagePixelBounds;  // output size = input size
    return YES;
}

- (BOOL)sourceTileRect:(FxRect *)sourceTileRect
      sourceImageIndex:(NSUInteger)sourceImageIndex
          sourceImages:(NSArray<FxImageTile *> *)sourceImages
   destinationTileRect:(FxRect)destinationTileRect
      destinationImage:(FxImageTile *)destinationImage
           pluginState:(NSData *)pluginState
                atTime:(CMTime)renderTime
                 error:(NSError **)outError {
    (void)destinationTileRect; (void)destinationImage; (void)pluginState; (void)renderTime; (void)outError;
    // Every stage needs the whole frame, whatever tile is being rendered.
    *sourceTileRect = sourceImageIndex < sourceImages.count ? sourceImages[sourceImageIndex].imagePixelBounds
                                                            : destinationTileRect;
    return YES;
}

// ---- render -----------------------------------------------------------------
static bool SameRect(const FxRect &a, const FxRect &b) {
    return a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom;
}

// Tile offset inside the full image, in the texture's own row order.
static void TileOffset(FxImageTile *dst, bool bottom_up, int *ox, int *oy) {
    const FxRect t = dst.tilePixelBounds, im = dst.imagePixelBounds;
    *ox = t.left - im.left;
    *oy = bottom_up ? (t.bottom - im.bottom) : (im.top - t.top);
}

- (BOOL)renderDestinationImage:(FxImageTile *)destinationImage
                  sourceImages:(NSArray<FxImageTile *> *)sourceImages
                   pluginState:(NSData *)pluginState
                        atTime:(CMTime)renderTime
                         error:(NSError **)outError {
    (void)renderTime;
    auto st = ParseState(pluginState);
    if (!st) { if (outError) *outError = MakeError(kFxError_InvalidParameter, @"invalid plug-in state"); return NO; }
    if (sourceImages.count == 0 || !sourceImages[0].ioSurface || !destinationImage.ioSurface) {
        if (outError) *outError = MakeError(kFxError_InvalidParameter, @"missing source/destination surface");
        return NO;
    }
    FxImageTile *src = sourceImages[0];
    InstanceState &S = *_state;
    id<MTLDevice> device = [NTSCRTMetal deviceForRegistryID:destinationImage.deviceRegistryID];
    Slot *slot = device ? S.acquire(device) : nullptr;
    if (!slot) { if (outError) *outError = MakeError(kFxError_ThirdPartyDeveloperStart + 1, @"cannot create ntscrt instance"); return NO; }

    // Caches this instance's parameter changes invalidated (only those: Model::cache_mask), plus all
    // temporal ones when a frame rendered before now has a different look (a keyframe edit the
    // host didn't report).
    size_t h = std::hash<std::string>{}(st->look) ^ (std::hash<std::string>{}(st->feedback) << 1);
    for (const auto &[f, s] : st->past_looks) h ^= std::hash<std::string>{}(s) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2) + size_t(f);
    uint32_t flush = S.take_pending(slot);
    auto seen = slot->look_hash.find(st->frame);
    if (seen != slot->look_hash.end() && seen->second != h) {
        flush |= NTSCRT_CACHE_ALL & ~NTSCRT_CACHE_SOURCE;
        slot->look_hash.clear();
    }
    if (flush) ntscrt_flush_caches_mask(slot->inst, flush);
    if (slot->look_hash.size() > 4096) slot->look_hash.clear();
    slot->look_hash[st->frame] = h;

    RenderContext ctx{sourceImages, &*st, {}};
    ntscrt_render_args ra{};
    ra.frame_index = st->frame;
    ra.fps = st->fps;
    ra.look_json = st->look.c_str();
    ra.feedback_json = st->feedback.empty() ? nullptr : st->feedback.c_str();
    // With kFxPixelTransform_Scale the pixel transform is a pure scale; scaleX is documented as
    // "the horizontal scale value" (its relation to pixelTransform isn't spelled out).
    ra.render_scale = destinationImage.scaleX > 0 ? destinationImage.scaleX : 1.0;
    ra.seed = st->seed;
    ra.fetch_source = FetchSource;
    ra.release_source = ReleaseSource;
    ra.look_at = LookAt;
    ra.source_premultiplied = 1;  // FxPlug / Motion images carry premultiplied alpha
    ra.user = &ctx;

    const auto t0 = std::chrono::steady_clock::now();
    bool used_cpu = false, reused = false;
    const bool src_up = BottomUp(src), dst_up = BottomUp(destinationImage);
    const bool full = SameRect(destinationImage.tilePixelBounds, destinationImage.imagePixelBounds);
    int ox = 0, oy = 0;
    TileOffset(destinationImage, dst_up, &ox, &oy);
    // A tile of a larger output: the frame's first tile renders the whole frame into S.whole,
    // the others (other threads wait here) copy from it.
    std::unique_lock<std::mutex> tile_lock(S.tile_m, std::defer_lock);
    if (!full) tile_lock.lock();

    ntscrt_status status = NTSCRT_ERR_INTERNAL;
    bool metal_done = false;  // the frame (maybe degraded) reached the destination texture
    NSString *why = nil;
    @autoreleasepool {
        id<MTLTexture> inTex = [src metalTextureForDevice:device];
        id<MTLTexture> outTex = [destinationImage metalTextureForDevice:device];
        id<MTLCommandBuffer> cb = [slot->queue commandBuffer];
        if (inTex && outTex && cb) {
            const ntscrt_pixel_format wf = NTSCRT_RGBA8;  // key only: texture formats are compared below
            if (!full && S.whole.tex && S.whole.matches(st->frame, h, device.registryID, int(inTex.width), int(inTex.height), wf) &&
                S.whole.tex.pixelFormat == outTex.pixelFormat) {
                [NTSCRTMetal encodeCopyFrom:S.whole.tex to:outTex offsetX:ox offsetY:oy flipY:dst_up commandBuffer:cb];
                status = NTSCRT_OK;
                reused = true;
            } else {
                id<MTLTexture> coreIn = inTex, coreOut = outTex;
                if (src_up) {  // core works top row first
                    coreIn = [NTSCRTMetal scratchTextureForDevice:device format:inTex.pixelFormat width:inTex.width height:inTex.height];
                    if (coreIn) {
                        // ntscrt_render_metal reads its source on its own command buffers, so the
                        // flipped copy must have executed before the call.
                        [NTSCRTMetal encodeCopyFrom:inTex to:coreIn offsetX:0 offsetY:0 flipY:YES commandBuffer:cb];
                        [cb commit];
                        [cb waitUntilCompleted];
                        cb = [slot->queue commandBuffer];
                    }
                }
                if (dst_up || !full || outTex.pixelFormat != inTex.pixelFormat) {
                    coreOut = [NTSCRTMetal scratchTextureForDevice:device format:outTex.pixelFormat width:inTex.width height:inTex.height];
                }
                if (coreIn && coreOut && cb) {
                    status = ntscrt_render_metal(slot->inst, &ra, (__bridge void *)coreIn, (__bridge void *)coreOut, (__bridge void *)cb);
                    if (FrameWritten(status) && coreOut != outTex) {
                        // dst row r <- core row (flip ? H-1-(oy+r) : oy+r)
                        [NTSCRTMetal encodeCopyFrom:coreOut to:outTex offsetX:ox offsetY:oy flipY:dst_up commandBuffer:cb];
                    }
                    if (FrameWritten(status) && !full) {
                        S.whole = WholeFrame{};
                        S.whole.valid = true;
                        S.whole.frame = st->frame;
                        S.whole.look = h;
                        S.whole.device = device.registryID;
                        S.whole.width = int(inTex.width);
                        S.whole.height = int(inTex.height);
                        S.whole.format = wf;
                        S.whole.tex = coreOut;
                    }
                }
            }
            if (cb) {
                [cb commit];
                [cb waitUntilCompleted];
            }
            if (cb.error) {
                why = cb.error.localizedDescription;
                status = NTSCRT_ERR_GPU;
                if (!full) S.whole.valid = false;  // under tile_m
            } else {
                metal_done = FrameWritten(status);
            }
        }
        if (!metal_done && !why) why = @(ntscrt_last_error(slot->inst));
    }

    if (!metal_done) {
        // CPU fallback straight through the IOSurfaces.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LogLine("Metal path failed (" + std::to_string(int(status)) + ": " + std::string(why ? why.UTF8String : "") +
                    "), using CPU (logged once)");
        used_cpu = true;
        @autoreleasepool {
            SurfaceView in, out;
            if (in.lock((__bridge IOSurfaceRef)src.ioSurface, true, src_up) &&
                out.lock((__bridge IOSurfaceRef)destinationImage.ioSurface, false, dst_up)) {
                if (full && out.img.width == in.img.width && out.img.height == in.img.height) {
                    status = ntscrt_render_cpu(slot->inst, &ra, &in.img, &out.img);
                } else {  // tile: the whole frame once (S.whole), then this tile's rows
                    const int bpp = out.img.format == NTSCRT_RGBA32F ? 16 : (out.img.format == NTSCRT_RGBA16F || out.img.format == NTSCRT_RGBA16) ? 8 : 4;
                    WholeFrame &W = S.whole;
                    if (W.matches(st->frame, h, device.registryID, in.img.width, in.img.height, out.img.format) && !W.cpu.empty()) {
                        status = NTSCRT_OK;
                        reused = true;
                    } else {
                        W = WholeFrame{};
                        W.cpu.assign(size_t(in.img.width) * in.img.height * bpp, 0);
                        ntscrt_image whole{in.img.width, in.img.height, ptrdiff_t(in.img.width) * bpp, out.img.format, W.cpu.data()};
                        status = ntscrt_render_cpu(slot->inst, &ra, &in.img, &whole);
                        if (FrameWritten(status)) {
                            W.valid = true;
                            W.frame = st->frame;
                            W.look = h;
                            W.device = device.registryID;
                            W.width = in.img.width;
                            W.height = in.img.height;
                            W.format = out.img.format;
                        }
                    }
                    if (FrameWritten(status)) {
                        // out.img rows are top-first (row_bytes sign handles orientation); its tile starts at
                        // image row top-first = (H - tileTop) for bottom-left tiles, or oy otherwise.
                        const FxRect t = destinationImage.tilePixelBounds, im = destinationImage.imagePixelBounds;
                        const int top_row = dst_up ? (im.top - t.top) : oy;
                        const ptrdiff_t wrb = ptrdiff_t(W.width) * bpp;
                        for (int r = 0; r < out.img.height && top_row + r < W.height; ++r) {
                            const uint8_t *s = W.cpu.data() + size_t(top_row + r) * size_t(wrb) + size_t(ox) * bpp;
                            uint8_t *d = static_cast<uint8_t *>(out.img.data) + ptrdiff_t(r) * out.img.row_bytes;
                            memcpy(d, s, size_t(std::max(0, std::min(out.img.width, W.width - ox))) * bpp);
                        }
                    }
                }
            } else {
                why = @"unsupported IOSurface pixel format";
                status = NTSCRT_ERR_INVALID_ARG;
            }
        }
    }
    if (tile_lock.owns_lock()) tile_lock.unlock();
    const std::string err = reused ? std::string() : std::string(ntscrt_last_error(slot->inst));  // before another thread reuses the slot
    S.release(slot);
    const long n = ++gRenders;
    const bool failed = !FrameWritten(status);
    const bool degraded = !failed && status != NTSCRT_OK;
    bool new_warning = false;
    {
        std::lock_guard<std::mutex> l(S.m);
        if (degraded && err != S.last_warning) new_warning = true;
        if (!failed) S.last_warning = degraded ? err : std::string();
    }
    if (n <= 20 || n % 500 == 0 || st->debug || failed || new_warning) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const FxRect ib = destinationImage.imagePixelBounds, tb = destinationImage.tilePixelBounds;
        std::ostringstream o;
        o << (st->debug ? "debug " : "") << "render #" << n << " frame=" << st->frame << " fps=" << st->fps
          << " scale=" << ra.render_scale << " image=" << (ib.right - ib.left) << 'x' << (ib.top - ib.bottom)
          << " tile=" << (tb.right - tb.left) << 'x' << (tb.top - tb.bottom)
          << " srcfmt=" << uint32_t(IOSurfaceGetPixelFormat((__bridge IOSurfaceRef)src.ioSurface))
          << " flip=" << int(src_up) << int(dst_up) << " history=" << st->history << " path=" << (used_cpu ? "cpu" : "metal")
          << (reused ? " tile-from-cache" : "") << " status=" << int(status)
          << (failed ? " error=" + err : degraded ? " degraded=" + err : std::string()) << " ms=" << ms;
        if (st->debug) {
            const size_t lh = std::hash<std::string>{}(st->look) ^ (std::hash<std::string>{}(st->feedback) << 1);
            o << " seed=" << st->seed << " past_looks=" << st->past_looks.size() << " flushed=" << flush
              << " scaleX=" << destinationImage.scaleX << " scaleY=" << destinationImage.scaleY
              << " pixelAspect=" << destinationImage.pixelAspect << " look#" << std::hex << lh << std::dec;
            if (gLastDebugLookHash.exchange(lh) != lh) {
                o << " look=" << st->look;
                if (!st->feedback.empty()) o << " feedback=" << st->feedback;
            }
        } else if (n == 1) {
            o << " look=" << st->look.substr(0, 400) << (st->look.size() > 400 ? "..." : "");
        }
        LogLine(o.str(), failed || new_warning);  // per-frame detail stays out of the system log
    }
    if (failed) {
        if (outError) *outError = MakeError(kFxError_ThirdPartyDeveloperStart + 2,
                                            [NSString stringWithFormat:@"NTSCRT render failed (%d): %s %@", int(status), err.c_str(), why ?: @""]);
        return NO;
    }
    // Degraded (CRT shader skipped, a source frame missing): the frame is complete, so it's a
    // success; the reason was logged once above.
    return YES;
}

@end
