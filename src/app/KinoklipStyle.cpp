// SPDX-License-Identifier: GPL-3.0-or-later
// KinoklipStyle implementation.
#include "app/KinoklipStyle.h"

#include "ui/common/StyleTokens.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QFontMetrics>
#include <QIcon>
#include <QPixmap>
#include <QHeaderView>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSlider>
#include <QStyleFactory>
#include <QStyleOptionComboBox>
#include <QStyleOptionGroupBox>
#include <QStyleOptionHeader>
#include <QStyleOptionMenuItem>
#include <QStyleOptionProgressBar>
#include <QStyleOptionSlider>
#include <QStyleOptionSpinBox>
#include <QStyleOptionTab>
#include <QStyleOptionToolButton>
#include <QTabBar>

#include <algorithm>

namespace kk {

namespace {

namespace tk = kk::style;

/// Colours of @p palette; the last palette's result is kept (controls of one window share it).
const tk::Colors& colorsFor(const QPalette& palette)
{
    static qint64 key = 0;
    static tk::Colors cached;
    if (key != palette.cacheKey() || key == 0) {
        cached = tk::colors(palette);
        key = palette.cacheKey();
    }
    return cached;
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

bool on(const QStyleOption* o, QStyle::StateFlag f)
{
    return o->state & f;
}

QRectF hairline(const QRect& r)
{
    return QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
}

void ring(QPainter* p, const QRectF& r, double radius, const QColor& color)
{
    p->setPen(QPen(color, tk::kFocusLine));
    p->setBrush(Qt::NoBrush);
    p->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

/// A chevron in @p r, pointing @p dir (0 right, 1 down, 2 left, 3 up).
// A tool button whose whole face opens a menu (no separate arrow part).
bool instantMenu(const QStyleOptionToolButton* tb)
{
    return (tb->features & QStyleOptionToolButton::HasMenu)
        && !(tb->features & QStyleOptionToolButton::MenuButtonPopup);
}

void chevron(QPainter* p, const QRectF& r, int dir, const QColor& color, double size = 3.5)
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(Qt::NoBrush);
    const QPointF c = r.center();
    QPolygonF poly;
    switch (dir) {
    case 0: poly << QPointF(c.x() - size / 2, c.y() - size) << QPointF(c.x() + size / 2, c.y()) << QPointF(c.x() - size / 2, c.y() + size); break;
    case 1: poly << QPointF(c.x() - size, c.y() - size / 2) << QPointF(c.x(), c.y() + size / 2) << QPointF(c.x() + size, c.y() - size / 2); break;
    case 2: poly << QPointF(c.x() + size / 2, c.y() - size) << QPointF(c.x() - size / 2, c.y()) << QPointF(c.x() + size / 2, c.y() + size); break;
    default: poly << QPointF(c.x() - size, c.y() + size / 2) << QPointF(c.x(), c.y() - size / 2) << QPointF(c.x() + size, c.y() + size / 2); break;
    }
    p->drawPolyline(poly);
    p->restore();
}

/// @p icon redrawn in one colour (Resolve tints its toolbar and page bar glyphs like the text).
QIcon tintedIcon(const QIcon& icon, const QSize& size, qreal dpr, const QColor& color, QIcon::State state)
{
    QPixmap pm = icon.pixmap(size, dpr, QIcon::Normal, state);
    if (pm.isNull())
        return icon;
    QPainter q(&pm);
    q.setCompositionMode(QPainter::CompositionMode_SourceIn);
    q.fillRect(pm.rect(), color);
    q.end();
    return QIcon(pm);
}

bool flag(const QWidget* w, const char* name)
{
    return w && w->property(name).toBool();
}

bool isToggleWidget(const QWidget* w)
{
    return tk::isToggle(qobject_cast<const QAbstractButton*>(w));
}

} // namespace

QString KinoklipStyle::key()
{
    return QStringLiteral("Kinoklip");
}

KinoklipStyle::KinoklipStyle()
    : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
{
    setObjectName(key());
}

void KinoklipStyle::polish(QWidget* widget)
{
    QProxyStyle::polish(widget);
    if (qobject_cast<QAbstractButton*>(widget) || qobject_cast<QComboBox*>(widget)
        || qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QScrollBar*>(widget)
        || qobject_cast<QSlider*>(widget) || qobject_cast<QTabBar*>(widget)
        || qobject_cast<QHeaderView*>(widget))
        widget->setAttribute(Qt::WA_Hover, true);
}

void KinoklipStyle::unpolish(QWidget* widget)
{
    if (qobject_cast<QAbstractButton*>(widget) || qobject_cast<QComboBox*>(widget)
        || qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QScrollBar*>(widget)
        || qobject_cast<QSlider*>(widget) || qobject_cast<QTabBar*>(widget)
        || qobject_cast<QHeaderView*>(widget))
        widget->setAttribute(Qt::WA_Hover, false);
    QProxyStyle::unpolish(widget);
}

// ---- Metrics ---------------------------------------------------------------------------

int KinoklipStyle::pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const
{
    switch (metric) {
    case PM_ScrollBarExtent: return tk::kScrollBar;
    case PM_ScrollBarSliderMin: return tk::kScrollMinSlider;
    case PM_SliderThickness:
    case PM_SliderControlThickness:
    case PM_SliderLength: return tk::kHandleHit;
    case PM_IndicatorWidth:
    case PM_ExclusiveIndicatorWidth: return isToggleWidget(widget) ? tk::kToggleWidth : tk::kIndicator;
    case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorHeight: return isToggleWidget(widget) ? tk::kToggleHeight : tk::kIndicator;
    case PM_CheckBoxLabelSpacing:
    case PM_RadioButtonLabelSpacing: return 6;
    case PM_DefaultFrameWidth:
    case PM_SpinBoxFrameWidth:
    case PM_ComboBoxFrameWidth: return tk::kLine;
    case PM_MenuPanelWidth: return tk::kLine;
    case PM_MenuHMargin:
    case PM_MenuVMargin: return tk::kMenuPadding;
    case PM_TabBarTabHSpace: return tk::kPaddingH * 2;
    case PM_TabBarTabVSpace: return tk::kPaddingV * 2;
    case PM_TabBarTabShiftHorizontal:
    case PM_TabBarTabShiftVertical: return 0;
    case PM_ButtonShiftHorizontal:
    case PM_ButtonShiftVertical: return 0;
    case PM_ProgressBarChunkWidth: return 1;
    case PM_MenuBarItemSpacing:
    case PM_MenuBarHMargin:
    case PM_MenuBarVMargin:
    case PM_MenuBarPanelWidth: return 0;
    default: break;
    }
    return QProxyStyle::pixelMetric(metric, option, widget);
}

QSize KinoklipStyle::sizeFromContents(
    ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const
{
    QSize s = QProxyStyle::sizeFromContents(type, option, size, widget);
    switch (type) {
    case CT_PushButton:
    case CT_ComboBox:
    case CT_SpinBox:
    case CT_LineEdit:
        s.setHeight(std::max(s.height(), tk::controlHeight(option ? option->fontMetrics.height() : 0)));
        break;
    case CT_TabBarTab:
        if (const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
            tab && (tab->shape == QTabBar::RoundedNorth || tab->shape == QTabBar::RoundedSouth
                       || tab->shape == QTabBar::TriangularNorth || tab->shape == QTabBar::TriangularSouth))
            s.setHeight(std::max(s.height(), tk::controlHeight(option->fontMetrics.height()) + 4));
        break;
    case CT_HeaderSection:
        s.setHeight(std::max(s.height(), tk::kHeaderHeight));
        break;
    case CT_ToolButton:
        // A button that opens its menu on click gets a chevron after the text instead of
        // Fusion's small corner arrow, which overlaps the label.
        if (const auto* tb = qstyleoption_cast<const QStyleOptionToolButton*>(option);
            tb && instantMenu(tb) && tb->toolButtonStyle != Qt::ToolButtonIconOnly)
            s.setWidth(s.width() + tk::kMenuArrowWidth);
        break;
    case CT_MenuItem: {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (!item)
            break;
        if (item->menuItemType == QStyleOptionMenuItem::Separator)
            return QSize(10, 7);
        const QFontMetrics fm(item->font);
        const QString text = item->text.section(QLatin1Char('\t'), 0, 0);
        const QString shortcut = item->text.section(QLatin1Char('\t'), 1);
        const bool withColumn = item->menuHasCheckableItems || item->maxIconWidth > 0;
        int w = tk::kPaddingH * 2 + fm.horizontalAdvance(text);
        if (withColumn)
            w += std::max(item->maxIconWidth, 16) + 6;
        if (!shortcut.isEmpty())
            w += 24 + fm.horizontalAdvance(shortcut);
        if (item->menuItemType == QStyleOptionMenuItem::SubMenu)
            w += 24; // room for the arrow after long titles (as MenuMetricsStyle does)
        s = QSize(w, std::max(tk::kMenuItemHeight, fm.height() + 2 * tk::kPaddingV + 1));
        break;
    }
    case CT_MenuBarItem: // tight: the text plus a small padding on each side
        s = QSize(size.width() + 2 * tk::kMenuBarPadding, std::max(size.height() + 2 * tk::kPaddingV, tk::kMenuItemHeight));
        break;
    default: break;
    }
    return s;
}

int KinoklipStyle::styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* returnData) const
{
    switch (hint) {
    case SH_Menu_FadeOutOnHide:
    case SH_Menu_FlashTriggeredItem: return 0; // menus close at once, like Resolve's
    case SH_UnderlineShortcut: return 0; // no always-visible mnemonic underlines, like Resolve
    case SH_EtchDisabledText: return 0;
    case SH_ToolTipLabel_Opacity: return 255;
    case SH_Table_GridLineColor:
        return int(colorsFor(option ? option->palette : QPalette()).border.rgba());
    default: break;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

QRect KinoklipStyle::subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const
{
    switch (element) {
    case SE_ProgressBarGroove:
    case SE_ProgressBarContents:
    case SE_ProgressBarLabel: return option->rect;
    default: break;
    }
    return QProxyStyle::subElementRect(element, option, widget);
}

QRect KinoklipStyle::subControlRect(
    ComplexControl control, const QStyleOptionComplex* option, SubControl sub, const QWidget* widget) const
{
    switch (control) {
    case CC_ScrollBar: {
        const auto* sb = qstyleoption_cast<const QStyleOptionSlider*>(option);
        if (!sb)
            break;
        const QRect r = sb->rect;
        const bool horizontal = sb->orientation == Qt::Horizontal;
        const int length = horizontal ? r.width() : r.height();
        const int range = sb->maximum - sb->minimum;
        int sliderLen = range > 0 ? int((qint64(sb->pageStep) * length) / (range + sb->pageStep)) : length;
        sliderLen = std::clamp(sliderLen, std::min(tk::kScrollMinSlider, length), length);
        const int pos = range > 0 ? sliderPositionFromValue(sb->minimum, sb->maximum, sb->sliderPosition, length - sliderLen, sb->upsideDown) : 0;
        const auto make = [&](int from, int len) {
            return horizontal ? QRect(r.x() + from, r.y(), len, r.height()) : QRect(r.x(), r.y() + from, r.width(), len);
        };
        switch (sub) {
        case SC_ScrollBarGroove: return r;
        case SC_ScrollBarSlider: return make(pos, sliderLen);
        case SC_ScrollBarSubPage: return make(0, pos);
        case SC_ScrollBarAddPage: return make(pos + sliderLen, length - pos - sliderLen);
        case SC_ScrollBarAddLine:
        case SC_ScrollBarSubLine:
        case SC_ScrollBarFirst:
        case SC_ScrollBarLast: return QRect();
        default: break;
        }
        break;
    }
    case CC_Slider: {
        const auto* sl = qstyleoption_cast<const QStyleOptionSlider*>(option);
        if (!sl)
            break;
        const QRect r = sl->rect;
        const bool horizontal = sl->orientation == Qt::Horizontal;
        const int length = horizontal ? r.width() : r.height();
        const int span = length - tk::kHandleHit;
        const int pos = sliderPositionFromValue(sl->minimum, sl->maximum, sl->sliderPosition, std::max(span, 0), sl->upsideDown);
        if (sub == SC_SliderGroove) {
            return horizontal ? QRect(r.x(), r.center().y() - tk::kHandleHit / 2, r.width(), tk::kHandleHit)
                              : QRect(r.center().x() - tk::kHandleHit / 2, r.y(), tk::kHandleHit, r.height());
        }
        if (sub == SC_SliderHandle) {
            return horizontal ? QRect(r.x() + pos, r.center().y() - tk::kHandleHit / 2, tk::kHandleHit, tk::kHandleHit)
                              : QRect(r.center().x() - tk::kHandleHit / 2, r.y() + pos, tk::kHandleHit, tk::kHandleHit);
        }
        break;
    }
    case CC_SpinBox: {
        const auto* sp = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
        if (!sp)
            break;
        const QRect r = sp->rect;
        const int bw = sp->buttonSymbols == QAbstractSpinBox::NoButtons ? 0 : tk::kSpinButtons;
        switch (sub) {
        case SC_SpinBoxFrame: return r;
        case SC_SpinBoxUp: return bw ? QRect(r.right() - bw, r.y() + 1, bw, r.height() / 2 - 1) : QRect();
        case SC_SpinBoxDown: return bw ? QRect(r.right() - bw, r.y() + r.height() / 2, bw, r.height() - r.height() / 2 - 1) : QRect();
        case SC_SpinBoxEditField: return r.adjusted(tk::kLine + 2, tk::kLine, -(bw + tk::kLine), -tk::kLine);
        default: break;
        }
        break;
    }
    case CC_ComboBox: {
        const auto* cb = qstyleoption_cast<const QStyleOptionComboBox*>(option);
        if (!cb)
            break;
        const QRect r = cb->rect;
        switch (sub) {
        case SC_ComboBoxFrame: return r;
        case SC_ComboBoxArrow: return QRect(r.right() - tk::kComboArrow + 1, r.y(), tk::kComboArrow, r.height());
        case SC_ComboBoxEditField:
            return r.adjusted(tk::kLine + 3, tk::kLine, -(tk::kComboArrow + tk::kLine), -tk::kLine);
        case SC_ComboBoxListBoxPopup: return r;
        default: break;
        }
        break;
    }
    case CC_GroupBox: {
        const auto* gb = qstyleoption_cast<const QStyleOptionGroupBox*>(option);
        if (!gb)
            break;
        const int barH = std::max(tk::kHeaderHeight, gb->fontMetrics.height() + 2 * tk::kPaddingV);
        const bool box = gb->subControls & SC_GroupBoxCheckBox;
        const int left = gb->rect.x() + tk::kPaddingH + (box ? tk::kIndicator + 6 : 0);
        switch (sub) {
        case SC_GroupBoxFrame: return gb->rect;
        case SC_GroupBoxCheckBox:
            return QRect(gb->rect.x() + tk::kPaddingH, gb->rect.y() + (barH - tk::kIndicator) / 2, tk::kIndicator, tk::kIndicator);
        case SC_GroupBoxLabel: return QRect(left, gb->rect.y(), gb->rect.right() - tk::kPaddingH - left, barH);
        case SC_GroupBoxContents:
            return gb->rect.adjusted(tk::kPaddingH, barH + tk::kPaddingH, -tk::kPaddingH, -tk::kPaddingH);
        default: break;
        }
        break;
    }
    default: break;
    }
    return QProxyStyle::subControlRect(control, option, sub, widget);
}

// ---- Primitives ------------------------------------------------------------------------

void KinoklipStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* o, QPainter* p, const QWidget* widget) const
{
    const tk::Colors& c = colorsFor(o->palette);
    const bool enabled = on(o, State_Enabled);
    const bool hover = enabled && on(o, State_MouseOver);
    const bool pressed = enabled && on(o, State_Sunken);
    switch (element) {
    case PE_PanelButtonCommand: {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        QColor fill = c.button;
        if (on(o, State_On))
            fill = c.accent;
        else if (pressed)
            fill = c.buttonPressed;
        else if (hover)
            fill = c.buttonHover;
        if (!enabled)
            fill = withAlpha(fill, 140);
        p->setPen(on(o, State_On) ? Qt::NoPen : QPen(c.border, tk::kLine));
        p->setBrush(fill);
        p->drawRoundedRect(QRectF(o->rect).adjusted(0.5, 0.5, -0.5, -0.5), tk::kRadius, tk::kRadius);
        if (enabled && on(o, State_HasFocus) && on(o, State_KeyboardFocusChange))
            ring(p, QRectF(o->rect), tk::kRadius, c.focus);
        p->restore();
        return;
    }
    case PE_PanelButtonTool: {
        // Page bar buttons have no hover or pressed plate (Resolve): the current page's square
        // block and underline, and the brighter text on hover, are all they show. A rounded
        // plate here put dark corners on the current page's square block.
        if (flag(widget, "kkPage"))
            return;
        const bool frameless = flag(widget, "kkFrameless") || flag(widget, "kkTab");
        if (!(hover || pressed || (on(o, State_On) && !frameless)))
            return;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(pressed || on(o, State_On) ? c.toolPressed : c.toolHover);
        p->drawRoundedRect(QRectF(o->rect), tk::kRadius, tk::kRadius);
        p->restore();
        return;
    }
    case PE_FrameDefaultButton:
    case PE_FrameFocusRect: return;
    case PE_PanelLineEdit: {
        const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(o);
        if (frame && frame->lineWidth <= 0)
            return; // inside a spin box or combo box: they draw the well
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(c.border, tk::kLine));
        p->setBrush(on(o, State_MouseOver) && enabled ? c.wellHover : c.well);
        p->drawRoundedRect(QRectF(o->rect).adjusted(0.5, 0.5, -0.5, -0.5), tk::kWellRadius, tk::kWellRadius);
        if (enabled && on(o, State_HasFocus))
            ring(p, QRectF(o->rect), tk::kWellRadius, c.focus);
        p->restore();
        return;
    }
    case PE_FrameLineEdit: return;
    case PE_IndicatorCheckBox:
    case PE_IndicatorRadioButton: {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const bool checked = on(o, State_On);
        const bool partial = on(o, State_NoChange);
        if (element == PE_IndicatorCheckBox && isToggleWidget(widget)) {
            const QRectF track(o->rect.center().x() - tk::kToggleWidth / 2.0 + 0.5, o->rect.center().y() - tk::kToggleHeight / 2.0 + 0.5, tk::kToggleWidth, tk::kToggleHeight);
            p->setPen(Qt::NoPen);
            p->setBrush(hover ? c.wellHover : c.well);
            p->drawRoundedRect(track, track.height() / 2, track.height() / 2);
            const double inset = (tk::kToggleHeight - tk::kToggleDot) / 2.0;
            double x = track.left() + inset;
            if (checked)
                x = track.right() - inset - tk::kToggleDot;
            else if (partial)
                x = track.center().x() - tk::kToggleDot / 2.0;
            QColor dot = checked ? c.accent : c.handle;
            if (!enabled)
                dot = withAlpha(dot, 90);
            else if (hover && !checked)
                dot = dot.lighter(120);
            p->setBrush(dot);
            p->drawEllipse(QRectF(x, track.center().y() - tk::kToggleDot / 2.0, tk::kToggleDot, tk::kToggleDot));
            if (enabled && on(o, State_HasFocus) && on(o, State_KeyboardFocusChange))
                ring(p, track.adjusted(-1, -1, 1, 1), track.height() / 2 + 1, c.focus);
            p->restore();
            return;
        }
        const QRectF box = QRectF(o->rect.center().x() - tk::kIndicator / 2.0 + 0.5, o->rect.center().y() - tk::kIndicator / 2.0 + 0.5, tk::kIndicator, tk::kIndicator);
        const bool radio = element == PE_IndicatorRadioButton;
        const double radius = radio ? box.width() / 2 : tk::kWellRadius;
        p->setPen(Qt::NoPen);
        QColor fill = hover ? c.wellHover : c.well;
        if ((checked || partial) && !radio)
            fill = c.accent;
        if (!enabled)
            fill = withAlpha(fill, 120);
        p->setBrush(fill);
        p->drawRoundedRect(box, radius, radius);
        if (!radio && !checked && !partial) {
            p->setPen(QPen(c.border, 1));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        }
        const QColor mark = !enabled ? c.textDisabled : (radio ? c.accent : c.accentText);
        if (radio && checked) {
            p->setPen(Qt::NoPen);
            p->setBrush(mark);
            p->drawEllipse(box.center(), 3.0, 3.0);
        } else if (!radio && checked) {
            p->setPen(QPen(mark, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->setBrush(Qt::NoBrush);
            QPolygonF tick;
            tick << QPointF(box.left() + 3.5, box.center().y()) << QPointF(box.center().x() - 0.7, box.bottom() - 4.2)
                 << QPointF(box.right() - 3.2, box.top() + 4);
            p->drawPolyline(tick);
        } else if (!radio && partial) {
            p->setPen(QPen(mark, 1.8, Qt::SolidLine, Qt::RoundCap));
            p->drawLine(QPointF(box.left() + 3.5, box.center().y()), QPointF(box.right() - 3.5, box.center().y()));
        }
        if (enabled && on(o, State_HasFocus) && on(o, State_KeyboardFocusChange))
            ring(p, box.adjusted(-1, -1, 1, 1), radius + 1, c.focus);
        p->restore();
        return;
    }
    case PE_IndicatorBranch: {
        if (!(o->state & State_Children))
            return;
        const QRectF r(o->rect);
        const QColor col = hover ? c.text : tk::mix(c.window, c.text, 0.6);
        chevron(p, r, on(o, State_Open) ? 1 : 0, col, 3.0);
        return;
    }
    case PE_Frame:
    case PE_FrameTabWidget:
    case PE_FrameWindow: {
        p->save();
        p->setPen(QPen(c.border, 1));
        p->setBrush(Qt::NoBrush);
        p->drawRect(hairline(o->rect));
        p->restore();
        return;
    }
    case PE_FrameGroupBox: return; // drawn with the header bar in drawComplexControl
    case PE_FrameMenu:
    case PE_PanelMenu:
    case PE_PanelTipLabel: {
        p->save();
        p->setPen(QPen(c.border, 1));
        p->setBrush(c.popup);
        p->drawRect(hairline(o->rect));
        p->restore();
        return;
    }
    default: break;
    }
    QProxyStyle::drawPrimitive(element, o, p, widget);
}

// ---- Controls --------------------------------------------------------------------------

void KinoklipStyle::drawControl(ControlElement element, const QStyleOption* o, QPainter* p, const QWidget* widget) const
{
    const tk::Colors& c = colorsFor(o->palette);
    const bool enabled = on(o, State_Enabled);
    switch (element) {
    case CE_TabBarTabShape: {
        const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(o);
        if (!tab)
            break;
        const bool selected = on(o, State_Selected);
        const bool hover = enabled && on(o, State_MouseOver);
        p->save();
        if (hover && !selected)
            p->fillRect(o->rect, c.toolHover);
        if (selected) {
            const QRect r = o->rect;
            switch (tab->shape) {
            case QTabBar::RoundedSouth:
            case QTabBar::TriangularSouth: p->fillRect(QRect(r.left(), r.top(), r.width(), tk::kTabUnderline), c.mark); break;
            case QTabBar::RoundedWest:
            case QTabBar::TriangularWest: p->fillRect(QRect(r.right() - tk::kTabUnderline + 1, r.top(), tk::kTabUnderline, r.height()), c.mark); break;
            case QTabBar::RoundedEast:
            case QTabBar::TriangularEast: p->fillRect(QRect(r.left(), r.top(), tk::kTabUnderline, r.height()), c.mark); break;
            default: p->fillRect(QRect(r.left(), r.bottom() - tk::kTabUnderline + 1, r.width(), tk::kTabUnderline), c.mark); break;
            }
        }
        p->restore();
        return;
    }
    case CE_TabBarTabLabel: {
        QStyleOption copy(*o);
        QPalette pal = o->palette;
        const QColor col = on(o, State_Selected) ? c.text : tk::mix(c.window, c.text, enabled ? 0.62 : 0.35);
        pal.setColor(QPalette::WindowText, col);
        pal.setColor(QPalette::ButtonText, col);
        QStyleOptionTab tab(*qstyleoption_cast<const QStyleOptionTab*>(o));
        tab.palette = pal;
        tab.state &= ~State_Selected; // no raised-tab text shift or bold of the base style
        QProxyStyle::drawControl(element, &tab, p, widget);
        return;
    }
    case CE_MenuItem: {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(o);
        if (!item)
            break;
        p->save();
        if (item->menuItemType == QStyleOptionMenuItem::Separator) {
            p->setPen(c.border);
            const int y = item->rect.center().y();
            p->drawLine(item->rect.left() + tk::kPaddingH, y, item->rect.right() - tk::kPaddingH, y);
            p->restore();
            return;
        }
        if (item->menuItemType == QStyleOptionMenuItem::EmptyArea) {
            p->restore();
            return;
        }
        const bool selected = enabled && on(o, State_Selected);
        if (selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(c.toolPressed);
            p->drawRect(item->rect);
        }
        const QColor fg = enabled ? c.text : c.textDisabled;
        const bool withColumn = item->menuHasCheckableItems || item->maxIconWidth > 0;
        const int column = withColumn ? std::max(item->maxIconWidth, 16) : 0;
        QRect inner = item->rect.adjusted(tk::kPaddingH, 0, -tk::kPaddingH, 0);
        if (withColumn) {
            const QRect col(inner.left(), inner.top(), column, inner.height());
            if (item->checkType != QStyleOptionMenuItem::NotCheckable && item->checked) {
                p->setRenderHint(QPainter::Antialiasing);
                if (item->checkType == QStyleOptionMenuItem::Exclusive) {
                    p->setPen(Qt::NoPen);
                    p->setBrush(c.accent);
                    p->drawEllipse(col.center(), 3, 3);
                } else {
                    p->setPen(QPen(c.accent, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    QPolygonF tick;
                    const QPointF m = col.center();
                    tick << QPointF(m.x() - 4, m.y()) << QPointF(m.x() - 1.2, m.y() + 3) << QPointF(m.x() + 4, m.y() - 3);
                    p->drawPolyline(tick);
                }
            } else if (!item->icon.isNull()) {
                const QIcon::Mode mode = enabled ? (selected ? QIcon::Active : QIcon::Normal) : QIcon::Disabled;
                const QPixmap pm = item->icon.pixmap(QSize(column, column), widget ? widget->devicePixelRatioF() : 1.0, mode, item->checked ? QIcon::On : QIcon::Off);
                const QSizeF sz = pm.deviceIndependentSize();
                p->drawPixmap(QPointF(col.center().x() - sz.width() / 2, col.center().y() - sz.height() / 2), pm);
            }
            inner.setLeft(inner.left() + column + 6);
        }
        const int arrowRoom = item->menuItemType == QStyleOptionMenuItem::SubMenu ? 14 : 0;
        inner.setRight(inner.right() - arrowRoom);
        p->setFont(item->font);
        p->setPen(fg);
        const QString text = item->text.section(QLatin1Char('\t'), 0, 0);
        const QString shortcut = item->text.section(QLatin1Char('\t'), 1);
        p->drawText(inner, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextShowMnemonic | Qt::TextDontClip | Qt::TextSingleLine, text);
        if (!shortcut.isEmpty()) {
            p->setPen(enabled ? tk::mix(c.window, c.text, 0.6) : c.textDisabled);
            p->drawText(inner, Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, shortcut);
        }
        if (item->menuItemType == QStyleOptionMenuItem::SubMenu)
            chevron(p, QRectF(item->rect.right() - tk::kPaddingH - 8, item->rect.top(), 8, item->rect.height()), 0, fg, 3.0);
        p->restore();
        return;
    }
    case CE_MenuBarEmptyArea: {
        p->fillRect(o->rect, c.window);
        return;
    }
    case CE_MenuBarItem: {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(o);
        if (!item)
            break;
        p->save();
        if (on(o, State_Selected) || on(o, State_Sunken)) {
            p->setPen(Qt::NoPen);
            p->setBrush(c.toolPressed);
            p->drawRect(o->rect);
        }
        p->setPen(enabled ? c.text : c.textDisabled);
        p->setFont(item->font);
        p->drawText(o->rect, Qt::AlignCenter | Qt::TextShowMnemonic | Qt::TextSingleLine, item->text);
        p->restore();
        return;
    }
    case CE_ProgressBarGroove: {
        const int h = tk::kProgressHeight;
        const QRectF r(o->rect.x(), o->rect.center().y() - h / 2.0 + 0.5, o->rect.width(), h);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(c.well);
        p->drawRoundedRect(r, h / 2.0, h / 2.0);
        p->restore();
        return;
    }
    case CE_ProgressBarContents: {
        const auto* pb = qstyleoption_cast<const QStyleOptionProgressBar*>(o);
        if (!pb)
            break;
        const int h = tk::kProgressHeight;
        const double span = pb->maximum - pb->minimum;
        const double t = span > 0 ? double(pb->progress - pb->minimum) / span : 0.0;
        QRectF r(o->rect.x(), o->rect.center().y() - h / 2.0 + 0.5, o->rect.width(), h);
        if (span <= 0) { // busy: a sliding chunk (the position follows the progress counter)
            const double w = r.width() * 0.3;
            r = QRectF(r.x() + (r.width() - w) * ((pb->progress % 100) / 100.0), r.y(), w, r.height());
        } else {
            r.setWidth(std::max(double(h), r.width() * std::clamp(t, 0.0, 1.0)));
            if (t <= 0)
                return;
        }
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(enabled ? c.accent : withAlpha(c.accent, 100));
        p->drawRoundedRect(r, h / 2.0, h / 2.0);
        p->restore();
        return;
    }
    case CE_HeaderSection: {
        p->save();
        p->fillRect(o->rect, (enabled && on(o, State_MouseOver)) ? tk::mix(c.header, c.text, 0.06) : c.header);
        p->setPen(c.border);
        p->drawLine(o->rect.bottomLeft(), o->rect.bottomRight());
        const auto* h = qstyleoption_cast<const QStyleOptionHeader*>(o);
        if (!h || h->position != QStyleOptionHeader::End)
            p->drawLine(o->rect.topRight() + QPoint(0, 4), o->rect.bottomRight() - QPoint(0, 4));
        p->restore();
        return;
    }
    case CE_Splitter: {
        p->fillRect(o->rect, on(o, State_MouseOver) || on(o, State_Sunken) ? c.toolPressed : tk::mix(c.window, QColor(0, 0, 0), c.dark ? 0.3 : 0.08));
        return;
    }
    default: break;
    }
    QProxyStyle::drawControl(element, o, p, widget);
}

void KinoklipStyle::drawComplexControl(ComplexControl control, const QStyleOptionComplex* o, QPainter* p, const QWidget* widget) const
{
    const tk::Colors& c = colorsFor(o->palette);
    const bool enabled = on(o, State_Enabled);
    const bool hover = enabled && on(o, State_MouseOver);
    switch (control) {
    case CC_ScrollBar: {
        const auto* sb = qstyleoption_cast<const QStyleOptionSlider*>(o);
        if (!sb)
            break;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect slider = subControlRect(control, o, SC_ScrollBarSlider, widget);
        const bool horizontal = sb->orientation == Qt::Horizontal;
        const bool grabbed = on(o, State_Sunken) && (sb->activeSubControls & SC_ScrollBarSlider);
        const bool over = hover && (sb->activeSubControls & SC_ScrollBarSlider);
        QColor col = tk::mix(c.window, c.text, c.dark ? 0.22 : 0.30);
        if (grabbed)
            col = tk::mix(c.window, c.text, 0.50);
        else if (over)
            col = tk::mix(c.window, c.text, 0.36);
        const int thin = 4; // thin bar inside the scroll bar's hit area, wider on hover
        const int w = (over || grabbed) ? tk::kScrollBar - 2 : thin;
        QRectF bar = QRectF(slider);
        if (horizontal)
            bar = QRectF(slider.x(), slider.center().y() - w / 2.0 + 0.5, slider.width(), w);
        else
            bar = QRectF(slider.center().x() - w / 2.0 + 0.5, slider.y(), w, slider.height());
        if (sb->maximum > sb->minimum) {
            p->setPen(Qt::NoPen);
            p->setBrush(enabled ? col : withAlpha(col, 80));
            p->drawRoundedRect(bar, w / 2.0, w / 2.0);
        }
        p->restore();
        return;
    }
    case CC_Slider: {
        const auto* sl = qstyleoption_cast<const QStyleOptionSlider*>(o);
        if (!sl)
            break;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const bool horizontal = sl->orientation == Qt::Horizontal;
        const QRect groove = subControlRect(control, o, SC_SliderGroove, widget);
        const QRect handle = subControlRect(control, o, SC_SliderHandle, widget);
        QRectF line;
        if (horizontal)
            line = QRectF(groove.x(), groove.center().y() - tk::kGroove / 2.0 + 0.5, groove.width(), tk::kGroove);
        else
            line = QRectF(groove.center().x() - tk::kGroove / 2.0 + 0.5, groove.y(), tk::kGroove, groove.height());
        p->setPen(Qt::NoPen);
        p->setBrush(enabled ? c.groove : withAlpha(c.groove, 100));
        p->drawRoundedRect(line, tk::kGroove / 2.0, tk::kGroove / 2.0);
        if (flag(widget, "kkFilled") && sl->maximum > sl->minimum) {
            // A volume-like slider: the groove is filled from its start up to the handle (as in Resolve).
            QRectF part = line;
            if (horizontal)
                part.setRight(QRectF(handle).center().x());
            else
                part.setTop(QRectF(handle).center().y());
            p->setBrush(enabled ? c.mark : withAlpha(c.mark, 100));
            p->drawRoundedRect(part, tk::kGroove / 2.0, tk::kGroove / 2.0);
        }
        const bool grabbed = on(o, State_Sunken) && (sl->activeSubControls & SC_SliderHandle);
        QColor col = c.handle;
        if (grabbed)
            col = c.accent;
        else if (hover && (sl->activeSubControls & SC_SliderHandle))
            col = col.lighter(120);
        p->setBrush(enabled ? col : withAlpha(col, 90));
        p->drawEllipse(QRectF(handle).center(), tk::kHandle / 2.0 - 1.0, tk::kHandle / 2.0 - 1.0);
        if (enabled && on(o, State_HasFocus) && on(o, State_KeyboardFocusChange)) {
            p->setPen(QPen(c.focus, tk::kFocusLine));
            p->setBrush(Qt::NoBrush);
            p->drawEllipse(QRectF(handle).center(), tk::kHandle / 2.0 + 1.0, tk::kHandle / 2.0 + 1.0);
        }
        p->restore();
        return;
    }
    case CC_SpinBox: {
        const auto* sp = qstyleoption_cast<const QStyleOptionSpinBox*>(o);
        if (!sp)
            break;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (sp->frame) {
            p->setPen(QPen(c.border, tk::kLine));
            p->setBrush(hover ? c.wellHover : c.well);
            p->drawRoundedRect(QRectF(o->rect).adjusted(0.5, 0.5, -0.5, -0.5), tk::kWellRadius, tk::kWellRadius);
            if (enabled && on(o, State_HasFocus))
                ring(p, QRectF(o->rect), tk::kWellRadius, c.focus);
        }
        p->restore();
        if (sp->buttonSymbols != QAbstractSpinBox::NoButtons && (hover || on(o, State_HasFocus))) {
            const QRect up = subControlRect(control, o, SC_SpinBoxUp, widget);
            const QRect down = subControlRect(control, o, SC_SpinBoxDown, widget);
            const bool upOn = sp->stepEnabled & QAbstractSpinBox::StepUpEnabled;
            const bool downOn = sp->stepEnabled & QAbstractSpinBox::StepDownEnabled;
            const QColor col = tk::mix(c.well, c.text, 0.65);
            chevron(p, QRectF(up), 3, upOn ? col : c.textDisabled, 2.5);
            chevron(p, QRectF(down), 1, downOn ? col : c.textDisabled, 2.5);
        }
        return;
    }
    case CC_ComboBox: {
        const auto* cb = qstyleoption_cast<const QStyleOptionComboBox*>(o);
        if (!cb)
            break;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (cb->frame) {
            p->setPen(QPen(c.border, tk::kLine));
            p->setBrush(hover || on(o, State_On) ? c.wellHover : c.well);
            p->drawRoundedRect(QRectF(o->rect).adjusted(0.5, 0.5, -0.5, -0.5), tk::kWellRadius, tk::kWellRadius);
            if (enabled && on(o, State_HasFocus))
                ring(p, QRectF(o->rect), tk::kWellRadius, c.focus);
        }
        p->restore();
        chevron(p, QRectF(subControlRect(control, o, SC_ComboBoxArrow, widget)), 1,
            enabled ? tk::mix(c.well, c.text, 0.7) : c.textDisabled, 3.0);
        return;
    }
    case CC_ToolButton: {
        const bool page = flag(widget, "kkPage");
        const bool tab = flag(widget, "kkTab");
        if (const auto* tb = qstyleoption_cast<const QStyleOptionToolButton*>(o);
            tb && instantMenu(tb) && tb->toolButtonStyle != Qt::ToolButtonIconOnly && !page && !tab) {
            // A dropdown-like button (mixer FX and bus slots) is framed like a combo box, so
            // it does not look like a label beside the combo boxes of the same strip.
            if (flag(widget, "kkWell")) {
                p->save();
                p->setRenderHint(QPainter::Antialiasing);
                p->setPen(QPen(c.border, tk::kLine));
                p->setBrush(hover || on(o, State_Sunken) ? c.wellHover : c.well);
                p->drawRoundedRect(QRectF(o->rect).adjusted(0.5, 0.5, -0.5, -0.5), tk::kWellRadius, tk::kWellRadius);
                p->restore();
            }
            // Draw the button without Fusion's corner arrow in the room left of a chevron.
            QStyleOptionToolButton copy(*tb);
            copy.features &= ~QStyleOptionToolButton::HasMenu;
            copy.rect.setRight(o->rect.right() - tk::kMenuArrowWidth + 4);
            const QRect arrow(o->rect.right() - tk::kMenuArrowWidth + 1, o->rect.top(), tk::kMenuArrowWidth, o->rect.height());
            drawComplexControl(control, &copy, p, widget);
            chevron(p, QRectF(arrow).adjusted(0, 0, -3, 0), 1,
                enabled ? tk::mix(c.window, c.text, 0.9) : c.textDisabled, 3.0);
            return;
        }
        if (!flag(widget, "kkFrameless") && !page && !tab)
            break;
        // Bar buttons (top toolbar, page bar, tab-like tool buttons): no frame; the current one has
        // brighter text and icon, the rest dimmer; the icon is tinted like the text.
        const auto* tb = qstyleoption_cast<const QStyleOptionToolButton*>(o);
        if (!tb)
            break;
        QStyleOptionToolButton copy(*tb);
        QPalette pal = o->palette;
        const QColor bright = c.dark ? QColor(255, 255, 255) : QColor(0, 0, 0); // current: white (7:1 and more)
        const bool current = on(o, State_On) || hover;
        const QColor col = current ? bright : tk::mix(c.window, c.text, enabled ? 0.9 : 0.4); // others: clearly readable
        pal.setColor(QPalette::ButtonText, col);
        pal.setColor(QPalette::WindowText, col);
        copy.palette = pal;
        if (!tb->icon.isNull())
            copy.icon = tintedIcon(tb->icon, tb->iconSize, widget->devicePixelRatioF(), col, on(o, State_On) ? QIcon::On : QIcon::Off);
        if (page && on(o, State_On))
            p->fillRect(o->rect, c.pageActive);
        QProxyStyle::drawComplexControl(control, &copy, p, widget);
        if ((page || tab) && on(o, State_On)) {
            const int h = page ? tk::kPageUnderline : tk::kTabUnderline;
            p->fillRect(QRect(o->rect.left(), o->rect.bottom() - h + 1, o->rect.width(), h), c.mark);
        }
        return;
    }
    case CC_GroupBox: {
        const auto* gb = qstyleoption_cast<const QStyleOptionGroupBox*>(o);
        if (!gb)
            break;
        // Header bar over the full width with the title; the body is not framed.
        const QFontMetrics fm(gb->fontMetrics);
        const int barH = std::max(tk::kHeaderHeight, fm.height() + 2 * tk::kPaddingV);
        const QRect bar(o->rect.x(), o->rect.y(), o->rect.width(), barH);
        p->save();
        p->fillRect(bar, c.header);
        p->setPen(enabled ? c.text : c.textDisabled);
        p->drawText(subControlRect(control, o, SC_GroupBoxLabel, widget), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextShowMnemonic, gb->text);
        p->restore();
        if (gb->subControls & SC_GroupBoxCheckBox) {
            QStyleOptionButton box;
            box.QStyleOption::operator=(*o);
            box.rect = subControlRect(control, o, SC_GroupBoxCheckBox, widget);
            drawPrimitive(PE_IndicatorCheckBox, &box, p, widget);
        }
        return;
    }
    default: break;
    }
    QProxyStyle::drawComplexControl(control, o, p, widget);
}

} // namespace kk
