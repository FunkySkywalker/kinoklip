// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/color/ColorLightbox.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/color/ColorFrameSource.h"
#include "ui/color/ColorSession.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr int kGap = 8;
constexpr int kPad = 6;
constexpr const char* kSizeKey = "color/lightboxSize";
constexpr const char* kLabelsKey = "color/lightboxLabels";
constexpr const char* kInfoKey = "color/lightboxGradeInfo";
} // namespace

// ---- ColorLightboxGrid ----

ColorLightboxGrid::ColorLightboxGrid(
    ColorSession* session, Project* project, ColorFrameSource* frames, QWidget* parent)
    : QAbstractScrollArea(parent)
    , m_session(session)
    , m_project(project)
    , m_frames(frames)
{
    setObjectName(QStringLiteral("ColorLightboxGrid"));
    setFrameShape(QFrame::NoFrame);
    verticalScrollBar()->setSingleStep(24);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        viewport()->update();
        requestPictures();
    });
    connect(project->timeline(), &Timeline::changed, this, &ColorLightboxGrid::rebuild);
    connect(project, &Project::projectReset, this, &ColorLightboxGrid::rebuild);
    if (m_session) {
        connect(m_session, &ColorSession::clipChanged, viewport(), qOverload<>(&QWidget::update));
        connect(m_session, &ColorSession::selectedClipsChanged, viewport(),
            qOverload<>(&QWidget::update));
    }
    if (m_frames)
        connect(m_frames, &ColorFrameSource::detailChanged, viewport(), qOverload<>(&QWidget::update));
    rebuild();
}

int ColorLightboxGrid::widthOf(Size size)
{
    switch (size) {
    case Size::Small:
        return 120;
    case Size::Large:
        return 320;
    case Size::Medium:
        break;
    }
    return 200;
}

void ColorLightboxGrid::setThumbnailSize(Size size)
{
    if (size == m_size)
        return;
    m_size = size;
    relayout();
    requestPictures();
}

void ColorLightboxGrid::setShowLabels(bool on)
{
    if (on == m_labels)
        return;
    m_labels = on;
    relayout();
}

void ColorLightboxGrid::setShowGradeInfo(bool on)
{
    if (on == m_info)
        return;
    m_info = on;
    relayout();
}

int ColorLightboxGrid::labelHeight() const
{
    const int line = fontMetrics().height();
    return (m_labels ? line : 0) + (m_info ? line : 0) + ((m_labels || m_info) ? 4 : 0);
}

QSize ColorLightboxGrid::cellSize() const
{
    const TimelineData& data = m_project->timeline()->data();
    const int w = widthOf(m_size);
    const int h = data.width > 0 ? std::max(1, int(std::lround(double(w) * data.height / data.width)))
                                 : w * 9 / 16;
    return {w + 2 * kPad, h + 2 * kPad + labelHeight()};
}

int ColorLightboxGrid::columns() const
{
    const int cell = cellSize().width() + kGap;
    return std::max(1, viewport()->width() / cell);
}

void ColorLightboxGrid::rebuild()
{
    m_clips = m_session ? m_session->stripClips() : QVector<quint64>();
    relayout();
    requestPictures();
}

