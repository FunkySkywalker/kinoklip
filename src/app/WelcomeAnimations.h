// SPDX-License-Identifier: GPL-3.0-or-later
// WelcomeAnimations -- the moving parts of the welcome window: the step dots, the card the pages
// sit on, the page change and the cascade in which a page's elements enter one after another.
// Calm and a little slow, like KDE's Welcome Center: small moves, soft fades, no bounce.
// Everything follows uianim::enabled() and uianim::speed(): with animations off (or offscreen,
// as in tests and screenshots) each change shows its end state at once.
#pragma once

#include <QGraphicsEffect>
#include <QList>
#include <QPixmap>
#include <QWidget>

class QStackedWidget;
class QVariantAnimation;

namespace kk::welcomeanim {

/// A row of quiet dots, one per step; the current one is brighter, and the brightness passes
/// softly to the next dot when the step changes.
class StepIndicator : public QWidget
{
public:
    explicit StepIndicator(int count, QWidget* parent = nullptr);
    /// Makes step @p index the bright one (animated when animations are on and the widget is visible).
    void setCurrent(int index);
    [[nodiscard]] int current() const { return m_current; }
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int m_count;
    int m_current = 0;
    double m_pos = 0; ///< the drawn pill position in steps (fractional while gliding)
    QVariantAnimation* m_anim;
};

/// A rounded panel a little lighter (dark schemes) or darker (light schemes) than the window.
class Card : public QWidget
{
public:
    using QWidget::QWidget;
    /// The panel colour for @p palette (also the page slide's background).
    [[nodiscard]] static QColor fill(const QPalette& palette);

protected:
    void paintEvent(QPaintEvent* event) override;
};

/// The app icon, drawn with whole art pixels; it settles and fades in when revealed.
class IconBadge : public QWidget
{
public:
    IconBadge(const QPixmap& art, int size, QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override { return {m_size, m_size}; }
    void setProgress(double t); ///< 0 hidden .. 1 shown

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPixmap m_art;
    int m_size;
    double m_t = 1;
};

/// Opacity plus a small vertical offset for an element that enters: at progress 0 it is
/// invisible and @p rise pixels low, at 1 it is drawn as it is.
class FadeRise : public QGraphicsEffect
{
public:
    explicit FadeRise(int rise, QObject* parent = nullptr);
    void setProgress(double t);

protected:
    void draw(QPainter* painter) override;
    QRectF boundingRectFor(const QRectF& rect) const override;

private:
    int m_rise;
    double m_t = 0;
};

/// Widgets that enter together (a form row's label and field). An IconBadge in a group scales
/// and fades by itself instead of rising.
using Group = QList<QWidget*>;

/// Cascade: the groups enter one after another (about 70 ms apart, after @p leadMs), each fading
/// in while rising 10 px. They are hidden at once; the effects are removed when each is done.
/// A cascade already running under @p owner ends first. Does nothing when animations are off.
void enter(QWidget* owner, const QList<Group>& groups, int leadMs = 0);

/// Switches @p pages to @p index: the old page fades out as one picture while drifting a little
/// toward the side it leaves to (@p forward: left), then @p groups of the new page cascade in.
/// Instant when animations are off.
void changePage(QStackedWidget* pages, int index, bool forward, const QList<Group>& groups);

} // namespace kk::welcomeanim
