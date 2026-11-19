// SPDX-License-Identifier: GPL-3.0-or-later
// AutomationLanes implementation: the lane widget (curve editor of one track) and its manager.
#include "ui/timeline/AutomationLanes.h"

#include "core/MixerOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineLayout.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk {

namespace {

constexpr double kPointRadius = 4.0;
constexpr double kPointGrab = 7.0; ///< px around a point that grab it
constexpr double kLineGrab = 5.0; ///< px above / below the curve that grab a segment
constexpr double kPadTop = 10.0;
constexpr double kPadBottom = 10.0;
constexpr double kDragStart = 3.0; ///< px before a press on a segment becomes a drag
constexpr double kDbFloor = -60.0; ///< lowest dB shown in a lane (the bottom edge means "-inf")

} // namespace

// ---- AutomationLaneWidget ----

AutomationLaneWidget::AutomationLaneWidget(
    AppContext& ctx, const TimelineLayout& layout, quint64 trackId, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
    , m_layout(layout)
    , m_trackId(trackId)
    , m_key(mixkeys::kVolumeDb)
{
    setObjectName(QStringLiteral("AutomationLane"));
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(false);
    setToolTip(tr("Automation lane: click to add a point, drag a point to move it, drag a line "
                  "up or down to move both its points, Delete removes the selected point, "
                  "right-click a point to shape the line after it"));
    m_picker = new QComboBox(this);
    m_picker->setObjectName(QStringLiteral("AutomationParameter"));
    m_picker->setToolTip(tr("What this lane shows: Volume, Pan, Mute, the level of a send or a "
                            "parameter of an effect slot of the track. A dot marks parameters "
                            "that already have a curve"));
    m_picker->setFocusPolicy(Qt::NoFocus);
    connect(m_picker, &QComboBox::activated, this, [this](int index) {
        const QString key = m_picker->itemData(index).toString();
        if (key.isEmpty() || key == m_key)
            return;
        m_key = key;
        m_selected.reset();
        update();
        emit parameterChanged(m_trackId, m_key);
    });
    connect(ctx.project->timeline(), &Timeline::changed, this, [this] {
        refreshPicker();
        update();
    });
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, [this] { update(); });
    refreshPicker();
    layoutPicker();
}

const TrackData* AutomationLaneWidget::track() const
{
    return mixerops::audioTrack(m_ctx.project->timeline()->data(), m_trackId);
}

const AnimatedValue* AutomationLaneWidget::curve() const
{
    const TrackData* t = track();
    if (!t)
        return nullptr;
    const auto it = t->mix.automationKeys.values.constFind(m_key);
    return it != t->mix.automationKeys.values.cend() && it->isAnimated() ? &*it : nullptr;
}

void AutomationLaneWidget::setParameterKey(const QString& key)
{
    if (key == m_key)
        return;
    m_key = key;
    m_selected.reset();
    refreshPicker();
    update();
}

void AutomationLaneWidget::refreshPicker()
{
    const TrackData* t = track();
    if (!t)
        return;
    const TimelineData& d = m_ctx.project->timeline()->data();
    const QVector<mixerops::AutomationParam> params = mixerops::automationParams(d, *t);
    // Rebuild only when the list or the dots changed (a drag changes the timeline each move).
    QStringList labels;
    QStringList keys;
    bool known = false;
    for (const mixerops::AutomationParam& p : params) {
        const auto it = t->mix.automationKeys.values.constFind(p.key);
        const bool has = it != t->mix.automationKeys.values.cend() && it->isAnimated();
        labels.append((has ? QStringLiteral("• ") : QString()) + p.label);
        keys.append(p.key);
        known |= p.key == m_key;
    }
    if (!known) {
        m_key = mixkeys::kVolumeDb;
        m_selected.reset();
    }
    bool same = m_picker->count() == int(keys.size());
    for (int i = 0; same && i < keys.size(); ++i)
        same = m_picker->itemData(i).toString() == keys[i] && m_picker->itemText(i) == labels[i];
    if (!same) {
        const QSignalBlocker block(m_picker);
        m_picker->clear();
        for (int i = 0; i < keys.size(); ++i)
            m_picker->addItem(labels[i], keys[i]);
    }
    const QSignalBlocker block(m_picker);
    m_picker->setCurrentIndex(m_picker->findData(m_key));
}

void AutomationLaneWidget::layoutPicker()
{
    using namespace timelinemetrics;
    m_picker->setGeometry(8, 8 - m_yOffset, kHeaderWidth - 16, m_picker->sizeHint().height());
}

