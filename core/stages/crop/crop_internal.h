// Crop geometry — port of upstream Sources/CrtCore/SourceCrop.swift (agent A).
// Private to core/stages/crop; visible to tests.
#pragma once

#include <optional>
#include <string>

#include "../../src/internal.h"

namespace ntscrt::crop {

struct Ratio {
    int width = 1, height = 1;
    double value() const { return double(width) / double(height); }
    std::string label() const { return std::to_string(width) + ":" + std::to_string(height); }
    bool operator==(const Ratio &o) const { return width == o.width && height == o.height; }
};

// "9:16" -> Ratio; nullopt unless two positive integers.
std::optional<Ratio> parse_ratio(const std::string &label);

enum class Cut { Width, Height, None };

struct Rect { int x = 0, y = 0, width = 0, height = 0; };

constexpr double kMaxScale = 8.0;

struct SourceCrop {
    Ratio ratio;
    double x = 0.5, y = 0.5, scale = 1.0;

    Cut cut(int width, int height) const;
    // Largest crop for the ratio (width, height).
    Rect largest(int width, int height) const;
    // The crop in whole pixels; even sizes where the picture allows; inside the picture.
    Rect rect(int width, int height) const;
    bool crops(int width, int height) const;
    // Inverse of rect for a dragged pixel rectangle (SourceCrop.placed).
    SourceCrop placed(double left, double top, double w, int width, int height) const;
};

// look["crop"] -> the crop in force, or nullopt when off/absent/invalid.
// Keys (upstream AppState.lookDictionary): "ratio" ("W:H"), "x", "y", "scale",
// legacy "position" (one value for x and y); NTSCRT-Plugins adds "enabled"
// (absent = on whenever a valid ratio is present, as upstream).
std::optional<SourceCrop> from_look(const json &look);

}  // namespace ntscrt::crop
