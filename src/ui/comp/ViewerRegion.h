// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerRegion — the Region of Interest of a Compositing viewer (spec 121): a rectangle (fractions of
// the picture) that is drawn and edited over the viewer while the ROI tool is on. Outside it the
// viewer shows nothing (the picture is cut to the region by viewerfx::applyRegion()).
#pragma once


#include <QRectF>
#include <QWidget>

#include <functional>

namespace kk {

class ViewerSurface;

/// Transparent overlay over a viewer surface that draws the region and lets the user change it:
/// drag on empty picture to draw a new one, drag inside to move it, drag a corner or edge to
/// resize it, double-click to reset it to the whole picture. While the tool is off the overlay
/// is hidden and passes no mouse events.
///
/// Thread-safety: GUI thread only.
class ViewerRegion : public QWidget
{
    Q_OBJECT
public:
    /// Smallest side of a region (fraction of the picture).
    static constexpr double kMinSide = 0.02;
    /// Distance (px) within which a handle is grabbed.
    static constexpr double kGrabPx = 8.0;

    explicit ViewerRegion(ViewerSurface* surface, QWidget* parent = nullptr);

    /// The region as fractions of the picture (0..1); the whole picture when none is set.
    [[nodiscard]] QRectF region() const { return m_region; }
    /// True if the region is smaller than the picture.
    [[nodiscard]] bool isSet() const { return m_region != QRectF(0, 0, 1, 1); }
    /// Sets the region (clamped into the picture, at least kMinSide wide and high) and repaints;
    /// emits changed() if it differs.
    void setRegion(const QRectF& region);
    /// Back to the whole picture.
    void reset() { setRegion(QRectF(0, 0, 1, 1)); }

    /// Region in widget pixels (over the picture rectangle of the surface).
    [[nodiscard]] QRectF regionInWidget() const;
    /// Which part of the region a widget point is on: 0 none, 1 inside (move), 2..9 the corners
    /// and edges (tl, t, tr, r, br, b, bl, l). Public for tests.
    [[nodiscard]] int partAt(QPointF widgetPos) const;

signals:
    /// The region changed (a drag, setRegion()).
    void changed(QRectF region);
    /// A drag started / ended (the viewer renders live while it runs, once at the end).
    void dragFinished();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QPointF toFraction(QPointF widgetPos) const;

    ViewerSurface* m_surface = nullptr; ///< the viewer surface (the overlay is its child)
    QRectF m_region {0, 0, 1, 1};
    int m_part = 0;
    QPointF m_pressFraction;
    QRectF m_pressRegion;
};

} // namespace kk
