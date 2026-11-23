// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/color/ColorKeyframes.h"

#include "ui/common/VimRowOps.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr int kLabelWidth = 104;
constexpr int kRulerHeight = 16;
constexpr int kRowHeight = 20;
constexpr int kKeyRadius = 5;
constexpr int kHitRadius = 7;
constexpr int kRightMargin = 10;

QToolButton* barButton(const QString& text, QWidget* parent, bool checkable)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setCheckable(checkable);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

/// Runs @p fn(row) for every row, as one undo step when there are several.
template<typename Fn>
bool forRows(ColorSession* session, const QVector<ColorSession::KeyRow>& rows, const QString& label, Fn&& fn)
{
    if (!session || rows.isEmpty())
        return false;
    const bool group = rows.size() > 1;
    if (group)
        session->beginGesture(label);
    bool any = false;
    for (const ColorSession::KeyRow& r : rows)
        any = fn(r) || any;
    if (group)
        session->endGesture();
    return any;
}

struct DissolveType
{
    Interp interp;
    const char* label;
};

const DissolveType* dissolveTypes()
{
    static const DissolveType types[] = {{Interp::Linear, QT_TRANSLATE_NOOP("kk::ColorKeyframesPanel", "Linear")},
        {Interp::EaseIn, QT_TRANSLATE_NOOP("kk::ColorKeyframesPanel", "Ease In")},
        {Interp::EaseOut, QT_TRANSLATE_NOOP("kk::ColorKeyframesPanel", "Ease Out")},
        {Interp::EaseInOut, QT_TRANSLATE_NOOP("kk::ColorKeyframesPanel", "Ease In and Out")},
        {Interp::Smooth, QT_TRANSLATE_NOOP("kk::ColorKeyframesPanel", "Smooth")}};
    return types;
}
constexpr int kDissolveCount = 5;

} // namespace

// ---- ColorKeyStrip ----

ColorKeyStrip::ColorKeyStrip(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("ColorKeyStrip"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &ColorKeyStrip::rebuild);
        connect(m_session, &ColorSession::sizingChanged, this, &ColorKeyStrip::rebuild);
        connect(m_session, &ColorSession::clipChanged, this, &ColorKeyStrip::rebuild);
        connect(m_session, &ColorSession::selectedNodeChanged, this, [this] { update(); });
    }
    rebuild();
}

void ColorKeyStrip::rebuild()
{
    m_rows = m_session ? m_session->keyRows() : QVector<ColorSession::KeyRow> {};
    if (!m_dragStarted
        && (m_selection.row >= rowCount()
        || (m_selection.row >= 0 && !frames(m_selection.row).contains(m_selection.frame)))) {
        m_selection = {};
        Q_EMIT selectionChanged();
    }
    updateGeometry();
    update();
}

QString ColorKeyStrip::rowLabel(int row) const
{
    if (row <= 0 || row > m_rows.size())
        return tr("All");
    const ColorSession::KeyRow& r = m_rows[row - 1];
    if (r.kind == ColorSession::KeyRow::Kind::Input)
        return tr("Input Sizing");
    if (r.kind == ColorSession::KeyRow::Kind::Output)
        return tr("Output Sizing");
    if (m_session) {
        const ClipGrade& g = m_session->grade();
        return gradeNodeName(g, g.indexOf(r.nodeId));
    }
    return {};
}

QVector<int64_t> ColorKeyStrip::frames(int row) const
{
    if (!m_session)
        return {};
    if (row > 0 && row <= m_rows.size())
        return m_session->rowFrames(m_rows[row - 1]);
    QVector<int64_t> all;
    for (const ColorSession::KeyRow& r : m_rows)
        all += m_session->rowFrames(r);
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    return all;
}

void ColorKeyStrip::select(int row, int64_t frame)
{
    const Selection next {row, frame};
    if (next == m_selection)
        return;
    m_selection = next;
    update();
    Q_EMIT selectionChanged();
}

void ColorKeyStrip::clearSelection()
{
    if (m_selection.row < 0)
        return;
    m_selection = {};
    update();
    Q_EMIT selectionChanged();
}

QVector<ColorSession::KeyRow> ColorKeyStrip::targets(const Selection& sel) const
{
    QVector<ColorSession::KeyRow> out;
    if (!m_session || sel.row < 0)
        return out;
    if (sel.row == 0) {
        for (const ColorSession::KeyRow& r : m_rows) {
            if (m_session->rowHasKey(r, sel.frame))
                out.append(r);
        }
    } else if (sel.row <= m_rows.size()) {
        out.append(m_rows[sel.row - 1]);
    }
    return out;
}

