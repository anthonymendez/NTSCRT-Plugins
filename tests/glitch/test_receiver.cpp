// Port of upstream Tests/CrtCoreTests/ReceiverSimulatorTests.swift: the
// receiver model's physics, checked without a GPU.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include "stages/glitch/receiver.h"

using namespace ntscrt::glitch;

namespace {

const ReceiverRaster kRaster(320, 240);
const double H = NTSCTiming::line;
const double kFrame = 1.0 / 30;

GlitchSettings settings(std::initializer_list<std::pair<const char *, double>> values) {
    GlitchSettings s;
    for (auto &[k, v] : values) REQUIRE(s.set(k, v));
    return s;
}

// A row's horizontal offset wrapped to (-1/2, 1/2] line.
double offset(const GlitchRow &row) {
    double u = row.u0;
    return u > double(row.lenCur) / 2 ? u - double(row.lenCur) : u;
}

std::vector<GlitchFieldPlan> plans(const GlitchSettings &s, double start = 1.0, int count = 1,
                                   const ReceiverRaster &raster = kRaster) {
    ReceiverSimulator sim(raster);
    std::vector<GlitchFieldPlan> out;
    for (int i = 0; i < count; i++) {
        sim.advance(start + double(i) * kFrame, s);
        out.push_back(sim.plan());
    }
    return out;
}

std::vector<int> topLines(const std::vector<GlitchFieldPlan> &ps) {
    const int L = kRaster.totalLines();
    std::vector<int> out;
    int unwrap = 0;
    bool havePrev = false;
    int prev = 0;
    for (auto &p : ps) {
        int line = p.rows[0].fieldLine;
        if (havePrev) {
            if (line - prev > L / 2) unwrap -= L;
            else if (prev - line > L / 2) unwrap += L;
        }
        out.push_back(line + unwrap);
        prev = line;
        havePrev = true;
    }
    return out;
}

double ragged_rms(const GlitchFieldPlan &p) {
    std::vector<double> u;
    for (auto &r : p.rows) u.push_back(offset(r));
    double mean = std::accumulate(u.begin(), u.end(), 0.0) / double(u.size());
    double acc = 0;
    for (double x : u) acc += (x - mean) * (x - mean);
    return std::sqrt(acc / double(u.size()));
}

double spread(const GlitchFieldPlan &p) {
    double lo = 1e9, hi = -1e9;
    for (auto &r : p.rows) {
        lo = std::min(lo, offset(r));
        hi = std::max(hi, offset(r));
    }
    return hi - lo;
}

}  // namespace

TEST_CASE("raster proportions") {
    CHECK(kRaster.vbiLines == 20);
    CHECK(kRaster.totalLines() == 260);
    CHECK(kRaster.ccLine() == 19);
}

TEST_CASE("healthy set shows the picture exactly") {
    ReceiverSimulator sim(kRaster);
    for (double t : {0.0, 0.5, 1.0, 2.37, 7.9}) {
        sim.advance(t, GlitchSettings());
        auto p = sim.plan();
        REQUIRE(int(p.rows.size()) == kRaster.activeLines);
        for (int y = 0; y < int(p.rows.size()); y++) {
            const auto &row = p.rows[y];
            CHECK(row.u0 == 0);
            CHECK(row.fieldLine == kRaster.vbiLines + y);
            CHECK(row.noiseIRE == 0);
            CHECK(row.humIRE == 0);
            CHECK(row.burstScale == 1);
        }
        CHECK(std::abs(p.chromaPhase) <= 1e-9);
        CHECK(std::abs(p.chromaGain - 1) <= 1e-9);
        CHECK(p.colorOn);
        CHECK(p.pictureGain == 1);
        CHECK(p.dropouts.empty());
    }
}

