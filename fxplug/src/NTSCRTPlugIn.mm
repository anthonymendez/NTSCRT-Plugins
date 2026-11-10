// NTSCRTPlugIn — FxPlug 4 FxTileableEffect over the ntscrt C ABI.
//
// Flow per frame (FxPlug 4):
//   pluginState:atTime:   read every param at renderTime (and at earlier frames when a
//                         temporal stage is on), build look/feedback JSON -> RenderState blob
//   scheduleInputs:       request the effect source at renderTime and N earlier frames
//   renderDestinationImage: render with ntscrt_render_metal on the host's MTLDevice
//                         (ntscrt_render_cpu through the IOSurfaces as fallback); earlier
//                         source frames reach the core through fetch_source.
// Params are created at runtime from ntscrt_param_schema_json() via ntscrt_fx::Model.
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

struct Preset {
    std::string name;
    json data;
};

struct Shared {
    Model model;
    std::vector<Preset> looks, loops;  // bundled look / Screen Loop presets
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
        for (const auto &w : warnings) NSLog(@"[NTSCRT] %s", w.c_str());

        // Bundled presets need an instance; it never renders.
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_METAL, nullptr, nullptr, sh->resource_dir.c_str()};
        ntscrt_instance *meta = nullptr;
        if (ntscrt_create(&ci, &meta) == NTSCRT_OK && meta) {
            json arr = json::parse(ntscrt_bundled_presets_json(meta), nullptr, false);
            if (arr.is_array()) {
                for (auto &p : arr) {
                    Preset pr{p.value("name", std::string("?")), p.value("json", json::object())};
                    (p.value("kind", std::string("look")) == "feedback" ? sh->loops : sh->looks).push_back(std::move(pr));
                }
            }
            ntscrt_destroy(meta);
        }
        return sh;
    }();
    return *s;
}

// Bumped on every parameter change; a pooled instance whose generation differs flushes
// its temporal caches before rendering (the core validates history only by sampling).
std::atomic<uint64_t> gParamGeneration{1};

// One ntscrt instance renders one frame at a time; FxPlug renders on several threads.
struct Slot {
    ntscrt_instance *inst = nullptr;
    __strong id<MTLDevice> device;
    __strong id<MTLCommandQueue> queue;
    uint64_t generation = 0;
    std::unordered_map<int64_t, size_t> look_hash;  // frame -> hash of looks seen when rendered
};

class Pool {
public:
    Slot *acquire(id<MTLDevice> device) {
        {
            std::lock_guard<std::mutex> l(m_);
            auto &v = free_[device.registryID];
            if (!v.empty()) { Slot *s = v.back(); v.pop_back(); return s; }
        }
        auto *s = new Slot;
        s->device = device;
        s->queue = [device newCommandQueue];
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_METAL, (__bridge void *)device,
                              (__bridge void *)s->queue, GetShared().resource_dir.c_str()};
        if (ntscrt_create(&ci, &s->inst) != NTSCRT_OK || !s->inst) { delete s; return nullptr; }
        return s;
    }
    void release(Slot *s) {
        std::lock_guard<std::mutex> l(m_);
        free_[s->device.registryID].push_back(s);
    }

private:
    std::mutex m_;
    std::map<uint64_t, std::vector<Slot *>> free_;
};
Pool &GetPool() { static Pool *p = new Pool; return *p; }

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
    NSLog(@"[NTSCRT] %@", msg);
    return [NSError errorWithDomain:FxPlugErrorDomain code:code userInfo:@{NSLocalizedDescriptionKey : msg}];
}

}  // namespace

// ---------------------------------------------------------------------------
@interface NTSCRTPlugIn : NSObject <FxTileableEffect>
@end

@implementation NTSCRTPlugIn {
    id<PROAPIAccessing> _apiManager;
    BOOL _applying;  // suppress re-entrant parameterChanged while we set values
}

- (nullable instancetype)initWithAPIManager:(id<PROAPIAccessing>)newApiManager {
    self = [super init];
    if (self) {
        _apiManager = newApiManager;
        (void)GetShared();
    }
    return self;
}

