// SPDX-License-Identifier: GPL-3.0-or-later
// StyleTokens — the one source of sizes and colours of the Kinoklip widget style.
#pragma once

#include <QColor>
#include <QPalette>

class QAbstractButton;

namespace kk::style {

/// Sizes in device-independent pixels, measured from the reference look (a 1080p interface:
/// the 1280x720 reference shots times 1.5). Every control of KinoklipStyle and every widget that
/// has to look like one (mixer strips, keyframe rows, toggles in panels) reads these.
inline constexpr int kRadius = 3;           ///< push buttons, tool buttons, popups
inline constexpr int kWellRadius = 2;       ///< value fields, combo boxes, spin boxes
inline constexpr int kLine = 1;             ///< borders, separators
inline constexpr int kControlHeight = 20;   ///< smallest button / field height; the real one follows the font (controlHeight())
inline constexpr int kPaddingH = 8;         ///< text to the edge of a button or field
inline constexpr int kPaddingV = 3;
inline constexpr int kGroove = 2;           ///< slider groove thickness
inline constexpr int kHandle = 10;          ///< slider handle diameter (drawn 8 inside a 10 px hit box)
inline constexpr int kHandleHit = 12;       ///< slider handle extent along the groove
inline constexpr int kScrollBar = 8;        ///< scroll bar thickness
inline constexpr int kScrollMinSlider = 24;
inline constexpr int kIndicator = 14;       ///< check box / radio button box
inline constexpr int kToggleWidth = 22;     ///< pill switch
inline constexpr int kToggleHeight = 12;
inline constexpr int kToggleDot = 8;
inline constexpr int kTabUnderline = 2;     ///< active tab underline
inline constexpr int kMenuArrowWidth = 14;  ///< room for the chevron of a tool button that opens a menu
inline constexpr int kTabHeight = 26;
inline constexpr int kMenuItemHeight = 20;
inline constexpr int kMenuBarPadding = 10;  ///< each side of a menu bar title (about 20 px between words)
inline constexpr int kMenuPadding = 4;
inline constexpr int kFocusLine = 1;        ///< focus ring width
inline constexpr int kSpinButtons = 12;     ///< width of the spin box arrow column
inline constexpr int kComboArrow = 16;      ///< width of the combo box arrow column
inline constexpr int kProgressHeight = 6;
inline constexpr int kHeaderHeight = 22;
inline constexpr int kPanelTitleHeight = 26;   ///< panel title bars (Resolve 21: about 18 px at 1440 wide)
inline constexpr int kPageBarHeight = 39;      ///< bottom page bar (R21: 29 px at 1440 wide)
inline constexpr int kPageButtonWidth = 72;    ///< page bar button (icon above the label)
inline constexpr int kPageIcon = 14;
inline constexpr int kPageUnderline = 2;       ///< red line under the current page
/// Custom-painted controls (Fairlight faders and knobs, Color wheels, node tiles).
inline constexpr int kFaderGroove = 3;
inline constexpr int kFaderCapWidth = 12;
inline constexpr int kFaderCapHeight = 20;
inline constexpr int kFaderCapRadius = 2;
inline constexpr int kKnobArc = 2;             ///< value arc width
inline constexpr int kKnobPointer = 2;
inline constexpr int kWheelRing = 4;           ///< hue ring thickness
inline constexpr int kWheelLine = 1;           ///< crosshair, outer ring
inline constexpr int kWheelPuck = 4;           ///< puck radius
inline constexpr int kWheelStrip = 4;          ///< master level groove height
inline constexpr int kNodeRadius = 3;
inline constexpr int kNodeLine = 1;
inline constexpr int kNodeLineSelected = 2;
inline constexpr int kNodeStripe = 2;          ///< colour line along the bottom of a tile

/// The colours of the style, all derived from the current palette so every colour scheme (the
/// Kinoklip scheme, Breeze Dark and Light, user schemes) keeps working.
struct Colors
{
    QColor window, text, textDisabled;
    QColor well;        ///< value fields: darker than the panel, no bevel
    QColor wellHover;
    QColor button;      ///< push button face
    QColor buttonHover;
    QColor buttonPressed;
    QColor border;      ///< hairlines (wells and menus; buttons have none)
    QColor accent;      ///< selection / on state (red in the Kinoklip scheme)
    QColor accentText;  ///< text on the accent
    QColor focus;
    QColor groove;      ///< slider groove, unchecked toggle
    QColor handle;
    QColor popup;       ///< menus, combo lists, tooltips: darker than the panel
    QColor header;      ///< group and section header bars, table headers
    QColor toolHover;   ///< flat tool button background on hover
    QColor toolPressed;
    QColor mark;         ///< the scheme's focus colour (red in Kinoklip): tab and page underlines, selected nodes, knob arcs
    QColor pageActive;   ///< page bar button of the current page (near black)
    QColor faderCap, faderCapLine, faderGroove;
    QColor knobBody, knobArc;
    QColor wheelDisc, wheelRingOuter, wheelLine;
    QColor nodeFill, nodeBorder;
    bool dark = true;
};

/// Colours for @p palette (Active group).
Colors colors(const QPalette& palette);

/// True when the application's widget style is KinoklipStyle (custom-painted controls and
/// title bars follow Resolve's look only then; every other style keeps its own drawing).
bool kinoklipActive();

/// Height of a button, field or combo box for a font of @p fontHeight px: text plus padding, never
/// below kControlHeight (Resolve: 22 px at its 9 pt text).
int controlHeight(int fontHeight);

/// Mix of @p a and @p b, @p t = weight of @p b.
QColor mix(const QColor& a, const QColor& b, double t);

/// Marks a check box so KinoklipStyle draws it as a pill switch (red dot = on) instead of a box.
void setToggle(QAbstractButton* button, bool toggle = true);
bool isToggle(const QAbstractButton* button);

} // namespace kk::style
