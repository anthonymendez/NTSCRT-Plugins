// ntscrt-imgdiff — compare two PNGs the way test_golden does.
//
//   ntscrt-imgdiff a.png b.png [--threshold T] [--diff out.png]
//
// Prints mean/max abs RGB difference (0..255 units). Exit 0 if same size and
// mean <= threshold (default 2.0), 1 if over, 2 on usage/IO errors.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "golden_common.h"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    using namespace golden;
    std::string a, b, diff_path;
    double thr = 2.0;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--threshold" && i + 1 < argc) thr = std::atof(argv[++i]);
        else if (s == "--diff" && i + 1 < argc) diff_path = argv[++i];
        else if (a.empty()) a = s;
        else if (b.empty()) b = s;
        else { std::fprintf(stderr, "unexpected argument %s\n", s.c_str()); return 2; }
    }
    if (a.empty() || b.empty()) {
        std::fprintf(stderr, "usage: ntscrt-imgdiff a.png b.png [--threshold T] [--diff out.png]\n");
        return 2;
    }
    Image ia, ib, id;
    if (!load_png(fs::u8path(a), ia)) { std::fprintf(stderr, "cannot read %s\n", a.c_str()); return 2; }
    if (!load_png(fs::u8path(b), ib)) { std::fprintf(stderr, "cannot read %s\n", b.c_str()); return 2; }
    DiffStats s = compare_rgb(ia, ib, diff_path.empty() ? nullptr : &id);
    if (!s.same_size) {
        std::printf("size mismatch: %dx%d vs %dx%d\n", ia.w, ia.h, ib.w, ib.h);
        return 1;
    }
    if (!diff_path.empty()) write_png(fs::u8path(diff_path), id);
    std::printf("mean=%.4f max=%d over16=%.4f threshold=%.2f %s\n", s.mean_abs, s.max_abs, s.frac_over_16, thr,
                s.mean_abs <= thr ? "ok" : "FAIL");
    return s.mean_abs <= thr ? 0 : 1;
}