QRect ColorKeyStrip::lanes() const
{
    return QRect(kLabelWidth, 0, std::max(1, width() - kLabelWidth - kRightMargin), height());
}

QRect ColorKeyStrip::laneRect(int row) const
{
    const QRect l = lanes();
    return QRect(l.left(), kRulerHeight + row * kRowHeight, l.width(), kRowHeight);
}

double ColorKeyStrip::frameToX(int64_t frame) const
{
    int64_t first = 0, last = 0;
    if (!m_session || !m_session->stripRange(&first, &last))
        return kLabelWidth;
    const QRect l = lanes();
    const double span = double(std::max<int64_t>(1, last - first));
    return l.left() + kKeyRadius + double(frame - first) / span * (l.width() - 2 * kKeyRadius);
}

int64_t ColorKeyStrip::xToFrame(double x) const
{
    int64_t first = 0, last = 0;
    if (!m_session || !m_session->stripRange(&first, &last))
        return 0;
    const QRect l = lanes();
    const double span = double(std::max<int64_t>(1, last - first));
    const double t = (x - l.left() - kKeyRadius) / std::max(1.0, double(l.width() - 2 * kKeyRadius));
    return std::clamp<int64_t>(first + int64_t(std::llround(t * span)), first, last);
}

int ColorKeyStrip::rowAt(int y) const
{
    if (y < kRulerHeight)
        return -1;
    const int row = (y - kRulerHeight) / kRowHeight;
    return row < rowCount() ? row : -1;
}

std::optional<int64_t> ColorKeyStrip::keyAt(int row, const QPoint& pos) const
{
    std::optional<int64_t> best;
    double bestDistance = kHitRadius + 1;
    for (const int64_t f : frames(row)) {
        const double d = std::abs(frameToX(f) - pos.x());
        if (d < bestDistance) {
            bestDistance = d;
            best = f;
        }
    }
    return best;
}

QSize ColorKeyStrip::sizeHint() const
{
    return {420, kRulerHeight + rowCount() * kRowHeight + 4};
}

QSize ColorKeyStrip::minimumSizeHint() const
{
    return {200, kRulerHeight + rowCount() * kRowHeight + 4};
}

void ColorKeyStrip::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));
    int64_t first = 0, last = 0;
    const bool open = m_session && m_session->stripRange(&first, &last);
    if (!open) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(rect(), Qt::AlignCenter, tr("Nothing to key"));
        return;
    }
    const QColor line = pal.color(QPalette::Mid);
    const QColor text = pal.color(QPalette::Text);
    const QColor accent = pal.color(QPalette::Highlight);
    // Ruler: the first and last frame and a few ticks.
    p.setPen(text);
    const int ticks = 8;
    for (int i = 0; i <= ticks; ++i) {
        const int64_t f = first + (last - first) * i / ticks;
        const double x = frameToX(f);
        p.setPen(line);
        p.drawLine(QPointF(x, kRulerHeight - 4), QPointF(x, kRulerHeight));
        p.setPen(text);
        const QString label = QString::number(f);
        const QRectF box(x - 24, 0, 48, kRulerHeight - 4);
        if (i == 0)
            p.drawText(QRectF(x - 2, 0, 48, kRulerHeight - 4), Qt::AlignLeft | Qt::AlignVCenter, label);
        else if (i == ticks)
            p.drawText(QRectF(x - 46, 0, 48, kRulerHeight - 4), Qt::AlignRight | Qt::AlignVCenter, label);
        else
            p.drawText(box, Qt::AlignCenter, label);
    }
    const quint64 selectedNode = m_session->selectedNode();
    for (int row = 0; row < rowCount(); ++row) {
        const QRect lane = laneRect(row);
        const bool isMaster = row == 0;
        const bool nodeRow = !isMaster && m_rows[row - 1].kind == ColorSession::KeyRow::Kind::Node;
        const bool current = nodeRow && m_rows[row - 1].nodeId == selectedNode;
        QColor bg = pal.color(row % 2 ? QPalette::Base : QPalette::AlternateBase);
        p.fillRect(QRect(0, lane.top(), width(), lane.height()), bg);
        p.setPen(current ? accent : text);
        p.drawText(QRect(6, lane.top(), kLabelWidth - 10, lane.height()), Qt::AlignLeft | Qt::AlignVCenter,
            p.fontMetrics().elidedText(rowLabel(row), Qt::ElideRight, kLabelWidth - 12));
        p.setPen(line);
        p.drawLine(lane.left(), lane.center().y(), lane.right(), lane.center().y());
        for (const int64_t f : frames(row)) {
            const QPointF c(frameToX(f), lane.center().y());
            const bool selected = m_selection.row == row && m_selection.frame == f;
            bool isStatic = false;
            const QVector<ColorSession::KeyRow> rows = targets({row, f});
            if (!rows.isEmpty())
                isStatic = m_session->rowKeyMode(rows.first(), f) == keyrows::Mode::Static;
            p.setPen(QPen(selected ? pal.color(QPalette::BrightText) : text, 1.0));
            p.setBrush(selected ? accent : (isMaster ? text : pal.color(QPalette::Button)));
            if (isStatic) {
                p.drawRect(QRectF(c.x() - kKeyRadius + 1, c.y() - kKeyRadius + 1, 2 * kKeyRadius - 2, 2 * kKeyRadius - 2));
            } else {
                QPainterPath diamond;
                diamond.moveTo(c.x(), c.y() - kKeyRadius);
                diamond.lineTo(c.x() + kKeyRadius, c.y());
                diamond.lineTo(c.x(), c.y() + kKeyRadius);
                diamond.lineTo(c.x() - kKeyRadius, c.y());
                diamond.closeSubpath();
                p.drawPath(diamond);
            }
        }
    }
    // The playhead.
    const double px = frameToX(m_session->stripPlayhead());
    p.setPen(QPen(QColor(220, 60, 60), 1.0));
    p.drawLine(QPointF(px, kRulerHeight - 2), QPointF(px, height()));
}

