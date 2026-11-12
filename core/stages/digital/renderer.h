// renderer.h — port of upstream Sources/CrtCodec/DigitalVideoParams.swift and
// DigitalVideoRenderer.swift (upstream v0.21.0, 8b26092): the stage's
// settings, the camera (capture timing, optics, sensor, electronics) and the
// stream around the codec.
//
// Not ported: DigitalParam.startingValues (0.21: a new app window starts with
// the stage off but set to the "Cheap camcorder" look). A new plug-in instance
// starts from the controls' defaults (kParams / the schema's "default"), which
// upstream keeps for formats, resets and older looks: a host fills a preset's
// missing values from the schema default, so starting values there would change
// how older looks apply. The look itself is bundled (presets/Cheap camcorder.json).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codec.h"

namespace ntscrt::digital {

// ---- params (DigitalVideoParams.swift) ---------------------------------------

// Upstream DigitalParam.all order.
enum Param : int {
    kFrameRate, kFields, kAliasing, kSensorNoise, kLowLightNoise, kNoiseReduction, kCCDResponse, kWhiteBalance,
    kTint, kSaturation, kSharpening, kSharpeningWidth, kCodec, kChroma, kQuality, kKeyframeInterval,
    kFixedBitrate, kDeblocking, kDataLoss, kDatamosh, kParamCount
};

// Upstream DigitalCodecKind.
enum class CodecKind : int { DCT = 0, BlockColors = 1 };

struct ParamInfo {
    const char *id;
    double min, max, defaultValue;
    bool choice;
    bool structure;  // holds for a whole render (keyframes don't animate it)
};
extern const ParamInfo kParams[kParamCount];
std::optional<Param> param_by_id(std::string_view id);
// Upstream DigitalParam.unusedBy / applies(to:): controls a codec doesn't have.
bool param_applies(Param p, CodecKind codec);

struct Settings {
    std::array<double, kParamCount> v;
    Settings();  // upstream defaults
    double operator[](Param p) const { return v[size_t(p)]; }
    double &operator[](Param p) { return v[size_t(p)]; }
    bool set(std::string_view id, double value);  // false for unknown ids
    CodecKind codec() const;
    // The color layout the codec codes: Video 1 always keeps full color.
    Chroma chroma() const;
    // A control's value as the codec uses it: 0 for controls it doesn't have
    // (deblocking and datamosh under Video 1).
    double effective(Param p) const;
    // The same settings with the structure controls taken from `base`.
    Settings holdingStructure(const Settings &base) const;
    uint64_t fingerprint() const;
    bool operator==(const Settings &) const = default;
};

struct Format {
    const char *id;
    const char *name;
    int width;
    std::vector<std::pair<const char *, double>> values;
    Settings settings() const;  // defaults merged with `values`
};
const std::vector<Format> &formats();
const Format *format_by_id(std::string_view id);

// ---- timing ---------------------------------------------------------------------

struct Timing {
    double sourceFPS = 30, captureFPS = 15;
    int keyframeEvery = 1;
    Timing() = default;
    Timing(double sourceFPS, const Settings &settings);
    int captureOfFrame(int n) const;
    int frameOfCapture(int c) const;
    bool intraOnly() const { return keyframeEvery == 1; }
    FrameKind kind(int c, double datamosh, uint64_t seedMix) const;
    auto operator<=>(const Timing &) const = default;
};

// ---- the renderer -------------------------------------------------------------

struct Size {
    int width = 0, height = 0;
};

// Earlier source frames (BGRA8, any size, top row first) for re-running the
// stream; return nullptr when the frame can't be had.
struct SourceFrame {
    const uint8_t *data = nullptr;
    int width = 0, height = 0, rowBytes = 0;
    std::shared_ptr<const void> keepAlive;
};
using EarlierFrames = std::function<std::optional<SourceFrame>(int frame)>;
// Settings at a source frame (keyframes).
using SettingsAt = std::function<Settings(int frame)>;

class Renderer {
public:
    static constexpr size_t kCheckpointBudget = size_t(96) << 20;

    // The capture size for a picture at a capture width — the Downscale's rule.
    static Size captureSize(int width, int height, int captureWidth);

    // Replace frame `n` (BGRA8 in place) with what the camera and codec make
    // of it. `settingsAt` nil: settings never change. `earlier` nil: the
    // picture never changes (a still), so `pixels` stands in for every frame.
    void process(int n, uint8_t *pixels, int width, int height, int rowBytes, Size capture, double sourceFPS,
                 const Settings &settings, const SettingsAt &settingsAt = nullptr,
                 const EarlierFrames &earlier = nullptr, uint64_t seed = 0);
    void flush();

