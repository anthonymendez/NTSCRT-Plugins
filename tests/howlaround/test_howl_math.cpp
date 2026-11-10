// Screen Loop pure math: port of upstream Tests/CrtCoreTests/HowlaroundTests.swift
// (geometry, movement, migration, run-up) plus reference values computed from
// the Swift formulas (an independent transcription, see the commit message).
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <initializer_list>
#include <fstream>

#include "stages/howlaround/howl_counter.h"
#include "stages/howlaround/howl_math.h"

using namespace ntscrt::howl;
using Approx = doctest::Approx;

namespace {

// A plain centered camera unless a test says otherwise (upstream settings(_:)).
Settings settings(std::map<std::string, double> v = {}) {
    std::map<std::string, double> base = {
        {"zoom", 0.5},   {"center_x", 0}, {"center_y", 0},    {"roll", 0},      {"turn", 0},
        {"tilt", 0},     {"shake", 0},    {"seed", 1},        {"drift_x", 0},   {"drift_y", 0},
        {"push", 0},     {"spin", 0},     {"loop", 0},        {"focus", 0},     {"brightness", 1},
        {"contrast", 1}, {"color_drift", 0}, {"hue_drift", 0}, {"auto_exposure", 0}, {"key", 0},
        {"key_invert", 0}, {"delay", 1},  {"counter", 0}};
    for (auto &[k, x] : v) base[k] = x;
    return Settings(base);
}

void same_pose(const Pose &a, const Pose &b, double eps) {
    CHECK(a.zoom == Approx(b.zoom).epsilon(eps));
    CHECK(a.center_x == Approx(b.center_x).epsilon(eps));
    CHECK(a.center_y == Approx(b.center_y).epsilon(eps));
    CHECK(a.roll == Approx(b.roll).epsilon(eps));
    CHECK(a.turn == Approx(b.turn).epsilon(eps));
    CHECK(a.tilt == Approx(b.tilt).epsilon(eps));
}

}  // namespace

TEST_CASE("defaults and parameter table match upstream") {
    CHECK(params().size() == 23);
    auto d = default_values();
    CHECK(d["zoom"] == 0.87);
    CHECK(d["center_x"] == -0.2);
    CHECK(d["center_y"] == -0.22);
    CHECK(d["turn"] == -4);
    CHECK(d["shake"] == 0.4);
    CHECK(d["focus"] == 0.2);
    CHECK(d["contrast"] == 1.05);
    CHECK(d["color_drift"] == -0.3);
    CHECK(d["delay"] == 1);
    Settings s;
    CHECK(s.delay() == 1);
    CHECK(s.seed() == 1);
    CHECK(Settings({{"delay", 37}}).delay() == 20);
    CHECK(Settings({{"delay", 2.5}}).delay() == 3);  // Swift rounded(): half away from zero
    CHECK(Settings({{"zoom", -1}}).zoom() == 0);
}

// MARK: geometry

TEST_CASE("testCenteredScreenMapsTheFrameToTheMiddleHalf") {
    Settings s = settings();
    auto a = s.camera_point({0, 0}, 1), b = s.camera_point({1, 1}, 1);
    REQUIRE(a); REQUIRE(b);
    CHECK(a->x == Approx(0.25).epsilon(1e-9)); CHECK(a->y == Approx(0.25).epsilon(1e-9));
    CHECK(b->x == Approx(0.75).epsilon(1e-9)); CHECK(b->y == Approx(0.75).epsilon(1e-9));
    auto c = settings({{"center_x", 0.1}}).camera_point({0.6, 0.5}, 1.5);
    REQUIRE(c);
    CHECK(c->x == Approx(0.6).epsilon(1e-9));
    auto middle = settings({{"center_x", 0.1}}).camera_point({0.5, 0.5}, 1.5);
    REQUIRE(middle);
    CHECK(middle->x == Approx(0.55).epsilon(1e-9));
}