void ColorKeyStrip::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    if (!m_session || e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    const QPoint pos = e->position().toPoint();
    const int row = rowAt(pos.y());
    if (pos.x() < kLabelWidth) {
        if (row > 0 && m_rows[row - 1].kind == ColorSession::KeyRow::Kind::Node)
            m_session->selectNode(m_rows[row - 1].nodeId);
        return;
    }
    if (row < 0) { // the ruler
        m_session->seekStrip(xToFrame(pos.x()));
        return;
    }
    if (const auto f = keyAt(row, pos)) {
        select(row, *f);
        m_dragging = true;
        m_dragStarted = false;
        m_dragFrame = *f;
        m_session->seekStrip(*f);
        return;
    }
    clearSelection();
    m_session->seekStrip(xToFrame(pos.x()));
}

void ColorKeyStrip::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_dragging || !m_session || m_selection.row < 0)
        return;
    const int64_t to = xToFrame(e->position().x());
    if (to == m_dragFrame)
        return;
    const QVector<ColorSession::KeyRow> rows = targets({m_selection.row, m_dragFrame});
    if (rows.isEmpty())
        return;
    if (!m_dragStarted) {
        m_dragStarted = true;
        m_session->beginGesture(tr("Move Keyframe"));
    }
    const int64_t from = m_dragFrame;
    for (const ColorSession::KeyRow& r : rows)
        m_session->moveRowKey(r, from, to);
    m_dragFrame = to;
    m_selection.frame = to;
    m_session->seekStrip(to);
    update();
}

void ColorKeyStrip::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_dragging && m_dragStarted && m_session)
        m_session->endGesture();
    m_dragging = false;
    m_dragStarted = false;
    QWidget::mouseReleaseEvent(e);
}

void ColorKeyStrip::mouseDoubleClickEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    const int row = rowAt(pos.y());
    if (!m_session || row < 0 || pos.x() < kLabelWidth || keyAt(row, pos))
        return;
    addKey(row, xToFrame(pos.x()));
}

void ColorKeyStrip::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        deleteSelected();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

void ColorKeyStrip::contextMenuEvent(QContextMenuEvent* e)
{
    const int row = rowAt(e->pos().y());
    if (row < 0 || e->pos().x() < kLabelWidth)
        return;
    if (const auto f = keyAt(row, e->pos()))
        select(row, *f);
    if (m_selection.row < 0)
        return;
    QMenu menu(this);
    menu.addAction(tr("Static"), this, [this] { setSelectedMode(keyrows::Mode::Static); });
    menu.addAction(tr("Dynamic"), this, [this] { setSelectedMode(keyrows::Mode::Dynamic); });
    menu.addSeparator();
    for (int i = 0; i < kDissolveCount; ++i) {
        const Interp interp = dissolveTypes()[i].interp;
        menu.addAction(QCoreApplication::translate("kk::ColorKeyframesPanel", dissolveTypes()[i].label),
            this, [this, interp] { setSelectedInterp(interp); });
    }
    menu.addSeparator();
    menu.addAction(tr("Delete Keyframe"), this, [this] { deleteSelected(); });
    menu.exec(e->globalPos());
}

