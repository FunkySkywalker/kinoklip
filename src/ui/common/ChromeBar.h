// SPDX-License-Identifier: GPL-3.0-or-later
// ChromeBar — a bar that paints its own fill and hairlines from the scheme colours.
#pragma once

#include <QWidget>

namespace kk {

/// The panel title bars, the interface bar and the page bar used to get their background and
/// border from the application style sheet. An application style sheet makes every widget of
/// every page go through QStyleSheetStyle, so each colour scheme or style switch re-polished
/// thousands of widgets (about a second); a bar that paints itself costs nothing.
class ChromeBar : public QWidget
{
    Q_OBJECT
public:
    enum class Fill { Header, PanelTitle };
    enum Line { NoLine = 0, TopLine = 1, BottomLine = 2 };

    explicit ChromeBar(Fill fill, int lines, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Fill m_fill;
    int m_lines;
};

} // namespace kk