TEST_CASE("identity holds at other raster sizes") {
    for (auto [w, h] : std::vector<std::pair<int, int>>{{256, 332}, {320, 180}, {640, 480}, {1920, 1080}}) {
        ReceiverRaster r(w, h);
        ReceiverSimulator sim(r);
        sim.advance(1.3, GlitchSettings());
        auto p = sim.plan();
        bool ok = true;
        for (int y = 0; y < int(p.rows.size()); y++)
            ok = ok && p.rows[y].u0 == 0 && p.rows[y].fieldLine == r.vbiLines + y;
        CHECK_MESSAGE(ok, w << "x" << h);
    }
}

TEST_CASE("vertical hold near center locks and slides") {
    std::map<double, int> shifts;
    for (double knob : {-0.15, -0.05, 0.0, 0.05, 0.15}) {
        auto tops = topLines(plans(settings({{"vertical_hold", knob}}), 1.0, 30));
        CHECK_MESSAGE(std::set<int>(tops.begin(), tops.end()).size() == 1, "knob " << knob);
        int shift = tops[0] - kRaster.vbiLines;
        const int L = kRaster.totalLines();
        if (shift > L / 2) shift -= L;
        else if (shift < -L / 2) shift += L;
        shifts[knob] = shift;
    }
    CHECK(shifts[0.0] == 0);
    CHECK(std::abs(shifts[0.05]) > 1);
    CHECK(std::abs(shifts[0.15]) > std::abs(shifts[0.05]));
    CHECK((shifts[0.15] > 0) != (shifts[-0.15] > 0));
}

TEST_CASE("vertical hold rolls about equally both ways") {
    auto rate = [](double knob) {
        auto t = topLines(plans(settings({{"vertical_hold", knob}}), 1.0, 40));
        return double(t.back() - t.front()) / 39;
    };
    double up = rate(0.5), down = rate(-0.5);
    CHECK(std::abs(up) > 3);
    CHECK(std::abs(std::abs(up) - std::abs(down)) <= std::abs(up) * 0.35);
}

TEST_CASE("vertical hold past lock rolls both ways") {
    auto up = topLines(plans(settings({{"vertical_hold", 0.6}}), 1.0, 20));
    auto down = topLines(plans(settings({{"vertical_hold", -0.6}}), 1.0, 20));
    int dUp = up.back() - up.front();
    int dDown = down.back() - down.front();
    CHECK(std::abs(dUp) > 30);
    CHECK(std::abs(dDown) > 30);
    CHECK((dUp > 0) != (dDown > 0));
}

TEST_CASE("roll speed grows with detuning") {
    auto speed = [](double knob) {
        auto t = topLines(plans(settings({{"vertical_hold", knob}}), 1.0, 30));
        return std::abs(double(t.back() - t.front()));
    };
    CHECK(speed(0.35) < speed(0.9));
}

TEST_CASE("horizontal hold centered is exact") {
    auto p = plans(settings({{"horizontal_hold", 0.0}}))[0];
    for (auto &r : p.rows) CHECK(r.u0 == 0);
}

TEST_CASE("horizontal hold slides the picture before it tears") {
    auto p = plans(settings({{"horizontal_hold", 0.25}}))[0];
    CHECK(spread(p) < 0.5);
    double mean = 0;
    for (auto &r : p.rows) mean += offset(r);
    mean /= double(p.rows.size());
    CHECK(std::abs(mean) > 1.0);
    CHECK(std::abs(mean) < H / 4 + 1);
}

TEST_CASE("horizontal hold far out tears") {
    auto p = plans(settings({{"horizontal_hold", 0.95}}))[0];
    int slips = 0;
    for (size_t y = 1; y < p.rows.size(); y++) {
        int step = p.rows[y].fieldLine - p.rows[y - 1].fieldLine;
        if (step != 1 && step != 1 - kRaster.totalLines()) slips++;
    }
    CHECK(slips >= 3);
    float lo = 1e9f, hi = -1e9f;
    for (auto &r : p.rows) {
        lo = std::min(lo, r.u0);
        hi = std::max(hi, r.u0);
    }
    CHECK(double(hi - lo) > H / 2);
}

