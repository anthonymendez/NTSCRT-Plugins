// receiver.h — port of upstream Sources/CrtCore/Receiver.swift and
// GlitchParams.swift (NTSCRT 2b3013d): the simulated TV receiver's NTSC
// timing / sync / color-reference circuits. Pure CPU, deterministic: state
// depends only on (settings over time, seed), never on the picture.
// Owner: agent B.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ntscrt::glitch {

// ---- NTSC line timing (SMPTE 170M), µs ------------------------------------------
namespace NTSCTiming {
constexpr double line = 63.556;
constexpr double sync = 4.7;
constexpr double burstStart = 5.3;
constexpr double burstEnd = 7.8;
constexpr double gateStart = 4.55;
constexpr double gateEnd = 8.55;
constexpr double activeStart = 9.4;
constexpr double activeLength = 52.656;
constexpr double activeEnd = activeStart + activeLength;
constexpr double equalizingPulse = 2.3;
constexpr double broadPulse = 27.1;
constexpr double fieldRate = 60000.0 / 1001.0;
constexpr double linesPerField = 262.5;
constexpr double subcarrierMHz = 315.0 / 88.0;
constexpr double burstI = 0.5446390350150271;
constexpr double burstQ = -0.8386705679454240;
constexpr double burstAmplitude = 20.0 / 92.5;
}  // namespace NTSCTiming

// ---- params (GlitchParams.swift) ----------------------------------------------------
enum ParamId : int {
    kSignalStrength, kVerticalHold, kHorizontalHold, kAfcSpeed, kBrightness, kGhostLevel,
    kGhostDelay, kHum, kClosedCaptions, kHeadSwitch, kTimebaseJitter, kCrinkle, kHeadClog,
    kTracking, kSearchSpeed, kDropouts, kDropoutCompensation, kParamCount
};

struct ParamInfo {
    const char *id;
    double defaultValue;
    bool timing;  // GlitchSettings.timingIDs: drives the receiver's evolving state
};
extern const ParamInfo kParams[kParamCount];

// One moment's knob values (upstream GlitchSettings). Defaults = healthy set.
struct GlitchSettings {
    std::array<double, kParamCount> v{};
    GlitchSettings();
    double operator[](ParamId id) const { return v[id]; }
    double &operator[](ParamId id) { return v[id]; }
    bool flag(ParamId id) const { return v[id] >= 0.5; }
    // Set by upstream key name; false if unknown.
    bool set(std::string_view id, double value);
    // From {"key": value, ...} (numbers or bools); missing keys keep defaults.
    static GlitchSettings from_values(const nlohmann::json &values);
    // From a look: look["glitch"]["values"] (defaults when absent).
    static GlitchSettings from_look(const nlohmann::json &look);
    // Hash of the timing knobs only (bit-exact).
    uint64_t timing_hash() const;
    bool operator==(const GlitchSettings &o) const { return v == o.v; }
};

// ---- raster ------------------------------------------------------------------------
struct ReceiverRaster {
    int width = 1, activeLines = 1, vbiLines = 12;
    int preEqEnd = 1, vsyncEnd = 2, postEqEnd = 3;

    ReceiverRaster() = default;
    ReceiverRaster(int width, int activeLines);
    int totalLines() const { return activeLines + vbiLines; }
    double scale() const { return double(totalLines()) / NTSCTiming::linesPerField; }
    int ccLine() const { return vbiLines - 1; }
    double syncDuty(int jf) const;
    bool hasBurst(int jf) const { return jf >= postEqEnd; }
    bool operator==(const ReceiverRaster &o) const {
        return width == o.width && activeLines == o.activeLines;
    }
    bool operator!=(const ReceiverRaster &o) const { return !(*this == o); }
};

// GPU-mirrored structs (std430/HLSL structured buffer layout, 4-byte scalars).
struct GlitchRow {
    float u0 = 0;
    int32_t fieldLine = 0;
    float lenPrev = 0, lenCur = 0, lenNext = 0;
    float noiseIRE = 0, humIRE = 0, burstScale = 0;
    float tapeLoss = 0;
    float gateShift = 0;
    float pad1 = 0, pad2 = 0;
    bool operator==(const GlitchRow &o) const {
        return u0 == o.u0 && fieldLine == o.fieldLine && lenPrev == o.lenPrev && lenCur == o.lenCur &&
               lenNext == o.lenNext && noiseIRE == o.noiseIRE && humIRE == o.humIRE &&
               burstScale == o.burstScale && tapeLoss == o.tapeLoss && gateShift == o.gateShift;
    }
};
static_assert(sizeof(GlitchRow) == 48);

struct GlitchDropout {
    int32_t fieldLine = 0;
    float uStart = 0, uLength = 0, pad = 0;
    bool operator==(const GlitchDropout &o) const {
        return fieldLine == o.fieldLine && uStart == o.uStart && uLength == o.uLength;
    }
};
static_assert(sizeof(GlitchDropout) == 16);

