#include "pixel_convert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#define NTSCRT_X86 1
#include <emmintrin.h>
#endif
#if defined(_MSC_VER) && defined(_M_X64)
#define NTSCRT_SIMD 1  // SSE4.1 + F16C kernels, used when the CPU has them
#include <immintrin.h>
#include <intrin.h>
#endif

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

size_t pixel_size(ntscrt_pixel_format f) {
    switch (f) {
    case NTSCRT_RGBA8:
    case NTSCRT_BGRA8: return 4;
    case NTSCRT_RGBA16:
    case NTSCRT_RGBA16F: return 8;
    case NTSCRT_RGBA32F: return 16;
    }
    return 16;
}

// Host pictures are written with non-temporal (streaming) stores on x86:
// a UHD float picture is 133 MB, so the cache lines a normal store first
// reads in are wasted memory traffic. Off with NTSCRT_FAST_TRANSFER=0.
bool streaming_writes() {
#if defined(NTSCRT_X86)
    static const bool on = [] {
        const char *e = std::getenv("NTSCRT_FAST_TRANSFER");
        return !(e && *e == '0');
    }();
    return on;
#else
    return false;
#endif
}

// This thread's row buffer, at least `bytes` long.
uint8_t *row_buffer(size_t bytes) {
    thread_local std::vector<uint8_t> buf;
    if (buf.size() < bytes) buf.resize(bytes);
    return buf.data();
}

// dst[0, bytes) = src[0, bytes): streaming stores for the 16-byte aligned
// middle, plain copies for the ends; fenced, so the bytes are visible to
// other threads once it returns (as after an ordinary store).
void stream_row(uint8_t *dst, const uint8_t *src, size_t bytes) {
#if defined(NTSCRT_X86)
    size_t head = (16 - (reinterpret_cast<uintptr_t>(dst) & 15)) & 15;
    if (head > bytes) head = bytes;
    std::memcpy(dst, src, head);
    size_t i = head;
    for (; i + 16 <= bytes; i += 16)
        _mm_stream_si128(reinterpret_cast<__m128i *>(dst + i),
                         _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i)));
    std::memcpy(dst + i, src + i, bytes - i);
    _mm_sfence();
#else
    std::memcpy(dst, src, bytes);
#endif
}

// ---- SIMD pixel kernels (x86-64 with SSE4.1 + F16C, MSVC) ------------------
// One pixel per 128-bit register, the same IEEE operations as the scalar code
// above in the same order (division stays division, the 8/16-bit rounding
// goes through double exactly like round_unit), so the bytes are identical:
// tests/core/test_convert_threads.cpp compares both paths on every format,
// alpha mode and premultiplication.
#if defined(NTSCRT_SIMD)
bool cpu_has_simd() {
    int r[4];
    __cpuid(r, 1);
    const bool sse41 = (r[2] >> 19) & 1, f16c = (r[2] >> 29) & 1;
    return sse41 && f16c;
}
#endif

// Every 8-bit channel value k as the scalar code converts k / 255.f to each
// host format: opaque 8-bit pixels go through these tables (same bytes).
struct Unorm8Tables {
    uint8_t q8[256];
    uint16_t q16[256], h[256];
    float f[256];
};
const Unorm8Tables &unorm8_table() {
    static const Unorm8Tables t = [] {
        Unorm8Tables r{};
        for (int k = 0; k < 256; ++k) {
            const float v = k / 255.f;
            r.q8[k] = q8(v);
            r.q16[k] = q16(v);
            r.h[k] = float_to_half(v);
            r.f[k] = v;
        }
        return r;
    }();
    return t;
}

bool &simd_flag() {
    static bool on = [] {
#if defined(NTSCRT_SIMD)
        const char *e = std::getenv("NTSCRT_FAST_TRANSFER");
        return !(e && *e == '0') && cpu_has_simd();
#else
        return false;
#endif
    }();
    return on;
}

#if defined(NTSCRT_SIMD)
// The host pixel as floats r, g, b, a (read_px).
template <ntscrt_pixel_format F> inline __m128 load4(const uint8_t *row, int x) {
    if constexpr (F == NTSCRT_RGBA32F) {
        return _mm_loadu_ps(reinterpret_cast<const float *>(row) + size_t(x) * 4);
    } else if constexpr (F == NTSCRT_RGBA16F) {
        return _mm_cvtph_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(row + size_t(x) * 8)));
    } else if constexpr (F == NTSCRT_RGBA16) {
        const __m128i v = _mm_cvtepu16_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(row + size_t(x) * 8)));
        return _mm_div_ps(_mm_cvtepi32_ps(v), _mm_set1_ps(65535.f));
    } else {
        int32_t b;
        std::memcpy(&b, row + size_t(x) * 4, 4);
        __m128 v = _mm_div_ps(_mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(b))), _mm_set1_ps(255.f));
        if constexpr (F == NTSCRT_BGRA8) v = _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 0, 1, 2));
        return v;
    }
}

