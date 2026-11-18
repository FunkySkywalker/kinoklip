// SPDX-License-Identifier: GPL-3.0-or-later
// CompViewerOverlay implementation: frame <-> widget mapping, handle hit tests, drags as
// CompSession gestures (one undo step each).
#include "ui/comp/CompViewerOverlay.h"

#include "compositing/RotoAssist.h"
#include "compositing/tracking/TrackFollow.h"
#include "compositing/tracking/TrackLink.h"
#include "core/Mask.h"
#include "core/NodeRegistry.h"
#include "render/CompMath.h"
#include "ui/common/SchemeColors.h"
#include "ui/comp/CompSession.h"
#include "ui/viewer/ViewerSurface.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QWheelEvent>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk {

namespace {

constexpr double kHandleHitPx = 8.0; ///< pick radius of a handle
constexpr double kHandleSizePx = 7.0; ///< drawn handle square
constexpr double kKnobDistancePx = 24.0; ///< rotation knob above the top edge

/// Screen rotation of a parameter angle (degrees counter-clockwise; screen y points down).
QTransform rotation(double angleDeg)
{
    QTransform t;
    t.rotate(-angleDeg);
    return t;
}

double length(QPointF p)
{
    return std::hypot(p.x(), p.y());
}

bool transformLike(compadapter::ViewerControl kind)
{
    return kind == compadapter::ViewerControl::Transform
        || kind == compadapter::ViewerControl::TextPath;
}

/// Point on the cubic of segment @p i (i -> i + 1) of @p path at @p t.
QPointF segmentPoint(const MaskPath& path, qsizetype i, double t)
{
    const MaskPoint& a = path[i];
    const MaskPoint& b = path[(i + 1) % path.size()];
    return maskdetail::cubic(a.pos(), QPointF(a.x + a.outX, a.y + a.outY),
        QPointF(b.x + b.inX, b.y + b.inY), b.pos(), t);
}

/// Catmull-Rom tangents for point @p i (the neighbours decide; the ends of an open path look at
/// their one neighbour).
void autoSmooth(MaskPath& path, qsizetype i, bool closed)
{
    const qsizetype n = path.size();
    if (n < 2)
        return;
    const bool hasPrev = closed || i > 0, hasNext = closed || i + 1 < n;
    const QPointF prev = hasPrev ? path[(i + n - 1) % n].pos() : path[i].pos();
    const QPointF next = hasNext ? path[(i + 1) % n].pos() : path[i].pos();
    const QPointF d = (hasPrev && hasNext) ? (next - prev) / 6.0
        : hasNext                          ? (next - path[i].pos()) / 3.0
                                           : (path[i].pos() - prev) / 3.0;
    path[i].outX = d.x();
    path[i].outY = d.y();
    path[i].inX = -d.x();
    path[i].inY = -d.y();
    path[i].smooth = true;
}

} // namespace

CompViewerOverlay::CompViewerOverlay(CompSession* session, ViewerSurface* surface, int viewerIndex)
    : QWidget(surface->widget())
    , m_session(session)
    , m_surface(surface)
    , m_viewerIndex(viewerIndex)
{
    setObjectName(QStringLiteral("CompViewerOverlay"));
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::ClickFocus);
    setGeometry(surface->widget()->rect());
    surface->widget()->installEventFilter(this);
    connect(session, &CompSession::selectionChanged, this, &CompViewerOverlay::refresh);
    connect(session, &CompSession::compositionChanged, this, &CompViewerOverlay::refresh);
    connect(session, &CompSession::viewersChanged, this, &CompViewerOverlay::refresh);
    refresh();
    show();
}

bool CompViewerOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_surface->widget() && event->type() == QEvent::Resize)
        setGeometry(m_surface->widget()->rect());
    return QWidget::eventFilter(watched, event);
}