TEST_CASE("testTheTunnelCenterStaysPutAsTheCameraChanges") {
    for (double aspect : {1.0, 4.0 / 3.0, 16.0 / 9.0})
        for (double zoom : {0.5, 0.87, 0.97, 1.0, 1.2}) {
            Settings s = settings({{"center_x", 0.2}, {"center_y", -0.15}, {"zoom", zoom}, {"turn", -25},
                                   {"tilt", 15}, {"roll", 12}, {"shake", 0.4}, {"drift_x", 0.1},
                                   {"drift_y", -0.06}, {"push", 0.1}});
            for (double t : {-1.0, 0.0, 1.0, 3.0, 5.0}) {
                Pose pose = s.pose(t, 5);
                V2 center{pose.center_x + 0.5, pose.center_y + 0.5};
                auto filmed = Settings::camera_point(center, pose, aspect);
                REQUIRE(filmed);
                CHECK(std::abs(filmed->x - center.x) < 1e-10);
                CHECK(std::abs(filmed->y - center.y) < 1e-10);
            }
        }
}

TEST_CASE("testEvenTheKnobsExtremesKeepTheCenterInFront") {
    for (double zoom : {1.2, 1.3, 1.4})
        for (auto [cx, cy] : std::initializer_list<std::pair<double, double>>{{-0.5, -0.5}, {-0.5, 0.5}, {0.5, -0.5}, {0.5, 0.5}})
            for (auto a : std::initializer_list<std::array<double, 3>>{{-45, -45, -45}, {-45, 45, 45}, {45, -45, 45}, {45, 45, -45}}) {
                Settings s = settings({{"zoom", zoom}, {"center_x", cx}, {"center_y", cy}, {"turn", a[0]},
                                       {"tilt", a[1]}, {"roll", a[2]}});
                V2 center{cx + 0.5, cy + 0.5};
                auto filmed = Settings::camera_point(center, s.base_pose(), 16.0 / 9);
                REQUIRE(filmed);
                CHECK(std::abs(filmed->x - center.x) < 1e-9);
                CHECK(std::abs(filmed->y - center.y) < 1e-9);
            }
}

TEST_CASE("testCopiesFollowTheZoom") {
    CHECK(settings().visible_copies(1, 240) == 6);
    CHECK(settings({{"zoom", 0}}).visible_copies(1, 240) == 0);
    int z3 = *settings({{"zoom", 0.3}}).visible_copies(1, 240);
    int z6 = *settings({{"zoom", 0.6}}).visible_copies(1, 240);
    int z8 = *settings({{"zoom", 0.8}}).visible_copies(1, 240);
    CHECK(z3 < z6);
    CHECK(z6 < z8);
    CHECK_FALSE(settings({{"zoom", 1.2}}).visible_copies(1, 240).has_value());
    int edge = *settings({{"zoom", 0.8}, {"center_x", 0.45}}).visible_copies(1, 240);
    CHECK(edge >= z8 - 2);
}

TEST_CASE("testTheDefaultTunnelConvergesOnEveryFrameShape") {
    for (double aspect : {9.0 / 16, 3.0 / 4, 1.0, 4.0 / 3, 3.0 / 2, 16.0 / 9}) {
        Settings s({{"shake", 0}});
        for (int i = 0; i <= 8; ++i)
            for (int j = 0; j <= 8; ++j) {
                auto rate = s.shrink({i / 8.0, j / 8.0}, aspect);
                REQUIRE(rate);
                CHECK(*rate < 0.99);
            }
        auto copies = s.visible_copies(aspect, 240);
        REQUIRE(copies);
        CHECK(*copies > 25);
    }
}

TEST_CASE("testRunUpCoversTheTunnel") {
    auto plan = settings().run_up_plan(1, 240);
    CHECK(plan.build >= 6 + 4);
    CHECK(plan.settle == 1);
    CHECK(settings({{"zoom", 0}}).run_up_frames(1, 240) == 0);
    auto slow = settings({{"delay", 20}}).run_up_plan(1, 240);
    CHECK(slow.build == plan.build);
    CHECK(slow.settle == 20);
    auto loop = settings({{"delay", 20}, {"loop", 1}}).run_up_plan(1, 240);
    CHECK(loop.settle > 20 * 6);
    // Auto exposure needs 30 more trips to settle.
    CHECK(settings({{"auto_exposure", 0.5}}).run_up_plan(1, 240).build == plan.build + 30);
}