    int lastRunLength() const { return lastRunLength_; }

    // Where the last process() call's time went (ms of wall time, CPU side;
    // the façade adds them to the timings note: digital_ms=...). scale: the
    // current picture to the capture size; earlier: earlier frames fetched and
    // scaled for a re-run; camera: capture() (optics, sensor, electronics ->
    // Y'CbCr planes); codec: encode + decode; deblock; upsample: the chroma
    // upsample and Y'CbCr -> BGRA at the capture size (render() less deblock);
    // upscale: the capture picture back to the frame's size.
    struct Profile {
        double scale = 0, earlier = 0, camera = 0, codec = 0, deblock = 0, upsample = 0, upscale = 0;
        int captures = 0;  // captures coded (lastRunLength)
    };
    const Profile &lastProfile() const { return profile_; }

    // Exposed for tests (upstream statics).
    static std::vector<uint8_t> scale(const uint8_t *base, int width, int height, int rowBytes, Size size,
                                      const Settings &optics);
    static void scaleLanczos(const uint8_t *src, int width, int height, int rowBytes, uint8_t *dst, int cw, int ch);
    static PicturePtr capture(const std::vector<uint8_t> &bgra, const Geometry &g, const Settings &settings,
                              int index, uint64_t seedMix = 0);
    // Center-weighted mean of linear luminance (camcorder auto exposure).
    static float meteredMean(const std::vector<uint8_t> &bgra, int w, int h, float gr, float gg, float gb);
    // Low-light noise (upstream 0.21): the sensor gain a camera would have
    // needed for a scene this dark, judged by its median brightness (linear):
    // 1 down to gainKnee() (sRGB 100), rising as the median falls, up to 8x.
    static float gainKnee();
    static float sensorGain(float medianLuma);
    // The median of the picture's brightness (Y', every other pixel each way), as linear light.
    static float medianLuma(const std::vector<uint8_t> &bgra, int w, int h);
    // deblock_ms: when given, the deblocking's share of the time is added to it.
    static std::vector<uint8_t> render(const Picture &decoded, const Geometry &g, const Settings &settings,
                                       float step, double *deblock_ms = nullptr);
    static void upscale(const std::vector<uint8_t> &picture, Size size, uint8_t *base, int width, int height,
                        int rowBytes);

private:
    struct StreamKey {
        Geometry geometry;
        Timing timing;
        uint64_t seedMix = 0;
        bool still = false;
        auto operator<=>(const StreamKey &) const = default;
    };
    // The run since the decoder's picture was last known from scratch: the
    // settings fingerprints of captures origin, origin+1, ... — a checkpoint
    // (or the stream) is only reused while those still hold.
    struct Run {
        int origin = 0;
        std::vector<uint64_t> fps;
    };
    struct Checkpoint {
        std::shared_ptr<CodecState> state;
        Run run;
        size_t bytes = 0;
    };
    struct Call;  // per-process() context

    void rerun(Call &call, int c);
    void advance(Call &call, int k, const std::vector<uint8_t> &source);
    std::optional<std::vector<uint8_t>> earlierCapture(Call &call, int k);
    bool runHolds(Call &call, const Run &run, int from, int upTo);
    void store(const StreamKey &key, int capture, Checkpoint cp);

    std::optional<StreamKey> stream_;
    std::shared_ptr<CodecState> state_;
    std::optional<int> lastCapture_;
    std::optional<int> lastFrame_;
    Run run_;
    std::vector<uint8_t> shown_;

    std::map<std::pair<StreamKey, int>, Checkpoint> checkpoints_;
    std::vector<std::pair<StreamKey, int>> checkpointOrder_;
    size_t checkpointBytes_ = 0;
    int lastRunLength_ = 0;
    Profile profile_;
};

uint64_t seed_mix(uint64_t seed);  // 0 -> 0

// The tone of a 90s 3-CCD camcorder (the VX1000), fitted to its footage
// (upstream CCDResponse). `strength` blends every part from nothing (0) to
// the full response (1).
struct CCDResponse {
    float strength = 0;
    static constexpr float kTarget = 0.40f;
    static constexpr float kKneePoint = 0.72f * 255;
    float exposure(float meteredMean) const;
    float flare(float meteredMean) const { return 0.03f * strength * meteredMean; }
    float knee(float v) const {
        if (!(v > kKneePoint)) return v;
        const float slope = 1 - 0.62f * strength;
        return std::min(255.0f, kKneePoint + (v - kKneePoint) * slope);
    }
    float setup() const { return 0.065f * strength; }
};

}  // namespace ntscrt::digital
