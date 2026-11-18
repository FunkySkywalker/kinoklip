// SPDX-License-Identifier: GPL-3.0-or-later
// CompViewerOverlay — on-viewer controls of the selected Compositing page node: centre, size
// and angle handles (Transform, Merge foreground), mask shape handles (Rectangle, Ellipse) and
// polygon points.
#pragma once

#include "ui/comp/CompAdapter.h"

#include <QImage>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <optional>

namespace kk {

class CompSession;
class ViewerSurface;

/// Transparent child of a CompViewer's picture surface (the TransformOverlay / MaskOverlay
/// concepts for composition nodes). It draws the controls of CompSession::currentNode() in
/// frame coordinates mapped through the surface's pictureRect() (Fit / zoom), so they stay on
/// the picture. Which parameters it edits comes from compadapter::viewerControls().
///
/// Each drag is one undo step (a CompSession gesture); an animated parameter gets a key at
/// the comp frame, like an Inspector edit. Esc during a drag cancels it. Locked nodes show the
/// controls without handles.
///
/// Thread-safety: GUI thread only.
class CompViewerOverlay : public QWidget
{
    Q_OBJECT
public:
    /// Which handle a point hits.
    enum class Handle {
        None,
        Centre, ///< drag moves the centre
        Size, ///< corner: uniform size (Transform) / width + height (masks)
        Width, ///< right edge (masks)
        Height, ///< bottom edge (masks)
        Angle, ///< the knob above the top edge
        Point, ///< a polygon / Bezier point (index in pointIndex)
        TanIn, ///< the incoming tangent handle of Bezier point pointIndex
        TanOut, ///< the outgoing tangent handle
        Segment ///< a spot on a Bezier segment (double-click inserts a point there)
    };
    /// Gives the picture Roto Assist snaps to (the viewer's current frame); may return a null
    /// image, then nothing snaps.
    using SnapSource = std::function<QImage()>;
    void setSnapSource(SnapSource source) { m_snapSource = std::move(source); }
    /// The Bezier point last pressed (-1 = none); Delete removes it.
    [[nodiscard]] int selectedPoint() const { return m_selPoint; }
    /// The Point Follower: point @p index of the current node's path follows the track of node
    /// @p sourceId (a Tracker or Planar Tracker) from the comp frame on, one undo step.
    bool followPoint(int index, quint64 sourceId);

    /// Creates the overlay over @p surface (its widget becomes the parent).
    /// The overlay of viewer @p viewerIndex (0 = left, 1 = right): it draws handles only while
    /// that viewer shows a node (spec 42b.29).
    CompViewerOverlay(CompSession* session, ViewerSurface* surface, int viewerIndex = 0);

    /// True if the selected node has controls (the overlay then takes the mouse).
    [[nodiscard]] bool hasControls() const;
    /// Widget position of handle @p handle (polygon: point @p pointIndex), or nullopt.
    [[nodiscard]] std::optional<QPointF> handlePos(Handle handle, int pointIndex = 0) const;
    /// Handle under @p pos.
    [[nodiscard]] Handle handleAt(QPointF pos, int* pointIndex = nullptr) const;
    /// Re-evaluates visibility and repaints.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Values of the current node's controls at the comp frame.
    struct State
    {
        compadapter::ViewerControlKeys keys;
        QPointF centre {0.5, 0.5}; ///< normalised
        double size = 1.0;
        double width = 0.5, height = 0.5; ///< normalised
        double angleDeg = 0.0;
        QVector<QPointF>
            points; ///< Polygon: CompNode::path points (timeline px around the centre, y down)
        QVector<double> weights; ///< B-spline mask: the weight of each point
        MaskPath bez; ///< Bezier mask / Text path: the points with tangents (local px)
        bool showBez = false; ///< the Bezier path is drawn and edited
        bool bezClosed = false;
        bool roto = false; ///< Roto Assist is on
        bool locked = false;
    };
    [[nodiscard]] std::optional<State> state() const;
    [[nodiscard]] QPointF toWidget(QPointF normalised) const;
    [[nodiscard]] QPointF toNormalised(QPointF widget) const;
    /// Half extents of the drawn box in widget pixels (before rotation).
    [[nodiscard]] QSizeF halfExtents(const State& s) const;
    /// Widget position of polygon point @p p (timeline px around the centre).
    [[nodiscard]] QPointF pointToWidget(const State& s, QPointF p) const;
    /// Widget pixels per timeline pixel.
    [[nodiscard]] double pixelScale() const;
    void applyDrag(QPointF pos);
    /// Widget position of the tangent handle of point @p index (out = true: outgoing).
    [[nodiscard]] QPointF tangentToWidget(const State& s, int index, bool out) const;
    /// Nearest spot of the Bezier curve to @p pos within the pick radius: segment index and
    /// parameter, or -1.
    [[nodiscard]] int segmentAt(const State& s, QPointF pos, double* t) const;
    /// Edits the current node's path at the comp frame as one gesture.
    void editPath(const QString& label, const std::function<void(CompNode&, int64_t)>& edit);

    QPointer<CompSession> m_session;
    ViewerSurface* m_surface;
    int m_viewerIndex = 0;
    Handle m_drag = Handle::None;
    int m_dragPoint = -1;
    QPointF m_pressPos;
    std::optional<State> m_pressState;
    int m_selPoint = -1;
    bool m_altDown = false; ///< Alt was held at the latest mouse event of a drag
    SnapSource m_snapSource;
    QImage m_snapImage; ///< the picture for Roto Assist while a point is dragged
};

} // namespace kk
