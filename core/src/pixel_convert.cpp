#include "pixel_convert.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../stages/crt/crt_sizing.h"
#include "../stages/digital/parallel.h"

namespace ntscrt {
namespace {
// Same results as uint8_t(std::lround(std::clamp(v, 0, 1) * 255.0f)) (and
// 65535 for 16 bits) for every float, NaN -> 0, without lround's slow call:
// p is in [0, max], so p + 0.5 is exact in double and truncation is floor.
inline uint32_t round_unit(float v, float max) {
    if (!(v > 0.f)) return 0;  // also NaN
    if (v >= 1.f) return uint32_t(max);
    return uint32_t(double(v * max) + 0.5);
}
// Source alpha to 0..1; NaN counts as transparent (std::clamp lets NaN through).
inline float unit_alpha(float a) { return a > 0.f ? (a < 1.f ? a : 1.f) : 0.f; }
inline uint8_t q8(float v) { return uint8_t(round_unit(v, 255.0f)); }
inline uint16_t q16(float v) { return uint16_t(round_unit(v, 65535.0f)); }

inline float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu, f;
    if (e == 0) {
        if (m == 0) {
            f = s << 31;
        } else {
            e = 127 - 15 + 1;
            while (!(m & 0x400u)) { m <<= 1; --e; }
            m &= 0x3ffu;
            f = (s << 31) | (e << 23) | (m << 13);
        }
    } else if (e == 31) {
        f = (s << 31) | 0x7f800000u | (m << 13);
    } else {
        f = (s << 31) | ((e + 127 - 15) << 23) | (m << 13);
    }
    float r;
    std::memcpy(&r, &f, 4);
    return r;
}

inline uint16_t float_to_half(float x) {
    uint32_t f;
    std::memcpy(&f, &x, 4);
    uint32_t s = (f >> 16) & 0x8000u;
    int e = int((f >> 23) & 0xffu) - 127 + 15;
    uint32_t m = f & 0x7fffffu;
    if (e <= 0) return uint16_t(s);
    if (e >= 31) return uint16_t(s | 0x7c00u);
    return uint16_t(s | (uint32_t(e) << 10) | ((m + 0x1000u) >> 13));
}

// Per-format pixel access, resolved at compile time so the conversion loops
// don't switch on the format for every pixel.
template <ntscrt_pixel_format F> inline void read_px(const uint8_t *row, int x, float o[4]) {
    if constexpr (F == NTSCRT_RGBA8) { auto p = row + x * 4; for (int c = 0; c < 4; ++c) o[c] = p[c] / 255.f; }
    else if constexpr (F == NTSCRT_BGRA8) { auto p = row + x * 4; o[0] = p[2] / 255.f; o[1] = p[1] / 255.f; o[2] = p[0] / 255.f; o[3] = p[3] / 255.f; }
    else if constexpr (F == NTSCRT_RGBA16) { auto p = reinterpret_cast<const uint16_t *>(row) + x * 4; for (int c = 0; c < 4; ++c) o[c] = p[c] / 65535.f; }
    else if constexpr (F == NTSCRT_RGBA16F) { auto p = reinterpret_cast<const uint16_t *>(row) + x * 4; for (int c = 0; c < 4; ++c) o[c] = half_to_float(p[c]); }
    else { auto p = reinterpret_cast<const float *>(row) + x * 4; for (int c = 0; c < 4; ++c) o[c] = p[c]; }
}

template <ntscrt_pixel_format F> inline void write_px(uint8_t *row, int x, const float v[4]) {
    if constexpr (F == NTSCRT_RGBA8) { auto p = row + x * 4; for (int c = 0; c < 4; ++c) p[c] = q8(v[c]); }
    else if constexpr (F == NTSCRT_BGRA8) { auto p = row + x * 4; p[0] = q8(v[2]); p[1] = q8(v[1]); p[2] = q8(v[0]); p[3] = q8(v[3]); }
    else if constexpr (F == NTSCRT_RGBA16) { auto p = reinterpret_cast<uint16_t *>(row) + x * 4; for (int c = 0; c < 4; ++c) p[c] = q16(v[c]); }
    else if constexpr (F == NTSCRT_RGBA16F) { auto p = reinterpret_cast<uint16_t *>(row) + x * 4; for (int c = 0; c < 4; ++c) p[c] = float_to_half(v[c]); }
    else { auto p = reinterpret_cast<float *>(row) + x * 4; for (int c = 0; c < 4; ++c) p[c] = v[c]; }
}

// Calls fn.template operator()<F>() for the image's format.
template <class Fn> void with_format(ntscrt_pixel_format f, Fn &&fn) {
    switch (f) {
    case NTSCRT_RGBA8: fn.template operator()<NTSCRT_RGBA8>(); break;
    case NTSCRT_BGRA8: fn.template operator()<NTSCRT_BGRA8>(); break;
    case NTSCRT_RGBA16: fn.template operator()<NTSCRT_RGBA16>(); break;
    case NTSCRT_RGBA16F: fn.template operator()<NTSCRT_RGBA16F>(); break;
    case NTSCRT_RGBA32F: fn.template operator()<NTSCRT_RGBA32F>(); break;
    }
}
}  // namespace

