// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerRegion implementation.
#include "ui/comp/ViewerRegion.h"

#include "ui/common/SchemeColors.h"
#include "ui/viewer/ViewerSurface.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace kk {

ViewerRegion::ViewerRegion(ViewerSurface* surface, QWidget* parent)
    : QWidget(parent ? parent : surface->widget())
    , m_surface(surface)
{
    setObjectName(QStringLiteral("ViewerRegion"));
    setMouseTracking(true);
    setAttribute(Qt::WA_NoSystemBackground);
    setToolTip(tr("Region of Interest: drag to draw a region, drag inside to move it, drag a corner or edge to resize it, double-click to use the whole picture again"));
    setGeometry(surface->widget()->rect());
    surface->widget()->installEventFilter(this);
    hide();
}

bool ViewerRegion::eventFilter(QObject* watched, QEvent* event)
{
    if (m_surface && watched == m_surface->widget() && event->type() == QEvent::Resize)
        setGeometry(m_surface->widget()->rect());
    return QWidget::eventFilter(watched, event);
}

void ViewerRegion::setRegion(const QRectF& region)
{
    QRectF r = region.normalized();
    r.setLeft(std::clamp(r.left(), 0.0, 1.0 - kMinSide));
    r.setTop(std::clamp(r.top(), 0.0, 1.0 - kMinSide));
    r.setRight(std::clamp(r.right(), r.left() + kMinSide, 1.0));
    r.setBottom(std::clamp(r.bottom(), r.top() + kMinSide, 1.0));
    if (r == m_region)
        return;
    m_region = r;
    update();
    emit changed(m_region);
}

QRectF ViewerRegion::regionInWidget() const
{
    const QRectF pic = m_surface ? QRectF(m_surface->pictureRect()) : QRectF();
    return QRectF(pic.x() + m_region.x() * pic.width(), pic.y() + m_region.y() * pic.height(),
        m_region.width() * pic.width(), m_region.height() * pic.height());
}

QPointF ViewerRegion::toFraction(QPointF p) const
{
    const QRectF pic = m_surface ? QRectF(m_surface->pictureRect()) : QRectF();
    if (pic.isEmpty())
        return {};
    return QPointF(std::clamp((p.x() - pic.x()) / pic.width(), 0.0, 1.0),
        std::clamp((p.y() - pic.y()) / pic.height(), 0.0, 1.0));
}

int ViewerRegion::partAt(QPointF p) const
{
    const QRectF r = regionInWidget();
    const double g = kGrabPx;
    const bool nearL = std::abs(p.x() - r.left()) <= g, nearR = std::abs(p.x() - r.right()) <= g;
    const bool nearT = std::abs(p.y() - r.top()) <= g, nearB = std::abs(p.y() - r.bottom()) <= g;
    const bool inX = p.x() > r.left() - g && p.x() < r.right() + g;
    const bool inY = p.y() > r.top() - g && p.y() < r.bottom() + g;
    if (nearL && nearT) return 2;
    if (nearR && nearT) return 4;
    if (nearR && nearB) return 6;
    if (nearL && nearB) return 8;
    if (nearT && inX) return 3;
    if (nearR && inY) return 5;
    if (nearB && inX) return 7;
    if (nearL && inY) return 9;
    return r.contains(p) ? 1 : 0;
}

void ViewerRegion::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const SchemeColors& sc = schemeColors();
    const QRectF r = regionInWidget();
    const QRectF pic = m_surface ? QRectF(m_surface->pictureRect()) : QRectF();
    // The cut-away part is dimmed, the region framed with handles.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 110));
    QPainterPath outside;
    outside.addRect(pic);
    QPainterPath inside;
    inside.addRect(r);
    p.drawPath(outside.subtracted(inside));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(sc.highlight, 1.5, Qt::DashLine));
    p.drawRect(r);
    p.setPen(Qt::NoPen);
    p.setBrush(sc.highlight);
    const double h = 4.0;
    for (const QPointF& c : {r.topLeft(), QPointF(r.center().x(), r.top()), r.topRight(),
             QPointF(r.right(), r.center().y()), r.bottomRight(), QPointF(r.center().x(), r.bottom()),
             r.bottomLeft(), QPointF(r.left(), r.center().y())})
        p.drawRect(QRectF(c.x() - h, c.y() - h, 2 * h, 2 * h));
}

void ViewerRegion::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_pressFraction = toFraction(event->position());
    m_pressRegion = m_region;
    m_part = partAt(event->position());
    if (m_part == 0)
        m_part = 10; // a new region from this point
}

void ViewerRegion::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton) || m_part == 0) {
        // Cursor hints.
        switch (partAt(event->position())) {
        case 1: setCursor(Qt::SizeAllCursor); break;
        case 2: case 6: setCursor(Qt::SizeFDiagCursor); break;
        case 4: case 8: setCursor(Qt::SizeBDiagCursor); break;
        case 3: case 7: setCursor(Qt::SizeVerCursor); break;
        case 5: case 9: setCursor(Qt::SizeHorCursor); break;
        default: setCursor(Qt::CrossCursor); break;
        }
        return;
    }
    const QPointF f = toFraction(event->position());
    const QPointF d = f - m_pressFraction;
    QRectF r = m_pressRegion;
    switch (m_part) {
    case 1: // move: keep the size, stay inside
        r.translate(std::clamp(d.x(), -r.left(), 1.0 - r.right()),
            std::clamp(d.y(), -r.top(), 1.0 - r.bottom()));
        break;
    case 10: r = QRectF(m_pressFraction, f).normalized(); break;
    default:
        if (m_part == 2 || m_part == 8 || m_part == 9) r.setLeft(m_pressRegion.left() + d.x());
        if (m_part == 4 || m_part == 5 || m_part == 6) r.setRight(m_pressRegion.right() + d.x());
        if (m_part == 2 || m_part == 3 || m_part == 4) r.setTop(m_pressRegion.top() + d.y());
        if (m_part == 6 || m_part == 7 || m_part == 8) r.setBottom(m_pressRegion.bottom() + d.y());
        break;
    }
    setRegion(r);
}

void ViewerRegion::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_part != 0) {
        m_part = 0;
        emit dragFinished();
    }
}

void ViewerRegion::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        reset();
        emit dragFinished();
    }
}

} // namespace kk
