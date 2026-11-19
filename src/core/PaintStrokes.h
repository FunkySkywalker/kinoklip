// SPDX-License-Identifier: GPL-3.0-or-later
// PaintStrokes — the strokes of a Paint node of the Compositing page (spec 131): vector point
// lists with pressure and the brush they were drawn with, saved as one text value of the node
// (nodekeys::PaintStrokes) so they travel with the project, copies, instances and undo like any
// other parameter. Everything is in frame fractions, so a stroke is resolution independent.
#pragma once

#include "core/Animation.h"

#include <QColor>
#include <QString>
#include <QVector>

#include <cstdint>

namespace kk::paint {

enum class Tool : quint8 { Stroke, Clone, Eraser, Smear };

/// One sample of a stroke: x 0..1 from the left, y 0..1 from the BOTTOM (node conventions),
/// pressure 0..1 (a mouse gives 1).
struct Point
{
    float x = 0;
    float y = 0;
    float pressure = 1;
    friend bool operator==(const Point&, const Point&) = default;
};

/// A stroke lasts from @c first to @c last comp frame, both included.
inline constexpr int64_t kForever = 1000000000;

struct Stroke
{
    Tool tool = Tool::Stroke;
    double size = 0.03; ///< brush width, fraction of the frame width
    double softness = 0.3; ///< 0 hard edge .. 1 fades from the centre
    double opacity = 1.0;
    QColor color = QColor(255, 0, 0); ///< Stroke tool
    double cloneX = 0.1, cloneY = 0.0; ///< Clone: the source is the brush position plus this (normalised)
    int64_t first = 0;
    int64_t last = 0;
    QVector<Point> points;

    [[nodiscard]] bool visibleAt(int64_t frame) const { return frame >= first && frame <= last; }
    friend bool operator==(const Stroke&, const Stroke&) = default;
};

[[nodiscard]] QString toolId(Tool tool);
[[nodiscard]] Tool toolFromId(const QString& id);

/// The strokes saved as text (compact JSON), and back. Unknown or broken text gives no strokes.
[[nodiscard]] QString toText(const QVector<Stroke>& strokes);
[[nodiscard]] QVector<Stroke> fromText(const QString& text);

/// The strokes of a Paint node's parameters (the last parsed text is cached).
[[nodiscard]] QVector<Stroke> strokesOf(const ParamSet& params);
/// Stores @p strokes in @p params.
void setStrokes(ParamSet& params, const QVector<Stroke>& strokes);

/// A new, empty stroke with the brush settings of a Paint node's parameters: tool, size,
/// softness, opacity, colour, clone offset and the stroke duration, started at comp frame @p frame.
[[nodiscard]] Stroke newStroke(const ParamSet& params, int64_t frame);

} // namespace kk::paint