- (BOOL)properties:(NSDictionary *_Nonnull *_Nonnull)properties error:(NSError **)error {
    (void)error;
    *properties = @{
        kFxPropertyKey_MayRemapTime : @NO,
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
    for (const auto &p : sh.looks) [looks addObject:@(p.name.c_str())];
    ok &= [api addPopupMenuWithName:@"Preset" parameterID:kPresetMenu defaultValue:0 menuEntries:looks parameterFlags:fixed];
    ok &= [api addPushButtonWithName:@"Load Preset…" parameterID:kLoadPreset selector:@selector(loadPresetPressed) parameterFlags:PF(kFxParameterFlag_DEFAULT)];
    ok &= [api addPushButtonWithName:@"Save Preset…" parameterID:kSavePreset selector:@selector(savePresetPressed) parameterFlags:PF(kFxParameterFlag_DEFAULT)];
    NSMutableArray *loops = [NSMutableArray arrayWithObjects:@"Off", @"(Custom)", nil];
    for (const auto &p : sh.loops) [loops addObject:@(p.name.c_str())];
    ok &= [api addPopupMenuWithName:@"Screen Loop" parameterID:kScreenLoopMenu defaultValue:0 menuEntries:loops parameterFlags:fixed];
    ok &= [api addIntSliderWithName:@"Seed" parameterID:kSeed defaultValue:0 parameterMin:0 parameterMax:1000000
                          sliderMin:0 sliderMax:1000 delta:1 parameterFlags:fixed];
    ok &= [api addIntSliderWithName:@"History Frames" parameterID:kHistoryFrames defaultValue:8 parameterMin:0
                       parameterMax:kMaxHistory sliderMin:0 sliderMax:32 delta:1 parameterFlags:fixed];
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
        switch (p.kind) {
        case Kind::Bool:
            ok &= [api addToggleButtonWithName:name parameterID:p.host_id defaultValue:d.get<bool>() parameterFlags:flags];
            break;
        case Kind::Int:
            ok &= [api addIntSliderWithName:name parameterID:p.host_id defaultValue:int(d.get<int64_t>())
                               parameterMin:int(p.min) parameterMax:int(p.max) sliderMin:int(p.slider_min)
                                  sliderMax:int(p.slider_max) delta:std::max(1, int(p.step)) parameterFlags:flags];
            break;
        case Kind::Float:
            ok &= [api addFloatSliderWithName:name parameterID:p.host_id defaultValue:d.get<double>()
                                 parameterMin:p.min parameterMax:p.max sliderMin:p.slider_min
                                    sliderMax:p.slider_max delta:p.step parameterFlags:flags];
            break;
        case Kind::Choice: {
            NSMutableArray *entries = [NSMutableArray array];
            for (const auto &o : p.options) [entries addObject:@(o.label.c_str())];
            ok &= [api addPopupMenuWithName:name parameterID:p.host_id defaultValue:UInt32(d.get<int64_t>())
                                menuEntries:entries parameterFlags:PF(bits | uint32_t(kFxParameterFlag_NOT_ANIMATABLE))];
            break;
        }
        case Kind::Color:
            ok &= [api addColorParameterWithName:name parameterID:p.host_id defaultRed:d[0].get<double>()
                                    defaultGreen:d[1].get<double>() defaultBlue:d[2].get<double>() parameterFlags:flags];
            break;
        }
    }
    ok &= [api endParameterSubGroup];  // NTSCRT

    // Base JSON the params are overlaid on (from a chosen/loaded preset), saved in the project.
    const FxParameterFlags hidden = PF(uint32_t(kFxParameterFlag_HIDDEN) | uint32_t(kFxParameterFlag_NOT_ANIMATABLE));
    ok &= [api addStringParameterWithName:@"Base Look" parameterID:kBaseLook defaultValue:@"{}" parameterFlags:hidden];
    ok &= [api addStringParameterWithName:@"Base Screen Loop" parameterID:kBaseFeedback defaultValue:@"{}" parameterFlags:hidden];
    if (!ok && error) *error = MakeError(kFxError_InvalidParameter, @"Unable to create NTSCRT parameters");
    return ok;
}

