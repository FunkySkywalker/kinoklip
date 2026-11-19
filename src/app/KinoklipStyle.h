// SPDX-License-Identifier: GPL-3.0-or-later
// KinoklipStyle — the "Kinoklip" widget style: the standard Qt controls in a flat, dark, compact look.
#pragma once

#include <QProxyStyle>

namespace kk {

/// A QProxyStyle on Qt's own Fusion style (no KDE dependency) that draws every standard control
/// flat: push and tool buttons without gradient or bevel (tool buttons show a background only on
/// hover / press), value fields as dark wells, thin slider grooves with a small round handle,
/// pill switches (QCheckBox with the "kkToggle" property, see style::setToggle()), flat tabs with
/// an accent underline, thin scroll bars without arrows, flat popups, group and section headers
/// as slightly darker bars. Sizes and colours come from ui/common/StyleTokens.h, colours derived
/// from the widget's palette, so every colour scheme works.
///
/// Also folds in what MenuMetricsStyle does for the other styles: room for the submenu arrow and
/// menus that close at once (no fade-out, no flash of the triggered item).
///
/// Thread-safety: GUI thread only (like every QStyle).
class KinoklipStyle : public QProxyStyle
{
    Q_OBJECT
public:
    /// The factory key of the style in Preferences and QApplication::style()->name().
    static QString key();

    KinoklipStyle();

    void polish(QWidget* widget) override;
    void unpolish(QWidget* widget) override;
    using QProxyStyle::polish;
    using QProxyStyle::unpolish;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
        const QWidget* widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
        const QWidget* widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option,
        QPainter* painter, const QWidget* widget = nullptr) const override;
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex* option,
        SubControl subControl, const QWidget* widget = nullptr) const override;
    QRect subElementRect(SubElement element, const QStyleOption* option,
        const QWidget* widget = nullptr) const override;
    int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
        const QWidget* widget = nullptr) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
        const QWidget* widget = nullptr) const override;
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
        const QWidget* widget = nullptr, QStyleHintReturn* returnData = nullptr) const override;
};

} // namespace kk
