// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/CompTimeMath.h"

#include "core/NodeRegistry.h"

#include <algorithm>
#include <cmath>

namespace kk::render::timemath {

namespace k = nodekeys;

double speedSourceTime(const ParamSet& params, int64_t frame)
{
    const double speed = params.value(k::TimeSpeedKey, frame, 1.0);
    const double delay = params.value(k::TimeDelay, frame, 0.0);
    return (double(frame) - delay) * speed;
}

double stretchSourceTime(const ParamSet& params, int64_t frame)
{
    return params.value(k::SourceTime, frame, 0.0);
}

bool blends(const ParamSet& params)
{
    return params.staticValue(k::TimeInterp, timeinterps::Nearest).toString() == timeinterps::Blend;
}

Taps taps(double time, bool blend)
{
    Taps t;
    if (!blend) {
        t.from = t.to = int64_t(std::floor(time + 0.5));
        return t;
    }
    const double floorTime = std::floor(time);
    t.from = int64_t(floorTime);
    t.blend = time - floorTime;
    t.to = t.from + 1;
    if (t.blend < 1e-6) {
        t.blend = 0.0;
        t.to = t.from;
    }
    return t;
}

LoaderTiming loaderTiming(const ParamSet& p, int64_t frame)
{
    LoaderTiming t;
    t.globalIn = int64_t(std::lround(p.value(k::GlobalIn, frame, 0.0)));
    t.trimIn = std::max<int64_t>(0, std::lround(p.value(k::TrimIn, frame, 0.0)));
    t.trimOut = std::max<int64_t>(0, std::lround(p.value(k::TrimOut, frame, 0.0)));
    t.holdFirst = std::max<int64_t>(0, std::lround(p.value(k::HoldFirst, frame, 0.0)));
    t.holdLast = std::max<int64_t>(0, std::lround(p.value(k::HoldLast, frame, 0.0)));
    t.loop = p.staticValue(k::Loop, false).toBool();
    t.reverse = p.staticValue(k::Reverse, false).toBool();
    return t;
}

int loaderFrame(const LoaderTiming& t, int count, bool still, int64_t frame)
{
    if (count <= 0)
        return -1;
    if (still && count == 1)
        return 0;
    // At least one frame stays after the trims.
    const int64_t trimIn = std::min<int64_t>(t.trimIn, count - 1);
    const int64_t trimOut = std::min<int64_t>(t.trimOut, count - 1 - trimIn);
    const int64_t length = count - trimIn - trimOut;
    const int64_t total = t.holdFirst + length + t.holdLast;
    int64_t at = frame - t.globalIn;
    if (t.loop) {
        at %= total;
        if (at < 0)
            at += total;
    } else if (at < 0 || at >= total) {
        return -1;
    }
    if (t.reverse)
        at = total - 1 - at;
    const int64_t inClip = std::clamp<int64_t>(at - t.holdFirst, 0, length - 1);
    return int(trimIn + inClip);
}

} // namespace kk::render::timemath