// MARK: movement

TEST_CASE("testAStillCameraHoldsItsFraming") {
    Settings s = settings({{"center_x", 0.1}, {"roll", 5}});
    for (double t : {-1.0, 0.0, 0.7, 3.3}) CHECK(s.pose(t, 4) == s.base_pose());
}

TEST_CASE("testTheSameTakeMovesTheSameWay") {
    Settings a = settings({{"shake", 0.6}, {"seed", 3}}), b = settings({{"shake", 0.6}, {"seed", 3}});
    Settings c = settings({{"shake", 0.6}, {"seed", 4}});
    bool moved = false, differs = false;
    for (double t = 0; t < 4; t += 0.37) {
        Pose pa = a.pose(t, 4);
        CHECK(pa == b.pose(t, 4));
        moved = moved || !(pa == a.base_pose());
        differs = differs || !(pa == c.pose(t, 4));
    }
    CHECK(moved);
    CHECK(differs);
}

TEST_CASE("testHandsSwayGentlyAndStayInRange") {
    Settings s = settings({{"shake", 1}, {"seed", 7}});
    double biggest = 0, fastest = 0, last = s.pose(0, 10).center_x;
    for (int i = 1; i <= 300; ++i) {
        double x = s.pose(i / 30.0, 10).center_x;
        biggest = std::max(biggest, std::abs(x));
        fastest = std::max(fastest, std::abs(x - last));
        last = x;
    }
    CHECK(biggest > 0.02);
    CHECK(biggest < 0.12);
    CHECK(fastest < 0.012);
}

TEST_CASE("testAMoveRunsFromTheFramingToItsEnd") {
    Settings s = settings({{"drift_y", -0.3}, {"push", 0.2}, {"spin", 30}});
    Pose start = s.pose(0, 5), end = s.pose(5, 5);
    CHECK(start == s.base_pose());
    CHECK(std::abs(end.center_x) < 1e-9);
    CHECK(end.center_y == Approx(-0.3).epsilon(1e-9));
    CHECK(end.zoom == Approx(0.5 * 1.2).epsilon(1e-9));
    CHECK(end.roll == Approx(30).epsilon(1e-9));
    CHECK(s.pose(2.5, 5).center_y == Approx(-0.15).epsilon(1e-9));
    CHECK(std::abs(s.pose(0.25, 5).center_y) < 0.3 * 0.05);
    CHECK(s.pose(-0.5, 5) == s.base_pose());
}

