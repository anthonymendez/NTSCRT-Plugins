#pragma once
#include <ntscrt/ntscrt.h>

#include <algorithm>

#include "internal.h"

namespace ntscrt {
// How the effect treats the source's alpha (look "alpha.mode").
//   Clip:  the pipeline sees the element's own colours (unpremultiplied, with
//          transparent areas filled from the nearest edge colour so blurs don't
//          pull black into the edges); output alpha = source alpha.
//   Spill: the pipeline sees the element over black; output alpha =
//          max(source alpha, brightest channel), so ghosts, smear and glow
//          outside the element show as light over whatever is underneath.
enum class AlphaMode { Clip, Spill };
AlphaMode alpha_mode(const json &look);

// Host image -> internal BGRA8 (clamped, opaque) prepared for `mode` (see
// above). `src_premult`: host RGB is premultiplied. Source alpha copied to
// `alpha_out` (0..1 per pixel) when non-null; left EMPTY when every pixel is
// opaque (no buffer for ordinary video). Everything below reads an empty
// alpha vector as "all 1".
void to_bgra8(const ntscrt_image &src, ImageBGRA8 &out, std::vector<float> *alpha_out,
              AlphaMode mode = AlphaMode::Clip, bool src_premult = false);
// to_bgra8 into caller memory: rows `pitch` bytes apart, written in order and
// never read (GPU upload memory). false when the picture needs
// fill_transparent (Clip mode, some pixel transparent): the rows then aren't
// the final picture, and the caller converts with to_bgra8 instead.
bool to_bgra8_rows(const ntscrt_image &src, uint8_t *rows, ptrdiff_t pitch, std::vector<float> *alpha_out,
                   AlphaMode mode, bool src_premult);
// Clip mode: replace RGB where alpha is ~0 by the colour of the nearest
// non-transparent area (pull-push). No-op when nothing is transparent.
void fill_transparent(ImageBGRA8 &im, const std::vector<float> &alpha);
// Places `in` into a out_w x out_h picture the way the CRT stage's bypass
// does (fitted at its aspect, centred, black bars), for the no-GPU path.
void letterbox(const ImageBGRA8 &in, int out_w, int out_h, ImageBGRA8 &out);

// Output pixel from processed straight-or-over-black RGB `c` and source alpha
// `a` (alpha mode semantics above), in the destination's convention.
inline void merge_alpha(float c[4], float a, AlphaMode mode, bool premult) {
    if (mode == AlphaMode::Spill) {
        float o = std::max(a, std::max(c[0], std::max(c[1], c[2])));
        if (!premult)
            for (int i = 0; i < 3; ++i) c[i] = o > 0 ? std::min(1.f, c[i] / o) : 0.f;
        c[3] = o;
    } else {
        if (premult)
            for (int i = 0; i < 3; ++i) c[i] *= a;
        c[3] = a;
    }
}

// Internal BGRA8 -> host image (writes min of both sizes). `alpha` (same
// dimensions as `in`, or empty = opaque) is the source alpha; NULL = not
// aligned with the result (crop on): treated as 1 in Clip mode, 0 in Spill mode.
void from_bgra8(const ImageBGRA8 &in, const std::vector<float> *alpha, ntscrt_image &dst,
                AlphaMode mode = AlphaMode::Clip, bool premult = false);
// Same from a w x h RGBA16F picture (the CRT stage's output for 16-bit and
// float hosts: no 8-bit step). RGB is clamped to 0..1 (NaN = 0), the range
// the look is defined in, as an 8-bit target would store it.
void from_rgba16f(const uint16_t *px, int w, int h, const std::vector<float> *alpha, ntscrt_image &dst,
                  AlphaMode mode = AlphaMode::Clip, bool premult = false);
// The same for rows [y0, y1) of a `w`-wide result whose row y0 is at `rows`,
// rows `pitch` bytes apart (a band read straight from GPU readback memory):
// writes those rows of dst only. `alpha` as above, for the whole picture.
void from_bgra8_rows(const uint8_t *rows, ptrdiff_t pitch, int w, int y0, int y1, const std::vector<float> *alpha,
                     ntscrt_image &dst, AlphaMode mode, bool premult);
void from_rgba16f_rows(const uint8_t *rows, ptrdiff_t pitch, int w, int y0, int y1, const std::vector<float> *alpha,
                       ntscrt_image &dst, AlphaMode mode, bool premult);

namespace detail {
// The SIMD conversion kernels (x86-64 with SSE4.1 + F16C; the same bytes as
// the scalar code): on by default where the CPU has them, unless
// NTSCRT_FAST_TRANSFER=0. Tests switch them to compare both paths.
void set_simd(bool on);
bool simd();
}  // namespace detail
}  // namespace ntscrt