std::optional<CompViewerOverlay::State> CompViewerOverlay::state() const
{
    // A viewer showing no node ("No Node") has nothing to control.
    if (!m_session || !m_session->composition().node(m_session->viewerNode(m_viewerIndex)))
        return std::nullopt;
    const CompNode* n = m_session->composition().node(m_session->currentNode());
    if (!n)
        return std::nullopt;
    State s;
    s.keys = compadapter::viewerControls(n->type);
    if (s.keys.kind == compadapter::ViewerControl::None)
        return std::nullopt;
    const int64_t f = m_session->compFrame();
    auto value = [&](const QString& key, double fallback) {
        return key.isEmpty() ? fallback : n->params.value(key, f, fallback);
    };
    const bool tracker = s.keys.kind == compadapter::ViewerControl::Tracker;
    if (tracker) {
        // Several patterns in one Tracker: the box and the centre handle are the active one's.
        s.patterns = std::clamp(
            int(int(std::lround(n->params.value(nodekeys::Patterns, 0, 1)))), 1, nodekeys::kMaxPatterns);
        s.activePattern = std::clamp(
            int(int(std::lround(n->params.value(nodekeys::ActivePattern, 0, 1)))), 1, s.patterns);
        s.keys.centerX = nodekeys::patternKey(s.activePattern, false);
        s.keys.centerY = nodekeys::patternKey(s.activePattern, true);
        for (int i = 1; i <= s.patterns; ++i) {
            s.points.append(QPointF(value(nodekeys::patternKey(i, false), 0.5),
                value(nodekeys::patternKey(i, true), 0.5)));
        }
    }
    s.centre = QPointF(value(s.keys.centerX, 0.5), value(s.keys.centerY, 0.5));
    s.size = value(s.keys.size, tracker ? 48.0 : 1.0); // Tracker: pattern box, timeline px
    s.width = value(s.keys.width, tracker ? 160.0 : 0.5); // Tracker: search box, timeline px
    s.height = value(s.keys.height, 0.5);
    s.angleDeg = value(s.keys.angle, 0.0);
    if (s.keys.kind == compadapter::ViewerControl::Polygon) {
        for (const MaskPoint& pt : n->path.at(double(f))) {
            s.points.append(QPointF(pt.x, pt.y));
            s.weights.append(pt.weight);
        }
    }
    if (s.keys.kind == compadapter::ViewerControl::Planar) {
        QPointF sum;
        // Corner Positioner: the frame's own corners unless set (Planar Tracker: its region).
        const bool pin = n->type == nodetypes::CornerPositioner;
        static constexpr double kPinX[4] = {0, 1, 1, 0}, kPinY[4] = {1, 1, 0, 0};
        for (int i = 0; i < nodekeys::kPlanarCorners; ++i) {
            s.points.append(QPointF(value(nodekeys::planarCorner(i, false), pin ? kPinX[i] : 0.5),
                value(nodekeys::planarCorner(i, true), pin ? kPinY[i] : 0.5)));
            sum += s.points.last();
        }
        s.centre = sum / double(nodekeys::kPlanarCorners);
    }
    if (s.keys.kind == compadapter::ViewerControl::Grid) {
        // width / height hold the grid's columns and rows; points the current position of every
        // grid point (row by row, row 0 at the top), normalised, y up.
        const QSize frame = m_session->frameSize();
        const int cols = std::clamp(int(value(nodekeys::Columns, 4)), 2, nodekeys::kGridMaxSide);
        const int rows = std::clamp(int(value(nodekeys::Rows, 4)), 2, nodekeys::kGridMaxSide);
        s.width = cols;
        s.height = rows;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const double dx = value(nodekeys::gridKey(r, c, 0), 0.0);
                const double dy = value(nodekeys::gridKey(r, c, 1), 0.0);
                s.points.append(QPointF(double(c) / (cols - 1) + dx / std::max(frame.width(), 1),
                    1.0 - double(r) / (rows - 1) - dy / std::max(frame.height(), 1)));
            }
        }
    }
    const bool bezierNode = s.keys.kind == compadapter::ViewerControl::Bezier;
    const bool textPath = s.keys.kind == compadapter::ViewerControl::TextPath
        && n->params.staticValue(nodekeys::LayoutType, textlayouts::Point).toString()
            == textlayouts::Path;
    if (bezierNode || textPath) {
        s.bez = n->path.at(double(f));
        s.showBez = true;
        s.bezClosed = bezierNode;
        s.roto = n->params.staticValue(nodekeys::RotoAssist, false).toBool();
    }
    s.locked = n->locked;
    return s;
}

bool CompViewerOverlay::hasControls() const
{
    return state().has_value();
}

void CompViewerOverlay::refresh()
{
    const bool on = hasControls();
    setAttribute(Qt::WA_TransparentForMouseEvents, !on);
    update();
}

// Node centres are normalised with y from bottom to top (NodeRegistry); widgets are y down.
QPointF CompViewerOverlay::toWidget(QPointF n) const
{
    const QRectF p = m_surface->pictureRect();
    return QPointF(p.left() + n.x() * p.width(), p.top() + (1.0 - n.y()) * p.height());
}

QPointF CompViewerOverlay::toNormalised(QPointF w) const
{
    const QRectF p = m_surface->pictureRect();
    if (p.isEmpty())
        return {};
    return QPointF((w.x() - p.left()) / p.width(), 1.0 - (w.y() - p.top()) / p.height());
}

double CompViewerOverlay::pixelScale() const
{
    const QSize frame = m_session ? m_session->frameSize() : QSize();
    return frame.width() > 0 ? m_surface->pictureRect().width() / double(frame.width()) : 1.0;
}

QPointF CompViewerOverlay::pointToWidget(const State& s, QPointF p) const
{
    return toWidget(s.centre) + rotation(s.angleDeg).map(p * (s.size * pixelScale()));
}

QPointF CompViewerOverlay::tangentToWidget(const State& s, int index, bool out) const
{
    const MaskPoint& pt = s.bez[index];
    return pointToWidget(s, out ? QPointF(pt.x + pt.outX, pt.y + pt.outY)
                                : QPointF(pt.x + pt.inX, pt.y + pt.inY));
}

int CompViewerOverlay::segmentAt(const State& s, QPointF pos, double* t) const
{
    const qsizetype n = s.bez.size();
    const qsizetype segs = s.bezClosed ? n : n - 1;
    int bestSeg = -1;
    double bestDist = 6.0;
    for (qsizetype i = 0; i < segs; ++i) {
        for (int k = 0; k <= 32; ++k) {
            const double u = k / 32.0;
            const double d = length(pointToWidget(s, segmentPoint(s.bez, i, u)) - pos);
            if (d < bestDist) {
                bestDist = d;
                bestSeg = int(i);
                if (t)
                    *t = u;
            }
        }
    }
    return bestSeg;
}

QSizeF CompViewerOverlay::halfExtents(const State& s) const
{
    const QRectF p = m_surface->pictureRect();
    if (s.keys.kind == compadapter::ViewerControl::Tracker) {
        const double half = s.size * pixelScale() / 2;
        return QSizeF(half, half);
    }
    if (transformLike(s.keys.kind))
        return QSizeF(p.width() * s.size / 2, p.height() * s.size / 2);
    return QSizeF(p.width() * s.width / 2, p.height() * s.height / 2);
}