AlphaMode alpha_mode(const json &look) {
    auto it = look.find("alpha");
    if (it != look.end() && it->is_object()) {
        auto m = it->find("mode");
        if (m != it->end() && m->is_string() && m->get<std::string>() == "spill") return AlphaMode::Spill;
    }
    return AlphaMode::Clip;
}

namespace {
constexpr float kTransparent = 0.5f / 255.f;  // alpha at or below this counts as empty

// fn(y) for every row: on the Digital stage's thread pool for large pictures.
// Rows only write their own output, so the result is the same either way.
template <class Fn> void for_rows(int w, int h, Fn &&fn) {
    if (int64_t(w) * h >= (int64_t(1) << 18)) digital::parallel_for(h, fn);
    else
        for (int y = 0; y < h; ++y) fn(y);
}
}  // namespace

void to_bgra8(const ntscrt_image &src, ImageBGRA8 &out, std::vector<float> *alpha_out, AlphaMode mode,
              bool src_premult) {
    out.width = src.width;
    out.height = src.height;
    out.px.resize(size_t(src.width) * src.height * 4);  // every byte is written below
    std::vector<float> local;
    // Alpha is only stored once a pixel isn't opaque: empty = fully opaque
    // (most video), which saves the buffer and the fill below.
    std::vector<float> *alpha = alpha_out ? alpha_out : (mode == AlphaMode::Clip ? &local : nullptr);
    if (alpha) alpha->clear();
    // Per row: bit 0 = some alpha isn't 1, bit 1 = some pixel is transparent.
    std::vector<uint8_t> row_flags(size_t(std::max(0, src.height)), 0);
    with_format(src.format, [&]<ntscrt_pixel_format F>() {
        for_rows(src.width, src.height, [&](int y) {
            auto row = static_cast<const uint8_t *>(src.data) + ptrdiff_t(y) * src.row_bytes;
            uint8_t *o = out.row(y);
            uint8_t flags = 0;
            for (int x = 0; x < src.width; ++x) {
                float v[4];
                read_px<F>(row, x, v);
                const float a = unit_alpha(v[3]);
                if (mode == AlphaMode::Clip) {
                    // straight colour: what the element looks like on its own
                    if (src_premult && a > kTransparent && a < 1.f)
                        for (int c = 0; c < 3; ++c) v[c] /= a;
                } else if (!src_premult) {
                    // the element over black
                    for (int c = 0; c < 3; ++c) v[c] *= a;
                }
                flags |= uint8_t((a != 1.f ? 1 : 0) | (a <= kTransparent ? 2 : 0));
                o[x * 4 + 0] = q8(v[2]);
                o[x * 4 + 1] = q8(v[1]);
                o[x * 4 + 2] = q8(v[0]);
                o[x * 4 + 3] = 255;
            }
            row_flags[size_t(y)] = flags;
        });
        // Alpha is only stored once a pixel isn't opaque: a second pass over
        // just those rows (none for ordinary video).
        if (!alpha || std::none_of(row_flags.begin(), row_flags.end(), [](uint8_t f) { return f & 1; })) return;
        alpha->assign(size_t(src.width) * src.height, 1.f);
        for_rows(src.width, src.height, [&](int y) {
            if (!(row_flags[size_t(y)] & 1)) return;
            auto row = static_cast<const uint8_t *>(src.data) + ptrdiff_t(y) * src.row_bytes;
            float *dst = alpha->data() + size_t(y) * src.width;
            for (int x = 0; x < src.width; ++x) {
                float v[4];
                read_px<F>(row, x, v);
                dst[x] = unit_alpha(v[3]);
            }
        });
    });
    const bool any_transparent = std::any_of(row_flags.begin(), row_flags.end(), [](uint8_t f) { return f & 2; });
    if (mode == AlphaMode::Clip && any_transparent) fill_transparent(out, *alpha);
}

