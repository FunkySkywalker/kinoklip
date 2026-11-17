// SPDX-License-Identifier: GPL-3.0-or-later
// WelcomeAnimations -- the moving parts of the welcome window: the step dots, the card the pages
// sit on, the page slide and the opening reveal. Calm and a little slow, like KDE's Welcome Center.
// Everything follows uianim::enabled() and uianim::speed(): with animations off (or offscreen,
// as in tests and screenshots) each change shows its end state at once.
#pragma once

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

/// Switches @p pages to @p index: the old page slides out and fades while the new one slides in
/// from the side the user moves to (@p forward: from the right). Instant when animations are off.
void slideTo(QStackedWidget* pages, int index, bool forward);

/// The opening: @p icon fades in, then each of @p widgets fades in a little after the one
/// before it. Instant when animations are off.
void reveal(IconBadge* icon, const QList<QWidget*>& widgets);

} // namespace kk::welcomeanim
