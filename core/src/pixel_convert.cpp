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

void to_bgra8(const ntscrt_image &src, ImageBGRA8 &out, std::vector<float> *alpha_out) {
    out.resize(src.width, src.height);
    if (alpha_out) alpha_out->assign(size_t(src.width) * src.height, 1.f);
    for (int y = 0; y < src.height; ++y) {
        auto row = static_cast<const uint8_t *>(src.data) + ptrdiff_t(y) * src.row_bytes;
        uint8_t *o = out.row(y);
        for (int x = 0; x < src.width; ++x) {
            float v[4];
            read_px(row, x, src.format, v);
            o[x * 4 + 0] = q8(v[2]);
            o[x * 4 + 1] = q8(v[1]);
            o[x * 4 + 2] = q8(v[0]);
            o[x * 4 + 3] = 255;
            if (alpha_out) (*alpha_out)[size_t(y) * src.width + x] = v[3];
        }
    }
}

void from_bgra8(const ImageBGRA8 &in, const std::vector<float> *alpha, ntscrt_image &dst) {
    for (int y = 0; y < dst.height && y < in.height; ++y) {
        auto row = static_cast<uint8_t *>(dst.data) + ptrdiff_t(y) * dst.row_bytes;
        const uint8_t *s = in.row(y);
        for (int x = 0; x < dst.width && x < in.width; ++x) {
            float v[4] = {s[x * 4 + 2] / 255.f, s[x * 4 + 1] / 255.f, s[x * 4 + 0] / 255.f,
                          alpha ? (*alpha)[size_t(y) * in.width + x] : 1.f};
            write_px(row, x, dst.format, v);
        }
    }
}
}  // namespace ntscrt