std::optional<QPointF> CompViewerOverlay::handlePos(Handle handle, int pointIndex) const
{
    const std::optional<State> s = state();
    if (!s)
        return std::nullopt;
    if (s->showBez && pointIndex >= 0 && pointIndex < s->bez.size()
        && (handle == Handle::Point || handle == Handle::TanIn || handle == Handle::TanOut)) {
        return handle == Handle::Point
            ? pointToWidget(*s, s->bez[pointIndex].pos())
            : tangentToWidget(*s, pointIndex, handle == Handle::TanOut);
    }
    if (s->keys.kind == compadapter::ViewerControl::Bezier)
        return handle == Handle::Centre ? std::optional<QPointF>(toWidget(s->centre))
                                        : std::nullopt;
    if (s->keys.kind == compadapter::ViewerControl::Polygon) {
        if (handle != Handle::Point || pointIndex < 0 || pointIndex >= s->points.size())
            return std::nullopt;
        return pointToWidget(*s, s->points[pointIndex]);
    }
    if (s->keys.kind == compadapter::ViewerControl::Tracker && handle == Handle::Point) {
        if (pointIndex < 0 || pointIndex >= s->points.size())
            return std::nullopt;
        return toWidget(s->points[pointIndex]);
    }
    if (s->keys.kind == compadapter::ViewerControl::Grid) {
        if (handle != Handle::Point || pointIndex < 0 || pointIndex >= s->points.size())
            return std::nullopt;
        return toWidget(s->points[pointIndex]);
    }
    if (s->keys.kind == compadapter::ViewerControl::Planar) {
        if (handle == Handle::Point && pointIndex >= 0 && pointIndex < s->points.size())
            return toWidget(s->points[pointIndex]);
        return handle == Handle::Centre ? std::optional<QPointF>(toWidget(s->centre))
                                        : std::nullopt;
    }
    const QPointF c = toWidget(s->centre);
    const QSizeF h = halfExtents(*s);
    const QTransform r = rotation(s->angleDeg);
    if (s->keys.kind == compadapter::ViewerControl::Tracker) {
        const double search = s->width * pixelScale() / 2;
        switch (handle) {
        case Handle::Centre:
            return c;
        case Handle::Size: // pattern box corner
            return c + QPointF(h.width(), h.height());
        case Handle::Width: // search box corner
            return c + QPointF(search, search);
        default:
            return std::nullopt;
        }
    }
    switch (handle) {
    case Handle::Centre:
        return c;
    case Handle::Size:
        return c + r.map(QPointF(h.width(), h.height()));
    case Handle::Width:
        if (transformLike(s->keys.kind))
            return std::nullopt;
        return c + r.map(QPointF(h.width(), 0));
    case Handle::Height:
        if (transformLike(s->keys.kind))
            return std::nullopt;
        return c + r.map(QPointF(0, h.height()));
    case Handle::Angle:
        return c + r.map(QPointF(0, -h.height() - kKnobDistancePx));
    default:
        return std::nullopt;
    }
}

CompViewerOverlay::Handle CompViewerOverlay::handleAt(QPointF pos, int* pointIndex) const
{
    const std::optional<State> s = state();
    if (!s || s->locked)
        return Handle::None;
    if (s->showBez) {
        const auto hit = [&](Handle h, int i) {
            if (pointIndex)
                *pointIndex = i;
            return h;
        };
        // Tangent handles first (they sit close to their point), then the points, then the curve.
        for (int i = 0; i < s->bez.size(); ++i) {
            if (!s->bez[i].hasTangents() && i != m_selPoint)
                continue;
            if (length(tangentToWidget(*s, i, true) - pos) <= kHandleHitPx)
                return hit(Handle::TanOut, i);
            if (length(tangentToWidget(*s, i, false) - pos) <= kHandleHitPx)
                return hit(Handle::TanIn, i);
        }
        for (int i = 0; i < s->bez.size(); ++i) {
            if (length(pointToWidget(*s, s->bez[i].pos()) - pos) <= kHandleHitPx)
                return hit(Handle::Point, i);
        }
        if (s->keys.kind == compadapter::ViewerControl::Bezier) {
            if (length(toWidget(s->centre) - pos) <= kHandleHitPx)
                return Handle::Centre;
            const int seg = segmentAt(*s, pos, nullptr);
            return seg >= 0 ? hit(Handle::Segment, seg) : Handle::None;
        }
        const int seg = segmentAt(*s, pos, nullptr);
        if (seg >= 0)
            return hit(Handle::Segment, seg);
    }
    if (s->keys.kind == compadapter::ViewerControl::Bezier)
        return length(toWidget(s->centre) - pos) <= kHandleHitPx ? Handle::Centre : Handle::None;
    if (s->keys.kind == compadapter::ViewerControl::Polygon) {
        for (int i = 0; i < s->points.size(); ++i) {
            if (length(pointToWidget(*s, s->points[i]) - pos) <= kHandleHitPx) {
                if (pointIndex)
                    *pointIndex = i;
                return Handle::Point;
            }
        }
        return Handle::None;
    }
    if (s->keys.kind == compadapter::ViewerControl::Grid) {
        int best = -1;
        double bestDistance = kHandleHitPx;
        for (int i = 0; i < s->points.size(); ++i) {
            const double d = length(toWidget(s->points[i]) - pos);
            if (d <= bestDistance) {
                best = i;
                bestDistance = d;
            }
        }
        if (best < 0)
            return Handle::None;
        if (pointIndex)
            *pointIndex = best;
        return Handle::Point;
    }
    if (s->keys.kind == compadapter::ViewerControl::Planar) {
        QPolygonF quad;
        for (int i = 0; i < s->points.size(); ++i) {
            quad << toWidget(s->points[i]);
            if (length(quad.last() - pos) <= kHandleHitPx) {
                if (pointIndex)
                    *pointIndex = i;
                return Handle::Point;
            }
        }
        // Inside the quad: drag the whole region (the cross at its centre is the grip).
        return quad.containsPoint(pos, Qt::OddEvenFill) ? Handle::Centre : Handle::None;
    }
    if (s->keys.kind == compadapter::ViewerControl::Tracker && s->patterns > 1) {
        const QSizeF half = halfExtents(*s);
        for (int i = 0; i < s->points.size(); ++i) {
            if (i + 1 == s->activePattern)
                continue;
            const QPointF o = toWidget(s->points[i]);
            if (std::abs(pos.x() - o.x()) <= std::max(half.width(), kHandleHitPx)
                && std::abs(pos.y() - o.y()) <= std::max(half.height(), kHandleHitPx)) {
                if (pointIndex)
                    *pointIndex = i;
                return Handle::Point;
            }
        }
    }
    for (Handle h : {Handle::Angle, Handle::Size, Handle::Width, Handle::Height, Handle::Centre}) {
        const std::optional<QPointF> p = handlePos(h);
        if (p && length(*p - pos) <= kHandleHitPx)
            return h;
    }
    // Anywhere inside the box moves the centre (like dragging a clip in the Edit viewer).
    const QPointF local = rotation(s->angleDeg).inverted().map(pos - toWidget(s->centre));
    const QSizeF h = halfExtents(*s);
    if (std::abs(local.x()) <= h.width() && std::abs(local.y()) <= h.height())
        return Handle::Centre;
    return Handle::None;
}

