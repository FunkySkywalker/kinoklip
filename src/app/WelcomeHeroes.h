// SPDX-License-Identifier: GPL-3.0-or-later
// WelcomeHeroes -- the pictures at the top of the welcome window's pages. Each is painted with
// QPainter from the palette (so it suits light and dark schemes) and moves slowly while its page
// is visible and animations are on; otherwise it shows a still frame.
#pragma once

#include <QPixmap>
#include <QStringList>
#include <QWidget>

class QVariantAnimation;

namespace kk::welcomeanim {

/// Base of the pictures: about 520 x 170 px, rounded corners, a slow loop that runs only while
/// the widget is visible and uianim::enabled().
class Hero : public QWidget
{
public:
    explicit Hero(int loopMs, QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override { return {520, 170}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {240, 170}; }
    /// True while the loop runs (visible and animations on).
    [[nodiscard]] bool looping() const;

protected:
    void paintEvent(QPaintEvent* event) final;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    /// Draws the picture inside @p rect (already clipped to the rounded shape).
    virtual void paintContent(QPainter& p, const QRectF& rect) = 0;
    /// Loop position 0..1; stillPhase() when the loop is not running.
    [[nodiscard]] double phase() const;
    [[nodiscard]] virtual double stillPhase() const { return 0.0; }
    /// The picture's background, a little apart from the card it sits on.
    [[nodiscard]] QColor background() const;

private:
    QVariantAnimation* m_loop;
    int m_loopMs;
};

/// The app itself: the dark and light screenshot, cover-cropped.
class ThemeHero : public Hero
{
public:
    explicit ThemeHero(QWidget* parent = nullptr);

protected:
    void paintContent(QPainter& p, const QRectF& rect) override;

private:
    QPixmap m_picture;
};

/// A mock window title strip with the menu titles of the chosen layout (a soft pill drifts over
/// them) and a few keycaps; both change with a crossfade.
class MenusHero : public Hero
{
public:
    explicit MenusHero(QWidget* parent = nullptr);
    void setTitles(const QStringList& titles);
    [[nodiscard]] QStringList titles() const { return m_titles; }
    void setVim(bool on);
    [[nodiscard]] bool vim() const { return m_vim; }

protected:
    void paintContent(QPainter& p, const QRectF& rect) override;
    [[nodiscard]] double stillPhase() const override { return 0.0; }

private:
    void drawTitles(QPainter& p, const QRectF& strip, const QStringList& titles, double opacity,
        double pill);
    void drawKeys(QPainter& p, const QRectF& area, const QStringList& keys, double opacity);

    QStringList m_titles;
    QStringList m_oldTitles;
    double m_titlesFade = 1;
    QVariantAnimation* m_titlesAnim;
    bool m_vim = false;
    bool m_oldVim = false;
    double m_vimFade = 1;
    QVariantAnimation* m_vimAnim;
};

/// A mini timeline (ruler, two video tracks, one audio track, a playhead) that zooms in and out
/// around the playhead or around a mouse pointer.
class TimelineHero : public Hero
{
public:
    explicit TimelineHero(QWidget* parent = nullptr);
    void setZoomAroundMouse(bool mouse);
    [[nodiscard]] bool zoomAroundMouse() const { return m_mouse; }

protected:
    void paintContent(QPainter& p, const QRectF& rect) override;

private:
    bool m_mouse = false;
};

/// A graphics chip with frames flowing in and out, and a small "Render finished" card that
/// fades in and out when notifications are on.
class SystemHero : public Hero
{
public:
    explicit SystemHero(QWidget* parent = nullptr);
    void setNotifications(bool on);
    [[nodiscard]] bool notifications() const { return m_notify; }

protected:
    void paintContent(QPainter& p, const QRectF& rect) override;

private:
    bool m_notify = true;
    double m_notifyShown = 1; ///< 0..1, follows m_notify softly
    QVariantAnimation* m_notifyAnim;
};

} // namespace kk::welcomeanim