// Pull-push hole filling: average the non-transparent colours down a mip
// pyramid, then fill each empty pixel from the next coarser level (bilinear),
// so empty areas take the colour of the nearest content, smoothly.
void fill_transparent(ImageBGRA8 &im, const std::vector<float> &alpha) {
    struct Level { int w, h; std::vector<float> px; };  // r, g, b, weight (colour straight)
    const int W = im.width, H = im.height;
    if (W <= 0 || H <= 0 || alpha.size() < size_t(W) * H) return;  // empty alpha: opaque
    bool any = false, all = true;
    for (float a : alpha) { bool e = a <= kTransparent; any |= e; all &= e; }
    if (!any || all) return;  // nothing to fill / nothing to fill from

    auto px0 = [&](int x, int y, float o[4]) {  // level 0 read
        const uint8_t *p = im.row(y) + x * 4;
        float w = alpha[size_t(y) * W + x] > kTransparent ? 1.f : 0.f;
        o[0] = p[2] / 255.f; o[1] = p[1] / 255.f; o[2] = p[0] / 255.f; o[3] = w;
    };
    std::vector<Level> L;
    int w = W, h = H;
    while (w > 1 || h > 1) {
        const int nw = (w + 1) / 2, nh = (h + 1) / 2;
        Level n{nw, nh, std::vector<float>(size_t(nw) * nh * 4, 0.f)};
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                float s[4] = {0, 0, 0, 0};
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        int sx = std::min(2 * x + dx, w - 1), sy = std::min(2 * y + dy, h - 1);
                        float v[4];
                        if (L.empty()) px0(sx, sy, v);
                        else std::copy_n(&L.back().px[(size_t(sy) * w + sx) * 4], 4, v);
                        for (int c = 0; c < 3; ++c) s[c] += v[c] * v[3];
                        s[3] += v[3];
                    }
                float *o = &n.px[(size_t(y) * nw + x) * 4];
                if (s[3] > 0)
                    for (int c = 0; c < 3; ++c) o[c] = s[c] / s[3];
                o[3] = std::min(1.f, s[3]);
            }
        L.push_back(std::move(n));
        w = L.back().w;
        h = L.back().h;
    }
    // bilinear sample of level `c` at the position of pixel (x, y) of the level below it
    auto sample = [](const Level &c, int x, int y, float o[3]) {
        float fx = std::clamp((x + 0.5f) * 0.5f - 0.5f, 0.f, float(c.w - 1));
        float fy = std::clamp((y + 0.5f) * 0.5f - 0.5f, 0.f, float(c.h - 1));
        int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, c.w - 1), y1 = std::min(y0 + 1, c.h - 1);
        float tx = fx - x0, ty = fy - y0;
        for (int k = 0; k < 3; ++k) {
            auto at = [&](int xx, int yy) { return c.px[(size_t(yy) * c.w + xx) * 4 + k]; };
            o[k] = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
        }
    };
    for (size_t k = L.size() - 1; k-- > 0;) {  // push: coarse -> fine, levels 1+
        Level &f = L[k];
        const Level &c = L[k + 1];
        for (int y = 0; y < f.h; ++y)
            for (int x = 0; x < f.w; ++x) {
                float *p = &f.px[(size_t(y) * f.w + x) * 4];
                if (p[3] >= 1.f) continue;
                float u[3];
                sample(c, x, y, u);
                for (int i = 0; i < 3; ++i) p[i] = p[i] * p[3] + u[i] * (1 - p[3]);
                p[3] = 1.f;
            }
    }
    for (int y = 0; y < H; ++y) {  // level 0: only empty pixels change
        uint8_t *row = im.row(y);
        for (int x = 0; x < W; ++x) {
            if (alpha[size_t(y) * W + x] > kTransparent) continue;
            float u[3];
            sample(L[0], x, y, u);
            row[x * 4 + 0] = q8(u[2]);
            row[x * 4 + 1] = q8(u[1]);
            row[x * 4 + 2] = q8(u[0]);
        }
    }
}