struct GlitchFieldPlan {
    ReceiverRaster raster;
    std::vector<GlitchRow> rows;
    std::vector<GlitchDropout> dropouts;
    float chromaPhase = 0, chromaGain = 1, killer = 1;
    bool colorOn = true;
    float pictureGain = 1;
    uint32_t ccBits = 0;
    uint32_t fieldIndex = 0;
    GlitchSettings settings;
};

// ---- deterministic noise (GlitchRandom) ---------------------------------------------
namespace GlitchRandom {
uint64_t mix(uint64_t z0);
double uniform(uint64_t seed, uint64_t purpose, int64_t a, int64_t b = 0);
double gauss(uint64_t seed, uint64_t purpose, int64_t a, int64_t b = 0);
double value(uint64_t seed, uint64_t purpose, double x);
double phi(double x);
}  // namespace GlitchRandom

// ---- knobs -> circuit constants ---------------------------------------------------
struct Knobs {
    GlitchSettings settings;
    double sigma, agc, vTotal;
    double vLock = 0.015;
    double vCenter, vBeta, hR, kp, ki, uMax, humIRE, headSwitch, jitter, crinkle, clog, search,
        tracking, dropouts;
    bool captions;
    double gateAlpha, chromaAlpha, accGamma, killerKappa;
    Knobs(const GlitchSettings &s, const ReceiverRaster &raster);
};

struct ChromaReference {
    double phase = 0, gain = 1, killer = 1;
    bool colorOn = true;
    void step(double gi, double gq, const Knobs &k);
};

inline int64_t posmod(int64_t a, int64_t m) {
    int64_t r = a % m;
    return r < 0 ? r + m : r;
}
inline int64_t floordiv(int64_t a, int64_t m) { return a >= 0 ? a / m : -((-a + m - 1) / m); }

// ---- the receiver ------------------------------------------------------------------
class ReceiverSimulator {
public:
    static constexpr double warmup = 2.0;
    static constexpr uint64_t kDefaultSeed = 0x4E5453435254ull;

    explicit ReceiverSimulator(const ReceiverRaster &raster, uint64_t seed = kDefaultSeed);

    const ReceiverRaster &raster() const { return raster_; }
    uint64_t seed() const { return seed_; }
    int nominalTrigger() const { return nominalTrigger_; }
    double time() const;

    void reset();

    struct Snapshot {
        double time = 0;
        int64_t r = 0;
        double theta = 0, integ = 0, vPhase = 0, vInteg = 0;
        bool vAbove = false;
        double gateErr = 0;
        std::vector<double> gateLog;
        std::vector<int64_t> triggers;
        std::vector<double> thetaLog;
        size_t bytes() const {
            return sizeof(Snapshot) + (gateLog.size() + thetaLog.size()) * sizeof(double) +
                   triggers.size() * sizeof(int64_t);
        }
    };
    Snapshot snapshot() const;
    void restore(const Snapshot &s);

    // Run the set forward to `time` with `settings` in force since the previous call.
    void advance(double time, const GlitchSettings &settings);

    // Upstream resimulate(to:frameDuration:settingsAt:resumeFrom:checkpoint:).
    void resimulate(double time, double frameDuration,
                    const std::function<GlitchSettings(double)> &settingsAt,
                    const Snapshot *resumeFrom = nullptr,
                    const std::function<void(const Snapshot &)> &checkpoint = nullptr);

    // The most recently completed field, as the TV drew it.
    GlitchFieldPlan plan() const;

    double timebaseError(int64_t j) const;
    double signalLoss(int64_t j) const;

private:
    struct Uncalibrated {};
    ReceiverSimulator(const ReceiverRaster &raster, uint64_t seed, Uncalibrated);
    static int calibrate(const ReceiverRaster &raster, uint64_t seed);

    void step();
    bool inRetrace(int64_t tvLine) const;
    double loggedGate(int64_t tvLine) const;
    double loggedTheta(int64_t tvLine) const;
    std::pair<int64_t, double> signalLine(double tau) const;
    double fieldTime(int64_t j) const;
    int head(int64_t j) const;
    double trackingAt(int64_t j) const;
    std::pair<double, int> searchAt(int64_t j) const;
    double noiseSigma(double loss) const;
    double hum(double tau) const;
    std::pair<double, double> crinkleAt(int64_t j) const;
    std::vector<GlitchDropout> dropouts(int64_t f) const;
    uint32_t captionBits(int64_t f) const;

    ReceiverRaster raster_;
    uint64_t seed_;
    int L_;
    double H_ = NTSCTiming::line;
    double usPerSecond_;

    int64_t r_ = 0;
    double theta_ = 0, integ_ = 0;
    double vPhase_ = 0, vInteg_ = 0;
    bool vAbove_ = false;
    std::vector<int64_t> triggers_;
    double gateErr_ = 0;
    std::vector<double> gateLog_;
    std::vector<double> thetaLog_;
    int logCap_;

    Knobs knobs_;
    int nominalTrigger_ = 0;
};

}  // namespace ntscrt::glitch
