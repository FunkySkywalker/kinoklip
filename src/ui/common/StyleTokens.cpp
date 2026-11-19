// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/StyleTokens.h"

#include "ui/common/SchemeColors.h"

#include <QAbstractButton>
#include <QApplication>
#include <QStyle>

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
    c.mark = schemeColors().focus.isValid() ? schemeColors().focus : c.accent;
    c.pageActive = c.dark ? mix(c.window, black, 0.85) : mix(c.window, white, 0.5);
    c.faderCap = mix(c.window, c.text, c.dark ? 0.62 : 0.3);
    c.faderCapLine = mix(c.faderCap, black, 0.65);
    c.faderGroove = c.groove;
    c.knobBody = c.dark ? mix(c.window, black, 0.30) : mix(c.window, black, 0.06);
    c.knobArc = c.mark;
    c.wheelDisc = c.dark ? mix(c.window, black, 0.45) : mix(c.window, black, 0.12);
    c.wheelRingOuter = c.dark ? mix(c.window, black, 0.75) : mix(c.window, black, 0.25);
    c.wheelLine = mix(c.wheelDisc, c.text, 0.22);
    c.nodeFill = c.dark ? mix(c.window, black, 0.28) : mix(c.window, white, 0.5);
    c.nodeBorder = c.dark ? mix(c.window, black, 0.6) : mix(c.window, black, 0.25);
    return c;
}

bool kinoklipActive()
{
    // With an application style sheet QApplication::style() is the style sheet's wrapper, so Theme
    // also records the choice in a property.
    if (qApp && qApp->property("kkKinoklipStyle").toBool())
        return true;
    const QStyle* s = QApplication::style();
    return s && qstrcmp(s->metaObject()->className(), "kk::KinoklipStyle") == 0;
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