void letterbox(const ImageBGRA8 &in, int out_w, int out_h, ImageBGRA8 &out) {
    out.resize(out_w, out_h);  // black bars (opaque black, like the GPU bypass)
    for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
    if (in.width <= 0 || in.height <= 0 || out_w <= 0 || out_h <= 0) return;
    const crt::Rect r = crt::fit_rect(in.width, in.height, out_w, out_h);
    std::vector<int> sx(size_t(r.w));
    for (int x = 0; x < r.w; ++x) sx[size_t(x)] = std::min(in.width - 1, int((x + 0.5) * in.width / r.w));
    for (int y = 0; y < r.h; ++y) {
        const uint8_t *s = in.row(std::min(in.height - 1, int((y + 0.5) * in.height / r.h)));
        uint8_t *o = out.row(r.y + y) + size_t(r.x) * 4;
        for (int x = 0; x < r.w; ++x) std::memcpy(o + size_t(x) * 4, s + size_t(sx[size_t(x)]) * 4, 4);
    }
}

namespace {
// Result picture (in_w x in_h, read by `rgb(x, y, v)`) + source alpha -> host
// image; the alpha conventions of from_bgra8.
template <class Rgb>
void merge_out(int in_w, int in_h, Rgb &&rgb, const std::vector<float> *alpha, ntscrt_image &dst, AlphaMode mode,
               bool premult) {
    const float missing = mode == AlphaMode::Spill ? 0.f : 1.f;
    const bool opaque = alpha && alpha->empty();  // to_bgra8 stores no alpha for opaque sources
    if (opaque) alpha = nullptr;
    const float fixed = opaque ? 1.f : missing;
    with_format(dst.format, [&]<ntscrt_pixel_format F>() {
        for_rows(std::min(dst.width, in_w), std::min(dst.height, in_h), [&](int y) {
            auto row = static_cast<uint8_t *>(dst.data) + ptrdiff_t(y) * dst.row_bytes;
            for (int x = 0; x < dst.width && x < in_w; ++x) {
                float v[4];
                rgb(x, y, v);
                v[3] = 1.f;
                merge_alpha(v, alpha ? (*alpha)[size_t(y) * in_w + x] : fixed, mode, premult);
                write_px<F>(row, x, v);
            }
        });
    });
}
// 0..1, NaN -> 0 (what an 8-bit UNORM target stores)
inline float unit(float v) { return v > 0.f ? (v < 1.f ? v : 1.f) : 0.f; }
}  // namespace

void from_bgra8(const ImageBGRA8 &in, const std::vector<float> *alpha, ntscrt_image &dst, AlphaMode mode,
                bool premult) {
    merge_out(
        in.width, in.height,
        [&](int x, int y, float v[4]) {
            const uint8_t *s = in.row(y) + size_t(x) * 4;
            v[0] = s[2] / 255.f;
            v[1] = s[1] / 255.f;
            v[2] = s[0] / 255.f;
        },
        alpha, dst, mode, premult);
}

void from_rgba16f(const uint16_t *px, int w, int h, const std::vector<float> *alpha, ntscrt_image &dst,
                  AlphaMode mode, bool premult) {
    merge_out(
        w, h,
        [&](int x, int y, float v[4]) {
            const uint16_t *s = px + (size_t(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) v[c] = unit(half_to_float(s[c]));
        },
        alpha, dst, mode, premult);
}
}  // namespace ntscrt
