// howl_math.cpp — see howl_math.h. Formulas follow upstream Howlaround.swift
// line by line (comments name the Swift members).
#include "howl_math.h"

#include <cstdio>

namespace ntscrt::howl {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

// ---- HowlaroundParam.all -------------------------------------------------------
const std::vector<Param> &params() {
    static const std::vector<Param> p = {
        {"zoom", 0, 1.4, 0.87, 0},
        {"center_x", -0.5, 0.5, -0.2, 0},
        {"center_y", -0.5, 0.5, -0.22, 0},
        {"roll", -45, 45, 0, 0},
        {"turn", -45, 45, -4, 0},
        {"tilt", -45, 45, 0, 0},
        {"shake", 0, 1, 0.4, 0},
        {"seed", 1, 99, 1, 1},
        {"drift_x", -1, 1, 0, 0},
        {"drift_y", -1, 1, 0, 0},
        {"push", -0.5, 0.5, 0, 0},
        {"spin", -180, 180, 0, 0},
        {"loop", 0, 1, 0, 0},
        {"focus", 0, 1, 0.2, 0},
        {"brightness", 0.5, 1.5, 1.0, 1.0},
        {"contrast", 0.6, 1.6, 1.05, 1.0},
        {"color_drift", -1, 1, -0.3, 0},
        {"hue_drift", -30, 30, 0, 0},
        {"auto_exposure", 0, 1, 0, 0},
        {"key", 0, 1, 0, 0},
        {"key_invert", 0, 1, 0, 0},
        {"delay", 1, 20, 1, 1},
        {"counter", 0, 1, 0, 0},
    };
    return p;
}

std::map<std::string, double> default_values() {
    std::map<std::string, double> m;
    for (auto &p : params()) m[p.id] = p.default_value;
    return m;
}

std::map<std::string, double> migrated(std::map<std::string, double> v) {
    const std::pair<const char *, const char *> renames[] = {
        {"centre_x", "center_x"}, {"centre_y", "center_y"}, {"colour_drift", "color_drift"}};
    for (auto &[old_id, new_id] : renames) {
        auto it = v.find(old_id);
        if (it == v.end()) continue;
        double x = it->second;
        v.erase(it);
        if (!v.count(new_id)) v[new_id] = x;
    }
    std::optional<double> distance, degrees;
    if (auto it = v.find("drift"); it != v.end()) { distance = it->second; v.erase(it); }
    if (auto it = v.find("drift_dir"); it != v.end()) { degrees = it->second; v.erase(it); }
    if (distance && !v.count("drift_x") && !v.count("drift_y")) {
        double a = degrees.value_or(0) * kPi / 180;
        auto clean = [](double x) { return std::abs(x) < 1e-12 ? 0.0 : x; };
        v["drift_x"] = clean(*distance * std::cos(a));
        v["drift_y"] = clean(-*distance * std::sin(a));  // up is toward -y
    }
    return v;
}

// ---- GlitchRandom ------------------------------------------------------------
uint64_t glitch_mix(uint64_t z) {
    z = z + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double glitch_uniform(uint64_t seed, uint64_t purpose, int64_t a, int64_t b) {
    uint64_t h = glitch_mix(seed ^ glitch_mix(purpose ^ glitch_mix(uint64_t(a) ^ glitch_mix(uint64_t(b)))));
    return double(h >> 11) * (1.0 / 9007199254740992.0);
}

// ---- HowlaroundHands ---------------------------------------------------------
Hands::Hands(int seed_i, std::optional<double> period) {
    uint64_t seed = uint64_t(int64_t(seed_i));
    auto snap = [&](double f) {
        if (!period) return f;
        return std::max(1.0, std::round(f * *period)) / *period;
    };
    for (int axis = 0; axis < 6; ++axis) {
        std::vector<Part> p;
        double power = 0;
        for (int k = 0; k < 3; ++k) {
            double u = glitch_uniform(seed, 900 + uint64_t(axis), k);
            double v = glitch_uniform(seed, 920 + uint64_t(axis), k);
            double w = glitch_uniform(seed, 940 + uint64_t(axis), k);
            double amp = 0.5 + 0.5 * w;
            p.push_back({snap(0.15 + 0.55 * u), 2 * kPi * v, amp});
            power += amp * amp / 2;
        }
        // Normalize the sway to an RMS of 0.45, then add the tremor.
        double scale = 0.45 / std::sqrt(power);
        for (auto &q : p) q.amp *= scale;
        for (int k = 0; k < 2; ++k) {
            double u = glitch_uniform(seed, 960 + uint64_t(axis), k);
            double v = glitch_uniform(seed, 980 + uint64_t(axis), k);
            p.push_back({snap(4 + 3 * u), 2 * kPi * v, 0.02});
        }
        parts_[axis] = std::move(p);
    }
}

double Hands::value(int axis, double t) const {
    double sum = 0;
    for (auto &q : parts_[axis]) sum += q.amp * std::sin(2 * kPi * q.f * t + q.phase);
    return sum;
}

// ---- HowlaroundSettings --------------------------------------------------------
Settings::Settings() : values(default_values()) {}

Settings::Settings(const std::map<std::string, double> &v) : values(default_values()) {
    for (auto &[k, x] : v) values[k] = x;
}

double Settings::operator[](const std::string &id) const {
    auto it = values.find(id);
    if (it != values.end()) return it->second;
    for (auto &p : params())
        if (id == p.id) return p.default_value;
    return 0;
}

double Settings::zoom() const { return std::max(0.0, (*this)["zoom"]); }
int Settings::delay() const { return std::max(1, std::min(20, int(std::round((*this)["delay"])))); }
int Settings::seed() const { return std::max(1, int(std::round((*this)["seed"]))); }

Pose Settings::base_pose() const {
    return {zoom(), (*this)["center_x"], (*this)["center_y"], (*this)["roll"], (*this)["turn"], (*this)["tilt"]};
}

V2 Settings::drift_target() const {
    V2 v = vanishing_point();
    return {v.x + (*this)["drift_x"], v.y + (*this)["drift_y"]};
}

V2 Settings::handheld_reach(double length) const {
    double shake = (*this)["shake"];
    if (!(shake > 0) || !tv_in_view()) return {};
    double L = std::max(0.1, length);
    Hands hands(seed(), loop() ? std::optional<double>(L) : std::nullopt);
    int samples = std::max(400, std::min(4000, int(L * 60)));
    V2 reach;
    for (int i = 0; i <= samples; ++i) {
        double t = L * double(i) / double(samples);
        reach.x = std::max(reach.x, std::abs(hands.value(0, t)));
        reach.y = std::max(reach.y, std::abs(hands.value(1, t)));
    }
    return {shake * handheld_center_x * reach.x, shake * handheld_center_y * reach.y};
}

Pose Settings::pose(double t, double length) const {
    Pose p = base_pose();
    if (!tv_in_view()) return p;
    double L = std::max(0.1, length);
    double m;
    if (loop()) {
        double s = std::sin(kPi * t / L);
        m = s * s;
    } else {
        double x = std::min(1.0, std::max(0.0, t / L));
        m = x * x * (3 - 2 * x);
    }
    p.center_x += (*this)["drift_x"] * m;
    p.center_y += (*this)["drift_y"] * m;
    p.zoom *= std::max(0.05, 1 + (*this)["push"] * m);
    p.roll += (*this)["spin"] * m;
    double shake = (*this)["shake"];
    if (shake > 0) {
        Hands hands(seed(), loop() ? std::optional<double>(L) : std::nullopt);
        p.center_x += shake * handheld_center_x * hands.value(0, t);
        p.center_y += shake * handheld_center_y * hands.value(1, t);
        p.turn += shake * 6.0 * hands.value(2, t);
        p.tilt += shake * 4.5 * hands.value(3, t);
        p.roll += shake * 3.5 * hands.value(4, t);
        p.zoom *= 1 + shake * 0.06 * hands.value(5, t);
    }
    return p;
}

Screen Settings::screen(const Pose &pose, double aspect) {
    const double deg = kPi / 180;
    double yaw = std::max(-60.0, std::min(60.0, pose.turn)) * deg;
    double pitch = std::max(-60.0, std::min(60.0, pose.tilt)) * deg;
    double roll = pose.roll * deg;
    auto ry = [&](V3 v) {
        return V3{std::cos(yaw) * v.x + std::sin(yaw) * v.z, v.y, -std::sin(yaw) * v.x + std::cos(yaw) * v.z};
    };
    auto rx = [&](V3 v) {
        return V3{v.x, std::cos(pitch) * v.y - std::sin(pitch) * v.z, std::sin(pitch) * v.y + std::cos(pitch) * v.z};
    };
    auto rz = [&](V3 v) {
        return V3{std::cos(roll) * v.x - std::sin(roll) * v.y, std::sin(roll) * v.x + std::cos(roll) * v.y, v.z};
    };
    auto r = [&](V3 v) { return ry(rx(rz(v))); };
    V3 ax = r({1, 0, 0}), ay = r({0, 1, 0});
    V3 q{pose.center_x * aspect, pose.center_y, 1};
    V3 offset = pose.zoom * ((pose.center_x * aspect) * ax + pose.center_y * ay);
    double along = std::max(0.25, 1 + offset.z);
    return {ax, ay, r({0, 0, 1}), along * q - offset};
}

std::optional<V2> Settings::camera_point(V2 s, const Pose &pose, double aspect) {
    Screen sc = screen(pose, aspect);
    V3 p = sc.c + ((s.x - 0.5) * pose.zoom * aspect) * sc.ax + ((s.y - 0.5) * pose.zoom) * sc.ay;
    if (!(p.z > 1e-4)) return std::nullopt;
    return V2{p.x / p.z / aspect + 0.5, p.y / p.z + 0.5};
}

std::optional<double> Settings::shrink(V2 s, double aspect) const {
    const double e = 1e-4;
    auto px = camera_point({s.x + e, s.y}, aspect), mx = camera_point({s.x - e, s.y}, aspect);
    auto py = camera_point({s.x, s.y + e}, aspect), my = camera_point({s.x, s.y - e}, aspect);
    if (!px || !mx || !py || !my) return std::nullopt;
    V2 jx{(px->x - mx->x) / (2 * e), (px->y - mx->y) / (2 * e)};
    V2 jy{(py->x - my->x) / (2 * e), (py->y - my->y) / (2 * e)};
    double tr = jx.x + jy.y, det = jx.x * jy.y - jy.x * jx.y;
    double disc = tr * tr / 4 - det;
    if (disc >= 0) {
        double r = std::sqrt(disc);
        return std::max(std::abs(tr / 2 + r), std::abs(tr / 2 - r));
    }
    return std::sqrt(std::abs(det));  // a spiral: complex pair
}

std::optional<int> Settings::visible_copies(double aspect, int chain_height) const {
    if (!tv_in_view()) return 0;
    Pose p = base_pose();
    auto rate = shrink({p.center_x + 0.5, p.center_y + 0.5}, aspect);
    if (!rate || !(*rate < 0.995)) return std::nullopt;
    double n = std::log(2.0 / double(std::max(16, chain_height))) / std::log(*rate);
    return std::max(1, std::min(200, int(std::floor(n))));
}

Settings::RunUpPlan Settings::run_up_plan(double aspect, int chain_height) const {
    if (!tv_in_view()) return {0, 0};
    int copies = visible_copies(aspect, chain_height).value_or(60);
    int exposure = (*this)["auto_exposure"] > 0 ? 30 : 0;
    int build = copies + 4 + exposure;
    int settle = loop() ? std::min(400, delay() * std::min(copies + 4, 12)) : delay();
    return {build, settle};
}

// ---- feedback_json -----------------------------------------------------------
Feedback parse_feedback(const nlohmann::json &fb) {
    Feedback out;
    if (!fb.is_object()) return out;
    std::map<std::string, double> v;
    auto vals = fb.find("values");
    if (vals != fb.end() && vals->is_object()) {
        for (auto it = vals->begin(); it != vals->end(); ++it) {
            if (it->is_number()) v[it.key()] = it->get<double>();
            else if (it->is_boolean()) v[it.key()] = it->get<bool>() ? 1.0 : 0.0;
        }
    }
    out.settings = Settings(migrated(std::move(v)));
    auto num = [&](const char *k) -> std::optional<double> {
        auto it = fb.find(k);
        if (it != fb.end() && it->is_number()) return it->get<double>();
        return std::nullopt;
    };
    if (auto len = num("length"); len && *len > 0) out.length = *len;
    else if (auto sec = num("seconds")) out.length = std::min(60.0, std::max(0.5, *sec));
    auto en = fb.find("enabled");
    if (en != fb.end() && en->is_boolean()) out.enabled = en->get<bool>();
    return out;
}

// ---- per-frame -----------------------------------------------------------------
double pose_time(const Settings &s, double time, double length) {
    double L = std::max(0.1, length);
    return time >= 0 && !s.loop() ? std::fmod(time, L) : time;
}

Size feedback_size(int chain_w, int chain_h) {
    int w = std::max(1, chain_w), h = std::max(1, chain_h);
    int k = std::max(2, 2 * int(std::ceil(640.0 / double(w))));
    while (k > 1 && w * k > 2048) k -= 1;
    return {w * k, h * k};
}

double blur_sigma(const Settings &s, const Pose &pose, int feedback_h, int chain_h) {
    double sigma_lines = 2.5 * s["focus"];
    double scale = double(feedback_h) / double(std::max(1, chain_h));
    return std::min(24.0, sigma_lines * scale / std::max(0.25, std::min(1.0, pose.zoom)));
}

double exposure_step(double exposure, double scene_luma, double camera_luma, double auto_exposure) {
    double target = std::max(0.02, scene_luma), seen = std::max(0.02, camera_luma);
    double rate = 0.6 * auto_exposure;
    return std::min(4.0, std::max(0.25, exposure * std::pow(target / seen, rate)));
}

std::string counter_text(double time) {
    int secs = std::max(0, int(time));  // Swift Int(): toward zero
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d", secs / 60, secs % 60);
    return buf;
}

CameraUniforms camera_uniforms(const Settings &s, const Pose &pose, double aspect, double exposure,
                               double counter_aspect) {
    Screen sc = Settings::screen(pose, aspect);
    CameraUniforms u{};
    auto f4 = [](float *dst, V3 v, double w = 0) {
        dst[0] = float(v.x); dst[1] = float(v.y); dst[2] = float(v.z); dst[3] = float(w);
    };
    f4(u.axis_x, sc.ax);
    f4(u.axis_y, sc.ay);
    f4(u.normal, sc.n);
    f4(u.center, sc.c);
    // White balance: red against blue, keeping luminance where it was.
    double wb = 0.07 * s["color_drift"];
    double r = 1 + wb, b = 1 - wb;
    double luma = 0.299 * r + 0.587 + 0.114 * b;
    double hue = s["hue_drift"] * kPi / 180;
    u.balance[0] = float(r / luma); u.balance[1] = float(1 / luma); u.balance[2] = float(b / luma); u.balance[3] = 0;
    if (counter_aspect > 0) {
        double h = 0.075, w = h * counter_aspect / aspect;
        u.counter_rect[0] = 0.07f; u.counter_rect[1] = float(0.86 - h);
        u.counter_rect[2] = float(0.07 + w); u.counter_rect[3] = 0.86f;
    }
    bool tv = s.tv_in_view();
    u.params[0] = float(aspect); u.params[1] = float(std::max(0.001, pose.zoom));
    u.params[2] = float(s["brightness"]); u.params[3] = float(s["contrast"]);
    u.params2[0] = float(std::cos(hue)); u.params2[1] = float(std::sin(hue));
    u.params2[2] = float(exposure); u.params2[3] = tv ? 1.f : 0.f;
    u.key[0] = float(tv ? s["key"] : 0.0); u.key[1] = s["key_invert"] >= 0.5 ? 1.f : 0.f;
    return u;
}

}  // namespace ntscrt::howl