TEST_CASE("head switch jump bends the top of the picture") {
    auto slow = plans(settings({{"head_switch", 4}, {"afc_speed", 0}}), 1.0, 6);
    auto fast = plans(settings({{"head_switch", 4}, {"afc_speed", 1}}), 1.0, 6);
    auto topBend = [](const std::vector<GlitchFieldPlan> &ps) {
        double m = 0;
        for (auto &p : ps) {
            double base = offset(p.rows[kRaster.activeLines / 2]);
            m = std::max(m, std::abs(offset(p.rows[0]) - base));
        }
        return m;
    };
    CHECK(topBend(slow) > 1.0);
    CHECK(topBend(fast) < topBend(slow) / 3);
    const GlitchFieldPlan *p = &slow[0];
    for (auto &q : slow)
        if (std::abs(offset(q.rows[0])) > std::abs(offset(p->rows[0]))) p = &q;
    double mid = offset(p->rows[kRaster.activeLines / 2]);
    CHECK(std::abs(offset(p->rows[0]) - mid) > std::abs(offset(p->rows[40]) - mid));
}

TEST_CASE("weak signal breaks horizontal sync") {
    auto ragged = [](double strength) { return ragged_rms(plans(settings({{"signal_strength", strength}}), 1.5)[0]); };
    CHECK(ragged(0.45) < 0.3);
    CHECK(ragged(0.06) > 2.0);
}

TEST_CASE("snow follows signal strength") {
    float strong = plans(settings({{"signal_strength", 1.0}}))[0].rows[0].noiseIRE;
    float medium = plans(settings({{"signal_strength", 0.5}}))[0].rows[0].noiseIRE;
    float weak = plans(settings({{"signal_strength", 0.2}}))[0].rows[0].noiseIRE;
    CHECK(strong == 0);
    CHECK(medium > 1);
    CHECK(weak > medium * 3);
    float nearlyFull = plans(settings({{"signal_strength", 0.9}}))[0].rows[0].noiseIRE;
    CHECK(nearlyFull > 0.5);
}

TEST_CASE("full hum breaks sync in its bars but half does not") {
    auto ragged = [](double hum) { return spread(plans(settings({{"hum", hum}}), 1.7)[0]); };
    CHECK(ragged(0.5) < 5);
    CHECK(ragged(1.0) > 10);
}

TEST_CASE("horizontal hold acts from the first few percent") {
    auto p = plans(settings({{"horizontal_hold", 0.08}}))[0];
    CHECK(std::abs(offset(p.rows[kRaster.activeLines / 2])) > 0.5);
}

TEST_CASE("head clog drops fields at any frame rate") {
    for (double fps : {24.0, 30.0, 60.0}) {
        ReceiverSimulator sim(kRaster);
        auto s = settings({{"head_clog", 0.7}});
        bool lost = false, plays = false;
        for (int i = 0; i < 24; i++) {
            sim.advance(1 + double(i) / fps, s);
            float l = sim.plan().rows[kRaster.activeLines / 2].tapeLoss;
            lost = lost || l > 0.7f;
            plays = plays || l < 0.3f;
        }
        CHECK_MESSAGE(lost, fps << " fps: some fields lost");
        CHECK_MESSAGE(plays, fps << " fps: others play");
    }
}

TEST_CASE("same moment always looks the same") {
    auto s = settings({{"vertical_hold", 0.5}, {"horizontal_hold", 0.7}, {"signal_strength", 0.3},
                       {"crinkle", 0.8}, {"head_switch", 3}, {"timebase_jitter", 1}, {"dropouts", 0.5}});
    ReceiverSimulator a(kRaster);
    double t = 0;
    while (t < 1.3 - 1e-9) {
        t += kFrame;
        a.advance(std::min(t, 1.3), s);
    }
    ReceiverSimulator b(kRaster);
    b.resimulate(1.3, kFrame, [&](double) { return s; });
    ReceiverSimulator c(kRaster);
    c.advance(1.3, s);
    auto pa = a.plan(), pb = b.plan(), pc = c.plan();
    CHECK(pa.rows == pb.rows);
    CHECK(pa.rows == pc.rows);
    CHECK(pa.dropouts == pb.dropouts);
    CHECK(pa.ccBits == pc.ccBits);
}