TEST_CASE("testALoopEndsWhereItStarted (seamless loop out and back)") {
    Settings s = settings({{"drift_x", -0.28}, {"drift_y", 0.1}, {"push", -0.2}, {"spin", 45},
                           {"shake", 0.8}, {"seed", 5}, {"loop", 1}});
    const double L = 4.0;
    Pose a = s.pose(0, L), b = s.pose(L, L);
    CHECK(std::abs(a.center_x - b.center_x) < 1e-9);
    CHECK(std::abs(a.center_y - b.center_y) < 1e-9);
    CHECK(std::abs(a.zoom - b.zoom) < 1e-9);
    CHECK(std::abs(a.roll - b.roll) < 1e-9);
    CHECK(std::abs(a.turn - b.turn) < 1e-9);
    Pose mid = s.pose(L / 2, L);
    CHECK(std::abs(mid.roll - a.roll) > 30);
    Pose before = s.pose(-0.1, L), near_end = s.pose(L - 0.1, L);
    CHECK(std::abs(before.center_x - near_end.center_x) < 1e-9);

    // Property: the whole pose (hands included) is periodic in L, for any t
    // and seed — and so is the camera time the loop uses, frame by frame.
    for (int seed : {1, 9, 42})
        for (double len : {1.5, 3.0, 4.0}) {
            Settings ls = settings({{"drift_x", 0.2}, {"push", 0.3}, {"spin", -60}, {"shake", 1},
                                    {"seed", double(seed)}, {"loop", 1}});
            for (double t = -2; t < 2 * len; t += 0.173) {
                Pose p = ls.pose(t, len), q = ls.pose(t + len, len);
                CHECK(std::abs(p.center_x - q.center_x) < 1e-9);
                CHECK(std::abs(p.center_y - q.center_y) < 1e-9);
                CHECK(std::abs(p.zoom - q.zoom) < 1e-9);
                CHECK(std::abs(p.roll - q.roll) < 1e-7);
                CHECK(std::abs(p.turn - q.turn) < 1e-7);
                CHECK(std::abs(p.tilt - q.tilt) < 1e-7);
                CHECK(pose_time(ls, t, len) == t);  // a loop is never wrapped: it repeats by itself
            }
            Hands h(seed, len);
            for (int axis = 0; axis < 6; ++axis)
                CHECK(std::abs(h.value(axis, 0.3) - h.value(axis, 0.3 + len)) < 1e-9);
        }
    // A one-way move replays each pass instead (Loop count), the run-up is not wrapped.
    Settings one = settings({{"drift_x", 0.2}});
    CHECK(pose_time(one, 6.5, 5) == Approx(1.5));
    CHECK(pose_time(one, -0.5, 5) == -0.5);
}

TEST_CASE("testOldPresetsLoadIntoTheNewSettings") {
    std::map<std::string, double> old = {{"centre_x", -0.5}, {"centre_y", 0.25}, {"colour_drift", -0.49},
                                         {"drift", 0.3},     {"drift_dir", 30},  {"zoom", 0.63}};
    auto v = migrated(old);
    CHECK(v["center_x"] == -0.5);
    CHECK(v["center_y"] == 0.25);
    CHECK(v["color_drift"] == -0.49);
    CHECK(v["zoom"] == 0.63);
    for (auto gone : {"centre_x", "centre_y", "colour_drift", "drift", "drift_dir"}) CHECK(v.count(gone) == 0);
    const double pi = 3.14159265358979323846;
    for (double degrees : {0.0, 30.0, 90.0, 135.0, 200.0, 270.0, 333.0}) {
        double a = degrees * pi / 180;
        Settings m = settings(migrated({{"centre_x", 0.1}, {"centre_y", -0.2}, {"drift", 0.3}, {"drift_dir", degrees}}));
        for (double t : {0.0, 1.3, 2.5, 4.0, 5.0}) {
            double x = std::min(1.0, std::max(0.0, t / 5)), mm = x * x * (3 - 2 * x);
            Pose p = m.pose(t, 5);
            CHECK(std::abs(p.center_x - (0.1 + 0.3 * mm * std::cos(a))) < 1e-12);
            CHECK(std::abs(p.center_y - (-0.2 - 0.3 * mm * std::sin(a))) < 1e-12);
        }
    }
    auto current = default_values();
    CHECK(migrated(current) == current);
}

TEST_CASE("testTheHandheldCircleIsHowFarTheHandsGo") {
    CHECK(settings({{"shake", 0}}).handheld_reach(5) == V2{0, 0});
    struct C { int seed; double shake, loop, length; };
    for (C c : {C{1, 0.4, 0, 5}, C{7, 1, 0, 10}, C{23, 0.6, 1, 3}, C{42, 0.25, 1, 12}}) {
        Settings s = settings({{"shake", c.shake}, {"seed", double(c.seed)}, {"loop", c.loop}});
        V2 reach = s.handheld_reach(c.length);
        V2 furthest;
        for (int i = 0; i <= 6000; ++i) {
            Pose p = s.pose(c.length * i / 6000.0, c.length);
            furthest.x = std::max(furthest.x, std::abs(p.center_x));
            furthest.y = std::max(furthest.y, std::abs(p.center_y));
        }
        CHECK(std::abs(furthest.x - reach.x) <= reach.x * 0.01 + 1e-6);
        CHECK(std::abs(furthest.y - reach.y) <= reach.y * 0.01 + 1e-6);
        CHECK(reach.x > c.shake * 0.015);
        CHECK(reach.x < c.shake * 0.06);
    }
}

