// crt_sizing.h — output geometry of the CRT stage.
#pragma once

#include <algorithm>
#include <cmath>

namespace ntscrt::crt {

// Port of upstream ScanlineGrid.supersampleFactor: multiple of the chain
// input to render at before area-downsampling to the target, so scanlines
// don't band when a source line covers a fractional number of rows.
inline int supersample_factor(int input_height, int target_height) {
    if (input_height <= 0 || target_height <= 0) return 1;
    if (target_height % input_height == 0) return 1;  // exact multiple: periodic already
    double rows_per_line = double(target_height) / double(input_height);
    if (rows_per_line >= 3) return 1;  // well resolved
    int k = 4;  // even multiples only (odd ones jitter the glow shaders)
    while (k > 2 && (k * input_height > 4 * target_height || k * input_height > 4096)) k -= 2;
    if (k * input_height > 4 * target_height || k * input_height > 4096) return 1;
    return k;
}

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Rect &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// Where the chain input lands in an out_w x out_h frame: the largest rect of
// the input's aspect, centred on whole pixels (letterbox / pillarbox). An
// aspect within 1% of the output's fills it (the downscale rounds its height
// to even lines, so the raster's aspect is rarely exactly the source's).
inline Rect fit_rect(int in_w, int in_h, int out_w, int out_h) {
    Rect full{0, 0, out_w, out_h};
    if (in_w <= 0 || in_h <= 0 || out_w <= 0 || out_h <= 0) return full;
    double a_in = double(in_w) / in_h, a_out = double(out_w) / out_h;
    if (std::fabs(a_in / a_out - 1.0) < 0.01) return full;
    Rect r;
    if (a_in > a_out) {
        r.w = out_w;
        r.h = std::clamp(int(std::lround(out_w / a_in)), 1, out_h);
    } else {
        r.h = out_h;
        r.w = std::clamp(int(std::lround(out_h * a_in)), 1, out_w);
    }
    r.x = (out_w - r.w) / 2;
    r.y = (out_h - r.h) / 2;
    return r;
}

}  // namespace ntscrt::crt
