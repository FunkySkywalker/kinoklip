// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/color/ColorSplitScreen.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/color/ColorFrameSource.h"
#include "ui/color/ColorSession.h"
#include "ui/common/SchemeColors.h"
#include "ui/viewer/ViewerSurface.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr int kStripPictureWidth = 640;
constexpr int kGridPictureWidth = 480;
} // namespace

ColorSplitScreen::ColorSplitScreen(
    ColorSession* session, Project* project, ColorFrameSource* frames, ViewerSurface* surface)
    : QWidget(surface->widget())
    , m_session(session)
    , m_project(project)
    , m_frames(frames)
    , m_surface(surface)
{
    setObjectName(QStringLiteral("ColorSplitScreen"));
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setGeometry(surface->widget()->rect());
    surface->widget()->installEventFilter(this);
    if (m_session) {
        connect(m_session, &ColorSession::selectedClipsChanged, this, &ColorSplitScreen::updateWanted);
        connect(m_session, &ColorSession::clipChanged, this, &ColorSplitScreen::updateWanted);
    }
    connect(project->timeline(), &Timeline::changed, this, &ColorSplitScreen::updateWanted);
    connect(project, &Project::projectReset, this, &ColorSplitScreen::updateWanted);
    if (m_frames)
        connect(m_frames, &ColorFrameSource::detailChanged, this, [this] { update(); });
    show();
}

bool ColorSplitScreen::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_surface->widget() && event->type() == QEvent::Resize)
        setGeometry(m_surface->widget()->rect());
    return QWidget::eventFilter(watched, event);
}

void ColorSplitScreen::setMode(Mode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    setAttribute(Qt::WA_TransparentForMouseEvents, mode == Mode::Off);
    updateWanted();
}

QVector<quint64> ColorSplitScreen::clips() const
{
    QVector<quint64> out;
    if (m_mode == Mode::Off || !m_session)
        return out;
    const QVector<quint64> strip = m_session->stripClips();
    const quint64 current = m_session->clipId();
    if (m_mode == Mode::SelectedClips) {
        for (const quint64 id : strip) {
            if (m_session->selectedClips().contains(id))
                out.append(id);
        }
        if (out.size() < 2 && current && !out.contains(current)) {
            out.append(current);
            std::sort(out.begin(), out.end(), [&strip](quint64 a, quint64 b) {
                return strip.indexOf(a) < strip.indexOf(b);
            });
        }
        return out;
    }
    const int at = int(strip.indexOf(current));
    if (at < 0)
        return out;
    for (int i = std::max(0, at - kNeighbours); i <= std::min(int(strip.size()) - 1, at + kNeighbours);
         ++i)
        out.append(strip[i]);
    return out;
}

QVector<ColorSplitScreen::Tile> ColorSplitScreen::tiles() const
{
    QVector<Tile> out;
    const QVector<quint64> ids = clips();
    const QRectF pic = QRectF(m_surface->pictureRect());
    if (ids.isEmpty() || pic.isEmpty())
        return out;
    const int n = int(ids.size());
    if (m_mode == Mode::Timeline) {
        const double w = pic.width() / n;
        for (int i = 0; i < n; ++i) {
            out.append({ids[i], QRectF(pic.left() + i * w, pic.top(), w, pic.height()),
                QRectF(double(i) / n, 0.0, 1.0 / n, 1.0)});
        }
        return out;
    }
    const int cols = int(std::ceil(std::sqrt(double(n))));
    const int rows = (n + cols - 1) / cols;
    const double w = pic.width() / cols;
    const double h = pic.height() / rows;
    for (int i = 0; i < n; ++i) {
        out.append({ids[i], QRectF(pic.left() + (i % cols) * w, pic.top() + (i / cols) * h, w, h),
            QRectF(0.0, 0.0, 1.0, 1.0)});
    }
    return out;
}

void ColorSplitScreen::updateWanted()
{
    if (m_frames) {
        const QVector<quint64> ids = clips();
        m_frames->setDetailClips(ids,
            ids.isEmpty() ? kGridPictureWidth
                          : (m_mode == Mode::Timeline ? kStripPictureWidth : kGridPictureWidth));
    }
    update();
}

QString ColorSplitScreen::labelOf(quint64 clipId, int number) const
{
    const TimelineData& data = m_project->timeline()->data();
    const ClipData* c = editops::clipById(data, clipId);
    const QString name = c ? c->name : QString();
    const QString num = QString::number(number).rightJustified(2, QLatin1Char('0'));
    return name.isEmpty() ? num : num + QStringLiteral("  ") + name;
}

void ColorSplitScreen::paintEvent(QPaintEvent*)
{
    if (m_mode == Mode::Off)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF pic = QRectF(m_surface->pictureRect());
    p.fillRect(pic, QColor(8, 8, 10));
    const QVector<quint64> strip = m_session ? m_session->stripClips() : QVector<quint64>();
    const quint64 current = m_session ? m_session->clipId() : 0;
    const QColor accent = schemeColors().highlight;
    const QFontMetrics fm(font());
    for (const Tile& t : tiles()) {
        const QImage img = m_frames ? m_frames->detail(t.clip) : QImage();
        p.save();
        p.setClipRect(t.rect);
        if (!img.isNull()) {
            if (m_mode == Mode::Timeline) {
                // The strip shows its slice of the picture, filling the viewer picture's frame.
                const QRectF src(t.source.x() * img.width(), 0, t.source.width() * img.width(),
                    img.height());
                p.drawImage(t.rect, img, src);
            } else {
                const QSizeF s = QSizeF(img.size()).scaled(t.rect.size(), Qt::KeepAspectRatio);
                p.drawImage(QRectF(t.rect.center() - QPointF(s.width() / 2, s.height() / 2), s),
                    img);
            }
        } else {
            p.setPen(QColor(150, 150, 155));
            p.drawText(t.rect, Qt::AlignCenter, tr("Rendering…"));
        }
        // Label.
        const QString label = labelOf(t.clip, int(strip.indexOf(t.clip)) + 1);
        const QRectF bar(t.rect.left(), t.rect.bottom() - fm.height() - 6,
            std::min(t.rect.width(), double(fm.horizontalAdvance(label) + 14)), fm.height() + 6);
        p.fillRect(bar, QColor(0, 0, 0, 160));
        p.setPen(QColor(235, 235, 235));
        p.drawText(bar.adjusted(7, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, label);
        p.restore();
        // Frame: the graded clip in the highlight colour, the others a hair of white.
        p.setBrush(Qt::NoBrush);
        if (t.clip == current) {
            p.setPen(QPen(accent, 2));
            p.drawRect(t.rect.adjusted(1, 1, -1, -1));
        } else {
            p.setPen(QPen(QColor(255, 255, 255, 70), 1));
            p.drawRect(t.rect);
        }
    }
}

void ColorSplitScreen::mousePressEvent(QMouseEvent* event)
{
    if (m_mode == Mode::Off || event->button() != Qt::LeftButton || !m_session) {
        QWidget::mousePressEvent(event);
        return;
    }
    for (const Tile& t : tiles()) {
        if (t.rect.contains(event->position())) {
            if (t.clip != m_session->clipId())
                m_session->gradeClip(t.clip);
            break;
        }
    }
    event->accept();
}

} // namespace kk