// MARK: values from the Swift formulas

TEST_CASE("GlitchRandom and hands match the Swift formulas") {
    CHECK(glitch_uniform(1, 900, 0) == Approx(0.9614967849462421).epsilon(1e-15));
    CHECK(glitch_uniform(77, 961, 1) == Approx(0.5708040843005492).epsilon(1e-15));
    CHECK(glitch_uniform(5, 0, -3, 7) == Approx(0.06022336030681208).epsilon(1e-15));
    const double free3[6] = {0.005043458195325345, -0.299541913260661, 0.06244178863361066,
                             0.2546718032026668, -0.15631642226067183, -0.8420110357939465};
    const double per3[6] = {0.4060387986486711, 0.1411506584887897, 0.18272367564843742,
                            0.46501715071629185, -0.10998278659652512, -0.49817418829282517};
    Hands h(3, std::nullopt), hp(3, 2.0);
    for (int a = 0; a < 6; ++a) {
        CHECK(std::abs(h.value(a, 0.7) - free3[a]) < 1e-12);
        CHECK(std::abs(hp.value(a, 0.7) - per3[a]) < 1e-12);
    }
}

TEST_CASE("pose and screen solve match the Swift formulas") {
    struct Ref {
        const char *name;
        double t, L;
        double pose[6];
        double c[3], n[3], cp[2];
    };
    // From a line-by-line Python transcription of Howlaround.swift (aspect 16:9,
    // camera point of TV (0.1, 0.8)).
    const Ref refs[] = {
        {"default", 0.0, 5.0, {0.8736067899327393, -0.18876342489789666, -0.22847277766393545, -1.2120173294373664, -3.279814133665037, 1.6347913301742203}, {-0.0315049051369688, -0.03005720462043046, 1.0}, {-0.0571890109853115, -0.02852862005410443, 0.9979556778035439}, {0.12610969946564288, 0.7518912874553709}},
        {"default", 1.37, 5.0, {0.8536022093987952, -0.19769476998270927, -0.22061624028705673, 0.8600473760328118, -2.029468379876267, -0.8670671425740635}, {-0.051722523586468894, -0.02615965473667059, 1.0}, {-0.035409444009035944, 0.015132598864623783, 0.9992583128133452}, {0.11794075925208708, 0.7264976456362334}},
        {"default", -0.8, 5.0, {0.871233625655362, -0.21392676488704826, -0.22143931280383566, -0.5152717617259464, -5.665740842974093, -1.5499938391045696}, {-0.03780898393143617, -0.025421849897877796, 1.0}, {-0.09868863034663826, 0.027049196326765886, 0.994750669875814}, {0.10689595629091014, 0.7590572286718577}},
        {"loopy", 1.1, 4.0, {0.7696999955841924, -0.3762613983637792, -0.14805336520643386, 26.256008523533566, -1.3044707155497655, -1.8144654018081756}, {-0.25817400157264153, 0.18158539614715383, 0.9999999999999999}, {-0.022753927544153865, 0.031663102385398434, 0.9992395642330457}, {0.015268506664090242, 0.6483931115182757}},
        {"steep", 0.0, 5.0, {1.3, 0.5, -0.5, 45, 45, -45}, {-0.5018146747556229, -0.3777777777777778, 1.3315186585777108}, {0.5000000000000001, 0.7071067811865476, 0.5000000000000001}, {0.24942719956317738, 0.20381534414020447}},
        {"seed42", 2.5, 3.0, {0.8770869328766707, -0.20004690097600927, -0.26024833138211145, -2.062349993344489, 21.291995000966022, -9.037702450257486}, {-0.12251637377361668, -0.08454975761766664, 1.0}, {0.3586129773091476, 0.1570843615533169, 0.9201745681450099}, {0.16152103263874595, 0.6670510822261558}},
    };
    std::map<std::string, Settings> sets = {
        {"default", Settings()},
        {"loopy", Settings({{"loop", 1}, {"shake", 0.8}, {"seed", 5}, {"drift_x", -0.28}, {"drift_y", 0.1}, {"push", -0.2}, {"spin", 45}})},
        {"steep", Settings({{"zoom", 1.3}, {"center_x", 0.5}, {"center_y", -0.5}, {"turn", 45}, {"tilt", -45}, {"roll", 45}, {"shake", 0}})},
        {"seed42", Settings({{"seed", 42}, {"shake", 1}, {"turn", 20}, {"tilt", -10}})},
    };
    for (const Ref &r : refs) {
        CAPTURE(r.name);
        CAPTURE(r.t);
        const Settings &s = sets.at(r.name);
        Pose p = s.pose(r.t, r.L);
        same_pose(p, {r.pose[0], r.pose[1], r.pose[2], r.pose[3], r.pose[4], r.pose[5]}, 1e-12);
        Screen sc = Settings::screen(p, 16.0 / 9);
        CHECK(std::abs(sc.c.x - r.c[0]) < 1e-12); CHECK(std::abs(sc.c.y - r.c[1]) < 1e-12); CHECK(std::abs(sc.c.z - r.c[2]) < 1e-12);
        CHECK(std::abs(sc.n.x - r.n[0]) < 1e-12); CHECK(std::abs(sc.n.y - r.n[1]) < 1e-12); CHECK(std::abs(sc.n.z - r.n[2]) < 1e-12);
        auto cp = Settings::camera_point({0.1, 0.8}, p, 16.0 / 9);
        REQUIRE(cp);
        CHECK(std::abs(cp->x - r.cp[0]) < 1e-12);
        CHECK(std::abs(cp->y - r.cp[1]) < 1e-12);
    }
}