void CompViewerOverlay::paintEvent(QPaintEvent*)
{
    const std::optional<State> s = state();
    if (!s)
        return;
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // White over a dark halo: readable over any picture (the mask outline convention).
    auto stroke = [&p](const QPainterPath& path) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 160), 3.0));
        p.drawPath(path);
        p.setPen(QPen(QColor(255, 255, 255), 1.2));
        p.drawPath(path);
    };
    auto handle = [&](QPointF c) {
        if (s->locked)
            return;
        const QRectF r(c - QPointF(kHandleSizePx / 2, kHandleSizePx / 2),
            QSizeF(kHandleSizePx, kHandleSizePx));
        p.setPen(QPen(QColor(0, 0, 0, 160), 1.0));
        p.setBrush(sc.selection);
        p.drawRect(r);
    };
    if (s->showBez) {
        QPainterPath curve;
        const qsizetype n = s->bez.size();
        for (qsizetype i = 0; i < n; ++i) {
            const MaskPoint& a = s->bez[i];
            if (i == 0)
                curve.moveTo(pointToWidget(*s, a.pos()));
            if (i + 1 < n || s->bezClosed) {
                const MaskPoint& b = s->bez[(i + 1) % n];
                curve.cubicTo(pointToWidget(*s, QPointF(a.x + a.outX, a.y + a.outY)),
                    pointToWidget(*s, QPointF(b.x + b.inX, b.y + b.inY)),
                    pointToWidget(*s, b.pos()));
            }
        }
        if (s->bezClosed && n > 2)
            curve.closeSubpath();
        stroke(curve);
        for (int i = 0; i < n && !s->locked; ++i) {
            const QPointF pw = pointToWidget(*s, s->bez[i].pos());
            if (s->bez[i].hasTangents() || i == m_selPoint) {
                const QPointF a = tangentToWidget(*s, i, false), b = tangentToWidget(*s, i, true);
                QPainterPath arms;
                arms.moveTo(a);
                arms.lineTo(pw);
                arms.lineTo(b);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(255, 255, 255, 140), 1.0));
                p.drawPath(arms);
                p.setPen(QPen(QColor(0, 0, 0, 160), 1.0));
                p.setBrush(QColor(255, 255, 255));
                p.drawEllipse(a, 3.0, 3.0);
                p.drawEllipse(b, 3.0, 3.0);
            }
            // Corner points are squares, smooth points circles; the selected one is filled.
            p.setPen(QPen(QColor(0, 0, 0, 160), 1.0));
            p.setBrush(i == m_selPoint ? QColor(255, 255, 255) : sc.selection);
            const QRectF r(pw - QPointF(kHandleSizePx / 2, kHandleSizePx / 2),
                QSizeF(kHandleSizePx, kHandleSizePx));
            if (s->bez[i].smooth)
                p.drawEllipse(r);
            else
                p.drawRect(r);
        }
        if (s->keys.kind == compadapter::ViewerControl::Bezier) {
            const QPointF c = toWidget(s->centre);
            QPainterPath cross;
            cross.moveTo(c - QPointF(6, 0));
            cross.lineTo(c + QPointF(6, 0));
            cross.moveTo(c - QPointF(0, 6));
            cross.lineTo(c + QPointF(0, 6));
            stroke(cross);
            return;
        }
    }
    if (s->keys.kind == compadapter::ViewerControl::Grid) {
        const int cols = int(s->width), rows = int(s->height);
        QPainterPath lines;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const QPointF w = toWidget(s->points[r * cols + c]);
                if (c + 1 < cols) {
                    lines.moveTo(w);
                    lines.lineTo(toWidget(s->points[r * cols + c + 1]));
                }
                if (r + 1 < rows) {
                    lines.moveTo(w);
                    lines.lineTo(toWidget(s->points[(r + 1) * cols + c]));
                }
            }
        }
        stroke(lines);
        for (const QPointF& pt : s->points)
            handle(toWidget(pt));
        return;
    }
    if (s->keys.kind == compadapter::ViewerControl::Planar) {
        QPolygonF quad;
        for (const QPointF& pt : s->points)
            quad << toWidget(pt);
        QPainterPath outline, diagonals, cross;
        outline.addPolygon(quad);
        outline.closeSubpath();
        const QPointF mid = toWidget(s->centre);
        if (quad.size() == 4) {
            diagonals.moveTo(quad[0]);
            diagonals.lineTo(quad[2]);
            diagonals.moveTo(quad[1]);
            diagonals.lineTo(quad[3]);
        }
        cross.moveTo(mid - QPointF(6, 0));
        cross.lineTo(mid + QPointF(6, 0));
        cross.moveTo(mid - QPointF(0, 6));
        cross.lineTo(mid + QPointF(0, 6));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 160), 3.0));
        p.drawPath(diagonals);
        p.setPen(QPen(QColor(255, 255, 255, 170), 1.0, Qt::DashLine));
        p.drawPath(diagonals);
        stroke(outline);
        stroke(cross);
        for (const QPointF& corner : quad)
            handle(corner);
        return;
    }
    if (s->keys.kind == compadapter::ViewerControl::Polygon) {
        QPainterPath path;
        for (int i = 0; i < s->points.size(); ++i) {
            const QPointF w = pointToWidget(*s, s->points[i]);
            if (i == 0)
                path.moveTo(w);
            else
                path.lineTo(w);
        }
        if (s->points.size() > 2)
            path.closeSubpath();
        const CompNode* node = m_session->composition().node(m_session->currentNode());
        if (node && node->type == nodetypes::BSplineMask && s->points.size() > 2) {
            // The control cage in a thin line, the curve it makes in the full stroke.
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(255, 255, 255, 120), 1.0, Qt::DashLine));
            p.drawPath(path);
            MaskPath controls;
            for (int i = 0; i < s->points.size(); ++i) {
                MaskPoint mp;
                mp.x = s->points[i].x();
                mp.y = s->points[i].y();
                mp.weight = i < s->weights.size() ? s->weights[i] : 1.0;
                controls.append(mp);
            }
            QPainterPath curve;
            const MaskPath poly = render::compmath::bsplinePolygon(controls);
            for (qsizetype i = 0; i < poly.size(); ++i) {
                const QPointF w = pointToWidget(*s, poly[i].pos());
                if (i == 0)
                    curve.moveTo(w);
                else
                    curve.lineTo(w);
            }
            curve.closeSubpath();
            stroke(curve);
            for (const QPointF& pt : s->points)
                handle(pointToWidget(*s, pt));
            return;
        }
        stroke(path);
        for (const QPointF& pt : s->points)
            handle(pointToWidget(*s, pt));
        return;
    }
    const QPointF c = toWidget(s->centre);
    const QSizeF h = halfExtents(*s);
    if (s->keys.kind == compadapter::ViewerControl::Tracker) {
        // Pattern box (solid), search box (the larger one, dashed), centre cross, two corners.
        const double search = s->width * pixelScale() / 2;
        QPainterPath patternBox, searchBox, cross;
        patternBox.addRect(
            QRectF(c.x() - h.width(), c.y() - h.height(), h.width() * 2, h.height() * 2));
        searchBox.addRect(QRectF(c.x() - search, c.y() - search, search * 2, search * 2));
        cross.moveTo(c - QPointF(6, 0));
        cross.lineTo(c + QPointF(6, 0));
        cross.moveTo(c - QPointF(0, 6));
        cross.lineTo(c + QPointF(0, 6));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 160), 3.0));
        p.drawPath(searchBox);
        p.setPen(QPen(QColor(255, 255, 255), 1.2, Qt::DashLine));
        p.drawPath(searchBox);
        stroke(patternBox);
        stroke(cross);
        handle(*handlePos(Handle::Size));
        handle(*handlePos(Handle::Width));
        // The other patterns: a box of the same size with its number; click one to make it
        // the active pattern.
        for (int i = 0; i < s->points.size() && s->patterns > 1; ++i) {
            if (i + 1 == s->activePattern)
                continue;
            const QPointF o = toWidget(s->points[i]);
            QPainterPath other;
            other.addRect(QRectF(o.x() - h.width(), o.y() - h.height(), h.width() * 2, h.height() * 2));
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(255, 255, 255, 200), 1.0, Qt::DashLine));
            p.drawPath(other);
            p.setPen(QColor(255, 255, 255));
            p.drawText(o + QPointF(h.width() + 3, -h.height()), QString::number(i + 1));
        }
        p.setPen(QColor(255, 255, 255));
        p.drawText(c + QPointF(h.width() + 3, -h.height()), QString::number(s->activePattern));
        return;
    }
    QTransform t = rotation(s->angleDeg);
    t = t * QTransform::fromTranslate(c.x(), c.y());
    QPainterPath shape;
    const QRectF local(-h.width(), -h.height(), h.width() * 2, h.height() * 2);
    if (s->keys.kind == compadapter::ViewerControl::Ellipse)
        shape.addEllipse(local);
    else
        shape.addRect(local);
    stroke(t.map(shape));
    // Centre cross and the rotation knob on its stem.
    QPainterPath cross;
    cross.moveTo(c - QPointF(6, 0));
    cross.lineTo(c + QPointF(6, 0));
    cross.moveTo(c - QPointF(0, 6));
    cross.lineTo(c + QPointF(0, 6));
    stroke(cross);
    if (const auto knob = handlePos(Handle::Angle)) {
        QPainterPath stem;
        stem.moveTo(*handlePos(Handle::Angle));
        stem.lineTo(c + rotation(s->angleDeg).map(QPointF(0, -h.height())));
        stroke(stem);
        if (!s->locked) {
            p.setPen(QPen(QColor(0, 0, 0, 160), 1.0));
            p.setBrush(sc.selection);
            p.drawEllipse(*knob, kHandleSizePx / 2 + 1, kHandleSizePx / 2 + 1);
        }
    }
    for (Handle hd : {Handle::Size, Handle::Width, Handle::Height}) {
        if (const auto hp = handlePos(hd))
            handle(*hp);
    }
}

