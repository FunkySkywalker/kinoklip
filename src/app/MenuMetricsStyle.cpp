// SPDX-License-Identifier: GPL-3.0-or-later
// MenuMetricsStyle implementation.
#include "app/MenuMetricsStyle.h"

#include <QPainter>
#include <QStyleOptionMenuItem>
#include <QStyleOptionToolButton>

namespace kk {

namespace {
/// Room for the submenu arrow plus the gap to the text (Breeze: 20 px indicator + 4 px).
constexpr int kSubmenuArrowSpace = 24;
} // namespace

MenuMetricsStyle::MenuMetricsStyle(QStyle* base, bool centerBesideIconLabels)
    : QProxyStyle(base)
    , m_centerBesideIconLabels(centerBesideIconLabels)
{
    // Keep the factory name so code asking for the style's name still sees "Breeze".
    if (base)
        setObjectName(base->objectName());
}

QSize MenuMetricsStyle::sizeFromContents(
    ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const
{
    QSize result = QProxyStyle::sizeFromContents(type, option, size, widget);
    if (type == CT_MenuItem) {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (item && item->menuItemType == QStyleOptionMenuItem::SubMenu)
            result.rwidth() += kSubmenuArrowSpace;
    }
    return result;
}

int MenuMetricsStyle::styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
    QStyleHintReturn* returnData) const
{
    if (hint == SH_Menu_FadeOutOnHide || hint == SH_Menu_FlashTriggeredItem)
        return 0; // menus close at once, like Resolve's
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

void MenuMetricsStyle::drawControl(ControlElement element, const QStyleOption* option,
    QPainter* painter, const QWidget* widget) const
{
    const auto* tb = qstyleoption_cast<const QStyleOptionToolButton*>(option);
    if (!m_centerBesideIconLabels || element != CE_ToolButtonLabel || !tb
        || tb->toolButtonStyle != Qt::ToolButtonTextBesideIcon || tb->icon.isNull()
        || tb->text.isEmpty() || (tb->features & QStyleOptionToolButton::Arrow)) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }
    // Qt's own layout: icon, a 4 px gap, then the text; here the pair is centred in the rect.
    // The base style still paints both (its colours, font weight and icon states), only
    // inside a rect that is just as wide as the pair.
    constexpr int kGap = 4;
    const int textWidth = painter->fontMetrics().horizontalAdvance(tb->text);
    const int content = tb->iconSize.width() + kGap + textWidth + 2; // + 2: rounding room
    QStyleOptionToolButton copy(*tb);
    if (content < tb->rect.width())
        copy.rect = QRect(tb->rect.left() + (tb->rect.width() - content) / 2, tb->rect.top(),
            content, tb->rect.height());
    QProxyStyle::drawControl(element, &copy, painter, widget);
}

} // namespace kk
