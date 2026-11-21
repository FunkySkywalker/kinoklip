// SPDX-License-Identifier: GPL-3.0-or-later
// MenuMetricsStyle — a thin proxy over the widget style that fixes menu item metrics.
#pragma once

#include <QProxyStyle>

namespace kk {

/// Proxy around the real widget style (Breeze, Fusion, ...) that reserves room for the
/// submenu arrow in menu items. With an application style sheet active (our panel chrome),
/// Breeze sized submenu items without the arrow column, so the arrow was drawn over the end
/// of long titles ("Color Theme" in the Workspace menu, owner feedback on alpha.1). Drawing is
/// left entirely to the base style.
///
/// It also closes menus at once (instant popups, see InstantPopups.h): no fade-out on hide
/// (SH_Menu_FadeOutOnHide) and no blink of the triggered item before the menu closes
/// (SH_Menu_FlashTriggeredItem), whatever the base style asks for.
///
/// Thread-safety: GUI thread only (like every QStyle).
class MenuMetricsStyle : public QProxyStyle
{
    Q_OBJECT
public:
    /// Wraps @p base (takes ownership, as QProxyStyle does). With @p centerBesideIconLabels
    /// (Libadwaita only), tool buttons with the text beside the icon draw icon and text as one
    /// centred group: Libadwaita widens such buttons but its base drawing puts the content at
    /// the left, so all the spare room ended up on the right (owner feedback, interface bar).
    explicit MenuMetricsStyle(QStyle* base, bool centerBesideIconLabels = false);
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
        const QWidget* widget = nullptr) const override;

    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
        const QWidget* widget) const override;
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
        const QWidget* widget = nullptr, QStyleHintReturn* returnData = nullptr) const override;

private:
    bool m_centerBesideIconLabels = false;
};

} // namespace kk