void CompViewerOverlay::mousePressEvent(QMouseEvent* event)
{
    int point = -1;
    const Handle h
        = event->button() == Qt::LeftButton ? handleAt(event->position(), &point) : Handle::None;
    if (h == Handle::None || !m_session) {
        event->ignore();
        return;
    }
    static const char* const labels[] = {"", QT_TR_NOOP("Move"), QT_TR_NOOP("Resize"),
        QT_TR_NOOP("Resize"), QT_TR_NOOP("Resize"), QT_TR_NOOP("Rotate"), QT_TR_NOOP("Move Point"),
        QT_TR_NOOP("Move Tangent"), QT_TR_NOOP("Move Tangent"), QT_TR_NOOP("Select Point")};
    m_drag = h;
    m_dragPoint = point;
    m_pressPos = event->position();
    m_pressState = state();
    if (h == Handle::Point || h == Handle::TanIn || h == Handle::TanOut) {
        m_selPoint = point;
        update();
    }
    m_snapImage = QImage();
    if (h == Handle::Point && m_pressState && m_pressState->roto && m_snapSource)
        m_snapImage = m_snapSource();
    const CompNode* n = m_session->composition().node(m_session->currentNode());
    m_session->beginGesture(tr("%1 %2").arg(tr(labels[int(h)]), n ? n->name : QString()));
    setFocus(Qt::MouseFocusReason);
}