// round_unit(v, max) of 4 lanes: clamped to [0, 1] first (NaN -> 0: maxps
// returns its second operand for NaN), which round_unit's branches give too.
inline __m128i round4(__m128 v, float max) {
    v = _mm_min_ps(_mm_max_ps(v, _mm_setzero_ps()), _mm_set1_ps(1.f));
    const __m128 p = _mm_mul_ps(v, _mm_set1_ps(max));
    const __m128d half = _mm_set1_pd(0.5);
    const __m128i lo = _mm_cvttpd_epi32(_mm_add_pd(_mm_cvtps_pd(p), half));
    const __m128i hi = _mm_cvttpd_epi32(_mm_add_pd(_mm_cvtps_pd(_mm_movehl_ps(p, p)), half));
    return _mm_unpacklo_epi64(lo, hi);
}

// write_px for 4 lanes r, g, b, a (each in [0, 1]); RGBA16F stays scalar.
template <ntscrt_pixel_format F> inline void store4(uint8_t *row, int x, __m128 v) {
    if constexpr (F == NTSCRT_RGBA32F) {
        _mm_storeu_ps(reinterpret_cast<float *>(row) + size_t(x) * 4, v);
    } else if constexpr (F == NTSCRT_RGBA16) {
        const __m128i q = round4(v, 65535.f);
        _mm_storel_epi64(reinterpret_cast<__m128i *>(row + size_t(x) * 8), _mm_packus_epi32(q, q));
    } else if constexpr (F == NTSCRT_RGBA8 || F == NTSCRT_BGRA8) {
        if constexpr (F == NTSCRT_BGRA8) v = _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 0, 1, 2));
        __m128i q = round4(v, 255.f);
        q = _mm_packus_epi16(_mm_packs_epi32(q, q), q);
        const int32_t b = _mm_cvtsi128_si32(q);
        std::memcpy(row + size_t(x) * 4, &b, 4);
    } else {
        alignas(16) float f[4];
        _mm_store_ps(f, v);
        write_px<F>(row, x, f);
    }
}
#endif
}  // namespace

namespace detail {
void set_simd(bool on) { simd_flag() = on && [] {
#if defined(NTSCRT_SIMD)
    return cpu_has_simd();
#else
    return false;
#endif
}(); }
bool simd() { return simd_flag(); }
}  // namespace detail

namespace {
// to_bgra8's conversion into rows of `dst` (`pitch` apart), written in order
// and never read (dst may be GPU upload memory). `alpha` as to_bgra8's
// alpha_out (cleared first, filled only when some pixel isn't opaque).
// Returns whether some pixel is transparent (Clip mode then needs fill_transparent).
bool convert_in(const ntscrt_image &src, uint8_t *dst, ptrdiff_t pitch, std::vector<float> *alpha, AlphaMode mode,
                bool src_premult) {
    if (alpha) alpha->clear();
    // Per row: bit 0 = some alpha isn't 1, bit 1 = some pixel is transparent.
    std::vector<uint8_t> row_flags(size_t(std::max(0, src.height)), 0);
    [[maybe_unused]] const bool simd = simd_flag();
    with_format(src.format, [&]<ntscrt_pixel_format F>() {
        for_rows(src.width, src.height, [&](int y) {
            auto row = static_cast<const uint8_t *>(src.data) + ptrdiff_t(y) * src.row_bytes;
            uint8_t *o = dst + ptrdiff_t(y) * pitch;
            uint8_t flags = 0;
#if defined(NTSCRT_SIMD)
            if (simd) {
                for (int x = 0; x < src.width; ++x) {
                    if constexpr (F == NTSCRT_RGBA8 || F == NTSCRT_BGRA8) {
                        // Opaque 8-bit pixel: alpha 1 changes nothing; q8(k / 255) from a table.
                        const uint8_t *p = row + size_t(x) * 4;
                        if (p[3] == 255) {
                            const uint8_t *t = unorm8_table().q8;
                            constexpr int ri = F == NTSCRT_RGBA8 ? 0 : 2;  // red's byte
                            o[x * 4 + 0] = t[p[2 - ri]];
                            o[x * 4 + 1] = t[p[1]];
                            o[x * 4 + 2] = t[p[ri]];
                            o[x * 4 + 3] = 255;
                            continue;
                        }
                    }
                    __m128 v = load4<F>(row, x);
                    const float a = unit_alpha(_mm_cvtss_f32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3))));
                    if (mode == AlphaMode::Clip) {
                        if (src_premult && a > kTransparent && a < 1.f) v = _mm_div_ps(v, _mm_set1_ps(a));
                    } else if (!src_premult) {
                        v = _mm_mul_ps(v, _mm_set1_ps(a));
                    }
                    flags |= uint8_t((a != 1.f ? 1 : 0) | (a <= kTransparent ? 2 : 0));
                    __m128i q = round4(_mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 0, 1, 2)), 255.f);  // b, g, r
                    q = _mm_packus_epi16(_mm_packs_epi32(q, q), q);
                    const int32_t b = _mm_cvtsi128_si32(q) | int32_t(0xff000000u);
                    std::memcpy(o + size_t(x) * 4, &b, 4);
                }
                row_flags[size_t(y)] = flags;
                return;
            }
#endif
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
    return std::any_of(row_flags.begin(), row_flags.end(), [](uint8_t f) { return f & 2; });
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
    const bool any_transparent = convert_in(src, out.px.data(), ptrdiff_t(src.width) * 4, alpha, mode, src_premult);
    if (mode == AlphaMode::Clip && any_transparent) fill_transparent(out, *alpha);
}

