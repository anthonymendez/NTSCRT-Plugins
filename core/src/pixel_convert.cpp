#include "pixel_convert.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ntscrt {
namespace {
inline uint8_t q8(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

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

inline void read_px(const uint8_t *row, int x, ntscrt_pixel_format f, float o[4]) {
    switch (f) {
    case NTSCRT_RGBA8: { auto p = row + x * 4; for (int c = 0; c < 4; ++c) o[c] = p[c] / 255.f; break; }
    case NTSCRT_BGRA8: { auto p = row + x * 4; o[0] = p[2] / 255.f; o[1] = p[1] / 255.f; o[2] = p[0] / 255.f; o[3] = p[3] / 255.f; break; }
    case NTSCRT_RGBA16: { auto p = reinterpret_cast<const uint16_t *>(row) + x * 4; for (int c = 0; c < 4; ++c) o[c] = p[c] / 65535.f; break; }
    case NTSCRT_RGBA16F: { auto p = reinterpret_cast<const uint16_t *>(row) + x * 4; for (int c = 0; c < 4; ++c) o[c] = half_to_float(p[c]); break; }
    case NTSCRT_RGBA32F: { auto p = reinterpret_cast<const float *>(row) + x * 4; for (int c = 0; c < 4; ++c) o[c] = p[c]; break; }
    }
}

inline void write_px(uint8_t *row, int x, ntscrt_pixel_format f, const float v[4]) {
    switch (f) {
    case NTSCRT_RGBA8: { auto p = row + x * 4; for (int c = 0; c < 4; ++c) p[c] = q8(v[c]); break; }
    case NTSCRT_BGRA8: { auto p = row + x * 4; p[0] = q8(v[2]); p[1] = q8(v[1]); p[2] = q8(v[0]); p[3] = q8(v[3]); break; }
    case NTSCRT_RGBA16: { auto p = reinterpret_cast<uint16_t *>(row) + x * 4; for (int c = 0; c < 4; ++c) p[c] = uint16_t(std::lround(std::clamp(v[c], 0.f, 1.f) * 65535.f)); break; }
    case NTSCRT_RGBA16F: { auto p = reinterpret_cast<uint16_t *>(row) + x * 4; for (int c = 0; c < 4; ++c) p[c] = float_to_half(v[c]); break; }
    case NTSCRT_RGBA32F: { auto p = reinterpret_cast<float *>(row) + x * 4; for (int c = 0; c < 4; ++c) p[c] = v[c]; break; }
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
}

void to_bgra8(const ntscrt_image &src, ImageBGRA8 &out, std::vector<float> *alpha_out, AlphaMode mode,
              bool src_premult) {
    out.resize(src.width, src.height);
    std::vector<float> local;
    std::vector<float> *alpha = alpha_out ? alpha_out : (mode == AlphaMode::Clip ? &local : nullptr);
    if (alpha) alpha->assign(size_t(src.width) * src.height, 1.f);
    bool any_transparent = false;
    for (int y = 0; y < src.height; ++y) {
        auto row = static_cast<const uint8_t *>(src.data) + ptrdiff_t(y) * src.row_bytes;
        uint8_t *o = out.row(y);
        for (int x = 0; x < src.width; ++x) {
            float v[4];
            read_px(row, x, src.format, v);
            const float a = std::clamp(v[3], 0.f, 1.f);
            if (mode == AlphaMode::Clip) {
                // straight colour: what the element looks like on its own
                if (src_premult && a > kTransparent && a < 1.f)
                    for (int c = 0; c < 3; ++c) v[c] /= a;
            } else if (!src_premult) {
                // the element over black
                for (int c = 0; c < 3; ++c) v[c] *= a;
            }
            any_transparent |= a <= kTransparent;
            o[x * 4 + 0] = q8(v[2]);
            o[x * 4 + 1] = q8(v[1]);
            o[x * 4 + 2] = q8(v[0]);
            o[x * 4 + 3] = 255;
            if (alpha) (*alpha)[size_t(y) * src.width + x] = a;
        }
    }
    if (mode == AlphaMode::Clip && any_transparent) fill_transparent(out, *alpha);
}

// Pull-push hole filling: average the non-transparent colours down a mip
// pyramid, then fill each empty pixel from the next coarser level (bilinear),
// so empty areas take the colour of the nearest content, smoothly.
void fill_transparent(ImageBGRA8 &im, const std::vector<float> &alpha) {
    struct Level { int w, h; std::vector<float> px; };  // r, g, b, weight (colour straight)
    const int W = im.width, H = im.height;
    if (W <= 0 || H <= 0) return;
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

void from_bgra8(const ImageBGRA8 &in, const std::vector<float> *alpha, ntscrt_image &dst, AlphaMode mode,
                bool premult) {
    const float missing = mode == AlphaMode::Spill ? 0.f : 1.f;
    for (int y = 0; y < dst.height && y < in.height; ++y) {
        auto row = static_cast<uint8_t *>(dst.data) + ptrdiff_t(y) * dst.row_bytes;
        const uint8_t *s = in.row(y);
        for (int x = 0; x < dst.width && x < in.width; ++x) {
            float v[4] = {s[x * 4 + 2] / 255.f, s[x * 4 + 1] / 255.f, s[x * 4 + 0] / 255.f, 1.f};
            merge_alpha(v, alpha ? (*alpha)[size_t(y) * in.width + x] : missing, mode, premult);
            write_px(row, x, dst.format, v);
        }
    }
}
}  // namespace ntscrt
