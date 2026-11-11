// video1.h — port of upstream Sources/CrtCodec/Video1Codec.swift (v0.20.0).
//
// Microsoft Video 1 (1992, Video for Windows; "CRAM"), the Digital video
// stage's second codec: block truncation coding in 15-bit RGB, no transform
// at all. Every 4x4 block is coded the cheapest way that keeps its error
// under the quality's threshold: skip (between keyframes), one color, two
// colors, or eight colors (a pair per 2x2 quarter). Coded blocks are
// absolute, so a decoder that lost data heals wherever a block is coded
// again; skipped and lost blocks keep whatever it last had.
#pragma once

#include <array>
#include <optional>

#include "codec.h"

namespace ntscrt::digital::video1 {

struct RGB {
    float x = 0, y = 0, z = 0;  // red, green, blue on the 0..255 scale
    bool operator==(const RGB &) const = default;
};

enum class Mode { Skip, Fill, Two, Eight };

// The largest mean squared error (per pixel and channel) a block may carry in
// a cheaper mode; quality runs the square root linearly (0 -> 2500, 1 -> 1).
float threshold(double quality, float squeeze);

// 5 bits a channel.
float q555(float v);

struct Coded {
    Mode mode;
    std::array<RGB, 16> pixels;
};
// Code one block's 16 pixels the cheapest way within `limit` (squared error
// summed over the block), or no worse than the best any mode can do.
Coded codeBlock(const std::array<RGB, 16> &src, const std::array<RGB, 16> *reference, float limit);

// Code `source` as `kind`, advancing `state`: the same contract as
// codec::step (datamosh doesn't apply). Returns the decoder's picture.
PicturePtr step(CodecState &state, const PicturePtr &source, FrameKind kind, int index,
                const codec::FrameSettings &s, uint64_t seedMix);

}  // namespace ntscrt::digital::video1