void AutomationLaneWidget::placeLane(const QRect& lane, int minTop)
{
    const QRect visible = lane.intersected(QRect(lane.left(), minTop, lane.width(), 1 << 20));
    if (visible.isEmpty()) {
        hide();
        return;
    }
    setGeometry(visible);
    m_yOffset = visible.top() - lane.top();
    layoutPicker();
    show();
    update();
}

double AutomationLaneWidget::paramMin() const
{
    const TrackData* t = track();
    const auto p = t ? mixerops::automationParam(m_ctx.project->timeline()->data(), *t, m_key)
                     : std::nullopt;
    return p ? p->min : 0.0;
}

double AutomationLaneWidget::paramMax() const
{
    const TrackData* t = track();
    const auto p = t ? mixerops::automationParam(m_ctx.project->timeline()->data(), *t, m_key)
                     : std::nullopt;
    return p ? p->max : 1.0;
}

std::pair<double, double> AutomationLaneWidget::displayRange() const
{
    double lo = paramMin();
    const double hi = paramMax();
    if (m_key == mixkeys::kVolumeDb || m_key.startsWith(mixkeys::kSendPrefix))
        lo = std::max(lo, kDbFloor);
    return {lo, hi > lo ? hi : lo + 1.0};
}

QRectF AutomationLaneWidget::canvas() const
{
    using namespace timelinemetrics;
    return QRectF(kHeaderWidth - x(), kPadTop, width() - (kHeaderWidth - x()),
        double(kLaneHeight) - kPadTop - kPadBottom);
}

QPointF AutomationLaneWidget::pointPos(double frame, double value) const
{
    const auto [lo, hi] = displayRange();
    const QRectF c = canvas();
    const double t = std::clamp((value - lo) / (hi - lo), 0.0, 1.0);
    return QPointF(m_layout.frameToX(frame) - x(), c.bottom() - t * c.height() - m_yOffset);
}

double AutomationLaneWidget::valueAtY(double y) const
{
    const auto [lo, hi] = displayRange();
    const QRectF c = canvas();
    const double logical = y + m_yOffset;
    // The bottom edge of a dB parameter is silence.
    if ((m_key == mixkeys::kVolumeDb || m_key.startsWith(mixkeys::kSendPrefix))
        && logical >= c.bottom() - 1.0)
        return paramMin();
    const double t = std::clamp((c.bottom() - logical) / c.height(), 0.0, 1.0);
    return lo + t * (hi - lo);
}

double AutomationLaneWidget::frameAtX(double xw) const
{
    return m_layout.xToFrame(xw + x());
}

std::optional<int64_t> AutomationLaneWidget::pointAt(QPointF pos) const
{
    const AnimatedValue* c = curve();
    if (!c)
        return std::nullopt;
    std::optional<int64_t> best;
    double bestDistance = kPointGrab;
    for (const Keyframe& k : c->keys) {
        const QPointF p = pointPos(double(k.frame), k.value);
        const double dist = std::hypot(p.x() - pos.x(), p.y() - pos.y());
        if (dist <= bestDistance) {
            bestDistance = dist;
            best = k.frame;
        }
    }
    return best;
}

int AutomationLaneWidget::segmentAt(QPointF pos) const
{
    const AnimatedValue* c = curve();
    if (!c || c->keys.size() < 2)
        return -1;
    const double f = frameAtX(pos.x());
    for (int i = 0; i + 1 < c->keys.size(); ++i) {
        if (f < double(c->keys[i].frame) || f > double(c->keys[i + 1].frame))
            continue;
        const QPointF on = pointPos(f, c->valueAtTime(f));
        return std::abs(on.y() - pos.y()) <= kLineGrab ? i : -1;
    }
    return -1;
}

void AutomationLaneWidget::selectPoint(std::optional<int64_t> frame)
{
    m_selected = frame;
    update();
}

bool AutomationLaneWidget::edit(
    const QString& label, const std::function<bool(TimelineData&)>& change)
{
    m_ctx.project->beginEdit(label);
    if (!change(m_ctx.project->timeline()->mutableData())) {
        m_ctx.project->cancelEdit();
        return false;
    }
    m_ctx.project->endEdit();
    return true;
}

void AutomationLaneWidget::liveChange(const std::function<bool(TimelineData&)>& change)
{
    if (change(m_ctx.project->timeline()->mutableData()))
        m_ctx.project->timeline()->notifyChanged();
}

