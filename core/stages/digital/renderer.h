// renderer.h — port of upstream Sources/CrtCodec/DigitalVideoParams.swift and
// DigitalVideoRenderer.swift: the stage's settings, the camera (capture
// timing, optics, sensor, electronics) and the stream around the codec.
#pragma once

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

enum Param : int {
    kFrameRate, kFields, kLensBlur, kAliasing, kSensorNoise, kNoiseReduction, kExposure, kWhiteBalance, kTint,
    kSaturation, kBlackLevel, kSharpening, kChroma, kQuality, kKeyframeInterval, kFixedBitrate, kDeblocking,
    kDataLoss, kDatamosh, kParamCount
};

struct ParamInfo {
    const char *id;
    double min, max, defaultValue;
    bool choice;
    bool structure;  // holds for a whole render (keyframes don't animate it)
};
extern const ParamInfo kParams[kParamCount];
std::optional<Param> param_by_id(std::string_view id);

struct Settings {
    std::array<double, kParamCount> v;
    Settings();  // upstream defaults
    double operator[](Param p) const { return v[size_t(p)]; }
    double &operator[](Param p) { return v[size_t(p)]; }
    bool set(std::string_view id, double value);  // false for unknown ids
    Chroma chroma() const;
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

    // Exposed for tests (upstream statics).
    static std::vector<uint8_t> scale(const uint8_t *base, int width, int height, int rowBytes, Size size,
                                      const Settings &optics);
    static void scaleLanczos(const uint8_t *src, int width, int height, int rowBytes, uint8_t *dst, int cw, int ch);
    static void gaussianBlur(std::vector<uint8_t> &px, int w, int h, float sigma);
    static PicturePtr capture(const std::vector<uint8_t> &bgra, const Geometry &g, const Settings &settings,
                              int index, uint64_t seedMix = 0);
    static std::vector<uint8_t> render(const Picture &decoded, const Geometry &g, const Settings &settings,
                                       float step);
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
};

uint64_t seed_mix(uint64_t seed);  // 0 -> 0

}  // namespace ntscrt::digital
