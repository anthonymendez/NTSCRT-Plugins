// howl_counter.cpp — see howl_counter.h.
#include "howl_counter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Private copy of the rasterizer: static linkage, so another stage including
// stb_truetype's implementation can't clash with this one.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace ntscrt::howl {

namespace {

// 5x7 digit glyphs, rows top to bottom, bit 4 = leftmost column.
// Authored for NTSCRT-Plugins; public domain (CC0).
struct Glyph { char c; uint8_t rows[7]; };
constexpr Glyph kGlyphs[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
};

constexpr double kFontSize = 96;   // upstream: Menlo-Bold 96
constexpr int kCell = 10;          // px per glyph cell: 7 rows = 70 px ~ Menlo cap height
constexpr int kAdvanceCells = 6;   // 60 px ~ Menlo advance (0.602 em)
constexpr int kPad = 12;

const Glyph *glyph(char c) {
    for (auto &g : kGlyphs)
        if (g.c == c) return &g;
    return nullptr;
}

void square(std::vector<stbtt_vertex> &v, int x0, int y0, int x1, int y1) {
    auto put = [&](unsigned char type, int x, int y) {
        stbtt_vertex q{};
        q.type = type;
        q.x = stbtt_vertex_type(x);
        q.y = stbtt_vertex_type(y);
        v.push_back(q);
    };
    put(STBTT_vmove, x0, y0);
    put(STBTT_vline, x1, y0);
    put(STBTT_vline, x1, y1);
    put(STBTT_vline, x0, y1);
}

}  // namespace

CounterBitmap render_counter(const std::string &text) {
    CounterBitmap out;
    out.text = text;
    // Outline in cell units, y down, with ink bounds.
    std::vector<stbtt_vertex> verts;
    int ink_x0 = 1 << 20, ink_x1 = -(1 << 20), ink_y0 = 1 << 20, ink_y1 = -(1 << 20);
    for (size_t i = 0; i < text.size(); ++i) {
        const Glyph *g = glyph(text[i]);
        if (!g) continue;
        int gx = int(i) * kAdvanceCells;
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 5; ++c)
                if (g->rows[r] & (0x10 >> c)) {
                    square(verts, gx + c, r, gx + c + 1, r + 1);
                    ink_x0 = std::min(ink_x0, gx + c);
                    ink_x1 = std::max(ink_x1, gx + c + 1);
                    ink_y0 = std::min(ink_y0, r);
                    ink_y1 = std::max(ink_y1, r + 1);
                }
    }
    if (verts.empty()) { ink_x0 = ink_x1 = ink_y0 = ink_y1 = 0; }
    double bw = double(ink_x1 - ink_x0) * kCell, bh = double(ink_y1 - ink_y0) * kCell;
    // Upstream: w = ceil(bounds.width + 2 pad), h = ceil(fontSize * 0.9 + 2 pad),
    // ink centred vertically between the pads.
    int w = int(std::ceil(bw + 2 * kPad)), h = int(std::ceil(kFontSize * 0.9 + 2 * kPad));
    out.width = w;
    out.height = h;
    double ox = kPad - ink_x0 * kCell;
    double oy = kPad - ink_y0 * kCell + (double(h) - 2 * kPad - bh) / 2;

    auto coverage = [&](double dx, double dy) {
        std::vector<unsigned char> cov(size_t(w) * h, 0);
        if (verts.empty()) return cov;
        stbtt__bitmap bm{w, h, w, cov.data()};
        stbtt_Rasterize(&bm, 0.35f, verts.data(), int(verts.size()), float(kCell), float(kCell),
                        float(ox + dx), float(oy + dy), 0, 0, 0, nullptr);
        return cov;
    };
    // Premultiplied RGBA accumulation (source-over), as CGContext draws it.
    std::vector<float> rgb(size_t(w) * h, 0.f), a(size_t(w) * h, 0.f);
    // Upstream's shadow offsets in CoreGraphics (y up): (-4,0) (4,0) (0,-4) (0,4) (4,-4).
    const int offs[5][2] = {{-4, 0}, {4, 0}, {0, 4}, {0, -4}, {4, 4}};
    for (auto &o : offs) {
        auto cov = coverage(o[0], o[1]);
        for (size_t i = 0; i < cov.size(); ++i) {
            float sa = 0.75f * cov[i] / 255.f;
            rgb[i] = rgb[i] * (1 - sa);  // black
            a[i] = sa + a[i] * (1 - sa);
        }
    }
    auto cov = coverage(0, 0);
    for (size_t i = 0; i < cov.size(); ++i) {
        float sa = cov[i] / 255.f;
        rgb[i] = sa + rgb[i] * (1 - sa);  // white
        a[i] = sa + a[i] * (1 - sa);
    }
    out.bgra.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < rgb.size(); ++i) {
        uint8_t c = uint8_t(std::lround(std::clamp(rgb[i], 0.f, 1.f) * 255.f));
        uint8_t al = uint8_t(std::lround(std::clamp(a[i], 0.f, 1.f) * 255.f));
        out.bgra[i * 4 + 0] = c;
        out.bgra[i * 4 + 1] = c;
        out.bgra[i * 4 + 2] = c;
        out.bgra[i * 4 + 3] = al;
    }
    return out;
}

}  // namespace ntscrt::howl