// MARK: per-frame values

TEST_CASE("feedback size, blur, exposure, counter text") {
    // 240 lines: k = 2 * ceil(640/240) = 6.
    CHECK(feedback_size(240, 240).width == 1440);
    CHECK(feedback_size(320, 240).width == 1280);
    CHECK(feedback_size(320, 240).height == 960);
    CHECK(feedback_size(1920, 1080).width == 1920);  // k = 2, then down to 1 under 2048
    CHECK(feedback_size(720, 480).width == 1440);
    Settings s = settings({{"focus", 0.4}});
    CHECK(blur_sigma(s, s.base_pose(), 960, 240) == Approx(2.5 * 0.4 * 4 / 0.5));
    CHECK(blur_sigma(settings({{"focus", 1}, {"zoom", 0.1}}), settings({{"zoom", 0.1}}).base_pose(), 2000, 100) == 24);
    CHECK(blur_radius(3.0) == 8);
    CHECK(blur_radius(40) == 64);

    CHECK(exposure_step(1.0, 0.5, 0.5, 1) == 1.0);
    CHECK(exposure_step(1.0, 0.5, 0.25, 1) == Approx(std::pow(2.0, 0.6)));
    CHECK(exposure_step(1.0, 0.5, 0.25, 0.5) == Approx(std::pow(2.0, 0.3)));
    CHECK(exposure_step(3.9, 1.0, 0.0, 1) == 4.0);
    CHECK(exposure_step(0.3, 0.0, 1.0, 1) == 0.25);

    CHECK(counter_text(0) == "0:00");
    CHECK(counter_text(4.99) == "0:04");
    CHECK(counter_text(-3) == "0:00");
    CHECK(counter_text(75) == "1:15");
    CHECK(counter_text(3600) == "60:00");
}