bool to_bgra8_rows(const ntscrt_image &src, uint8_t *rows, ptrdiff_t pitch, std::vector<float> *alpha_out,
                   AlphaMode mode, bool src_premult) {
    std::vector<float> local;
    std::vector<float> *alpha = alpha_out ? alpha_out : (mode == AlphaMode::Clip ? &local : nullptr);
    const bool any_transparent = convert_in(src, rows, pitch, alpha, mode, src_premult);
    return !(mode == AlphaMode::Clip && any_transparent);
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
// 0..1, NaN -> 0 (what an 8-bit UNORM target stores)
inline float unit(float v) { return v > 0.f ? (v < 1.f ? v : 1.f) : 0.f; }

// Rows [y0, y1) of the result picture (in_w wide; row y0 at `rows`, rows
// `pitch` bytes apart; RGBA16F when kHalf, else BGRA8) + source alpha -> the
// same rows of the host image; the alpha conventions of from_bgra8.
template <bool kHalf>
void merge_out(const uint8_t *rows, ptrdiff_t pitch, int in_w, int y0, int y1, const std::vector<float> *alpha,
               ntscrt_image &dst, AlphaMode mode, bool premult) {
    const float missing = mode == AlphaMode::Spill ? 0.f : 1.f;
    const bool opaque = alpha && alpha->empty();  // to_bgra8 stores no alpha for opaque sources
    if (opaque) alpha = nullptr;
    const float fixed = opaque ? 1.f : missing;
    y1 = std::min(y1, dst.height);
    if (y1 <= y0) return;
    const int w = std::min(dst.width, in_w);
    auto rgb = [&](int x, int y, float v[4]) {
        const uint8_t *r = rows + ptrdiff_t(y - y0) * pitch;
        if constexpr (kHalf) {
            const uint16_t *s = reinterpret_cast<const uint16_t *>(r) + size_t(x) * 4;
            for (int c = 0; c < 3; ++c) v[c] = unit(half_to_float(s[c]));
        } else {
            const uint8_t *s = r + size_t(x) * 4;
            v[0] = s[2] / 255.f;
            v[1] = s[1] / 255.f;
            v[2] = s[0] / 255.f;
        }
    };
    [[maybe_unused]] const bool simd = simd_flag() && mode == AlphaMode::Clip;
    [[maybe_unused]] const bool tables = simd_flag() && !alpha && fixed == 1.f;
    with_format(dst.format, [&]<ntscrt_pixel_format F>() {
        const size_t row_bytes = size_t(w) * pixel_size(F);
        // Large rows: converted into a cached row buffer, then streamed to
        // the host picture (stream_row), which is far bigger than any cache.
        const bool stream = streaming_writes() && row_bytes >= 4096;
        for_rows(w, y1 - y0, [&](int r) {
            const int y = y0 + r;
            uint8_t *out = static_cast<uint8_t *>(dst.data) + ptrdiff_t(y) * dst.row_bytes;
            uint8_t *row = stream ? row_buffer(row_bytes) : out;
            if constexpr (!kHalf) {
                if (tables) {
                    // Opaque, from 8 bits: alpha 1 leaves the colour as is in
                    // both modes, so each channel is a table entry.
                    const Unorm8Tables &t = unorm8_table();
                    const uint8_t *src = rows + ptrdiff_t(r) * pitch;
                    for (int x = 0; x < w; ++x) {
                        const uint8_t *s = src + size_t(x) * 4;  // b, g, r
                        if constexpr (F == NTSCRT_RGBA8 || F == NTSCRT_BGRA8) {
                            constexpr int ri = F == NTSCRT_RGBA8 ? 0 : 2;
                            uint8_t *o = row + size_t(x) * 4;
                            o[ri] = t.q8[s[2]];
                            o[1] = t.q8[s[1]];
                            o[2 - ri] = t.q8[s[0]];
                            o[3] = 255;
                        } else if constexpr (F == NTSCRT_RGBA16 || F == NTSCRT_RGBA16F) {
                            const uint16_t *q = F == NTSCRT_RGBA16 ? t.q16 : t.h;
                            const uint16_t px[4] = {q[s[2]], q[s[1]], q[s[0]], q[255]};
                            std::memcpy(row + size_t(x) * 8, px, 8);
                        } else {
                            const float px[4] = {t.f[s[2]], t.f[s[1]], t.f[s[0]], 1.f};
                            std::memcpy(row + size_t(x) * 16, px, 16);
                        }
                    }
                    if (stream) stream_row(out, row, row_bytes);
                    return;
                }
            }
#if defined(NTSCRT_SIMD)
            if (simd) {
                // Clip mode (merge_alpha): rgb (* a when premultiplied), alpha a.
                const uint8_t *src = rows + ptrdiff_t(r) * pitch;
                const float *arow = alpha ? alpha->data() + size_t(y) * in_w : nullptr;
                for (int x = 0; x < w; ++x) {
                    __m128 v;
                    if constexpr (kHalf) {
                        v = _mm_cvtph_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(src + size_t(x) * 8)));
                        v = _mm_min_ps(_mm_max_ps(v, _mm_setzero_ps()), _mm_set1_ps(1.f));  // unit()
                    } else {
                        v = load4<NTSCRT_BGRA8>(src, x);
                    }
                    const __m128 a = _mm_set1_ps(arow ? arow[x] : fixed);
                    if (premult) v = _mm_mul_ps(v, a);
                    store4<F>(row, x, _mm_blend_ps(v, a, 8));
                }
                if (stream) stream_row(out, row, row_bytes);
                return;
            }
#endif
            for (int x = 0; x < w; ++x) {
                float v[4];
                rgb(x, y, v);
                v[3] = 1.f;
                merge_alpha(v, alpha ? (*alpha)[size_t(y) * in_w + x] : fixed, mode, premult);
                write_px<F>(row, x, v);
            }
            if (stream) stream_row(out, row, row_bytes);
        });
    });
}
}  // namespace

