// howl_math.h — Screen Loop (howlaround) pure math, CPU only, no GPU.
// Port of upstream Sources/CrtCore/Howlaround.swift: parameters (HowlaroundParam),
// settings + pose (HowlaroundSettings), the operator's hands (HowlaroundHands),
// the screen solve, run-up plan, auto-exposure step, counter text, and the
// uniforms the camera kernel takes. Everything here is deterministic and
// unit-tested in tests/howlaround/.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ntscrt::howl {

struct V2 {
    double x = 0, y = 0;
    bool operator==(const V2 &o) const { return x == o.x && y == o.y; }
};
struct V3 {
    double x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(double s, V3 a) { return {s * a.x, s * a.y, s * a.z}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// ---- parameters (HowlaroundParam.all) ----------------------------------------
struct Param {
    const char *id;
    double min, max;
    double default_value;
    double neutral_value;
};
// The 23 knobs in upstream order, with upstream defaults.
const std::vector<Param> &params();
std::map<std::string, double> default_values();
// Settings saved by upstream < 0.13.1 (British ids, drift + drift_dir), in today's terms.
std::map<std::string, double> migrated(std::map<std::string, double> v);

// ---- pose ------------------------------------------------------------------
struct Pose {
    double zoom = 0, center_x = 0, center_y = 0, roll = 0, turn = 0, tilt = 0;
    bool operator==(const Pose &o) const {
        return zoom == o.zoom && center_x == o.center_x && center_y == o.center_y && roll == o.roll &&
               turn == o.turn && tilt == o.tilt;
    }
};

// The camera operator's hands (HowlaroundHands): per axis three slow sways and
// two tremors, keyed by seed; with a period every frequency snaps to whole
// cycles over it, so the motion repeats exactly.
class Hands {
public:
    Hands(int seed, std::optional<double> period);
    double value(int axis, double t) const;
private:
    struct Part { double f, phase, amp; };
    std::array<std::vector<Part>, 6> parts_;
};

// GlitchRandom.uniform (upstream Receiver.swift): hash-keyed, call-order free.
uint64_t glitch_mix(uint64_t z);
double glitch_uniform(uint64_t seed, uint64_t purpose, int64_t a, int64_t b = 0);

struct Screen {
    V3 ax, ay, n, c;
};

// HowlaroundSettings: knob values merged over the defaults.
struct Settings {
    std::map<std::string, double> values;

    Settings();  // defaults
    explicit Settings(const std::map<std::string, double> &v);  // merged over defaults

    double operator[](const std::string &id) const;

    double zoom() const;
    bool tv_in_view() const { return zoom() > 0.001; }
    int delay() const;
    bool counter() const { return (*this)["counter"] >= 0.5; }
    bool loop() const { return (*this)["loop"] >= 0.5; }
    int seed() const;

    Pose base_pose() const;
    V2 vanishing_point() const { return {(*this)["center_x"], (*this)["center_y"]}; }
    V2 drift_target() const;

    static constexpr double handheld_center_x = 0.050;
    static constexpr double handheld_center_y = 0.045;

    V2 handheld_reach(double length) const;
    Pose pose(double t, double length) const;

    static Screen screen(const Pose &pose, double aspect);
    static std::optional<V2> camera_point(V2 tv, const Pose &pose, double aspect);
    std::optional<V2> camera_point(V2 tv, double aspect) const { return camera_point(tv, base_pose(), aspect); }
    std::optional<double> shrink(V2 s, double aspect) const;
    // nullopt = the copies grow instead.
    std::optional<int> visible_copies(double aspect, int chain_height) const;

    struct RunUpPlan { int build = 0, settle = 0; };
    RunUpPlan run_up_plan(double aspect, int chain_height) const;
    int run_up_frames(double aspect, int chain_height) const {
        auto p = run_up_plan(aspect, chain_height);
        return p.build + p.settle;
    }
};

// ---- feedback_json -----------------------------------------------------------
// Upstream "kind": "ntscrt-video-feedback" files: {"kind","version","values":{...},"seconds"?}.
// NTSCRT-Plugins extension: optional "length" (seconds) = the effect's duration on
// the host timeline, what a move spans and a loop repeats over (upstream: the
// clip's duration, or "seconds" for a still, default 5). Optional "enabled".
struct Feedback {
    Settings settings;
    double length = 5.0;
    bool enabled = true;
};
Feedback parse_feedback(const nlohmann::json &fb);

// ---- per-frame derived values ----------------------------------------------
// Camera time actually used for the pose: a one-way move repeats each pass.
double pose_time(const Settings &s, double time, double length);

// The fixed size the TV's picture is drawn at for the camera: a whole, even
// multiple of the chain input, about 1280 wide, at most ~2048.
struct Size { int width = 0, height = 0; };
Size feedback_size(int chain_w, int chain_h);

// Camera defocus sigma in TV-picture pixels.
double blur_sigma(const Settings &s, const Pose &pose, int feedback_h, int chain_h);
inline int blur_radius(double sigma) { return std::min(64, int(std::ceil(2.5 * sigma))); }

// Auto exposure: the camera reacts to what it measured a frame late.
double exposure_step(double exposure, double scene_luma, double camera_luma, double auto_exposure);

// The camcorder counter's text for `time` seconds: "0:04".
std::string counter_text(double time);

// Exactly the CameraUniforms struct of the kernel (9 x float4, 144 bytes).
struct CameraUniforms {
    float axis_x[4], axis_y[4], normal[4], center[4], balance[4], counter_rect[4];
    float params[4];   // aspect, zoom, brightness, contrast
    float params2[4];  // hue cos, hue sin, exposure, TV in view
    float key[4];      // key level, invert
};
static_assert(sizeof(CameraUniforms) == 144, "CameraUniforms layout");

struct BlurUniforms {
    float dir[2];
    float sigma;
    int32_t radius;
};
static_assert(sizeof(BlurUniforms) == 16, "BlurUniforms layout");

// counter_aspect: bitmap width / height, or 0 when no counter is drawn.
CameraUniforms camera_uniforms(const Settings &s, const Pose &pose, double aspect, double exposure,
                               double counter_aspect);

}  // namespace ntscrt::howl