bool ColorKeyStrip::deleteSelected()
{
    const Selection sel = m_selection;
    const bool ok = forRows(m_session, targets(sel), tr("Delete Keyframe"),
        [&](const ColorSession::KeyRow& r) { return m_session->deleteRowKey(r, sel.frame); });
    if (ok)
        clearSelection();
    return ok;
}

bool ColorKeyStrip::setSelectedMode(keyrows::Mode mode)
{
    const Selection sel = m_selection;
    return forRows(m_session, targets(sel),
        mode == keyrows::Mode::Static ? tr("Static Keyframe") : tr("Dynamic Keyframe"),
        [&](const ColorSession::KeyRow& r) { return m_session->setRowKeyMode(r, sel.frame, mode); });
}

bool ColorKeyStrip::setSelectedInterp(Interp interp)
{
    const Selection sel = m_selection;
    return forRows(m_session, targets(sel), tr("Keyframe Dissolve"),
        [&](const ColorSession::KeyRow& r) { return m_session->setRowKeyInterp(r, sel.frame, interp); });
}

bool ColorKeyStrip::addKey(int row, int64_t frame)
{
    if (!m_session)
        return false;
    QVector<ColorSession::KeyRow> rows;
    if (row <= 0)
        rows = m_rows;
    else if (row <= m_rows.size())
        rows.append(m_rows[row - 1]);
    const bool ok = forRows(m_session, rows, tr("Add Keyframe"),
        [&](const ColorSession::KeyRow& r) { return m_session->addRowKey(r, frame); });
    // The key sits in the model now (rebuild() ran): select it.
    if (ok || frames(std::max(0, row)).contains(frame))
        select(std::max(0, row), frame);
    return ok;
}

// ---- ColorKeyframesPanel ----