void from_bgra8(const ImageBGRA8 &in, const std::vector<float> *alpha, ntscrt_image &dst, AlphaMode mode,
                bool premult) {
    from_bgra8_rows(in.px.data(), ptrdiff_t(in.width) * 4, in.width, 0, in.height, alpha, dst, mode, premult);
}

void from_rgba16f(const uint16_t *px, int w, int h, const std::vector<float> *alpha, ntscrt_image &dst,
                  AlphaMode mode, bool premult) {
    from_rgba16f_rows(reinterpret_cast<const uint8_t *>(px), ptrdiff_t(w) * 8, w, 0, h, alpha, dst, mode, premult);
}

void from_bgra8_rows(const uint8_t *rows, ptrdiff_t pitch, int w, int y0, int y1, const std::vector<float> *alpha,
                     ntscrt_image &dst, AlphaMode mode, bool premult) {
    merge_out<false>(rows, pitch, w, y0, y1, alpha, dst, mode, premult);
}

void from_rgba16f_rows(const uint8_t *rows, ptrdiff_t pitch, int w, int y0, int y1, const std::vector<float> *alpha,
                       ntscrt_image &dst, AlphaMode mode, bool premult) {
    merge_out<true>(rows, pitch, w, y0, y1, alpha, dst, mode, premult);
}
}  // namespace ntscrt
