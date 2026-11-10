#pragma once
#include <ntscrt/ntscrt.h>

#include "internal.h"

namespace ntscrt {
// Host image -> internal BGRA8 (clamped, opaque). Source alpha copied to
// `alpha_out` (0..1 per pixel) when non-null.
void to_bgra8(const ntscrt_image &src, ImageBGRA8 &out, std::vector<float> *alpha_out);
// Internal BGRA8 -> host image (writes min of both sizes). Alpha from `alpha`
// (same dimensions as `in`) when non-null, else 1.
void from_bgra8(const ImageBGRA8 &in, const std::vector<float> *alpha, ntscrt_image &dst);
}  // namespace ntscrt
