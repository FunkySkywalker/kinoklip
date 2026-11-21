// SPDX-License-Identifier: GPL-3.0-or-later
// OutputBlanking — Timeline > Output Blanking: black bars that crop the output to a wider (or
// narrower) aspect ratio without changing the timeline resolution. A timeline setting
// (TimelineData::outputBlanking, saved with the timeline, undoable); the viewers, renders and
// exports all draw it as the last step, over the subtitles.
#pragma once

#include <QRect>
#include <QSize>
#include <QString>

#include <span>

namespace kk::outputblanking {

/// One entry of the Output Blanking menu.
struct Preset
{
    double aspect; ///< width / height of the picture left visible
    const char* label; ///< shown in the menu ("1.78"); never translated (a number)
    const char* id; ///< action id suffix ("1_78")
};

/// The presets of Resolve's Timeline > Output Blanking menu, narrowest first; Off (0) is not in it.
[[nodiscard]] std::span<const Preset> presets();
/// The visible picture rectangle of a @p frame sized output with blanking @p aspect: centred,
/// whole pixels, the full frame when @p aspect is 0 (Off) or equals the frame's aspect. Bars
/// appear above and below when @p aspect is wider than the frame, left and right when narrower.
[[nodiscard]] QRect pictureRect(QSize frame, double aspect);
/// True when @p aspect (0 = Off) leaves bars on a @p frame sized output.
[[nodiscard]] bool blanks(QSize frame, double aspect);
/// "Off" or the number with two decimals, for labels.
[[nodiscard]] QString aspectLabel(double aspect);

} // namespace kk::outputblanking