void ColorLightboxGrid::relayout()
{
    const QSize cell = cellSize();
    const int cols = columns();
    const int rows = int((m_clips.size() + cols - 1) / cols);
    const int total = rows * (cell.height() + kGap) + kGap;
    verticalScrollBar()->setRange(0, std::max(0, total - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    viewport()->update();
}

QRect ColorLightboxGrid::cellRect(quint64 clipId) const
{
    const int i = int(m_clips.indexOf(clipId));
    if (i < 0)
        return {};
    const QSize cell = cellSize();
    const int cols = columns();
    // Centre the columns in the viewport.
    const int used = cols * (cell.width() + kGap) - kGap;
    const int left = std::max(kGap, (viewport()->width() - used) / 2);
    return QRect(left + (i % cols) * (cell.width() + kGap),
        kGap + (i / cols) * (cell.height() + kGap) - verticalScrollBar()->value(), cell.width(),
        cell.height());
}

quint64 ColorLightboxGrid::clipAt(QPoint pos) const
{
    for (const quint64 id : m_clips) {
        if (cellRect(id).contains(pos))
            return id;
    }
    return 0;
}

void ColorLightboxGrid::requestPictures()
{
    if (!m_frames || !isVisible())
        return;
    QVector<quint64> visible;
    const QRect view = viewport()->rect().adjusted(0, -cellSize().height(), 0, cellSize().height());
    for (const quint64 id : std::as_const(m_clips)) {
        if (view.intersects(cellRect(id)))
            visible.append(id);
    }
    m_frames->setDetailClips(visible, widthOf(m_size));
}

QString ColorLightboxGrid::infoOf(quint64 clipId) const
{
    const TimelineData& data = m_project->timeline()->data();
    const ClipData* c = editops::clipById(data, clipId);
    if (!c || c->grade.isEmpty())
        return tr("Ungraded");
    int enabled = 0;
    bool lut = false;
    for (const GradeNode& n : c->grade.nodes) {
        if (n.isMixer())
            continue;
        if (n.enabled)
            ++enabled;
        lut = lut || !n.lutPath.isEmpty();
    }
    QString s = tr("Nodes: %1").arg(enabled);
    if (lut)
        s += tr(", LUT");
    return s;
}

void ColorLightboxGrid::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QPalette& pal = palette();
    p.fillRect(viewport()->rect(), pal.color(QPalette::Base).darker(115));
    const TimelineData& data = m_project->timeline()->data();
    const quint64 current = m_session ? m_session->clipId() : 0;
    const QFontMetrics fm = fontMetrics();
    int number = 0;
    for (const quint64 id : std::as_const(m_clips)) {
        ++number;
        const QRect cell = cellRect(id);
        if (!cell.intersects(viewport()->rect()))
            continue;
        const bool selected = m_session && m_session->selectedClips().contains(id);
        p.fillRect(cell,
            selected ? withAlpha(pal.color(QPalette::Highlight), 70)
                     : pal.color(QPalette::Base).darker(108));
        const QRect thumb(cell.left() + kPad, cell.top() + kPad, cell.width() - 2 * kPad,
            cell.height() - 2 * kPad - labelHeight());
        p.fillRect(thumb, QColor(14, 14, 16));
        const QImage img = m_frames ? m_frames->detail(id) : QImage();
        if (!img.isNull()) {
            const QSize s = img.size().scaled(thumb.size(), Qt::KeepAspectRatio);
            p.drawImage(QRect(thumb.center() - QPoint(s.width() / 2, s.height() / 2), s), img);
        } else if (m_frames && m_frames->thumbnail(id).isNull() == false) {
            const QImage small = m_frames->thumbnail(id);
            const QSize s = small.size().scaled(thumb.size(), Qt::KeepAspectRatio);
            p.drawImage(QRect(thumb.center() - QPoint(s.width() / 2, s.height() / 2), s), small);
        }
        int y = thumb.bottom() + 3;
        const ClipData* c = editops::clipById(data, id);
        if (m_labels && c) {
            const auto loc = editops::findClip(data, id);
            QString head = QString::number(number).rightJustified(2, QLatin1Char('0'));
            if (loc)
                head += QStringLiteral("  V%1").arg(loc->track.index + 1);
            head += QStringLiteral("  ") + timecode::formatRecord(c->start, data);
            p.setPen(pal.color(QPalette::Text));
            p.drawText(QRect(thumb.left(), y, thumb.width(), fm.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                fm.elidedText(c->name.isEmpty() ? head : head + QStringLiteral("  ") + c->name,
                    Qt::ElideRight, thumb.width()));
            y += fm.height();
        }
        if (m_info) {
            p.setPen(pal.color(QPalette::PlaceholderText));
            p.drawText(QRect(thumb.left(), y, thumb.width(), fm.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                fm.elidedText(infoOf(id), Qt::ElideRight, thumb.width()));
        }
        if (id == current) {
            p.setPen(QPen(schemeColors().highlight, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRect(cell.adjusted(1, 1, -1, -1));
        } else if (selected) {
            p.setPen(QPen(pal.color(QPalette::Highlight), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(cell);
        }
    }
}

void ColorLightboxGrid::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    relayout();
    requestPictures();
}

void ColorLightboxGrid::showEvent(QShowEvent* event)
{
    QAbstractScrollArea::showEvent(event);
    rebuild();
}

void ColorLightboxGrid::hideEvent(QHideEvent* event)
{
    QAbstractScrollArea::hideEvent(event);
    if (m_frames)
        m_frames->setDetailClips({}, widthOf(m_size)); // free the pictures
}

void ColorLightboxGrid::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_session) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    const quint64 id = clipAt(event->position().toPoint());
    if (!id)
        return;
    if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) {
        QVector<quint64> sel = m_session->selectedClips();
        if (!sel.removeOne(id))
            sel.append(id);
        m_session->setSelectedClips(sel);
    } else {
        m_session->setSelectedClips({id});
        m_session->gradeClip(id);
    }
    viewport()->update();
}

void ColorLightboxGrid::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && clipAt(event->position().toPoint()))
        emit closeRequested();
}

// ---- ColorLightbox ----

ColorLightbox::ColorLightbox(
    ColorSession* session, Project* project, ColorFrameSource* frames, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ColorLightbox"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* bar = new QWidget(this);
    auto* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(8, 3, 8, 3);
    bl->setSpacing(8);
    bl->addWidget(new QLabel(tr("Thumbnail size"), bar));
    m_sizeBox = new QComboBox(bar);
    m_sizeBox->setObjectName(QStringLiteral("ColorLightboxSize"));
    m_sizeBox->addItem(tr("Small"), int(ColorLightboxGrid::Size::Small));
    m_sizeBox->addItem(tr("Medium"), int(ColorLightboxGrid::Size::Medium));
    m_sizeBox->addItem(tr("Large"), int(ColorLightboxGrid::Size::Large));
    bl->addWidget(m_sizeBox);
    m_labelsBox = new QCheckBox(tr("Clip labels"), bar);
    m_labelsBox->setObjectName(QStringLiteral("ColorLightboxLabels"));
    m_infoBox = new QCheckBox(tr("Grade info"), bar);
    m_infoBox->setObjectName(QStringLiteral("ColorLightboxGradeInfo"));
    bl->addWidget(m_labelsBox);
    bl->addWidget(m_infoBox);
    bl->addStretch(1);
    auto* hint = new QLabel(tr("Click a clip to grade it, double-click to return to the viewer"), bar);
    hint->setEnabled(false);
    bl->addWidget(hint);
    layout->addWidget(bar);
    m_grid = new ColorLightboxGrid(session, project, frames, this);
    layout->addWidget(m_grid, 1);

    const QSettings s;
    m_sizeBox->setCurrentIndex(std::clamp(s.value(QLatin1String(kSizeKey), 1).toInt(), 0, 2));
    m_labelsBox->setChecked(s.value(QLatin1String(kLabelsKey), true).toBool());
    m_infoBox->setChecked(s.value(QLatin1String(kInfoKey), true).toBool());
    m_grid->setThumbnailSize(ColorLightboxGrid::Size(m_sizeBox->currentData().toInt()));
    m_grid->setShowLabels(m_labelsBox->isChecked());
    m_grid->setShowGradeInfo(m_infoBox->isChecked());
    connect(m_sizeBox, &QComboBox::currentIndexChanged, this, [this] {
        m_grid->setThumbnailSize(ColorLightboxGrid::Size(m_sizeBox->currentData().toInt()));
        QSettings().setValue(QLatin1String(kSizeKey), m_sizeBox->currentIndex());
    });
    connect(m_labelsBox, &QCheckBox::toggled, this, [this](bool on) {
        m_grid->setShowLabels(on);
        QSettings().setValue(QLatin1String(kLabelsKey), on);
    });
    connect(m_infoBox, &QCheckBox::toggled, this, [this](bool on) {
        m_grid->setShowGradeInfo(on);
        QSettings().setValue(QLatin1String(kInfoKey), on);
    });
    connect(m_grid, &ColorLightboxGrid::closeRequested, this, &ColorLightbox::closeRequested);
}

} // namespace kk
