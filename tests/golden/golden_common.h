// golden_common.h — shared by test_golden and ntscrt-imgdiff.
// Image loading/comparison plus the manifest-entry -> look JSON mapping that
// mirrors what scripts/ci/render_golden.py feeds upstream crt-smoke.
// The including TU must compile stb_image / stb_image_write implementations.
#pragma once

#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace golden {

namespace fs = std::filesystem;
using nlohmann::json;

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // RGBA8, top row first
};

inline bool load_png(const fs::path &p, Image &out) {
    int n = 0;
    unsigned char *d = stbi_load(p.string().c_str(), &out.w, &out.h, &n, 4);
    if (!d) return false;
    out.px.assign(d, d + size_t(out.w) * out.h * 4);
    stbi_image_free(d);
    return true;
}

inline bool write_png(const fs::path &p, const Image &im) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    return stbi_write_png(p.string().c_str(), im.w, im.h, 4, im.px.data(), im.w * 4) != 0;
}

struct DiffStats {
    bool same_size = false;
    double mean_abs = 0;    // mean |a-b| over R,G,B, in 0..255 units
    int max_abs = 0;        // largest single-channel difference
    double frac_over_16 = 0; // fraction of pixels with any channel off by > 16
};

// Compares RGB only: crt-smoke writes premultiplied-first BGRA with shader
// alpha, ntscrt passes source alpha through, so alpha is not comparable.
inline DiffStats compare_rgb(const Image &a, const Image &b, Image *diff = nullptr) {
    DiffStats s;
    if (a.w != b.w || a.h != b.h || a.w == 0 || a.h == 0) return s;
    s.same_size = true;
    const size_t n = size_t(a.w) * a.h;
    uint64_t sum = 0, over = 0;
    if (diff) { diff->w = a.w; diff->h = a.h; diff->px.assign(n * 4, 255); }
    for (size_t i = 0; i < n; ++i) {
        int pm = 0;
        for (int c = 0; c < 3; ++c) {
            int d = std::abs(int(a.px[i * 4 + c]) - int(b.px[i * 4 + c]));
            sum += uint64_t(d);
            pm = std::max(pm, d);
            if (diff) diff->px[i * 4 + c] = uint8_t(std::min(255, d * 8));  // amplified
        }
        s.max_abs = std::max(s.max_abs, pm);
        if (pm > 16) ++over;
    }
    s.mean_abs = double(sum) / double(n * 3);
    s.frac_over_16 = double(over) / double(n);
    return s;
}

// Upstream AppState.downscaleHeight: max(16, 2 * round(width / aspect / 2)).
inline int downscale_height(int width, int src_w, int src_h) {
    double aspect = std::max(0.05, double(src_w) / double(src_h));
    return std::max(16, 2 * int(std::lround(double(width) / aspect / 2.0)));
}

inline std::optional<json> read_json(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

// ntsc-rs settings for an entry: null = stage off, {} = ntsc-rs defaults,
// else the "ntsc.settings" object of presets/<name>.json.
inline std::optional<json> ntsc_settings(const json &entry, const fs::path &presets_dir, std::string *err) {
    if (!entry.contains("ntsc") || entry["ntsc"].is_null()) return json();  // off
    const json &n = entry["ntsc"];
    if (n.is_string() && n.get<std::string>() == "default") return json::object();
    if (n.is_object() && n.contains("preset")) {
        auto p = presets_dir / fs::u8path(n["preset"].get<std::string>() + ".json");
        auto j = read_json(p);
        if (!j || !j->contains("ntsc") || !(*j)["ntsc"].contains("settings")) {
            if (err) *err = "cannot read ntsc settings from " + p.string();
            return std::nullopt;
        }
        return (*j)["ntsc"]["settings"];
    }
    if (err) *err = "bad ntsc field";
    return std::nullopt;
}

// Look JSON equivalent to the crt-smoke invocation for `entry`.
// `sidecar` is ref/<id>.json written by render_golden.py (may be null): its
// "shader_params" holds every shader param crt-smoke ended up overriding
// (crt-smoke always sets the preset's first param to its midpoint).
inline std::optional<json> look_for_entry(const json &entry, const json &sidecar, const fs::path &presets_dir,
                                          std::string *err) {
    json look;
    if (entry.contains("downscale") && entry["downscale"].is_object()) {
        const json &d = entry["downscale"];
        look["downscale"] = {{"enabled", true}, {"method", d.value("method", "area")}, {"width", d.value("width", 320)}};
    } else {
        look["downscale"] = {{"enabled", false}};
    }
    auto ns = ntsc_settings(entry, presets_dir, err);
    if (!ns) return std::nullopt;
    if (ns->is_null()) look["ntsc"] = {{"enabled", false}};
    else look["ntsc"] = {{"enabled", true}, {"settings", *ns}};

    json params = json::object();
    if (sidecar.is_object() && sidecar.contains("shader_params")) params.update(sidecar["shader_params"]);
    if (entry.contains("shader_params")) params.update(entry["shader_params"]);
    look["shader"] = {{"enabled", true}, {"preset", entry.value("shader", "")}, {"params", params}};
    look["glitch"] = {{"enabled", false}};
    look["digital"] = {{"enabled", false}};
    look["crop"] = {{"enabled", false}};
    return look;
}

inline std::string env_or(const char *name, const std::string &fallback) {
    const char *v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

}  // namespace golden
