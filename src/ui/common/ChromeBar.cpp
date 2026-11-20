// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/ChromeBar.h"

#include "ui/common/SchemeColors.h"

#include <QPainter>

namespace kk {

ChromeBar::ChromeBar(Fill fill, int lines, QWidget* parent)
    : QWidget(parent)
    , m_fill(fill)
    , m_lines(lines)
{
    connect(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed, this,
        qOverload<>(&QWidget::update));
}

void ChromeBar::paintEvent(QPaintEvent*)
{
    const SchemeColors& c = schemeColors();
    QPainter p(this);
    p.fillRect(rect(), m_fill == Fill::Header ? c.header : c.chromePanelTitle);
    if (m_lines & TopLine)
        p.fillRect(0, 0, width(), 1, c.chromeSeparator);
    if (m_lines & BottomLine)
        p.fillRect(0, height() - 1, width(), 1, c.chromeSeparator);
}

} // namespace kk