TEST_CASE("counter bitmap: upstream's layout") {
    CounterBitmap c = render_counter("0:04");
    // Ink 4 advances of 60 px minus the last glyph's 10 px gap, plus 12 px pads;
    // height ceil(96 * 0.9 + 24).
    CHECK(c.width == 254);
    CHECK(c.height == 111);
    CHECK(c.aspect() == Approx(254.0 / 111));
    int white = 0, shadow = 0;
    for (int i = 0; i < c.width * c.height; ++i) {
        uint8_t v = c.bgra[i * 4], a = c.bgra[i * 4 + 3];
        CHECK(v <= a);  // premultiplied
        if (v == 255) ++white;
        if (a > 100 && v < 10) ++shadow;
    }
    CHECK(white > 1000);
    CHECK(shadow > 500);
    CHECK(c.bgra[0 + 3] == 0);  // corner clear
}

TEST_CASE("camera uniforms") {
    Settings s = settings({{"color_drift", 1}, {"hue_drift", 90}, {"key", 0.4}, {"key_invert", 1}});
    CameraUniforms u = camera_uniforms(s, s.base_pose(), 2.0, 1.5, 2.0);
    double r = 1.07, b = 0.93, luma = 0.299 * r + 0.587 + 0.114 * b;
    CHECK(u.balance[0] == Approx(r / luma));
    CHECK(u.balance[1] == Approx(1 / luma));
    CHECK(u.balance[2] == Approx(b / luma));
    CHECK(u.params[0] == 2.f);
    CHECK(u.params2[0] == Approx(0).epsilon(1e-6));
    CHECK(u.params2[1] == Approx(1));
    CHECK(u.params2[2] == 1.5f);
    CHECK(u.params2[3] == 1.f);
    CHECK(u.key[0] == 0.4f);
    CHECK(u.key[1] == 1.f);
    CHECK(u.counter_rect[0] == 0.07f);
    CHECK(u.counter_rect[2] == Approx(0.07 + 0.075 * 2.0 / 2.0));
    CHECK(u.counter_rect[3] == 0.86f);
    // No TV in view: no key either.
    CameraUniforms z = camera_uniforms(settings({{"zoom", 0}, {"key", 0.4}}), settings({{"zoom", 0}}).base_pose(), 1, 1, 0);
    CHECK(z.params2[3] == 0.f);
    CHECK(z.key[0] == 0.f);
    CHECK(z.counter_rect[2] == 0.f);
}

TEST_CASE("bundled Screen Loop presets parse") {
    const char *names[] = {"12 copies luma key", "15 copies color drift", "22 copies bottom left",
                           "7 copies slow luma", "Bottom to top"};
    for (auto n : names) {
        std::ifstream f(std::string(NTSCRT_SOURCE_DIR) + "/presets/Screen Loop/" + n + ".json");
        REQUIRE(f.good());
        auto j = nlohmann::json::parse(f);
        CHECK(j["kind"] == "ntscrt-video-feedback");
        Feedback fb = parse_feedback(j);
        CHECK(fb.enabled);
        for (auto it = j["values"].begin(); it != j["values"].end(); ++it)
            CHECK(fb.settings[it.key()] == it->get<double>());
    }
    Feedback fb = parse_feedback(nlohmann::json::parse(R"({"kind":"ntscrt-video-feedback","seconds":2,"values":{"zoom":0.5}})"));
    CHECK(fb.length == 2);
    CHECK(fb.settings["zoom"] == 0.5);
    CHECK(fb.settings["turn"] == -4);  // default
    CHECK(parse_feedback(nlohmann::json::parse(R"({"length":12.5,"seconds":2})")).length == 12.5);
    CHECK(parse_feedback(nlohmann::json::parse(R"({"seconds":200})")).length == 60);
    CHECK(parse_feedback(nlohmann::json::parse(R"({})")).length == 5);
    CHECK_FALSE(parse_feedback(nlohmann::json::parse(R"({"enabled":false})")).enabled);
    // Version-1 files migrate.
    Feedback v1 = parse_feedback(nlohmann::json::parse(R"({"values":{"centre_x":0.3,"drift":0.2,"drift_dir":90}})"));
    CHECK(v1.settings["center_x"] == 0.3);
    CHECK(v1.settings["drift_x"] == 0);
    CHECK(v1.settings["drift_y"] == Approx(-0.2));
}