bool AutomationLaneWidget::deleteSelected()
{
    if (!m_selected)
        return false;
    const int64_t frame = *m_selected;
    const bool done = edit(tr("Delete Automation Point"), [&](TimelineData& d) {
        return mixerops::removeAutomationPoint(d, m_trackId, m_key, frame);
    });
    if (done)
        m_selected.reset();
    return done;
}

bool AutomationLaneWidget::setSelectedInterp(Interp interp)
{
    if (!m_selected)
        return false;
    const int64_t frame = *m_selected;
    return edit(tr("Automation Curve"), [&](TimelineData& d) {
        return mixerops::setAutomationPointInterp(d, m_trackId, m_key, frame, interp);
    });
}

bool AutomationLaneWidget::clearCurve()
{
    const bool done = edit(tr("Clear Automation"), [&](TimelineData& d) {
        return mixerops::clearAutomation(d, m_trackId, m_key);
    });
    if (done)
        m_selected.reset();
    return done;
}

void AutomationLaneWidget::addPointAt(QPointF pos)
{
    const int64_t frame = std::max<int64_t>(0, int64_t(std::llround(frameAtX(pos.x()))));
    const double value = valueAtY(pos.y());
    m_ctx.project->beginEdit(tr("Add Automation Point"));
    liveChange([&](TimelineData& d) {
        return mixerops::addAutomationPoint(d, m_trackId, m_key, frame, value);
    });
    m_selected = frame;
    m_dragFrame = frame;
    m_drag = Drag::Point;
    update();
}

void AutomationLaneWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || event->position().x() < canvas().left()) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus();
    const QPointF pos = event->position();
    m_pressPos = pos;
    if (const auto frame = pointAt(pos)) {
        m_selected = *frame;
        m_dragFrame = *frame;
        m_drag = Drag::Point;
        m_ctx.project->beginEdit(tr("Move Automation Point"));
        update();
        return;
    }
    const int seg = segmentAt(pos);
    if (seg >= 0) {
        const AnimatedValue* c = curve();
        m_segA = c->keys[seg].frame;
        m_segB = c->keys[seg + 1].frame;
        m_segValueA = c->keys[seg].value;
        m_segValueB = c->keys[seg + 1].value;
        m_drag = Drag::PendingSegment;
        return;
    }
    addPointAt(pos);
}

void AutomationLaneWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    if (m_drag == Drag::PendingSegment
        && std::hypot(pos.x() - m_pressPos.x(), pos.y() - m_pressPos.y()) >= kDragStart) {
        m_drag = Drag::Segment;
        m_ctx.project->beginEdit(tr("Move Automation Segment"));
    }
    if (m_drag == Drag::Point) {
        const AnimatedValue* c = curve();
        if (!c)
            return;
        // A point stays between its neighbours (it never jumps over one).
        int64_t lo = 0, hi = std::numeric_limits<int64_t>::max();
        for (const Keyframe& k : c->keys) {
            if (k.frame < m_dragFrame)
                lo = k.frame + 1;
            else if (k.frame > m_dragFrame) {
                hi = k.frame - 1;
                break;
            }
        }
        const int64_t frame
            = std::clamp<int64_t>(int64_t(std::llround(frameAtX(pos.x()))), lo, hi);
        const double value = valueAtY(pos.y());
        const int64_t from = m_dragFrame;
        liveChange([&](TimelineData& d) {
            return mixerops::moveAutomationPoint(d, m_trackId, m_key, from, frame, value);
        });
        m_dragFrame = frame;
        m_selected = frame;
        update();
    } else if (m_drag == Drag::Segment) {
        const double delta = valueAtY(pos.y()) - valueAtY(m_pressPos.y());
        liveChange([&](TimelineData& d) {
            const double lo = paramMin(), hi = paramMax();
            const bool a = mixerops::moveAutomationPoint(
                d, m_trackId, m_key, m_segA, m_segA, std::clamp(m_segValueA + delta, lo, hi));
            const bool b = mixerops::moveAutomationPoint(
                d, m_trackId, m_key, m_segB, m_segB, std::clamp(m_segValueB + delta, lo, hi));
            return a || b;
        });
        update();
    }
}

void AutomationLaneWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const Drag was = m_drag;
    m_drag = Drag::None;
    if (was == Drag::Point || was == Drag::Segment) {
        m_ctx.project->endEdit();
    } else if (was == Drag::PendingSegment) {
        // A click on a line adds a point there.
        addPointAt(m_pressPos);
        m_drag = Drag::None;
        m_ctx.project->endEdit();
    }
    update();
}

bool AutomationLaneWidget::event(QEvent* event)
{
    // With a point selected, bare Delete / Backspace delete it instead of the clip commands
    // bound to the same keys (like the keyframe views).
    if (event->type() == QEvent::ShortcutOverride && m_selected) {
        auto* ke = static_cast<QKeyEvent*>(event);
        const bool bare = (ke->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
        if (bare && (ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace)) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void AutomationLaneWidget::keyPressEvent(QKeyEvent* event)
{
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && deleteSelected()) {
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void AutomationLaneWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (event->pos().x() < canvas().left())
        return;
    QMenu menu(this);
    if (const auto frame = pointAt(QPointF(event->pos()))) {
        selectPoint(*frame);
        struct Shape
        {
            Interp interp;
            QString label;
        };
        const Shape shapes[] = {{Interp::Linear, tr("Linear")}, {Interp::Smooth, tr("Smooth")},
            {Interp::EaseIn, tr("Ease In")}, {Interp::EaseOut, tr("Ease Out")},
            {Interp::EaseInOut, tr("Ease In and Out")}, {Interp::Hold, tr("Hold (step)")}};
        for (const Shape& s : shapes) {
            menu.addAction(s.label, this, [this, interp = s.interp] { setSelectedInterp(interp); });
        }
        menu.addSeparator();
        menu.addAction(tr("Delete Point"), this, [this] { deleteSelected(); });
    } else if (curve()) {
        menu.addAction(tr("Clear Curve"), this, [this] { clearCurve(); });
    } else {
        return;
    }
    menu.exec(event->globalPos());
}

void AutomationLaneWidget::paintEvent(QPaintEvent*)
{
    using namespace timelinemetrics;
    QPainter p(this);
    p.translate(0, -m_yOffset);
    const QPalette& pal = palette();
    const QRect all(0, 0, width(), kLaneHeight);
    p.fillRect(all, pal.window().color().darker(112));
    const QRectF c = canvas();
    const TrackData* t = track();
    if (!t)
        return;
    const bool off = t->mix.automation == AutomationMode::Off;
    const QColor line = off ? pal.mid().color() : pal.highlight().color();
    const QColor grid = pal.mid().color().darker(130);
    p.setPen(grid);
    p.drawLine(QPointF(0, kLaneHeight - 0.5), QPointF(width(), kLaneHeight - 0.5));
    p.drawLine(QPointF(c.left(), c.top()), QPointF(c.right(), c.top()));
    p.drawLine(QPointF(c.left(), c.bottom()), QPointF(c.right(), c.bottom()));
    const auto [lo, hi] = displayRange();
    p.setPen(pal.windowText().color().darker(150));
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.85));
    p.setFont(small);
    const auto param = mixerops::automationParam(m_ctx.project->timeline()->data(), *t, m_key);
    const QString unit = param ? param->unit : QString();
    p.drawText(QRectF(c.left() + 4, c.top() - 1, 90, 12), Qt::AlignLeft | Qt::AlignTop,
        QString::number(hi, 'g', 4) + unit);
    p.drawText(QRectF(c.left() + 4, c.bottom() - 12, 90, 12), Qt::AlignLeft | Qt::AlignBottom,
        QString::number(lo, 'g', 4) + unit);
    if (off) {
        p.drawText(QRectF(c.left(), c.top(), c.width() - 6, 14), Qt::AlignRight | Qt::AlignTop,
            tr("Automation is off: the curves are ignored"));
    }

    // The curve, sampled across the visible width (hold keys and shaped segments included).
    p.setClipRect(QRectF(c.left(), 0, c.width(), kLaneHeight));
    p.setRenderHint(QPainter::Antialiasing, true);
    const AnimatedValue* cv = curve();
    if (cv) {
        QPainterPath path;
        for (double px = c.left(); px <= c.right() + 2; px += 2.0) {
            const double f = std::max(0.0, frameAtX(px));
            const QPointF pt = pointPos(f, cv->valueAtTime(f));
            if (px == c.left())
                path.moveTo(pt.x(), pt.y() + m_yOffset);
            else
                path.lineTo(pt.x(), pt.y() + m_yOffset);
        }
        p.setPen(QPen(line, 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        for (const Keyframe& k : cv->keys) {
            const QPointF pt = pointPos(double(k.frame), k.value);
            if (pt.x() < c.left() - 8 || pt.x() > c.right() + 8)
                continue;
            const bool sel = m_selected && *m_selected == k.frame;
            p.setPen(QPen(line.darker(150), 1.2));
            p.setBrush(sel ? pal.highlightedText().color() : line);
            p.drawEllipse(QPointF(pt.x(), pt.y() + m_yOffset), kPointRadius, kPointRadius);
        }
    } else {
        // No curve: the static value, dashed.
        const double v = mixerops::staticAutomationValue(*t, m_key);
        const QPointF a = pointPos(0.0, v);
        p.setPen(QPen(pal.mid().color(), 1.2, Qt::DashLine));
        p.drawLine(QPointF(c.left(), a.y() + m_yOffset), QPointF(c.right(), a.y() + m_yOffset));
    }
    // The playhead.
    const double px = m_layout.frameToX(double(m_ctx.project->timeline()->playhead())) - x();
    if (px >= c.left() && px <= c.right()) {
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(pal.windowText().color(), 1));
        p.drawLine(QPointF(px, 0), QPointF(px, kLaneHeight));
    }
}

// ---- AutomationLanes ----

AutomationLanes::AutomationLanes(
    AppContext& ctx, const TimelineLayout& layout, QWidget* viewport, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
    , m_layout(layout)
    , m_viewport(viewport)
{ }

AutomationLanes::~AutomationLanes() = default;

void AutomationLanes::setOpen(quint64 trackId, bool open)
{
    if (open == m_open.contains(trackId))
        return;
    if (open) {
        const TimelineData& d = m_ctx.project->timeline()->data();
        if (!mixerops::audioTrack(d, trackId))
            return;
        m_open.insert(trackId);
    } else {
        m_open.remove(trackId);
        if (AutomationLaneWidget* w = m_widgets.take(trackId))
            w->deleteLater();
    }
    emit layoutChanged();
}

QString AutomationLanes::parameter(quint64 trackId) const
{
    return m_parameter.value(trackId, mixkeys::kVolumeDb);
}

void AutomationLanes::setParameter(quint64 trackId, const QString& key)
{
    m_parameter.insert(trackId, key);
    if (AutomationLaneWidget* w = widget(trackId))
        w->setParameterKey(key);
}

QHash<quint64, int> AutomationLanes::laneHeights(const TimelineData& data)
{
    QHash<quint64, int> heights;
    const QSet<quint64> open = m_open;
    for (quint64 id : open) {
        if (mixerops::audioTrack(data, id))
            heights.insert(id, AutomationLaneWidget::kLaneHeight);
        else {
            m_open.remove(id);
            m_parameter.remove(id);
            if (AutomationLaneWidget* w = m_widgets.take(id))
                w->deleteLater();
        }
    }
    return heights;
}

AutomationLaneWidget* AutomationLanes::widget(quint64 trackId) const
{
    return m_widgets.value(trackId).data();
}

void AutomationLanes::syncGeometry()
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    for (quint64 id : std::as_const(m_open)) {
        int index = -1;
        for (int i = 0; i < int(data.audioTracks.size()); ++i) {
            if (data.audioTracks[size_t(i)].id == id)
                index = i;
        }
        const int row = index >= 0 ? m_layout.rowOf(TrackRef {TrackType::Audio, index}) : -1;
        const QRect lane = row >= 0 ? m_layout.automationLaneRect(row) : QRect();
        AutomationLaneWidget* w = m_widgets.value(id).data();
        if (lane.isEmpty()) {
            if (w)
                w->hide();
            continue;
        }
        if (!w) {
            w = new AutomationLaneWidget(m_ctx, m_layout, id, m_viewport);
            w->setParameterKey(parameter(id));
            connect(w, &AutomationLaneWidget::parameterChanged, this,
                [this](quint64 trackId, const QString& key) { m_parameter.insert(trackId, key); });
            m_widgets.insert(id, w);
        }
        w->placeLane(lane, timelinemetrics::kRulerHeight);
    }
}

void AutomationLanes::reset()
{
    if (m_open.isEmpty())
        return;
    for (auto it = m_widgets.begin(); it != m_widgets.end(); ++it) {
        if (it.value())
            it.value()->deleteLater();
    }
    m_widgets.clear();
    m_open.clear();
    m_parameter.clear();
    emit layoutChanged();
}

} // namespace kk
