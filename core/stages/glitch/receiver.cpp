// receiver.cpp — port of upstream Receiver.swift + GlitchParams.swift (NTSCRT 2b3013d).
// Swift `x.rounded()` = std::round (half away from zero), `.rounded(.down)` = floor,
// `truncatingRemainder` = fmod, `Int(x)` = truncation toward zero.
#include "receiver.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ntscrt::glitch {

namespace {
constexpr double kPi = 3.14159265358979323846;
inline int64_t roundI(double x) { return int64_t(std::round(x)); }
}  // namespace

// ---- params --------------------------------------------------------------------------
const ParamInfo kParams[kParamCount] = {
    {"signal_strength", 1, true},
    {"vertical_hold", 0, true},
    {"horizontal_hold", 0, true},
    {"afc_speed", 0.5, true},
    {"brightness", 0, false},
    {"ghost_level", 0, false},
    {"ghost_delay", 2, false},
    {"hum", 0, true},
    {"closed_captions", 1, false},
    {"head_switch", 0, true},
    {"timebase_jitter", 0, true},
    {"crinkle", 0, true},
    {"head_clog", 0, true},
    {"tracking", 0, true},
    {"search_speed", 1, true},
    {"dropouts", 0, false},
    {"dropout_compensation", 1, false},
};

GlitchSettings::GlitchSettings() {
    for (int i = 0; i < kParamCount; i++) v[i] = kParams[i].defaultValue;
}

bool GlitchSettings::set(std::string_view id, double value) {
    for (int i = 0; i < kParamCount; i++) {
        if (id == kParams[i].id) {
            v[i] = value;
            return true;
        }
    }
    return false;
}

GlitchSettings GlitchSettings::from_values(const nlohmann::json &values) {
    GlitchSettings s;
    if (!values.is_object()) return s;
    for (auto it = values.begin(); it != values.end(); ++it) {
        const auto &val = it.value();
        if (val.is_number()) s.set(it.key(), val.get<double>());
        else if (val.is_boolean()) s.set(it.key(), val.get<bool>() ? 1.0 : 0.0);
    }
    return s;
}

GlitchSettings GlitchSettings::from_look(const nlohmann::json &look) {
    if (!look.is_object()) return {};
    auto g = look.find("glitch");
    if (g == look.end() || !g->is_object()) return {};
    auto vals = g->find("values");
    if (vals == g->end()) return {};
    return from_values(*vals);
}

uint64_t GlitchSettings::timing_hash() const {
    uint64_t h = 0x243F6A8885A308D3ull;
    for (int i = 0; i < kParamCount; i++) {
        if (!kParams[i].timing) continue;
        uint64_t bits;
        double d = v[i] == 0 ? 0.0 : v[i];  // -0 == 0
        std::memcpy(&bits, &d, 8);
        h = GlitchRandom::mix(h ^ bits ^ (uint64_t(i) << 56));
    }
    return h;
}

// ---- raster --------------------------------------------------------------------------
ReceiverRaster::ReceiverRaster(int w, int a) {
    width = std::max(1, w);
    activeLines = std::max(1, a);
    int v = std::max(12, int(roundI(double(activeLines) * 20.0 / 242.5)));
    vbiLines = v;
    double s = double(activeLines + v) / NTSCTiming::linesPerField;
    int eq = std::max(1, int(roundI(3 * s)));
    preEqEnd = eq;
    vsyncEnd = 2 * eq;
    postEqEnd = 3 * eq;
}

double ReceiverRaster::syncDuty(int jf) const {
    if (jf < preEqEnd) return 2 * NTSCTiming::equalizingPulse / NTSCTiming::line;
    if (jf < vsyncEnd) return 2 * NTSCTiming::broadPulse / NTSCTiming::line;
    if (jf < postEqEnd) return 2 * NTSCTiming::equalizingPulse / NTSCTiming::line;
    return NTSCTiming::sync / NTSCTiming::line;
}