void CompViewerOverlay::mouseMoveEvent(QMouseEvent* event)
{
    m_altDown = event->modifiers().testFlag(Qt::AltModifier);
    if (m_drag != Handle::None)
        applyDrag(event->position());
}

void CompViewerOverlay::applyDrag(QPointF pos)
{
    if (!m_pressState || !m_session)
        return;
    const State& s0 = *m_pressState;
    const QRectF pic = m_surface->pictureRect();
    if (pic.isEmpty())
        return;
    const QPointF c = toWidget(s0.centre);
    QMap<QString, double> scalars;
    std::optional<std::pair<int, QPointF>> movedPoint; ///< polygon point index, new position
    std::function<void(MaskPath&)> pathEdit; ///< Bezier / Text path: edits the path in place
    int activate = 0; ///< Tracker: the pattern that becomes the active one
    switch (m_drag) {
    case Handle::Centre: {
        const QPointF d = pos - m_pressPos;
        if (s0.keys.kind == compadapter::ViewerControl::Planar) {
            for (int i = 0; i < s0.points.size(); ++i) {
                scalars.insert(
                    nodekeys::planarCorner(i, false), s0.points[i].x() + d.x() / pic.width());
                scalars.insert(
                    nodekeys::planarCorner(i, true), s0.points[i].y() - d.y() / pic.height());
            }
            break;
        }
        scalars.insert(s0.keys.centerX, s0.centre.x() + d.x() / pic.width());
        scalars.insert(s0.keys.centerY, s0.centre.y() - d.y() / pic.height()); // y up
        break;
    }
    case Handle::Size:
    case Handle::Width:
    case Handle::Height: {
        if (s0.keys.kind == compadapter::ViewerControl::Tracker) {
            // A square box: the farther of the two distances from the centre decides.
            const QPointF d = pos - c;
            const double side = 2 * std::max(std::abs(d.x()), std::abs(d.y())) / pixelScale();
            if (m_drag == Handle::Size) {
                scalars.insert(s0.keys.size, std::max(8.0, side));
                // The search box stays larger than the pattern (it would cover its handle).
                scalars.insert(s0.keys.width, std::max(s0.width, side * 1.25));
            } else
                scalars.insert(s0.keys.width, std::max(s0.size, side));
            break;
        }
        if (transformLike(s0.keys.kind)) {
            const double from = length(m_pressPos - c);
            if (from > 1)
                scalars.insert(s0.keys.size, std::max(0.0, s0.size * length(pos - c) / from));
            break;
        }
        const QPointF local = rotation(s0.angleDeg).inverted().map(pos - c);
        if (m_drag != Handle::Height)
            scalars.insert(s0.keys.width, 2 * std::abs(local.x()) / pic.width());
        if (m_drag != Handle::Width)
            scalars.insert(s0.keys.height, 2 * std::abs(local.y()) / pic.height());
        break;
    }
    case Handle::Angle: {
        // Screen angles grow clockwise (y down); parameter angles counter-clockwise.
        const QPointF a = m_pressPos - c, b = pos - c;
        const double delta = std::atan2(b.y(), b.x()) - std::atan2(a.y(), a.x());
        scalars.insert(s0.keys.angle, s0.angleDeg - delta * 180.0 / std::numbers::pi);
        break;
    }
    case Handle::Point: {
        if (s0.keys.kind == compadapter::ViewerControl::Grid) {
            const int cols = int(s0.width), rows = int(s0.height);
            if (m_dragPoint >= 0 && m_dragPoint < cols * rows) {
                const int r = m_dragPoint / cols, c = m_dragPoint % cols;
                const QPointF n = toNormalised(pos);
                const QSize frame = m_session->frameSize();
                scalars.insert(nodekeys::gridKey(r, c, 0),
                    (n.x() - double(c) / (cols - 1)) * frame.width());
                scalars.insert(nodekeys::gridKey(r, c, 1),
                    -(n.y() - (1.0 - double(r) / (rows - 1))) * frame.height());
            }
            break;
        }
        if (s0.keys.kind == compadapter::ViewerControl::Tracker) {
            if (m_dragPoint >= 0 && m_dragPoint < s0.points.size()) {
                // Another pattern: it becomes the active one and moves.
                const QPointF n = toNormalised(pos);
                scalars.insert(nodekeys::patternKey(m_dragPoint + 1, false), n.x());
                scalars.insert(nodekeys::patternKey(m_dragPoint + 1, true), n.y());
                activate = m_dragPoint + 1;
            }
            break;
        }
        if (s0.keys.kind == compadapter::ViewerControl::Planar) {
            if (m_dragPoint >= 0 && m_dragPoint < s0.points.size()) {
                const QPointF n = toNormalised(pos);
                scalars.insert(nodekeys::planarCorner(m_dragPoint, false), n.x());
                scalars.insert(nodekeys::planarCorner(m_dragPoint, true), n.y());
            }
            break;
        }
        const double k = s0.size * pixelScale();
        if (s0.showBez) {
            if (m_dragPoint >= 0 && m_dragPoint < s0.bez.size() && k > 0) {
                QPointF target = pos;
                if (!m_snapImage.isNull() && m_surface->pictureRect().width() > 0) {
                    const QRectF pr = m_surface->pictureRect();
                    const double f = m_snapImage.width() / pr.width();
                    const QPointF img((pos.x() - pr.left()) * f, (pos.y() - pr.top()) * f);
                    if (const auto snapped = roto::snapToEdge(m_snapImage, img, 12.0))
                        target = QPointF(pr.left() + snapped->x() / f, pr.top() + snapped->y() / f);
                }
                const QPointF local = rotation(s0.angleDeg).inverted().map(target - c) / k;
                const int idx = m_dragPoint;
                pathEdit = [idx, local](MaskPath& path) {
                    path[idx].x = local.x();
                    path[idx].y = local.y();
                };
            }
            break;
        }
        if (m_dragPoint >= 0 && m_dragPoint < s0.points.size() && k > 0)
            movedPoint
                = std::make_pair(m_dragPoint, rotation(s0.angleDeg).inverted().map(pos - c) / k);
        break;
    }
    case Handle::TanIn:
    case Handle::TanOut: {
        const double k = s0.size * pixelScale();
        if (m_dragPoint >= 0 && m_dragPoint < s0.bez.size() && k > 0) {
            const bool out = m_drag == Handle::TanOut;
            const bool breakSmooth = m_altDown;
            const int idx = m_dragPoint;
            const QPointF local = rotation(s0.angleDeg).inverted().map(pos - c) / k;
            pathEdit = [=](MaskPath& path) {
                MaskPoint& pt = path[idx];
                const QPointF v = local - pt.pos();
                (out ? pt.outX : pt.inX) = v.x();
                (out ? pt.outY : pt.inY) = v.y();
                if (breakSmooth) {
                    pt.smooth = false;
                } else if (pt.smooth) {
                    // Smooth: the other handle turns to stay on the same line, keeping its length.
                    const double mine = std::hypot(v.x(), v.y());
                    const double otherLen = out ? std::hypot(pt.inX, pt.inY)
                                                : std::hypot(pt.outX, pt.outY);
                    const double len = otherLen > 1e-9 ? otherLen : mine;
                    if (mine > 1e-9) {
                        (out ? pt.inX : pt.outX) = -v.x() / mine * len;
                        (out ? pt.inY : pt.outY) = -v.y() / mine * len;
                    }
                }
            };
        }
        break;
    }
    case Handle::Segment:
        return;
    case Handle::None:
        return;
    }
    const quint64 id = m_session->currentNode();
    const int64_t frame = m_session->compFrame();
    m_session->modifyLive([&](Composition& comp) {
        CompNode* n = comp.node(id);
        if (!n)
            return;
        for (auto it = scalars.cbegin(); it != scalars.cend(); ++it) {
            if (!it.key().isEmpty())
                n->params.animated(it.key(), it.value()).setValue(frame, it.value());
        }
        if (activate > 0)
            n->params.setBase(nodekeys::ActivePattern, activate);
        if (pathEdit) {
            MaskPath path = n->path.at(double(frame));
            pathEdit(path);
            n->path.setShape(frame, path);
        }
        if (movedPoint) {
            MaskPath path = n->path.at(double(frame));
            if (movedPoint->first < path.size()) {
                path[movedPoint->first].x = movedPoint->second.x();
                path[movedPoint->first].y = movedPoint->second.y();
                n->path.setShape(frame, path);
            }
        }
    });
}

