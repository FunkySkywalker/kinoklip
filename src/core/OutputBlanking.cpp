// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/OutputBlanking.h"

#include <algorithm>
#include <cmath>

namespace kk::outputblanking {

std::span<const Preset> presets()
{
    static const Preset kPresets[] = {
        {1.33, "1.33", "1_33"},
        {1.66, "1.66", "1_66"},
        {16.0 / 9.0, "1.78", "1_78"},
        {1.85, "1.85", "1_85"},
        {2.00, "2.00", "2_00"},
        {2.35, "2.35", "2_35"},
        {2.39, "2.39", "2_39"},
        {2.40, "2.40", "2_40"},
    };
    return kPresets;
}

QRect pictureRect(QSize frame, double aspect)
{
    const QRect full(QPoint(0, 0), frame);
    if (aspect <= 0.0 || frame.isEmpty())
        return full;
    const double frameAspect = double(frame.width()) / frame.height();
    if (std::abs(aspect - frameAspect) < 1e-3)
        return full;
    if (aspect > frameAspect) {
        // Wider than the frame: bars above and below.
        const int h = std::clamp(int(std::lround(frame.width() / aspect)), 1, frame.height());
        return QRect(0, (frame.height() - h) / 2, frame.width(), h);
    }
    const int w = std::clamp(int(std::lround(frame.height() * aspect)), 1, frame.width());
    return QRect((frame.width() - w) / 2, 0, w, frame.height());
}

bool blanks(QSize frame, double aspect)
{
    return pictureRect(frame, aspect) != QRect(QPoint(0, 0), frame);
}

QString aspectLabel(double aspect)
{
    return aspect <= 0.0 ? QStringLiteral("Off") : QString::number(aspect, 'f', 2);
}

} // namespace kk::outputblanking
