// Crop — port of upstream Sources/CrtCore/SourceCrop.swift (geometry exact,
// Swift `.rounded()` = round half away from zero = std::round) and of the
// look["crop"] load rules in upstream AppState.loadLook.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "../stages.h"
#include "crop_internal.h"

namespace ntscrt {
namespace crop {
namespace {

int even(double length, int limit) {
    int n = 2 * int(std::round(length / 2));
    return std::max(1, std::min(limit, n));
}
double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

std::optional<double> number(const json &o, const char *key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_number()) return std::nullopt;
    double v = it->get<double>();
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

}  // namespace

std::optional<Ratio> parse_ratio(const std::string &label) {
    auto colon = label.find(':');
    if (colon == std::string::npos || label.find(':', colon + 1) != std::string::npos) return std::nullopt;
    auto parse_int = [](std::string s) -> std::optional<int> {
        auto b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
        if (b == std::string::npos) return std::nullopt;
        s = s.substr(b, e - b + 1);
        size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;  // Swift Int("…") accepts a sign
        if (i == s.size()) return std::nullopt;
        for (size_t k = i; k < s.size(); ++k)
            if (s[k] < '0' || s[k] > '9') return std::nullopt;
        if (s.size() - i > 9) return std::nullopt;
        int v = std::stoi(s);
        return v;
    };
    auto w = parse_int(label.substr(0, colon)), h = parse_int(label.substr(colon + 1));
    if (!w || !h || *w <= 0 || *h <= 0) return std::nullopt;
    return Ratio{*w, *h};
}

Cut SourceCrop::cut(int width, int height) const {
    if (width <= 0 || height <= 0) return Cut::None;
    double aspect = double(width) / double(height);
    if (std::abs(aspect / ratio.value() - 1) < 0.005) return Cut::None;
    return aspect > ratio.value() ? Cut::Width : Cut::Height;
}

Rect SourceCrop::largest(int width, int height) const {
    switch (cut(width, height)) {
    case Cut::None: return {0, 0, width, height};
    case Cut::Width: return {0, 0, even(double(height) * ratio.value(), width), height};
    case Cut::Height: return {0, 0, width, even(double(width) / ratio.value(), height)};
    }
    return {0, 0, width, height};
}

Rect SourceCrop::rect(int width, int height) const {
    Rect base = largest(width, height);
    double s = std::min(kMaxScale, std::max(1.0, scale));
    int w = s < 1.0001 ? base.width : even(double(base.width) / s, base.width);
    int h = s < 1.0001 ? base.height : even(double(base.height) / s, base.height);
    double px = clamp01(x), py = clamp01(y);
    return {int(std::round(double(width - w) * px)), int(std::round(double(height - h) * py)), w, h};
}

bool SourceCrop::crops(int width, int height) const {
    Rect r = rect(width, height);
    return r.width != width || r.height != height;
}

SourceCrop SourceCrop::placed(double left, double top, double w, int width, int height) const {
    Rect base = largest(width, height);
    double s = std::min(kMaxScale, std::max(1.0, double(base.width) / std::max(1.0, w)));
    SourceCrop c{ratio, x, y, s};
    Rect r = c.rect(width, height);
    double roomX = double(width - r.width), roomY = double(height - r.height);
    if (roomX > 0) c.x = clamp01(left / roomX);
    if (roomY > 0) c.y = clamp01(top / roomY);
    return c;
}

std::optional<SourceCrop> from_look(const json &look) {
    if (!look.is_object()) return std::nullopt;
    auto it = look.find("crop");
    if (it == look.end() || !it->is_object()) return std::nullopt;
    const json &c = *it;

    std::string label;
    if (auto r = c.find("ratio"); r != c.end() && r->is_string()) label = r->get<std::string>();
    else if (auto a = c.find("aspect"); a != c.end() && a->is_string()) label = a->get<std::string>();  // Wave-0 spelling
    auto ratio = parse_ratio(label);

    if (auto e = c.find("enabled"); e != c.end()) {
        bool on = e->is_boolean() ? e->get<bool>() : (e->is_number() && e->get<double>() >= 0.5);
        if (!on) return std::nullopt;
    }
    if (!ratio) return std::nullopt;

    // Looks from before the crop could zoom have one position, for the axis it cut.
    double position = number(c, "position").value_or(0.5);
    double cx = position, cy = position;
    if (auto ctr = c.find("center"); ctr != c.end() && ctr->is_array() && ctr->size() == 2 &&
                                     (*ctr)[0].is_number() && (*ctr)[1].is_number()) {  // Wave-0 spelling
        cx = (*ctr)[0].get<double>();
        cy = (*ctr)[1].get<double>();
    }
    cx = number(c, "x").value_or(cx);
    cy = number(c, "y").value_or(cy);
    double scale = number(c, "scale").value_or(number(c, "zoom").value_or(1.0));

    SourceCrop out;
    out.ratio = *ratio;
    out.x = clamp01(cx);
    out.y = clamp01(cy);
    out.scale = std::min(kMaxScale, std::max(1.0, scale));
    return out;
}

}  // namespace crop

namespace {
// The rect `look` cuts out of a w x h picture, or nullopt when it cuts nothing.
std::optional<crop::Rect> cut_rect(const json &look, int w, int h) {
    auto c = crop::from_look(look);
    if (!c || w <= 0 || h <= 0) return std::nullopt;
    crop::Rect r = c->rect(w, h);
    if (r.width == w && r.height == h) return std::nullopt;  // cuts nothing: the picture itself
    return r;
}
void cut(const ImageBGRA8 &in, const crop::Rect &r, ImageBGRA8 &out) {
    ImageBGRA8 tmp;
    tmp.width = r.width;
    tmp.height = r.height;
    tmp.px.resize(size_t(r.width) * r.height * 4);
    for (int y = 0; y < r.height; ++y)
        std::memcpy(tmp.row(y), in.row(r.y + y) + size_t(r.x) * 4, size_t(r.width) * 4);
    out = std::move(tmp);
}
}  // namespace

void Crop::apply(const json &look, const ImageBGRA8 &in, ImageBGRA8 &out) {
    if (auto r = cut_rect(look, in.width, in.height)) cut(in, *r, out);
    else if (&out != &in) out = in;
}

void Crop::apply(const json &look, ImageBGRA8 &&in, ImageBGRA8 &out) {
    if (auto r = cut_rect(look, in.width, in.height)) cut(in, *r, out);
    else if (&out != &in) out = std::move(in);
}

}  // namespace ntscrt