ColorKeyframesPanel::ColorKeyframesPanel(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    markVimForm(this);
    setObjectName(QStringLiteral("ColorKeyframesPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(3);

    auto* bar = new QHBoxLayout;
    m_auto = barButton(tr("Auto"), this, true);
    m_auto->setObjectName(QStringLiteral("KeyframesAuto"));
    m_auto->setToolTip(tr("Auto Keyframe: an edit of any grade or sizing value sets a keyframe at the "
                          "playhead"));
    m_prev = barButton(QStringLiteral("<"), this, false);
    m_prev->setObjectName(QStringLiteral("KeyframesPrevious"));
    m_prev->setToolTip(tr("Go to the previous keyframe"));
    m_add = barButton(tr("Add"), this, false);
    m_add->setObjectName(QStringLiteral("KeyframesAdd"));
    m_add->setToolTip(tr("Add a keyframe at the playhead to the selected row (All: every row)"));
    m_delete = barButton(tr("Delete"), this, false);
    m_delete->setObjectName(QStringLiteral("KeyframesDelete"));
    m_delete->setToolTip(tr("Delete the selected keyframe"));
    m_next = barButton(QStringLiteral(">"), this, false);
    m_next->setObjectName(QStringLiteral("KeyframesNext"));
    m_next->setToolTip(tr("Go to the next keyframe"));
    m_static = barButton(tr("Static"), this, true);
    m_static->setObjectName(QStringLiteral("KeyframesStatic"));
    m_static->setToolTip(tr("Static keyframe: the value is held until the next keyframe"));
    m_dynamic = barButton(tr("Dynamic"), this, true);
    m_dynamic->setObjectName(QStringLiteral("KeyframesDynamic"));
    m_dynamic->setToolTip(tr("Dynamic keyframe: the value changes smoothly to the next keyframe"));
    m_dissolve = new QComboBox(this);
    m_dissolve->setObjectName(QStringLiteral("KeyframesDissolve"));
    m_dissolve->setToolTip(tr("Dissolve: how a Dynamic keyframe changes to the next one"));
    for (int i = 0; i < kDissolveCount; ++i) {
        m_dissolve->addItem(
            QCoreApplication::translate("kk::ColorKeyframesPanel", dissolveTypes()[i].label),
            int(dissolveTypes()[i].interp));
    }
    for (QWidget* w : std::initializer_list<QWidget*> {m_auto, m_prev, m_add, m_delete, m_next})
        bar->addWidget(w);
    bar->addSpacing(12);
    for (QWidget* w : std::initializer_list<QWidget*> {m_static, m_dynamic, m_dissolve})
        bar->addWidget(w);
    bar->addStretch(1);
    layout->addLayout(bar);

    m_strip = new ColorKeyStrip(session, this);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("KeyframesScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_strip);
    layout->addWidget(scroll, 1);

    connect(m_auto, &QToolButton::toggled, this, [this](bool on) {
        if (!m_updating && m_session)
            m_session->setAutoKeyframe(on);
    });
    connect(m_add, &QToolButton::clicked, this, [this] { addKeys(); });
    connect(m_delete, &QToolButton::clicked, this, [this] { deleteKeys(); });
    connect(m_prev, &QToolButton::clicked, this, [this] { step(false); });
    connect(m_next, &QToolButton::clicked, this, [this] { step(true); });
    connect(m_static, &QToolButton::clicked, this, [this] { setMode(keyrows::Mode::Static); });
    connect(m_dynamic, &QToolButton::clicked, this, [this] { setMode(keyrows::Mode::Dynamic); });
    connect(m_dissolve, &QComboBox::activated, this, [this](int i) { setDissolve(i); });
    connect(m_strip, &ColorKeyStrip::selectionChanged, this, &ColorKeyframesPanel::updateControls);
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &ColorKeyframesPanel::updateControls);
        connect(m_session, &ColorSession::sizingChanged, this, &ColorKeyframesPanel::updateControls);
        connect(m_session, &ColorSession::autoKeyframeChanged, this, &ColorKeyframesPanel::updateControls);
    }
    updateControls();
}

void ColorKeyframesPanel::updateControls()
{
    m_updating = true;
    const ColorKeyStrip::Selection sel = m_strip->selection();
    const QVector<ColorSession::KeyRow> rows = m_strip->targets(sel);
    const bool has = !rows.isEmpty();
    m_auto->setChecked(m_session && m_session->autoKeyframe());
    m_delete->setEnabled(has);
    m_static->setEnabled(has);
    m_dynamic->setEnabled(has);
    m_dissolve->setEnabled(has);
    bool isStatic = false;
    if (has) {
        isStatic = m_session->rowKeyMode(rows.first(), sel.frame) == keyrows::Mode::Static;
        const Interp interp = m_session->rowKeyInterp(rows.first(), sel.frame);
        const int idx = m_dissolve->findData(int(interp));
        if (idx >= 0)
            m_dissolve->setCurrentIndex(idx);
    }
    m_static->setChecked(has && isStatic);
    m_dynamic->setChecked(has && !isStatic);
    m_updating = false;
}

void ColorKeyframesPanel::addKeys()
{
    if (!m_session)
        return;
    const int row = std::max(0, m_strip->selection().row);
    m_strip->addKey(row, m_session->stripPlayhead());
}

void ColorKeyframesPanel::deleteKeys()
{
    if (!m_session)
        return;
    if (m_strip->selection().row < 0)
        m_strip->select(0, m_session->stripPlayhead());
    m_strip->deleteSelected();
}

void ColorKeyframesPanel::step(bool forward)
{
    if (!m_session)
        return;
    const int row = std::max(0, m_strip->selection().row);
    const int64_t now = m_session->stripPlayhead();
    std::optional<int64_t> best;
    for (const int64_t f : m_strip->frames(row)) {
        if (forward && f > now && (!best || f < *best))
            best = f;
        if (!forward && f < now && (!best || f > *best))
            best = f;
    }
    if (best)
        m_session->seekStrip(*best);
}

void ColorKeyframesPanel::setMode(keyrows::Mode mode)
{
    if (m_updating)
        return;
    m_strip->setSelectedMode(mode);
    updateControls();
}

void ColorKeyframesPanel::setDissolve(int index)
{
    if (m_updating || index < 0)
        return;
    m_strip->setSelectedInterp(Interp(m_dissolve->itemData(index).toInt()));
    updateControls();
}

void ColorKeyframesPanel::resetAll()
{
    if (!m_session)
        return;
    // Every key of the selected row goes (All: of every row); the values stay as they are.
    const int row = std::max(0, m_strip->selection().row);
    const QVector<int64_t> frames = m_strip->frames(row);
    if (frames.isEmpty())
        return;
    m_session->beginGesture(tr("Reset Keyframes"));
    for (const int64_t f : frames) {
        m_strip->select(row, f);
        m_strip->deleteSelected();
    }
    m_session->endGesture();
}

} // namespace kk