// ---- GlitchRandom ----------------------------------------------------------------------
namespace GlitchRandom {
uint64_t mix(uint64_t z0) {
    uint64_t z = z0 + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
double uniform(uint64_t seed, uint64_t purpose, int64_t a, int64_t b) {
    uint64_t h = mix(seed ^ mix(purpose ^ mix(uint64_t(a) ^ mix(uint64_t(b)))));
    return double(h >> 11) * (1.0 / 9007199254740992.0);
}
double gauss(uint64_t seed, uint64_t purpose, int64_t a, int64_t b) {
    double u1 = std::max(1e-12, uniform(seed, purpose, a, b));
    double u2 = uniform(seed, purpose + 0x51, a, b);
    return std::sqrt(-2 * std::log(u1)) * std::cos(2 * kPi * u2);
}
double value(uint64_t seed, uint64_t purpose, double x) {
    double i = std::floor(x);
    double f = x - i;
    double a = uniform(seed, purpose, int64_t(i)) * 2 - 1;
    double b = uniform(seed, purpose, int64_t(i) + 1) * 2 - 1;
    double s = f * f * (3 - 2 * f);
    return a + (b - a) * s;
}
double phi(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }
}  // namespace GlitchRandom

// ---- knobs ---------------------------------------------------------------------------
Knobs::Knobs(const GlitchSettings &st, const ReceiverRaster &raster) : settings(st) {
    double s = raster.scale();
    double strength = std::min(1.0, std::max(0.0, st[kSignalStrength]));
    sigma = std::max(0.0, 100 * std::pow(10.0, -1.9 * strength) - 1.26 * std::pow(strength, 4));
    agc = std::min(1.0, strength / 0.45);
    double vKnob = st[kVerticalHold];
    double builtIn = 0.002;
    vTotal = builtIn + (vKnob < 0 ? -1 : 1) * 0.10 * std::pow(std::abs(vKnob), 1.37);
    vCenter = std::asin(builtIn / vLock) / (2 * kPi);
    vBeta = 1 - std::exp(-1 / (1.5 * s));
    double hKnob = std::abs(st[kHorizontalHold]);
    double kpN = 0.04 * std::pow(10.0, std::min(1.0, std::max(0.0, st[kAfcSpeed])));
    double edge = 0.04 * std::pow(10.0, 0.5) / (2 * kPi);
    double hFree = hKnob <= 0.35 ? edge * std::pow(hKnob / 0.35, 2) : edge + 0.0265 * (hKnob - 0.35) / 0.65;
    hR = (st[kHorizontalHold] < 0 ? -1 : 1) * hFree / s;
    kp = kpN / s;
    ki = std::pow(kpN / 1.4, 2) / (s * s);
    uMax = 0;
    humIRE = st[kHum] * 60;
    headSwitch = st[kHeadSwitch];
    jitter = st[kTimebaseJitter];
    crinkle = st[kCrinkle];
    clog = st[kHeadClog];
    search = std::max(1.0, st[kSearchSpeed]);
    tracking = std::min(1.0, std::max(0.0, st[kTracking]));
    dropouts = st[kDropouts];
    captions = st.flag(kClosedCaptions);
    gateAlpha = 1 - std::exp(-1 / (6 * s));
    chromaAlpha = 1 - std::exp(-1 / (4 * s));
    accGamma = 1 - std::exp(-1 / (8 * s));
    killerKappa = 1 - std::exp(-1 / (15 * s));
}

void ChromaReference::step(double gi, double gq, const Knobs &k) {
    using namespace NTSCTiming;
    double b0 = burstAmplitude;
    double mag = std::sqrt(gi * gi + gq * gq);
    double c = std::cos(-phase), s = std::sin(-phase);
    double ri = gi * c - gq * s;
    double rq = gi * s + gq * c;
    double dot = ri * burstI + rq * burstQ;
    double cross = burstI * rq - burstQ * ri;
    if (mag > 0) phase += k.chromaAlpha * std::min(1.0, mag / b0) * std::atan2(cross, dot);
    double c2 = std::cos(-phase), s2 = std::sin(-phase);
    double inPhase = (gi * c2 - gq * s2) * burstI + (gi * s2 + gq * c2) * burstQ;
    gain += k.accGamma * (b0 / std::max(inPhase, 0.25 * b0) - gain);
    gain = std::min(3.0, std::max(0.3, gain));
    killer += k.killerKappa * (std::min(1.0, std::max(-1.0, inPhase / b0)) - killer);
    if (colorOn && killer < 0.3) colorOn = false;
    else if (!colorOn && killer > 0.5) colorOn = true;
}

// ---- simulator ---------------------------------------------------------------------------
ReceiverSimulator::ReceiverSimulator(const ReceiverRaster &raster, uint64_t seed, Uncalibrated)
    : raster_(raster),
      seed_(seed),
      L_(raster.totalLines()),
      usPerSecond_(NTSCTiming::line * double(raster.totalLines()) * NTSCTiming::fieldRate),
      logCap_(4 * raster.totalLines() + 64),
      knobs_(GlitchSettings(), raster) {
    thetaLog_.assign(logCap_, 0.0);
    gateLog_.assign(logCap_, 0.0);
    nominalTrigger_ = 0;
}

ReceiverSimulator::ReceiverSimulator(const ReceiverRaster &raster, uint64_t seed)
    : ReceiverSimulator(raster, seed, Uncalibrated{}) {
    nominalTrigger_ = calibrate(raster, seed);
    reset();
}

int ReceiverSimulator::calibrate(const ReceiverRaster &raster, uint64_t seed) {
    ReceiverSimulator sim(raster, seed, Uncalibrated{});
    sim.advance(0, GlitchSettings());
    if (sim.triggers_.empty()) return raster.preEqEnd + 2;
    return int(posmod(sim.triggers_.back(), int64_t(raster.totalLines())));
}

double ReceiverSimulator::time() const { return (double(r_) * H_ + theta_) / usPerSecond_ - warmup; }

void ReceiverSimulator::reset() {
    r_ = 0;
    theta_ = 0;
    integ_ = 0;
    vPhase_ = 0;
    vInteg_ = 0;
    vAbove_ = false;
    gateErr_ = 0;
    triggers_.clear();
    std::fill(thetaLog_.begin(), thetaLog_.end(), 0.0);
    std::fill(gateLog_.begin(), gateLog_.end(), 0.0);
}

ReceiverSimulator::Snapshot ReceiverSimulator::snapshot() const {
    Snapshot s;
    s.time = time();
    s.r = r_;
    s.theta = theta_;
    s.integ = integ_;
    s.vPhase = vPhase_;
    s.vInteg = vInteg_;
    s.vAbove = vAbove_;
    s.gateErr = gateErr_;
    s.gateLog = gateLog_;
    s.triggers = triggers_;
    s.thetaLog = thetaLog_;
    return s;
}

void ReceiverSimulator::restore(const Snapshot &s) {
    r_ = s.r;
    theta_ = s.theta;
    integ_ = s.integ;
    vPhase_ = s.vPhase;
    vInteg_ = s.vInteg;
    vAbove_ = s.vAbove;
    gateErr_ = s.gateErr;
    gateLog_ = s.gateLog;
    triggers_ = s.triggers;
    thetaLog_ = s.thetaLog;
}

void ReceiverSimulator::advance(double t, const GlitchSettings &settings) {
    knobs_ = Knobs(settings, raster_);
    double target = (t + warmup) * usPerSecond_;
    while (double(r_) * H_ + theta_ <= target) step();
}

void ReceiverSimulator::resimulate(double t, double frameDuration,
                                   const std::function<GlitchSettings(double)> &settingsAt,
                                   const Snapshot *resumeFrom,
                                   const std::function<void(const Snapshot &)> &checkpoint) {
    int64_t k = 1;
    if (resumeFrom && frameDuration > 0) {
        restore(*resumeFrom);
        k = roundI(resumeFrom->time / frameDuration) + 1;
    } else {
        reset();
        advance(0, settingsAt(0));
    }
    if (!(t > 0) || !(frameDuration > 0)) return;
    double nextCheckpoint = std::floor(time() + 1);
    while (true) {
        double tk = std::min(t, double(k) * frameDuration);
        advance(tk, settingsAt(tk));
        if (tk >= t) break;
        if (checkpoint && tk >= nextCheckpoint && tk == double(k) * frameDuration) {
            checkpoint(snapshot());
            nextCheckpoint = std::floor(tk) + 1;
        }
        k += 1;
    }
}

// ---- one scan line ---------------------------------------------------------------------------
void ReceiverSimulator::step() {
    const Knobs &k = knobs_;
    const double H = H_;
    double tau = double(r_) * H + theta_;

    int64_t j = roundI(tau / H);
    double e = timebaseError(j);
    double edge = double(j) * H + e;
    if (tau - edge > H / 2) {
        j += 1;
        e = timebaseError(j);
        edge = double(j) * H + e;
    } else if (edge - tau > H / 2) {
        j -= 1;
        e = timebaseError(j);
        edge = double(j) * H + e;
    }
    int jf = int(posmod(j, int64_t(L_)));
    double loss = signalLoss(j);
    double sigma = noiseSigma(loss);
    double sigmaSep = sigma * 0.35;
    double humNow = hum(tau);
    double humShift = 0.4 * humNow;
    double syncMargin = 20 - humShift;
    double blankMargin = 20 + humShift;
    double pDetect = sigmaSep > 0 ? GlitchRandom::phi(syncMargin / sigmaSep) : (syncMargin > 0 ? 1 : 0);
    double pNoisePeak = sigmaSep > 0 ? GlitchRandom::phi(-blankMargin / sigmaSep) : (blankMargin > 0 ? 0 : 1);
    double syncPresent = 1 - std::min(1.0, loss * loss * 1.2);

    std::optional<double> detected;
    double roll = GlitchRandom::uniform(seed_, 11, r_);
    double pFalse = std::min(1.0, 10 * pNoisePeak);
    if (roll < pFalse) {
        detected = (GlitchRandom::uniform(seed_, 12, r_) - 0.5) * H * 0.6;
    } else if (GlitchRandom::uniform(seed_, 13, r_) < pDetect * syncPresent) {
        double d = edge - tau;
        d += GlitchRandom::gauss(seed_, 14, r_) * sigmaSep * 0.0125;
        d += humNow * 0.05;
        detected = d;
    }

    double next = theta_ + H * k.hR + integ_;
    if (detected) {
        double d0 = *detected;
        bool linear = std::abs(d0) <= H / 4;
        double d = (H / (2 * kPi)) * std::sin(2 * kPi * d0 / H);
        next += k.kp * d;
        if (linear) integ_ = std::min(k.uMax, std::max(-k.uMax, integ_ + k.ki * d));
    }

    double duty = raster_.syncDuty(jf) * syncPresent;
    double measured = duty * pDetect + (1 - duty) * pNoisePeak;
    double variance = (duty * pDetect * (1 - pDetect) + (1 - duty) * pNoisePeak * (1 - pNoisePeak)) / 60;
    if (variance > 0) measured += GlitchRandom::gauss(seed_, 15, r_) * std::sqrt(variance);
    vInteg_ += (measured - vInteg_) * k.vBeta;
    bool vsync = false;
    if (vAbove_) {
        if (vInteg_ < 0.25) vAbove_ = false;
    } else if (vInteg_ >= 0.45) {
        vAbove_ = true;
        vsync = true;
    }

    vPhase_ += 1 + (next - theta_) / H;
    double period = double(L_) * (1 + k.vTotal);
    if (vsync) {
        double ev = vPhase_ / period - k.vCenter;
        ev -= std::round(ev);
        vPhase_ -= k.vLock * period * std::sin(2 * kPi * ev);
    }
    if (vPhase_ >= period) {
        vPhase_ -= period;
        triggers_.push_back(r_);
        if (triggers_.size() > 16) triggers_.erase(triggers_.begin());
    }

    if (detected) {
        double d = (edge - tau) - gateErr_;
        d -= H * std::round(d / H);
        gateErr_ += k.gateAlpha * d;
        gateErr_ -= H * std::round(gateErr_ / H);
    }
    size_t li = size_t(posmod(r_, int64_t(logCap_)));
    thetaLog_[li] = theta_;
    gateLog_[li] = gateErr_;
    theta_ = next;
    r_ += 1;
}

// ---- the field to show ------------------------------------------------------------------------
GlitchFieldPlan ReceiverSimulator::plan() const {
    const Knobs &k = knobs_;
    const double H = H_;
    const int A = raster_.activeLines;
    const int64_t offset = int64_t(raster_.vbiLines - nominalTrigger_);
    const int64_t lastDrawn = r_ - 1;
    int64_t scanStart = lastDrawn - offset - int64_t(A) + 1;
    for (auto it = triggers_.rbegin(); it != triggers_.rend(); ++it) {
        if (*it + offset + int64_t(A) - 1 <= lastDrawn) {
            scanStart = *it;
            break;
        }
    }
    const int64_t first = scanStart + offset;

    GlitchFieldPlan p;
    p.raster = raster_;
    p.rows.reserve(A);
    for (int y = 0; y < A; y++) {
        int64_t tvLine = first + y;
        double tau = double(tvLine) * H + loggedTheta(tvLine);
        auto [j, u0] = signalLine(tau);
        double loss = signalLoss(j);
        GlitchRow row;
        row.u0 = float(u0);
        row.fieldLine = int32_t(posmod(j, int64_t(L_)));
        row.lenPrev = float(H + timebaseError(j) - timebaseError(j - 1));
        row.lenCur = float(H + timebaseError(j + 1) - timebaseError(j));
        row.lenNext = float(H + timebaseError(j + 2) - timebaseError(j + 1));
        row.noiseIRE = float(k.sigma);
        row.humIRE = float(hum(tau));
        row.burstScale = float(std::max(0.0, 1 - loss));
        row.tapeLoss = float(std::min(1.0, std::max(0.0, loss)));
        row.gateShift = float(loggedGate(tvLine));
        p.rows.push_back(row);
    }

    ChromaReference chroma;
    int warm = std::max(4, int(roundI(12 * raster_.scale())));
    for (int w = warm; w > 0; w--) {
        int64_t tvLine = first - w;
        if (inRetrace(tvLine)) continue;
        double tau = double(tvLine) * H + loggedTheta(tvLine);
        auto [j, u0] = signalLine(tau);
        int jf = int(posmod(j, int64_t(L_)));
        double gi = 0, gq = 0;
        double g0 = u0 + loggedGate(tvLine);
        if (raster_.hasBurst(jf)) {
            double lo = std::max(g0 + NTSCTiming::gateStart, NTSCTiming::burstStart);
            double hi = std::min(g0 + NTSCTiming::gateEnd, NTSCTiming::burstEnd);
            double overlap = std::max(0.0, hi - lo) / (NTSCTiming::burstEnd - NTSCTiming::burstStart);
            double amp = NTSCTiming::burstAmplitude * overlap * k.agc * std::max(0.0, 1 - signalLoss(j));
            gi = amp * NTSCTiming::burstI;
            gq = amp * NTSCTiming::burstQ;
        }
        double n = noiseSigma(signalLoss(j)) / 92.5 * 0.45;
        if (n > 0) {
            gi += GlitchRandom::gauss(seed_, 21, tvLine) * n;
            gq += GlitchRandom::gauss(seed_, 22, tvLine) * n;
        }
        chroma.step(gi, gq, k);
    }

    int64_t topSignalLine = signalLine(double(first) * H + loggedTheta(first)).first;
    int64_t signalField = floordiv(topSignalLine, int64_t(L_));
    p.dropouts = dropouts(signalField);
    p.chromaPhase = float(chroma.phase);
    p.chromaGain = float(chroma.gain);
    p.killer = float(chroma.killer);
    p.colorOn = chroma.colorOn;
    p.pictureGain = float(k.agc);
    p.ccBits = captionBits(signalField);
    p.fieldIndex = uint32_t(uint64_t(scanStart));
    p.settings = k.settings;
    return p;
}

bool ReceiverSimulator::inRetrace(int64_t tvLine) const {
    int64_t retrace = int64_t(std::max(2.0, std::round(9 * raster_.scale())));
    for (int64_t t : triggers_)
        if (tvLine >= t && tvLine < t + retrace) return true;
    return false;
}

double ReceiverSimulator::loggedGate(int64_t tvLine) const {
    if (!(tvLine >= 0 && tvLine < r_ && r_ - tvLine <= int64_t(logCap_))) return gateErr_;
    return gateLog_[size_t(posmod(tvLine, int64_t(logCap_)))];
}

double ReceiverSimulator::loggedTheta(int64_t tvLine) const {
    if (!(tvLine >= 0 && tvLine < r_ && r_ - tvLine <= int64_t(logCap_))) return theta_;
    return thetaLog_[size_t(posmod(tvLine, int64_t(logCap_)))];
}

std::pair<int64_t, double> ReceiverSimulator::signalLine(double tau) const {
    const double H = H_;
    int64_t j = int64_t(std::floor(tau / H));
    if (tau < double(j) * H + timebaseError(j)) j -= 1;
    else if (tau >= double(j + 1) * H + timebaseError(j + 1)) j += 1;
    return {j, tau - (double(j) * H + timebaseError(j))};
}

// ---- the signal ---------------------------------------------------------------------------
double ReceiverSimulator::fieldTime(int64_t j) const { return double(j) / (double(L_) * NTSCTiming::fieldRate); }

int ReceiverSimulator::head(int64_t j) const {
    int64_t switchLine = int64_t(L_) - roundI(3.5 * raster_.scale());
    int64_t f = floordiv(j, int64_t(L_));
    int64_t jf = posmod(j, int64_t(L_));
    return int(posmod(f + (jf >= switchLine ? 1 : 0), 2));
}

double ReceiverSimulator::timebaseError(int64_t j) const {
    const Knobs &k = knobs_;
    double e = 0;
    if (k.headSwitch > 0) {
        int64_t f = floordiv(j + roundI(3.5 * raster_.scale()), int64_t(L_));
        double wobble = 1 + 0.15 * GlitchRandom::gauss(seed_, 31, f);
        e += (head(j) == 0 ? 0.5 : -0.5) * k.headSwitch * wobble;
    }
    if (k.jitter > 0) {
        double lineScale = 1 / raster_.scale();
        double slow = GlitchRandom::value(seed_, 32, double(j) * lineScale / 40);
        double fast = GlitchRandom::gauss(seed_, 33, j) * 0.35;
        e += k.jitter * (slow + fast);
    }
    if (k.search > 1.05) {
        int64_t crossing = int64_t(searchAt(j).second);
        int64_t f = floordiv(j, int64_t(L_));
        e += 1.2 * GlitchRandom::gauss(seed_, 36, f, crossing);
    }
    if (k.crinkle > 0) {
        auto [intensity, envelope] = crinkleAt(j);
        if (intensity > 0) {
            double lineScale = 1 / raster_.scale();
            e += intensity * 4.0 * GlitchRandom::value(seed_, 34, double(j) * lineScale / 3);
            e += envelope * 1.5 * GlitchRandom::value(seed_, 35, double(j) * lineScale / 60);
        }
    }
    return e;
}

double ReceiverSimulator::signalLoss(int64_t j) const {
    const Knobs &k = knobs_;
    double loss = 0;
    if (k.clog > 0) {
        int64_t f = floordiv(j, int64_t(L_));
        bool dropped = GlitchRandom::uniform(seed_, 41, f) < k.clog * 0.8;
        double level = dropped ? 0.85 + 0.15 * GlitchRandom::uniform(seed_, 42, f) : 0.35 * k.clog * k.clog;
        loss = std::max(loss, std::min(1.0, level));
    }
    if (k.crinkle > 0) loss = std::max(loss, crinkleAt(j).first);
    if (k.search > 1.05) loss = std::max(loss, searchAt(j).first);
    if (k.tracking > 0) loss = std::max(loss, trackingAt(j));
    return loss;
}

double ReceiverSimulator::trackingAt(int64_t j) const {
    double e = knobs_.tracking;
    double pos = double(posmod(j, int64_t(L_))) / double(L_);
    double wander = 0.03 * GlitchRandom::value(seed_, 37, fieldTime(j) * 1.7);
    double center = 1 - 0.55 * e + wander;
    double width = 0.02 + 0.10 * e;
    double d = std::abs(pos - center);
    d = std::min(d, 1 - d);
    return std::min(1.0, std::max(0.0, 1 - d / width) * std::min(1.0, 0.4 + e));
}

std::pair<double, int> ReceiverSimulator::searchAt(int64_t j) const {
    double bars = knobs_.search - 1;
    double pos = double(posmod(j, int64_t(L_))) / double(L_);
    double drift = std::fmod(fieldTime(j) * 0.23, 1.0);
    double x = pos * bars + drift;
    double phase = x - std::floor(x);
    double d = std::min(phase, 1 - phase);
    double loss = std::min(1.0, std::max(0.0, 1 - d / 0.15) * 1.2);
    return {loss, int(std::floor(x))};
}

double ReceiverSimulator::noiseSigma(double loss) const {
    double tape = loss * 90;
    return std::sqrt(knobs_.sigma * knobs_.sigma + tape * tape);
}

double ReceiverSimulator::hum(double tau) const {
    if (!(knobs_.humIRE > 0)) return 0;
    double t = tau / usPerSecond_;
    return knobs_.humIRE * (0.8 * std::sin(2 * kPi * 60 * t) + 0.3 * std::sin(2 * kPi * 120 * t + 0.7));
}

std::pair<double, double> ReceiverSimulator::crinkleAt(int64_t j) const {
    double t = fieldTime(j);
    const double slot = 0.5;
    double p = knobs_.crinkle * 0.6;
    int64_t s = int64_t(std::floor(t / slot));
    double bestI = 0, bestE = 0;
    for (int64_t k = s - 1; k <= s; k++) {
        if (!(GlitchRandom::uniform(seed_, 51, k) < p)) continue;
        double start = double(k) * slot + GlitchRandom::uniform(seed_, 52, k) * slot;
        double duration = 0.12 + 0.35 * GlitchRandom::uniform(seed_, 53, k);
        if (!(t >= start && t < start + duration)) continue;
        double severity = 0.55 + 0.45 * GlitchRandom::uniform(seed_, 54, k);
        double progress = (t - start) / duration;
        double envelope = std::sin(kPi * progress);
        double pos = double(posmod(j, int64_t(L_))) / double(L_);
        double speed = 0.8 + 1.6 * GlitchRandom::uniform(seed_, 55, k);
        double center = std::fmod(GlitchRandom::uniform(seed_, 56, k) + progress * speed, 1.0);
        double d = std::abs(pos - center);
        d = std::min(d, 1 - d);
        double width = 0.05 + 0.08 * severity;
        double band = std::max(0.0, 1 - d / width);
        double intensity = std::min(1.0, severity * envelope * band * 1.4);
        if (intensity > bestI) {
            bestI = intensity;
            bestE = envelope;
        } else if (envelope > bestE) {
            bestE = envelope;
        }
    }
    return {bestI, bestE};
}

std::vector<GlitchDropout> ReceiverSimulator::dropouts(int64_t f) const {
    double rate = knobs_.dropouts;
    std::vector<GlitchDropout> out;
    if (!(rate > 0)) return out;
    double mean = rate * rate * 14;
    int n = int(std::max<int64_t>(
        0, std::min<int64_t>(64, roundI(mean + std::sqrt(mean) * GlitchRandom::gauss(seed_, 61, f)))));
    out.reserve(n);
    for (int i = 0; i < n; i++) {
        int line = raster_.vbiLines + int(GlitchRandom::uniform(seed_, 62, f, i) * double(raster_.activeLines));
        double start = NTSCTiming::activeStart + GlitchRandom::uniform(seed_, 63, f, i) * NTSCTiming::activeLength * 0.95;
        double length = std::min(
            30.0, std::max(0.4, -std::log(std::max(1e-6, GlitchRandom::uniform(seed_, 64, f, i))) * (1.5 + 5 * rate)));
        int span = 1 + int(std::pow(GlitchRandom::uniform(seed_, 65, f, i), 2) * 4);
        for (int k = 0; k < span; k++) {
            if (!(out.size() < 64)) continue;
            int l = line + k;
            if (!(l < raster_.totalLines())) break;
            double drift = GlitchRandom::gauss(seed_, 66, f, int64_t(i * 8 + k)) * 0.8;
            double scale = 0.7 + 0.6 * GlitchRandom::uniform(seed_, 67, f, int64_t(i * 8 + k));
            GlitchDropout d;
            d.fieldLine = int32_t(l);
            d.uStart = float(start + drift);
            d.uLength = float(length * scale);
            out.push_back(d);
        }
    }
    return out;
}

uint32_t ReceiverSimulator::captionBits(int64_t f) const {
    if (!knobs_.captions) return 0;
    auto ch = [&](int64_t i) -> uint32_t {
        uint32_t c = uint32_t(0x20 + int(GlitchRandom::uniform(seed_, 71, f, i) * 90));
        int bits = 0;
        for (uint32_t x = c; x; x &= x - 1) bits++;
        uint32_t parity = bits % 2 == 0 ? 0x80u : 0u;
        return c | parity;
    };
    return 0b100u | (ch(0) << 3) | (ch(1) << 11);
}

}  // namespace ntscrt::glitch
