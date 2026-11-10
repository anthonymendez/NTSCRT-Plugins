// howl_counter.h — the camcorder's elapsed-time counter ("0:04"), burned into
// its picture. Port of upstream HowlaroundLoop.counter(for:) (CoreText,
// Menlo-Bold 96 pt, white figures with a dark 4 px edge, 12 px padding).
//
// Font: upstream only ever draws "0"-"9" and ":", so instead of shipping a TTF
// the glyphs are a bundled 5x7 camcorder-OSD digit set (howl_counter.cpp,
// authored for this project, public domain / CC0) whose outlines are
// rasterized with stb_truetype's anti-aliasing rasterizer. Sizes match
// Menlo-Bold 96 pt closely (cap height ~70 px, advance ~60 px), so the
// counter covers the same part of the frame as upstream's.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ntscrt::howl {

struct CounterBitmap {
    std::string text;
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;  // premultiplied BGRA8, top row first
    double aspect() const { return height > 0 ? double(width) / double(height) : 0; }
};

CounterBitmap render_counter(const std::string &text);

}  // namespace ntscrt::howl
