// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/StyleTokens.h"

#include <QAbstractButton>

#include <algorithm>

namespace kk::style {

int controlHeight(int fontHeight)
{
    return std::max(kControlHeight, fontHeight + 2 * kPaddingV + 2);
}

QColor mix(const QColor& a, const QColor& b, double t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
        a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

Colors colors(const QPalette& p)
{
    Colors c;
    c.window = p.color(QPalette::Active, QPalette::Window);
    c.text = p.color(QPalette::Active, QPalette::WindowText);
    c.textDisabled = p.color(QPalette::Disabled, QPalette::WindowText);
    c.dark = c.window.lightness() < 128;
    const QColor black(0, 0, 0), white(255, 255, 255);
    if (c.dark) {
        c.well = mix(c.window, black, 0.38);
        c.wellHover = mix(c.well, c.text, 0.06);
        c.popup = mix(c.window, black, 0.30);
        c.header = mix(c.window, black, 0.18);
    } else {
        c.well = p.color(QPalette::Active, QPalette::Base);
        c.wellHover = mix(c.well, black, 0.03);
        c.popup = p.color(QPalette::Active, QPalette::Base);
        c.header = mix(c.window, black, 0.06);
    }
    c.button = p.color(QPalette::Active, QPalette::Button);
    c.buttonHover = c.dark ? mix(c.button, white, 0.08) : mix(c.button, black, 0.05);
    c.buttonPressed = c.dark ? mix(c.button, black, 0.25) : mix(c.button, black, 0.14);
    c.border = mix(c.window, c.text, c.dark ? 0.14 : 0.22);
    c.accent = p.color(QPalette::Active, QPalette::Highlight);
    c.accentText = p.color(QPalette::Active, QPalette::HighlightedText);
    c.focus = c.accent;
    c.groove = c.dark ? mix(c.window, black, 0.45) : mix(c.window, black, 0.18);
    c.handle = mix(c.window, c.text, c.dark ? 0.72 : 0.45);
    c.toolHover = mix(c.window, c.text, 0.10);
    c.toolPressed = mix(c.window, c.text, 0.18);
    return c;
}

void setToggle(QAbstractButton* button, bool toggle)
{
    if (button)
        button->setProperty("kkToggle", toggle);
}

bool isToggle(const QAbstractButton* button)
{
    return button && button->property("kkToggle").toBool();
}

} // namespace kk::style