void CompViewerOverlay::editPath(
    const QString& label, const std::function<void(CompNode&, int64_t)>& edit)
{
    const quint64 id = m_session->currentNode();
    const int64_t frame = m_session->compFrame();
    const CompNode* node = m_session->composition().node(id);
    if (!node)
        return;
    m_session->beginGesture(tr("%1 %2").arg(label, node->name));
    m_session->modifyLive([&](Composition& comp) {
        if (CompNode* n = comp.node(id))
            edit(*n, frame);
    });
    m_session->endGesture();
}

void CompViewerOverlay::mouseDoubleClickEvent(QMouseEvent* event)
{
    int index = -1;
    const std::optional<State> s = state();
    if (!s || !s->showBez || !m_session || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    const Handle h = handleAt(event->position(), &index);
    if (h == Handle::Point) {
        // Smooth <-> corner: a smooth point gets Catmull-Rom tangents, a corner none.
        m_selPoint = index;
        editPath(tr("Smooth Point"), [&](CompNode& n, int64_t frame) {
            MaskPath path = n.path.at(double(frame));
            if (index >= path.size())
                return;
            if (path[index].smooth) {
                path[index].inX = path[index].inY = path[index].outX = path[index].outY = 0;
                path[index].smooth = false;
            } else {
                autoSmooth(path, index, s->bezClosed);
            }
            n.path.setShape(frame, path);
        });
    } else if (h == Handle::Segment) {
        double t = 0.5;
        const int seg = segmentAt(*s, event->position(), &t);
        if (seg < 0) {
            event->ignore();
            return;
        }
        editPath(tr("Add Point"), [&](CompNode& n, int64_t frame) {
            MaskData m;
            m.path = n.path;
            m.shape = MaskShape::Bezier;
            m.closed = s->bezClosed;
            const qsizetype at = insertPathPoint(m, seg, t);
            // The new point may snap to a nearby edge (Roto Assist).
            if (s->roto && m_snapSource && at >= 0) {
                const QImage img = m_snapSource();
                const QRectF pr = m_surface->pictureRect();
                if (!img.isNull() && pr.width() > 0) {
                    const double f = img.width() / pr.width();
                    const QPointF w = pointToWidget(*s, m.path.shapes[0][at].pos());
                    const QPointF imgPos((w.x() - pr.left()) * f, (w.y() - pr.top()) * f);
                    if (const auto snapped = roto::snapToEdge(img, imgPos, 12.0)) {
                        const QPointF target(pr.left() + snapped->x() / f, pr.top() + snapped->y() / f);
                        const double k = s->size * pixelScale();
                        const QPointF local
                            = rotation(s->angleDeg).inverted().map(target - toWidget(s->centre)) / k;
                        m.path.forEachShape([&](MaskPath& sp) {
                            sp[at].x = local.x();
                            sp[at].y = local.y();
                        });
                    }
                }
            }
            n.path = m.path;
            m_selPoint = int(at);
            (void)frame;
        });
    } else {
        event->ignore();
        return;
    }
    event->accept();
}

bool CompViewerOverlay::followPoint(int index, quint64 sourceId)
{
    if (!m_session)
        return false;
    const quint64 id = m_session->currentNode();
    const int64_t frame = m_session->compFrame();
    const QSizeF frameSize(m_session->frameSize());
    const CompNode* source = m_session->composition().node(sourceId);
    return m_session->edit(tr("Follow %1").arg(source ? source->name : QString()),
        [=](Composition& c) {
            CompNode* t = c.node(id);
            const CompNode* s = c.node(sourceId);
            return t && s && tracking::followPoints(*t, {index}, *s, frame, frameSize);
        });
}

void CompViewerOverlay::contextMenuEvent(QContextMenuEvent* event)
{
    int index = -1;
    const std::optional<State> s = state();
    if (!s || !s->showBez || !m_session
        || handleAt(event->pos(), &index) != Handle::Point) {
        event->ignore();
        return;
    }
    m_selPoint = index;
    update();
    QMenu menu(this);
    QAction* smooth = menu.addAction(s->bez[index].smooth
            ? tr("Corner Point")
            : tr("Smooth Point"));
    QAction* remove = menu.addAction(tr("Delete Point"));
    remove->setEnabled(s->bez.size() > (s->bezClosed ? 3 : 2));
    QMenu* follow = menu.addMenu(tr("Follow Track"));
    const std::vector<quint64> sources = tracking::trackSources(m_session->composition());
    for (const quint64 src : sources) {
        if (const CompNode* n = m_session->composition().node(src))
            follow->addAction(n->name)->setData(qulonglong(src));
    }
    follow->setEnabled(!sources.empty());
    QAction* chosen = menu.exec(event->globalPos());
    if (!chosen)
        return;
    if (chosen == smooth) {
        QMouseEvent dc(QEvent::MouseButtonDblClick, QPointF(event->pos()), event->globalPos(),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        mouseDoubleClickEvent(&dc);
    } else if (chosen == remove) {
        QKeyEvent key(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        keyPressEvent(&key);
    } else if (chosen->data().isValid()) {
        followPoint(index, chosen->data().toULongLong());
    }
}

void CompViewerOverlay::wheelEvent(QWheelEvent* event)
{
    // B-spline mask: the wheel over a point changes its weight (how hard it pulls the curve).
    int point = -1;
    const CompNode* node = m_session ? m_session->composition().node(m_session->currentNode())
                                     : nullptr;
    if (!node || node->type != nodetypes::BSplineMask || m_drag != Handle::None
        || handleAt(event->position(), &point) != Handle::Point || point < 0
        || event->angleDelta().y() == 0) {
        event->ignore();
        return;
    }
    const double factor = std::pow(1.15, event->angleDelta().y() / 120.0);
    const quint64 id = node->id;
    const int64_t frame = m_session->compFrame();
    m_session->beginGesture(tr("Point Weight %1").arg(node->name));
    m_session->modifyLive([&](Composition& comp) {
        CompNode* n = comp.node(id);
        if (!n)
            return;
        MaskPath path = n->path.at(double(frame));
        if (point < path.size()) {
            path[point].weight = std::clamp(path[point].weight * factor, 0.05, 20.0);
            n->path.setShape(frame, path);
        }
    });
    m_session->endGesture();
    event->accept();
}

void CompViewerOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_drag == Handle::None) {
        event->ignore();
        return;
    }
    m_altDown = event->modifiers().testFlag(Qt::AltModifier);
    applyDrag(event->position());
    m_drag = Handle::None;
    m_pressState.reset();
    if (m_session)
        m_session->endGesture();
}

void CompViewerOverlay::keyPressEvent(QKeyEvent* event)
{
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && m_session
        && m_drag == Handle::None && m_selPoint >= 0) {
        const std::optional<State> s = state();
        const int index = m_selPoint;
        const qsizetype minimum = s && s->bezClosed ? 3 : 2;
        if (s && s->showBez && !s->locked && s->bez.size() > minimum && index < s->bez.size()) {
            editPath(tr("Delete Point"), [&](CompNode& n, int64_t) {
                MaskData m;
                m.path = n.path;
                m.shape = MaskShape::Bezier;
                removePathPoint(m, index);
                n.path = m.path;
            });
            m_selPoint = -1;
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Escape && m_drag != Handle::None && m_pressState) {
        // Cancel: put the values from the press back, then close the (now empty) gesture.
        const QPointF press = m_pressPos;
        applyDrag(press);
        m_drag = Handle::None;
        m_pressState.reset();
        if (m_session)
            m_session->endGesture();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace kk