TEST_CASE("search shows one fewer noise bar than the speed") {
    for (double speed : {3.0, 5.0}) {
        ReceiverSimulator sim(kRaster);
        sim.advance(1.0, settings({{"search_speed", speed}}));
        auto rows = sim.plan().rows;
        int bands = 0;
        for (size_t y = 1; y < rows.size(); y++)
            if (rows[y].tapeLoss > 0.5f && !(rows[y - 1].tapeLoss > 0.5f)) bands++;
        if (rows[0].tapeLoss > 0.5f) bands++;
        CHECK_MESSAGE(std::abs(double(bands) - (speed - 1)) <= 1, speed << "x search: " << bands);
    }
    ReceiverSimulator normal(kRaster);
    normal.advance(1.0, settings({{"search_speed", 1}}));
    for (auto &r : normal.plan().rows) CHECK(r.tapeLoss == 0);
}

TEST_CASE("tracking band climbs into the picture as the error grows") {
    auto noisyRows = [](double e) {
        ReceiverSimulator sim(kRaster);
        sim.advance(1.0, settings({{"tracking", e}}));
        std::vector<int> out;
        auto rows = sim.plan().rows;
        for (int y = 0; y < int(rows.size()); y++)
            if (rows[y].tapeLoss > 0.5f) out.push_back(y);
        return out;
    };
    CHECK(noisyRows(0).empty());
    auto mild = noisyRows(0.3), bad = noisyRows(0.9);
    CHECK_FALSE(bad.empty());
    double mildCenter = mild.empty() ? double(kRaster.activeLines)
                                     : std::accumulate(mild.begin(), mild.end(), 0.0) / double(mild.size());
    double badCenter = std::accumulate(bad.begin(), bad.end(), 0.0) / double(bad.size());
    CHECK(badCenter < mildCenter);
    CHECK(bad.size() > mild.size());
}

TEST_CASE("snapshot restore continues bit-exactly") {
    auto s = settings({{"vertical_hold", 0.5}, {"horizontal_hold", 0.65}, {"crinkle", 0.7}, {"head_switch", 3}});
    ReceiverSimulator a(kRaster);
    a.advance(2.0, s);
    auto snap = a.snapshot();
    a.advance(3.1, s);
    ReceiverSimulator b(kRaster);
    b.restore(snap);
    b.advance(3.1, s);
    CHECK(a.plan().rows == b.plan().rows);
    CHECK(a.time() == b.time());
}

TEST_CASE("different seeds give different glitches, same seed the same") {
    auto s = settings({{"horizontal_hold", 0.95}, {"signal_strength", 0.3}});
    ReceiverSimulator a(kRaster, 1234), b(kRaster, 1234), c(kRaster, 99);
    a.advance(1.0, s);
    b.advance(1.0, s);
    c.advance(1.0, s);
    CHECK(a.plan().rows == b.plan().rows);
    CHECK_FALSE(a.plan().rows == c.plan().rows);
}

TEST_CASE("settings parse upstream keys and fall back to defaults") {
    auto j = nlohmann::json::parse(R"({"glitch":{"enabled":true,"values":{"vertical_hold":0.6,"closed_captions":false,"bogus":3}}})");
    auto s = GlitchSettings::from_look(j);
    CHECK(s[kVerticalHold] == 0.6);
    CHECK(s[kClosedCaptions] == 0);
    CHECK(s[kSignalStrength] == 1);
    CHECK(s[kAfcSpeed] == 0.5);
    CHECK(s[kGhostDelay] == 2);
    CHECK(GlitchSettings::from_look(nlohmann::json::object()) == GlitchSettings());
    GlitchSettings t;
    t[kGhostLevel] = 0.5;  // not a timing knob
    CHECK(t.timing_hash() == GlitchSettings().timing_hash());
    t[kHum] = 0.5;
    CHECK(t.timing_hash() != GlitchSettings().timing_hash());
}
