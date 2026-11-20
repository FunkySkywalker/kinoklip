// SPDX-License-Identifier: GPL-3.0-or-later
// ComboPopups — a combo box's list is never narrower than its widest entry.
#pragma once

class QApplication;
class QWidget;

namespace kk::combopopups {

/// Qt opens a combo box's list as wide as the combo box itself, so the entries of a narrow
/// combo box (the Audio page's "Main mix", a viewer's "Fit") are cut off. install() adds an
/// application filter that widens every combo list, in every widget style, to its widest
/// entry just before it is shown (and moves it left if it would leave the screen). Idempotent;
/// called by theme::apply(). GUI thread only.
void install(QApplication& app);

/// Width a combo list window needs for its entries: the view's widest row, the frames of the
/// view and of the container and a vertical scroll bar if the list has one. 0 for a widget
/// that is not a combo list. Exposed for tests.
[[nodiscard]] int neededWidth(const QWidget* listWindow);

} // namespace kk::combopopups
