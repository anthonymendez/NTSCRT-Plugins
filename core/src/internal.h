// internal.h — C++ contract between the façade (core/src/ntscrt.cpp) and the
// stages. CONTRACT (Wave 0): stage owners implement the classes declared in
// their stage header; the façade wires them in Wave 2.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ntscrt {

using json = nlohmann::json;

// Internal CPU image: tightly packed 8-bit BGRA, top row first — the format
// every upstream CPU stage (ntsc-rs, CrtCodec, Receiver) works in.
struct ImageBGRA8 {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> px;  // width*height*4
    uint8_t *row(int y) { return px.data() + size_t(y) * width * 4; }
    const uint8_t *row(int y) const { return px.data() + size_t(y) * width * 4; }
    void resize(int w, int h) { width = w; height = h; px.assign(size_t(w) * h * 4, 0); }
};

// Everything a stage may need about "now" and the past.
struct FrameContext {
    int64_t frame = 0;      // effect-local frame index (0-based)
    double fps = 30.0;
    double render_scale = 1.0;
    uint64_t seed = 0;
    const json *look = nullptr;  // look at `frame`
    // Picture rows each chain line stands for: 2 when the chain gets one field
    // (Digital video's One field, upstream DownscaleSpec.lineHeight), else 1.
    // Set by the façade; the Downscale halves its lines and the CRT keeps the
    // picture's shape.
    int line_height = 1;

    // Source frame at a past/future frame index, converted to BGRA8 and with
    // the crop stage already applied (façade caches these). Returns nullptr
    // when the host cannot supply it; stages must degrade gracefully.
    std::function<std::shared_ptr<const ImageBGRA8>(int64_t)> source_at;
    // Look JSON at a past frame (keyframe history). Never null; returns
    // current look when the host gave no history.
    std::function<const json &(int64_t)> look_at;
};

// Upstream frame-counter conventions (Mp4Exporter.swift video path), keep exact:
inline int64_t ntsc_frame(const FrameContext &c) { return c.frame + 1; }
inline int64_t shader_frame(const FrameContext &c) { return c.frame + 1; }
inline int64_t digital_frame(const FrameContext &c) { return c.frame; }
inline double glitch_time(const FrameContext &c) { return double(c.frame) / c.fps; }

}  // namespace ntscrt
