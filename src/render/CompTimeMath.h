// SPDX-License-Identifier: GPL-3.0-or-later
// CompTimeMath — the frame mapping of the Compositing page's time nodes (Time Speed, Time
// Stretcher) and of the Loader's clip timing (spec 128). Pure functions: the CPU and the GPU
// evaluation both call them, so they agree on which frames a node shows.
#pragma once

#include "core/Animation.h"

#include <cstdint>

namespace kk::render::timemath {

/// The input frames a time node blends: @p from, and when @p blend > 0 also @p to, mixed as
/// from * (1 - blend) + to * blend.
struct Taps
{
    int64_t from = 0;
    int64_t to = 0;
    double blend = 0.0;

    [[nodiscard]] bool single() const { return blend <= 0.0; }
};

/// Where the input of a Time Speed node is read at comp frame @p frame: (frame - Delay) * Speed.
[[nodiscard]] double speedSourceTime(const ParamSet& params, int64_t frame);
/// Where the input of a Time Stretcher node is read: its Source Time at @p frame.
[[nodiscard]] double stretchSourceTime(const ParamSet& params, int64_t frame);
/// The frames to read for source time @p time. Nearest: the closest frame (halves round up).
/// Blend: the frames around it and the fraction between them (a whole time is one frame).
[[nodiscard]] Taps taps(double time, bool blend);
/// True when the node's Interpolation parameter says Blend.
[[nodiscard]] bool blends(const ParamSet& params);

/// The Loader's clip timing, from its parameters.
struct LoaderTiming
{
    int64_t globalIn = 0;
    int64_t trimIn = 0;
    int64_t trimOut = 0;
    int64_t holdFirst = 0;
    int64_t holdLast = 0;
    bool loop = false;
    bool reverse = false;
};
[[nodiscard]] LoaderTiming loaderTiming(const ParamSet& params, int64_t frame);

/// Which frame of a clip of @p count frames (0-based, before the trims) the Loader shows at comp
/// frame @p frame, or -1 for nothing. A still image (@p still) shows on every frame. Otherwise the
/// clip, with its trims, plays from Global In; the first frame is held Hold First Frame frames
/// before it plays and the last Hold Last Frame frames after it; with Loop all of that repeats,
/// with Reverse it runs backwards, and without either nothing shows outside it.
[[nodiscard]] int loaderFrame(const LoaderTiming& t, int count, bool still, int64_t frame);

} // namespace kk::render::timemath
