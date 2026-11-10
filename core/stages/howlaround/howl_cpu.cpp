// howl_cpu.cpp — see howl_cpu.h. Mirrors shaders/howlaround.slang.
#include "howl_cpu.h"

#include <algorithm>
#include <cmath>

namespace ntscrt::howl {

namespace {
inline float saturate(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
inline float smoothstep(float a, float b, float x) {
    float t = saturate((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
}  // namespace

ImageF ImageF::from_bgra8(const uint8_t *bgra, int w, int h) {
    ImageF im(w, h);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        im.px[i * 4 + 0] = bgra[i * 4 + 2] / 255.f;
        im.px[i * 4 + 1] = bgra[i * 4 + 1] / 255.f;
        im.px[i * 4 + 2] = bgra[i * 4 + 0] / 255.f;
        im.px[i * 4 + 3] = bgra[i * 4 + 3] / 255.f;
    }
    return im;
}

void ImageF::to_bgra8(uint8_t *bgra) const {
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        auto q = [](float x) { return uint8_t(int(saturate(x) * 255.f + 0.5f)); };
        bgra[i * 4 + 0] = q(px[i * 4 + 2]);
        bgra[i * 4 + 1] = q(px[i * 4 + 1]);
        bgra[i * 4 + 2] = q(px[i * 4 + 0]);
        bgra[i * 4 + 3] = q(px[i * 4 + 3]);
    }
}

void sample_linear(const ImageF &img, float u, float v, float out[4]) {
    // Clamped to the outer texel centres first (as the shader does), which
    // makes the result independent of the sampler's address mode.
    float W = float(img.width), H = float(img.height);
    u = std::clamp(u, 0.5f / W, 1.f - 0.5f / W);
    v = std::clamp(v, 0.5f / H, 1.f - 0.5f / H);
    float x = u * W - 0.5f, y = v * H - 0.5f;
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    float fx = x - x0, fy = y - y0;
    int x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    const float *a = img.at(x0, y0), *b = img.at(x1, y0), *c = img.at(x0, y1), *d = img.at(x1, y1);
    for (int k = 0; k < 4; ++k) out[k] = mixf(mixf(a[k], b[k], fx), mixf(c[k], d[k], fx), fy);
}

ImageF camera_ref(const ImageF &scene, const ImageF *tv, const ImageF *counter, const CameraUniforms &u) {
    const int W = scene.width, H = scene.height;
    ImageF out(W, H);
    const float aspect = u.params[0], zoom = u.params[1];
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const float *src = scene.at(gx, gy);
            float c[3] = {src[0], src[1], src[2]};
            float uvx = (float(gx) + 0.5f) / float(W), uvy = (float(gy) + 0.5f) / float(H);
            if (u.params2[3] > 0.5f) {
                float d[3] = {(uvx - 0.5f) * aspect, uvy - 0.5f, 1.0f};
                const float *n = u.normal, *ce = u.center;
                float denom = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
                if (std::fabs(denom) > 1e-6f) {
                    float t = (ce[0] * n[0] + ce[1] * n[1] + ce[2] * n[2]) / denom;
                    if (t > 0) {
                        float p[3] = {d[0] * t - ce[0], d[1] * t - ce[1], d[2] * t - ce[2]};
                        float sx = (p[0] * u.axis_x[0] + p[1] * u.axis_x[1] + p[2] * u.axis_x[2]) / (zoom * aspect) + 0.5f;
                        float sy = (p[0] * u.axis_y[0] + p[1] * u.axis_y[1] + p[2] * u.axis_y[2]) / zoom + 0.5f;
                        float ex = std::min(sx, 1.0f - sx) * zoom * float(W);
                        float ey = std::min(sy, 1.0f - sy) * zoom * float(H);
                        float cover = saturate(std::min(ex, ey) + 0.5f);
                        if (cover > 0.0f) {
                            float s4[4] = {0, 0, 0, 1};
                            if (tv) sample_linear(*tv, sx, sy, s4);
                            float s[3];
                            for (int k = 0; k < 3; ++k) {
                                s[k] = s4[k] * u.params[2];
                                s[k] = (s[k] - 0.5f) * u.params[3] + 0.5f;
                                s[k] *= u.balance[k];
                            }
                            float y = s[0] * 0.299f + s[1] * 0.587f + s[2] * 0.114f;
                            float i = s[0] * 0.596f + s[1] * -0.274f + s[2] * -0.322f;
                            float q = s[0] * 0.211f + s[1] * -0.523f + s[2] * 0.312f;
                            float i2 = i * u.params2[0] - q * u.params2[1];
                            float q2 = i * u.params2[1] + q * u.params2[0];
                            float r3[3] = {y + 0.956f * i2 + 0.621f * q2, y - 0.272f * i2 - 0.647f * q2,
                                           y - 1.106f * i2 + 1.703f * q2};
                            for (int k = 0; k < 3; ++k) c[k] = mixf(c[k], saturate(r3[k]), cover);
                        }
                    }
                }
            }
            if (u.key[0] > 0.0f) {
                float y = src[0] * 0.299f + src[1] * 0.587f + src[2] * 0.114f;
                if (u.key[1] > 0.5f) y = 1.0f - y;
                float level = 1.0f - u.key[0];
                float m = smoothstep(level - 0.06f, level + 0.06f, y);
                for (int k = 0; k < 3; ++k) c[k] = mixf(c[k], src[k], m);
            }
            for (int k = 0; k < 3; ++k) c[k] *= u.params2[2];
            const float *r = u.counter_rect;
            if (counter && r[2] > r[0] && uvx >= r[0] && uvx <= r[2] && uvy >= r[1] && uvy <= r[3]) {
                float t[4];
                sample_linear(*counter, (uvx - r[0]) / (r[2] - r[0]), (uvy - r[1]) / (r[3] - r[1]), t);
                for (int k = 0; k < 3; ++k) c[k] = c[k] * (1.0f - t[3]) + t[k];
            }
            float *o = out.at(gx, gy);
            for (int k = 0; k < 3; ++k) o[k] = quantize8(c[k]);
            o[3] = 1.0f;
        }
    return out;
}

ImageF blur_ref(const ImageF &src, const BlurUniforms &u) {
    const int W = src.width, H = src.height;
    ImageF out(W, H);
    int sx = int(u.dir[0]), sy = int(u.dir[1]);
    std::vector<float> w(size_t(2 * u.radius + 1));
    float wsum = 0;
    for (int k = -u.radius; k <= u.radius; ++k) {
        w[size_t(k + u.radius)] = std::exp(-0.5f * float(k * k) / (u.sigma * u.sigma));
        wsum += w[size_t(k + u.radius)];
    }
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = -u.radius; k <= u.radius; ++k) {
                int px = std::clamp(gx + sx * k, 0, W - 1), py = std::clamp(gy + sy * k, 0, H - 1);
                const float *s = src.at(px, py);
                float wk = w[size_t(k + u.radius)];
                for (int c = 0; c < 4; ++c) acc[c] += s[c] * wk;
            }
            float *o = out.at(gx, gy);
            for (int c = 0; c < 4; ++c) o[c] = quantize8(acc[c] / wsum);
        }
    return out;
}

ImageF resample_ref(const ImageF &src, int w, int h) {
    ImageF out(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s[4];
            sample_linear(src, (x + 0.5f) / float(w), (y + 0.5f) / float(h), s);
            float *o = out.at(x, y);
            for (int c = 0; c < 4; ++c) o[c] = quantize8(s[c]);
        }
    return out;
}

double mean_luma(const ImageF &img) {
    double sum = 0;
    for (size_t i = 0; i < size_t(img.width) * img.height; ++i)
        sum += 0.299 * img.px[i * 4 + 0] + 0.587 * img.px[i * 4 + 1] + 0.114 * img.px[i * 4 + 2];
    return img.width * img.height > 0 ? sum / (double(img.width) * img.height) : 0;
}

ImageF counter_image(const CounterBitmap &c) { return ImageF::from_bgra8(c.bgra.data(), c.width, c.height); }

}  // namespace ntscrt::howl