// ---- value access -----------------------------------------------------------
static json GetHostValue(id<FxParameterRetrievalAPI_v6> api, const Model &m, const Param &p, CMTime t) {
    switch (p.kind) {
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
    switch (p.kind) {
    case Kind::Bool: return [api setBoolValue:Model::truthy(v) toParameter:p.host_id atTime:t];
    case Kind::Int:
    case Kind::Choice: return [api setIntValue:int(v.get<int64_t>()) toParameter:p.host_id atTime:t];
    case Kind::Float: return [api setFloatValue:v.get<double>() toParameter:p.host_id atTime:t];
    case Kind::Color:
        return [api setRedValue:v[0].get<double>() greenValue:v[1].get<double>() blueValue:v[2].get<double>()
                    toParameter:p.host_id atTime:t];
    }
    return NO;
}

static json GetJSONString(id<FxParameterRetrievalAPI_v6> api, UInt32 pid) {
    NSString *s = nil;
    if (![api getStringParameterValue:&s fromParameter:pid] || !s) return json::object();
    json j = json::parse(s.UTF8String, nullptr, false);
    return j.is_object() ? j : json::object();
}

static int GetInt(id<FxParameterRetrievalAPI_v6> api, UInt32 pid, CMTime t, int fallback) {
    int v = fallback;
    return [api getIntValue:&v fromParameter:pid atTime:t] ? v : fallback;
}

struct Built {
    json look;
    std::optional<json> feedback;
};

static Built BuildAt(id<FxParameterRetrievalAPI_v6> api, CMTime t) {
    const Model &m = GetShared().model;
    auto get = [&](const Param &p) { return GetHostValue(api, m, p, t); };
    Built b;
    b.look = m.build_look(GetJSONString(api, kBaseLook), get);
    const bool loop_on = GetInt(api, kScreenLoopMenu, t, 0) != 0;
    b.feedback = m.build_feedback(loop_on, GetJSONString(api, kBaseFeedback), get);
    return b;
}

// Grey out / hide params per enabled_if / visible_if at time t.
- (void)updateParameterStatesAtTime:(CMTime)t {
    id<FxParameterRetrievalAPI_v6> get = [_apiManager apiForProtocol:@protocol(FxParameterRetrievalAPI_v6)];
    id<FxParameterSettingAPI_v5> set = [_apiManager apiForProtocol:@protocol(FxParameterSettingAPI_v5)];
    if (!get || !set) return;
    const Model &m = GetShared().model;
    Built b = BuildAt(get, t);
    const json fb = b.feedback.value_or(json::object());
    for (const Param &p : m.params) {
        if (p.enabled_if.empty() && p.visible_path.empty()) continue;
        FxParameterFlags flags = PF(0);
        if (![get getParameterFlags:&flags fromParameter:p.host_id]) continue;
        const uint32_t have = uint32_t(flags);
        uint32_t want = have & ~(uint32_t(kFxParameterFlag_DISABLED) | uint32_t(kFxParameterFlag_HIDDEN));
        if (!m.is_enabled(p, b.look, fb)) want |= uint32_t(kFxParameterFlag_DISABLED);
        if (!m.is_visible(p, b.look, fb)) want |= uint32_t(kFxParameterFlag_HIDDEN);
        if (want != have) [set setParameterFlags:PF(want) toParameter:p.host_id];
    }
}

// Writes a preset into the host params (look or Screen Loop half) at time t.
- (void)applyPreset:(const json &)preset feedback:(bool)feedback atTime:(CMTime)t {
    id<FxParameterSettingAPI_v5> set = [_apiManager apiForProtocol:@protocol(FxParameterSettingAPI_v5)];
    if (!set) return;
    const Model &m = GetShared().model;
    _applying = YES;
    [set setStringParameterValue:@(preset.dump().c_str()) toParameter:feedback ? kBaseFeedback : kBaseLook];
    for (const auto &[id, v] : m.host_values_from_preset(preset, feedback)) {
        if (const Param *p = m.find_host(id)) SetHostValue(set, *p, v, t);
    }
    _applying = NO;
    gParamGeneration.fetch_add(1);
}

- (BOOL)parameterChanged:(UInt32)paramID atTime:(CMTime)time error:(NSError **)error {
    (void)error;
    gParamGeneration.fetch_add(1);
    if (_applying) return YES;
    id<FxParameterRetrievalAPI_v6> get = [_apiManager apiForProtocol:@protocol(FxParameterRetrievalAPI_v6)];
    const Shared &sh = GetShared();
    if (get && paramID == kPresetMenu) {
        int idx = GetInt(get, kPresetMenu, time, 0);
        if (idx >= 1 && size_t(idx) <= sh.looks.size()) [self applyPreset:sh.looks[size_t(idx - 1)].data feedback:false atTime:time];
    } else if (get && paramID == kScreenLoopMenu) {
        int idx = GetInt(get, kScreenLoopMenu, time, 0);
        if (idx >= 2 && size_t(idx - 2) < sh.loops.size()) [self applyPreset:sh.loops[size_t(idx - 2)].data feedback:true atTime:time];
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
        const bool fb = j.value("kind", std::string()) == "ntscrt-video-feedback";
        [action startAction:strongSelf];
        CMTime t = [action currentTime];
        [strongSelf applyPreset:j feedback:fb atTime:t];
        id<FxParameterSettingAPI_v5> set = [strongSelf->_apiManager apiForProtocol:@protocol(FxParameterSettingAPI_v5)];
        strongSelf->_applying = YES;
        if (fb) [set setIntValue:1 toParameter:kScreenLoopMenu atTime:t];  // "(Custom)" = on, base from file
        else [set setIntValue:0 toParameter:kPresetMenu atTime:t];          // "(Custom)"
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
    Built b = BuildAt(get, [action currentTime]);
    std::string look = b.look.dump(2);
    std::string loop = b.feedback ? b.feedback->dump(2) : std::string();
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

    Built now = BuildAt(get, renderTime);
    st.look = now.look.dump();
    st.feedback = now.feedback ? now.feedback->dump() : std::string();
    st.seed = uint64_t(std::max(0, GetInt(get, kSeed, renderTime, 0)));
    if (needs_history(now.look, now.feedback)) {
        const int want = std::clamp(GetInt(get, kHistoryFrames, renderTime, 8), 0, kMaxHistory);
        st.history = int(std::min<int64_t>(want, st.frame));
        for (int k = 1; k <= std::min(st.history, kMaxPastLooks); ++k) {
            CMTime tk = CMTimeSubtract(renderTime, CMTimeMultiply(frameDur, k));
            std::string lk = BuildAt(get, tk).look.dump();
            if (lk != st.look) st.past_looks[st.frame - k] = std::move(lk);
        }
    }
    const std::string bytes = st.serialize();
    *pluginState = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    return YES;
}

static std::optional<RenderState> ParseState(NSData *d) {
    if (!d) return std::nullopt;
    return RenderState::parse(std::string((const char *)d.bytes, d.length));
}

- (BOOL)scheduleInputs:(NSArray *_Nullable *_Nullable)inputImageRequests
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
    id<MTLDevice> device = [NTSCRTMetal deviceForRegistryID:destinationImage.deviceRegistryID];
    Slot *slot = device ? GetPool().acquire(device) : nullptr;
    if (!slot) { if (outError) *outError = MakeError(kFxError_ThirdPartyDeveloperStart + 1, @"cannot create ntscrt instance"); return NO; }

    // Flush temporal caches after any parameter / keyframe change.
    const uint64_t gen = gParamGeneration.load();
    size_t h = std::hash<std::string>{}(st->look) ^ (std::hash<std::string>{}(st->feedback) << 1);
    for (const auto &[f, s] : st->past_looks) h ^= std::hash<std::string>{}(s) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2) + size_t(f);
    auto seen = slot->look_hash.find(st->frame);
    if (slot->generation != gen || (seen != slot->look_hash.end() && seen->second != h)) {
        ntscrt_flush_caches(slot->inst);
        slot->look_hash.clear();
        slot->generation = gen;
    }
    if (slot->look_hash.size() > 4096) slot->look_hash.clear();
    slot->look_hash[st->frame] = h;

    RenderContext ctx{sourceImages, &*st, {}};
    ntscrt_render_args ra{};
    ra.frame_index = st->frame;
    ra.fps = st->fps;
    ra.look_json = st->look.c_str();
    ra.feedback_json = st->feedback.empty() ? nullptr : st->feedback.c_str();
    ra.render_scale = destinationImage.scaleX > 0 ? destinationImage.scaleX : 1.0;
    ra.seed = st->seed;
    ra.fetch_source = FetchSource;
    ra.release_source = ReleaseSource;
    ra.look_at = LookAt;
    ra.source_premultiplied = 1;  // FxPlug / Motion images carry premultiplied alpha
    ra.user = &ctx;

    const bool src_up = BottomUp(src), dst_up = BottomUp(destinationImage);
    const bool full = SameRect(destinationImage.tilePixelBounds, destinationImage.imagePixelBounds);
    int ox = 0, oy = 0;
    TileOffset(destinationImage, dst_up, &ox, &oy);

    ntscrt_status status = NTSCRT_ERR_INTERNAL;
    NSString *why = nil;
    @autoreleasepool {
        id<MTLTexture> inTex = [src metalTextureForDevice:device];
        id<MTLTexture> outTex = [destinationImage metalTextureForDevice:device];
        id<MTLCommandBuffer> cb = [slot->queue commandBuffer];
        if (inTex && outTex && cb) {
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
            if (coreIn && coreOut) {
                status = ntscrt_render_metal(slot->inst, &ra, (__bridge void *)coreIn, (__bridge void *)coreOut, (__bridge void *)cb);
                if ((status == NTSCRT_OK || status == NTSCRT_ERR_SOURCE_UNAVAILABLE) && coreOut != outTex) {
                    // dst row r <- core row (flip ? H-1-(oy+r) : oy+r)
                    [NTSCRTMetal encodeCopyFrom:coreOut to:outTex offsetX:ox offsetY:oy flipY:dst_up commandBuffer:cb];
                }
            }
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) { why = cb.error.localizedDescription; status = NTSCRT_ERR_GPU; }
        }
        if (status != NTSCRT_OK && status != NTSCRT_ERR_SOURCE_UNAVAILABLE && !why) why = @(ntscrt_last_error(slot->inst));
    }

    if (status != NTSCRT_OK && status != NTSCRT_ERR_SOURCE_UNAVAILABLE) {
        // CPU fallback straight through the IOSurfaces.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) NSLog(@"[NTSCRT] Metal path failed (%d: %@), using CPU (logged once)", int(status), why);
        SurfaceView in, out;
        if (in.lock((__bridge IOSurfaceRef)src.ioSurface, true, src_up) &&
            out.lock((__bridge IOSurfaceRef)destinationImage.ioSurface, false, dst_up)) {
            if (full && out.img.width == in.img.width && out.img.height == in.img.height) {
                status = ntscrt_render_cpu(slot->inst, &ra, &in.img, &out.img);
            } else {  // tile: render the whole frame, copy the tile's rows
                const int bpp = out.img.format == NTSCRT_RGBA32F ? 16 : (out.img.format == NTSCRT_RGBA16F || out.img.format == NTSCRT_RGBA16) ? 8 : 4;
                std::vector<uint8_t> tmp(size_t(in.img.width) * in.img.height * bpp);
                ntscrt_image whole{in.img.width, in.img.height, ptrdiff_t(in.img.width) * bpp, out.img.format, tmp.data()};
                status = ntscrt_render_cpu(slot->inst, &ra, &in.img, &whole);
                if (status == NTSCRT_OK || status == NTSCRT_ERR_SOURCE_UNAVAILABLE) {
                    // out.img rows are top-first (row_bytes sign handles orientation); its tile starts at
                    // image row top-first = (H - tileTop) for bottom-left tiles, or oy otherwise.
                    const FxRect t = destinationImage.tilePixelBounds, im = destinationImage.imagePixelBounds;
                    const int top_row = dst_up ? (im.top - t.top) : oy;
                    for (int r = 0; r < out.img.height && top_row + r < whole.height; ++r) {
                        const uint8_t *s = tmp.data() + size_t(top_row + r) * whole.row_bytes + size_t(ox) * bpp;
                        uint8_t *d = static_cast<uint8_t *>(out.img.data) + ptrdiff_t(r) * out.img.row_bytes;
                        memcpy(d, s, size_t(std::min(out.img.width, whole.width - ox)) * bpp);
                    }
                }
            }
        } else {
            why = @"unsupported IOSurface pixel format";
        }
    }
    const std::string err = ntscrt_last_error(slot->inst);  // copy before another thread reuses the slot
    GetPool().release(slot);
    if (status != NTSCRT_OK && status != NTSCRT_ERR_SOURCE_UNAVAILABLE) {
        if (outError) *outError = MakeError(kFxError_ThirdPartyDeveloperStart + 2,
                                            [NSString stringWithFormat:@"NTSCRT render failed (%d): %s %@", int(status), err.c_str(), why ?: @""]);
        return NO;
    }
    return YES;
}

@end
