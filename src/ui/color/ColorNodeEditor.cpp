// SPDX-License-Identifier: GPL-3.0-or-later
// ColorNodeEditor — painting, hit tests and gestures of the Color page node graph.
#include "ui/color/ColorNodeEditor.h"

#include "ui/common/StyleTokens.h"

#include "ui/color/ColorNodeOps.h"
#include "ui/color/ColorSession.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/UiFonts.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr double kDragStartPx = 4.0;
constexpr double kEndGap = 70.0; ///< editor units between the nodes and the source / output ends
constexpr double kLabelHeight = 16.0;
constexpr double kSmallSize = 40.0; ///< side of a mixer, Splitter, Combiner, Alpha Output tile
constexpr double kKeyPortFraction = 0.86; ///< height of a corrector's key ports on its tile

const QColor kPictureColour(80, 190, 110);
const QColor kKeyColour(80, 140, 235);
const QColor kLinkColour(120, 200, 140);
const QColor kKeyLinkColour(110, 160, 240);

/// Red, green and blue of the Splitter outputs and Combiner inputs.
QColor channelColour(int channel)
{
    static const QColor colours[3] = {QColor(235, 90, 80), QColor(90, 200, 100), QColor(90, 140, 240)};
    return colours[std::clamp(channel, 0, 2)];
}

double distance(QPointF a, QPointF b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

QAction* registryAction(ActionRegistry* actions, const char* id)
{
    const QString key = QLatin1String(id);
    return actions && actions->contains(key) ? actions->action(key) : nullptr;
}

} // namespace

ColorNodeEditor::ColorNodeEditor(ColorSession* session, ActionRegistry* actions, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_actions(actions)
{
    setObjectName(QStringLiteral("ColorNodeEditor"));
    new ColorNodeOps(this, session, actions); // the keys of Vim mode (spec 83)
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(160, 120);
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &ColorNodeEditor::onGradeChanged);
        connect(m_session, &ColorSession::clipChanged, this, &ColorNodeEditor::onGradeChanged);
        connect(m_session, &ColorSession::selectedNodeChanged, this, qOverload<>(&QWidget::update));
        connect(m_session, &ColorSession::selectionChanged, this, qOverload<>(&QWidget::update));
        connect(m_session, &ColorSession::openCompoundChanged, this, [this] {
            m_viewSet = false; // the other level shows its own nodes
            onGradeChanged();
            fitToView();
        });
    }
}

void ColorNodeEditor::onGradeChanged()
{
    // Another clip (or target): show its whole graph again unless the user moved the view.
    // Nodes added or removed: fit again (unless the user set the view); a moved node keeps it.
    const quint64 clip = m_session ? m_session->clipId() : 0;
    QVector<quint64> ids;
    if (m_session) {
        for (const GradeNode& n : m_session->grade().nodes) {
            if (isVisible(n))
                ids.append(n.id);
        }
    }
    if (clip != m_lastClip) {
        m_lastClip = clip;
        m_viewSet = false;
    }
    if (ids != m_lastIds) {
        m_lastIds = ids;
        if (!m_viewSet)
            fitToView();
    }
    update();
}

void ColorNodeEditor::setThumbnails(const QHash<quint64, QImage>& thumbnails)
{
    m_thumbs = thumbnails;
    update();
}

void ColorNodeEditor::setView(QPointF origin, double zoom)
{
    m_origin = origin;
    m_zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    update();
}

bool ColorNodeEditor::isVisible(const GradeNode& node) const
{
    return node.compound == (m_session ? m_session->openCompoundId() : 0);
}

const GradeNode* ColorNodeEditor::visibleNode(quint64 id) const
{
    const GradeNode* n = m_session && id ? m_session->grade().node(id) : nullptr;
    return n && isVisible(*n) ? n : nullptr;
}

quint64 ColorNodeEditor::levelOutput() const
{
    if (!m_session)
        return 0;
    const quint64 level = m_session->openCompoundId();
    if (level == 0)
        return m_session->grade().output;
    const GradeNode* c = m_session->grade().node(level);
    return c ? c->innerOutput : 0;
}

QSizeF ColorNodeEditor::tileSize(const GradeNode& node)
{
    switch (node.type) {
    case GradeNodeType::Corrector:
    case GradeNodeType::Compound:
        return kTileSize;
    case GradeNodeType::KeyMixer:
        return QSizeF(kSmallSize, std::max(kSmallSize, 14.0 * (node.inputs.size() + 2)));
    default:
        return QSizeF(kSmallSize, kSmallSize);
    }
}

QString ColorNodeEditor::typeName(const GradeNode& node)
{
    switch (node.type) {
    case GradeNodeType::Parallel:
        return tr("Parallel");
    case GradeNodeType::Layer:
        return tr("Layer");
    case GradeNodeType::KeyMixer:
        return tr("Key Mixer");
    case GradeNodeType::Splitter:
        return tr("Splitter");
    case GradeNodeType::Combiner:
        return tr("Combiner");
    case GradeNodeType::Compound:
        return tr("Compound");
    case GradeNodeType::AlphaOutput:
        return tr("Alpha Output");
    case GradeNodeType::Corrector:
        break;
    }
    return {};
}

QRectF ColorNodeEditor::nodesBounds() const
{
    const ClipGrade* g = m_session ? &m_session->grade() : nullptr;
    QRectF r;
    if (g) {
        for (const GradeNode& n : g->nodes) {
            if (!isVisible(n))
                continue;
            const QRectF t(n.pos, tileSize(n));
            r = r.isNull() ? t : r.united(t);
        }
    }
    return r.isNull() ? QRectF(QPointF(0, 0), kTileSize) : r;
}

void ColorNodeEditor::fitToView()
{
    const QRectF b = nodesBounds().adjusted(-kEndGap - 44, -30, kEndGap + 44, 34);
    if (width() <= 0 || height() <= 0)
        return;
    m_zoom = std::clamp(
        std::min({width() / b.width(), height() / b.height(), 1.0}), kMinZoom, kMaxZoom);
    m_origin = b.center() - QPointF(width(), height()) / (2 * m_zoom);
    update();
}

void ColorNodeEditor::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!m_viewSet)
        fitToView();
}

QPointF ColorNodeEditor::toWidget(QPointF e) const
{
    return (e - m_origin) * m_zoom;
}

QPointF ColorNodeEditor::toEditor(QPointF w) const
{
    return w / m_zoom + m_origin;
}

QPointF ColorNodeEditor::livePos(const GradeNode& node) const
{
    if (m_drag == Drag::Move && (node.id == m_pressNode || m_moveIds.contains(node.id)))
        return node.pos + m_moveDelta;
    return node.pos;
}

QRectF ColorNodeEditor::tileRect(const GradeNode& node) const
{
    const QSizeF s = tileSize(node);
    return QRectF(toWidget(livePos(node)), s * m_zoom);
}

int ColorNodeEditor::inputPortCount(const GradeNode& node)
{
    switch (node.type) {
    case GradeNodeType::Corrector:
    case GradeNodeType::Compound:
    case GradeNodeType::Splitter:
        return 1;
    case GradeNodeType::Parallel:
    case GradeNodeType::Layer:
        return int(node.inputs.size()) + 1; // + the free port
    case GradeNodeType::Combiner:
        return 3;
    case GradeNodeType::KeyMixer:
    case GradeNodeType::AlphaOutput:
        return 0;
    }
    return 0;
}

int ColorNodeEditor::keyInputPortCount(const GradeNode& node)
{
    switch (node.type) {
    case GradeNodeType::Corrector:
    case GradeNodeType::AlphaOutput:
        return 1;
    case GradeNodeType::KeyMixer:
        return int(node.inputs.size()) + 1; // + the free slot
    default:
        return 0;
    }
}

QPointF ColorNodeEditor::inputPos(const GradeNode& node, int index) const
{
    const QRectF r = tileRect(node);
    const int count = inputPortCount(node);
    if (node.type == GradeNodeType::Parallel || node.type == GradeNodeType::Layer
        || node.type == GradeNodeType::Combiner)
        return QPointF(r.left(), r.top() + r.height() * (index + 1) / (count + 1));
    return QPointF(r.left(), r.center().y());
}

QPointF ColorNodeEditor::outputPos(const GradeNode& node, int port) const
{
    const QRectF r = tileRect(node);
    if (node.type == GradeNodeType::Splitter)
        return QPointF(r.right(), r.top() + r.height() * (port + 1) / 4.0);
    return QPointF(r.right(), r.center().y());
}

QPointF ColorNodeEditor::keyInputPos(const GradeNode& node, int index) const
{
    const QRectF r = tileRect(node);
    if (node.type == GradeNodeType::Corrector)
        return QPointF(r.left(), r.top() + r.height() * kKeyPortFraction);
    const int count = keyInputPortCount(node);
    return QPointF(r.left(), r.top() + r.height() * (index + 1) / (count + 1));
}

QPointF ColorNodeEditor::keyOutputPos(const GradeNode& node) const
{
    const QRectF r = tileRect(node);
    if (node.type == GradeNodeType::KeyMixer)
        return QPointF(r.right(), r.center().y());
    return QPointF(r.right(), r.top() + r.height() * kKeyPortFraction);
}

QPointF ColorNodeEditor::sourcePos() const
{
    const QRectF b = nodesBounds();
    return toWidget(QPointF(b.left() - kEndGap, b.center().y()));
}

QPointF ColorNodeEditor::gradeOutputPos() const
{
    const QRectF b = nodesBounds();
    return toWidget(QPointF(b.right() + kEndGap, b.center().y()));
}

QPointF ColorNodeEditor::linkStart(quint64 id, int port) const
{
    const GradeNode* from = visibleNode(id);
    return from ? outputPos(*from, port) : sourcePos();
}

ColorNodeEditor::Hit ColorNodeEditor::hitTest(QPointF pos) const
{
    Hit hit;
    if (!m_session)
        return hit;
    const double portPick = std::max(kPortRadius * m_zoom, 4.0) + 3.0;
    const ClipGrade& g = m_session->grade();
    // Ports first (they sit on tile edges): the nearest one wins, topmost (last painted) node
    // first on a tie.
    double bestDistance = portPick + 1.0;
    auto consider = [&](Hit::Kind kind, quint64 node, int index, QPointF at) {
        const double d = distance(pos, at);
        if (d <= portPick && d < bestDistance) {
            bestDistance = d;
            hit = {kind, node, index};
        }
    };
    for (int i = int(g.nodes.size()) - 1; i >= 0; --i) {
        const GradeNode& n = g.nodes[i];
        if (!isVisible(n))
            continue;
        for (int port = 0; port < n.outputPortCount(); ++port)
            consider(Hit::Kind::Output, n.id, port, outputPos(n, port));
        if (n.hasKeyOutput())
            consider(Hit::Kind::KeyOutput, n.id, 0, keyOutputPos(n));
        for (int k = 0; k < inputPortCount(n); ++k)
            consider(Hit::Kind::Input, n.id, k, inputPos(n, k));
        for (int k = 0; k < keyInputPortCount(n); ++k)
            consider(Hit::Kind::KeyInput, n.id, k, keyInputPos(n, k));
    }
    if (hit.kind != Hit::Kind::None)
        return hit;
    for (int i = int(g.nodes.size()) - 1; i >= 0; --i) {
        if (isVisible(g.nodes[i]) && tileRect(g.nodes[i]).contains(pos))
            return {Hit::Kind::Tile, g.nodes[i].id, 0};
    }
    if (distance(pos, sourcePos()) <= portPick + 4)
        return {Hit::Kind::Source, 0, 0};
    if (distance(pos, gradeOutputPos()) <= portPick + 4)
        return {Hit::Kind::GradeOutput, 0, 0};
    return hit;
}

// ---- Painting ----

void ColorNodeEditor::paintLink(
    QPainter& p, QPointF a, QPointF b, const QColor& colour, bool dashed) const
{
    const double dx = std::max(30.0 * m_zoom, std::abs(b.x() - a.x()) * 0.5);
    QPainterPath path(a);
    path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
    QPen pen(colour, std::max(1.2, 1.6 * m_zoom));
    if (dashed)
        pen.setStyle(Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void ColorNodeEditor::paintTile(
    QPainter& p, const ClipGrade& g, int index, const QSet<quint64>& reachedIds) const
{
    const GradeNode& n = g.nodes[index];
    const QRectF r = tileRect(n);
    const QPalette& pal = palette();
    // The node 01 of an ungraded clip is the current node.
    const bool selected = m_session
        && (m_session->selectedNodes().contains(n.id) || m_session->grade().isEmpty());
    bool reached = reachedIds.contains(n.id);
    if (n.type == GradeNodeType::Compound) // the picture reaches it through its inner output
        reached = reached || (n.enabled && reachedIds.contains(n.innerOutput))
            || (!n.enabled && reachedIds.contains(n.inputs.value(0, 0)));
    QColor body = pal.color(QPalette::Button);
    QColor text = pal.color(QPalette::ButtonText);
    if (!n.enabled || !reached) {
        body = body.darker(130);
        text.setAlphaF(0.5f);
    }
    double radius = 4.0 * m_zoom;
    QColor edge = pal.color(QPalette::Dark);
    double edgeWidth = 1.0;
    if (style::kinoklipActive()) {
        // Resolve's node: a dark tile, a hairline border, the accent when selected.
        const style::Colors kc = style::colors(pal);
        body = (!n.enabled || !reached) ? kc.nodeFill.darker(130) : kc.nodeFill;
        edge = kc.nodeBorder;
        radius = style::kNodeRadius * m_zoom;
        edgeWidth = style::kNodeLine;
        if (selected) {
            edge = kc.mark;
            edgeWidth = style::kNodeLineSelected;
        }
    }
    p.setPen(QPen(selected && !style::kinoklipActive() ? pal.color(QPalette::Highlight) : edge,
        selected && !style::kinoklipActive() ? 2.0 : edgeWidth));
    p.setBrush(body);
    p.drawRoundedRect(r, radius, radius);

    const QFont f = uifonts::font(uifonts::Role::Tiny, font(), std::min(1.0, m_zoom));
    p.setFont(f);
    const bool large = n.type == GradeNodeType::Corrector || n.type == GradeNodeType::Compound;
    if (!large) {
        // A small node: its kind as a letter in the middle, its name under it.
        p.setPen(text);
        const QFont big = uifonts::font(uifonts::Role::Emphasis, f, std::min(1.0, m_zoom));
        p.setFont(big);
        QString letter;
        switch (n.type) {
        case GradeNodeType::Parallel:
            letter = tr("P", "Parallel mixer");
            break;
        case GradeNodeType::Layer:
            letter = tr("L", "Layer mixer");
            break;
        case GradeNodeType::KeyMixer:
            letter = tr("K", "Key Mixer");
            break;
        case GradeNodeType::Splitter:
            letter = tr("S", "Splitter");
            break;
        case GradeNodeType::Combiner:
            letter = tr("C", "Combiner");
            break;
        default:
            letter = tr("A", "Alpha Output");
            break;
        }
        p.drawText(r, Qt::AlignCenter, letter);
        p.setFont(f);
        const QString name = n.label.isEmpty() ? typeName(n) : n.label;
        p.drawText(QRectF(r.left() - 20 * m_zoom, r.bottom() + 1, r.width() + 40 * m_zoom,
                       kLabelHeight * m_zoom),
            Qt::AlignHCenter | Qt::AlignTop, name);
        if (!n.enabled) {
            p.setPen(QPen(QColor(230, 90, 70), 1.5));
            p.drawLine(r.topRight(), r.bottomLeft());
        }
    } else {
        const QRectF label(
            r.left() + 4 * m_zoom, r.top(), r.width() - 8 * m_zoom, kLabelHeight * m_zoom);
        p.setPen(text);
        // The label, else "01" (correctors only); a Compound node is named by its type.
        QString name = gradeNodeName(g, index);
        if (name.isEmpty())
            name = typeName(n);
        p.drawText(label, Qt::AlignLeft | Qt::AlignVCenter, name);
        // Markers of what the node uses: qualifier, windows, LUT, key input, members.
        QStringList marks;
        if (n.type == GradeNodeType::Compound)
            marks << QString::number(gradeops::compoundMembers(g, n.id).size());
        if (n.keyInput != 0)
            marks << (n.keyInputInvert ? tr("Outside", "key input inverted marker")
                                       : tr("Key", "key input marker"));
        if (n.qualifier.enabled)
            marks << tr("Q", "qualifier marker");
        if (!n.windows.isEmpty())
            marks << tr("W", "window marker");
        if (!n.lutPath.isEmpty())
            marks << tr("LUT");
        if (!marks.isEmpty())
            p.drawText(label, Qt::AlignRight | Qt::AlignVCenter, marks.join(QLatin1Char(' ')));
        const QRectF thumb = QRectF(r.left() + 6 * m_zoom, r.top() + kLabelHeight * m_zoom,
            r.width() - 12 * m_zoom, r.height() - (kLabelHeight + 6) * m_zoom);
        const QImage img = m_thumbs.value(n.id);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(16, 16, 18));
        p.drawRect(thumb);
        if (!img.isNull()) {
            QSizeF s = QSizeF(img.size()).scaled(thumb.size(), Qt::KeepAspectRatio);
            const QRectF t(thumb.center() - QPointF(s.width(), s.height()) / 2, s);
            p.drawImage(t, img);
        }
        if (!n.enabled) {
            // Bypassed: a diagonal over the thumbnail.
            p.setPen(QPen(QColor(230, 90, 70), 1.5));
            p.drawLine(thumb.topRight(), thumb.bottomLeft());
        }
    }
    // Ports: picture (green), key (blue), channels of the Splitter / Combiner in their colours.
    const double pr = std::max(2.5, kPortRadius * m_zoom);
    p.setPen(QPen(pal.color(QPalette::Dark), 1));
    const int inputs = inputPortCount(n);
    for (int k = 0; k < inputs; ++k) {
        const bool spare = (n.type == GradeNodeType::Parallel || n.type == GradeNodeType::Layer)
            && k == int(n.inputs.size());
        QColor c = kPictureColour;
        if (spare)
            c = pal.color(QPalette::Mid);
        else if (n.type == GradeNodeType::Combiner)
            c = channelColour(k);
        p.setBrush(c);
        p.drawEllipse(inputPos(n, k), pr, pr);
    }
    for (int port = 0; port < n.outputPortCount(); ++port) {
        p.setBrush(n.type == GradeNodeType::Splitter ? channelColour(port) : kPictureColour);
        p.drawEllipse(outputPos(n, port), pr, pr);
    }
    for (int k = 0; k < keyInputPortCount(n); ++k) {
        const bool spare = n.type == GradeNodeType::KeyMixer && k == int(n.inputs.size());
        p.setBrush(spare ? pal.color(QPalette::Mid) : kKeyColour);
        p.drawEllipse(keyInputPos(n, k), pr, pr);
    }
    if (n.hasKeyOutput()) {
        p.setBrush(kKeyColour);
        p.drawEllipse(keyOutputPos(n), pr, pr);
    }
}

void ColorNodeEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base).darker(115));
    // Dot grid every 20 editor units.
    {
        const double step = 20.0 * m_zoom;
        if (step >= 6.0) {
            QColor dot = pal.color(QPalette::Text);
            dot.setAlphaF(0.12f);
            p.setPen(QPen(dot, 1.0));
            const QPointF o = toWidget(QPointF(0, 0));
            const double x0 = std::fmod(o.x(), step);
            const double y0 = std::fmod(o.y(), step);
            for (double x = x0; x < width(); x += step) {
                for (double y = y0; y < height(); y += step)
                    p.drawPoint(QPointF(x, y));
            }
        }
    }
    if (!m_session || !m_session->hasGrade()) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(rect(), Qt::AlignCenter, tr("No clip to grade"));
        return;
    }
    // An ungraded clip shows node 01, as in Resolve (the first edit creates it for real).
    ClipGrade firstNode;
    if (m_session->grade().isEmpty())
        gradeops::initialize(firstNode);
    const ClipGrade& g = m_session->grade().isEmpty() ? firstNode : m_session->grade();
    const quint64 level = m_session->openCompoundId();
    QSet<quint64> reachedIds;
    for (const int i : gradeops::evaluationOrderWithAlpha(g))
        reachedIds.insert(g.nodes[i].id);
    // Picture links (green).
    for (const GradeNode& n : g.nodes) {
        if (!isVisible(n) || !n.hasPictureInput())
            continue;
        for (int k = 0; k < n.inputs.size(); ++k) {
            if (m_drag == Drag::Reconnect && !m_pickKey && n.id == m_pickNode && k == m_pickIndex)
                continue;
            paintLink(p, linkStart(n.inputs[k], n.portOf(k)), inputPos(n, k), kLinkColour);
        }
    }
    // Key links (blue; dashed when inverted).
    for (const GradeNode& n : g.nodes) {
        if (!isVisible(n))
            continue;
        if (n.hasKeyInput() && n.keyInput != 0 && visibleNode(n.keyInput)
            && !(m_drag == Drag::Reconnect && m_pickKey && n.id == m_pickNode)) {
            paintLink(p, keyOutputPos(*visibleNode(n.keyInput)), keyInputPos(n, 0), kKeyLinkColour,
                n.keyInputInvert);
        }
        if (n.type != GradeNodeType::KeyMixer)
            continue;
        for (int k = 0; k < n.inputs.size(); ++k) {
            if (n.inputs[k] == 0 || !visibleNode(n.inputs[k])
                || (m_drag == Drag::Reconnect && m_pickKey && n.id == m_pickNode
                    && k == m_pickIndex))
                continue;
            paintLink(p, keyOutputPos(*visibleNode(n.inputs[k])), keyInputPos(n, k),
                kKeyLinkColour, n.keyInverted(k));
        }
    }
    const quint64 outId = levelOutput();
    if (!(m_drag == Drag::Reconnect && !m_pickKey && m_pickNode == 0)) {
        if (const GradeNode* out = visibleNode(outId))
            paintLink(p, outputPos(*out), gradeOutputPos(), kLinkColour);
        else if (level != 0)
            paintLink(p, sourcePos(), gradeOutputPos(), kLinkColour); // passes its input
    }
    // Source and output ends.
    const double pr = std::max(3.0, kPortRadius * m_zoom * 1.2);
    p.setPen(QPen(pal.color(QPalette::Dark), 1));
    p.setBrush(kPictureColour);
    p.drawEllipse(sourcePos(), pr, pr);
    p.drawEllipse(gradeOutputPos(), pr, pr);
    p.setPen(pal.color(QPalette::Text));
    const QFont f = uifonts::font(uifonts::Role::Tiny, font());
    p.setFont(f);
    p.drawText(QRectF(sourcePos() - QPointF(40, 26), QSizeF(80, 16)), Qt::AlignCenter,
        level ? tr("Input") : tr("Source"));
    p.drawText(
        QRectF(gradeOutputPos() - QPointF(40, 26), QSizeF(80, 16)), Qt::AlignCenter, tr("Output"));
    if (level != 0) {
        // Where we are: the open compound (double-click the background or Esc closes it).
        QString name = tr("Compound");
        if (const GradeNode* c = g.node(level); c && !c->label.isEmpty())
            name = c->label;
        p.drawText(QRectF(8, 6, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter,
            tr("Inside %1 - double-click the background to close").arg(name));
    }
    // Tiles.
    for (int i = 0; i < g.nodes.size(); ++i) {
        if (isVisible(g.nodes[i]))
            paintTile(p, g, i, reachedIds);
    }
    // Live link of a connect gesture.
    if (m_drag == Drag::Connect || m_drag == Drag::Reconnect) {
        quint64 src = m_fromNode;
        int port = m_fromPort;
        bool key = m_fromKey;
        if (m_drag == Drag::Reconnect) {
            key = m_pickKey;
            port = 0;
            if (m_pickNode == 0) {
                src = outId;
            } else if (const GradeNode* n = g.node(m_pickNode)) {
                if (m_pickKey)
                    src = n->type == GradeNodeType::KeyMixer ? n->inputs.value(m_pickIndex, 0)
                                                             : n->keyInput;
                else if (m_pickIndex < n->inputs.size()) {
                    src = n->inputs[m_pickIndex];
                    port = n->portOf(m_pickIndex);
                }
            }
        }
        const GradeNode* from = visibleNode(src);
        const QPointF start = from ? (key ? keyOutputPos(*from) : outputPos(*from, port))
                                   : sourcePos();
        paintLink(p, start, m_mousePos, pal.color(QPalette::Highlight));
    }
    if (m_drag == Drag::Marquee) {
        QColor fill = pal.color(QPalette::Highlight);
        fill.setAlphaF(0.15f);
        p.setBrush(fill);
        p.setPen(QPen(pal.color(QPalette::Highlight), 1));
        p.drawRect(QRectF(m_pressPos, m_mousePos).normalized());
    }
}

// ---- Gestures ----

void ColorNodeEditor::mousePressEvent(QMouseEvent* event)
{
    m_pressPos = m_mousePos = event->position();
    setFocus();
    if (event->button() == Qt::MiddleButton) {
        m_drag = Drag::Pan;
        return;
    }
    if (event->button() != Qt::LeftButton || !m_session)
        return;
    const Hit hit = hitTest(m_pressPos);
    const ClipGrade& g = m_session->grade();
    const bool toggle = event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
    m_fromKey = false;
    m_fromPort = 0;
    m_pickKey = false;
    switch (hit.kind) {
    case Hit::Kind::Tile:
        if (toggle) {
            m_session->toggleNodeSelection(hit.node);
            break;
        }
        // A tile of a selection of several drags them all.
        if (!m_session->selectedNodes().contains(hit.node))
            m_session->selectNode(hit.node);
        else if (m_session->selectedNode() != hit.node && m_session->selectedNodes().size() == 1)
            m_session->selectNode(hit.node);
        m_pressNode = hit.node;
        m_moveIds = m_session->selectedNodes();
        m_moveDelta = {};
        m_drag = Drag::Pending;
        break;
    case Hit::Kind::Output:
        m_fromNode = hit.node;
        m_fromPort = hit.index;
        m_drag = Drag::Connect;
        break;
    case Hit::Kind::KeyOutput:
        m_fromNode = hit.node;
        m_fromKey = true;
        m_drag = Drag::Connect;
        break;
    case Hit::Kind::Source:
        m_fromNode = m_session->levelSource();
        m_drag = Drag::Connect;
        break;
    case Hit::Kind::Input: {
        const GradeNode* n = g.node(hit.node);
        if (n && hit.index < n->inputs.size() && n->hasPictureInput()) {
            m_pickNode = hit.node;
            m_pickIndex = hit.index;
            m_drag = Drag::Reconnect;
        }
        break;
    }
    case Hit::Kind::KeyInput: {
        const GradeNode* n = g.node(hit.node);
        const bool linked = n
            && (n->type == GradeNodeType::KeyMixer ? hit.index < n->inputs.size()
                       && n->inputs[hit.index] != 0
                                                   : n->keyInput != 0);
        if (linked) {
            m_pickNode = hit.node;
            m_pickIndex = hit.index;
            m_pickKey = true;
            m_drag = Drag::Reconnect;
        }
        break;
    }
    case Hit::Kind::GradeOutput:
        if (levelOutput() != 0) {
            m_pickNode = 0;
            m_pickIndex = 0;
            m_drag = Drag::Reconnect;
        }
        break;
    case Hit::Kind::None:
        m_drag = Drag::Marquee;
        break;
    }
    update();
}

void ColorNodeEditor::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    const QPointF last = m_mousePos;
    m_mousePos = pos;
    switch (m_drag) {
    case Drag::Pan:
        m_origin -= (pos - last) / m_zoom;
        m_viewSet = true;
        update();
        break;
    case Drag::Pending:
        if (distance(pos, m_pressPos) >= kDragStartPx)
            m_drag = Drag::Move;
        [[fallthrough]];
    case Drag::Move:
        if (m_drag == Drag::Move) {
            m_moveDelta = (pos - m_pressPos) / m_zoom;
            update();
        }
        break;
    case Drag::Marquee:
    case Drag::Connect:
    case Drag::Reconnect:
        update();
        break;
    case Drag::None:
        break;
    }
}

void ColorNodeEditor::finishKeyConnect(QPointF pos)
{
    const Hit hit = hitTest(pos);
    ClipGrade g = m_session->grade();
    quint64 from = m_fromNode;
    bool invert = false;
    if (m_drag == Drag::Reconnect) {
        const GradeNode* n = g.node(m_pickNode);
        if (!n)
            return;
        if (n->type == GradeNodeType::KeyMixer) {
            from = n->inputs.value(m_pickIndex, 0);
            invert = n->keyInverted(m_pickIndex);
        } else {
            from = n->keyInput;
            invert = n->keyInputInvert;
        }
    }
    int index = -1;
    quint64 to = 0;
    if (hit.kind == Hit::Kind::KeyInput) {
        to = hit.node;
        index = hit.index;
    } else if (hit.kind == Hit::Kind::Tile) {
        const GradeNode* n = g.node(hit.node);
        if (n && n->type == GradeNodeType::KeyMixer) {
            to = hit.node;
            index = int(n->inputs.size());
        } else if (n && n->hasKeyInput()) {
            to = hit.node;
            index = 0;
        }
    }
    bool changed = false;
    const bool sameSlot = m_drag == Drag::Reconnect && to == m_pickNode && index == m_pickIndex;
    if (to != 0 && from != 0 && !sameSlot && gradeops::connectKey(g, from, to, index, invert))
        changed = true;
    if (m_drag == Drag::Reconnect && !sameSlot) {
        // The picked-up end leaves its old input (dropped on nothing: the link is deleted).
        if (gradeops::disconnectKey(g, m_pickNode, m_pickIndex))
            changed = true;
    }
    if (changed && g != m_session->grade())
        m_session->commit(g, m_drag == Drag::Connect ? tr("Connect Keys") : tr("Reconnect Keys"));
}

void ColorNodeEditor::finishConnect(QPointF pos)
{
    if (!m_session)
        return;
    if (m_fromKey || (m_drag == Drag::Reconnect && m_pickKey)) {
        finishKeyConnect(pos);
        return;
    }
    const Hit hit = hitTest(pos);
    ClipGrade g = m_session->grade();
    const quint64 level = m_session->openCompoundId();
    bool changed = false;
    // Where the link should now start (the picked-up link keeps its source).
    quint64 from = m_fromNode;
    int port = m_fromPort;
    if (m_drag == Drag::Reconnect) {
        if (m_pickNode == 0)
            from = levelOutput();
        else if (const GradeNode* n = g.node(m_pickNode); n && m_pickIndex < n->inputs.size()) {
            from = n->inputs[m_pickIndex];
            port = n->portOf(m_pickIndex);
        }
    }
    auto connectTo = [&](quint64 to, int index) {
        if (gradeops::connect(g, from, to, index, port))
            changed = true;
    };
    // The output end: the grade output, or the open compound's inner output.
    auto setOutput = [&](quint64 node) {
        if (level == 0) {
            if (gradeops::connect(g, node, 0, 0))
                changed = true;
        } else if (GradeNode* c = g.node(level)) {
            const GradeNode* src = g.node(node);
            if (node == 0 || (src && src->compound == level && src->hasPictureOutput())) {
                if (c->innerOutput != node) {
                    c->innerOutput = node;
                    changed = true;
                }
            }
        }
    };
    if (m_drag == Drag::Reconnect && m_pickNode == 0) {
        // The output end picked up: drop it on a node (its output) to make that the output.
        if (hit.kind == Hit::Kind::Tile || hit.kind == Hit::Kind::Output)
            setOutput(hit.node);
        else if (hit.kind != Hit::Kind::GradeOutput)
            setOutput(0); // dropped into empty space: the grade is disconnected
    } else {
        switch (hit.kind) {
        case Hit::Kind::Input:
            if (!(m_drag == Drag::Reconnect && !m_pickKey && hit.node == m_pickNode
                    && hit.index == m_pickIndex))
                connectTo(hit.node, hit.index);
            break;
        case Hit::Kind::Tile: {
            const GradeNode* n = g.node(hit.node);
            if (n && n->hasPictureInput() && hit.node != from) {
                int index = 0;
                if (n->type == GradeNodeType::Parallel || n->type == GradeNodeType::Layer)
                    index = int(n->inputs.size());
                else if (n->type == GradeNodeType::Combiner) {
                    const int free = int(n->inputs.indexOf(0));
                    index = free < 0 ? 0 : free; // the first channel without a link
                }
                connectTo(hit.node, index);
            }
            break;
        }
        case Hit::Kind::GradeOutput:
            if (m_drag == Drag::Connect && (from || level != 0))
                setOutput(from == m_session->levelSource() && level != 0 ? 0 : from);
            break;
        default:
            break;
        }
        if (m_drag == Drag::Reconnect
            && !(hit.kind == Hit::Kind::Input && hit.node == m_pickNode
                && hit.index == m_pickIndex)) {
            // The picked-up end leaves its old input (a corrector's falls back to the source).
            if (const GradeNode* n = g.node(m_pickNode); n && m_pickIndex < n->inputs.size()) {
                if (gradeops::disconnect(g, m_pickNode, m_pickIndex))
                    changed = true;
            }
        }
    }
    if (changed && g != m_session->grade())
        m_session->commit(g, m_drag == Drag::Connect ? tr("Connect Nodes") : tr("Reconnect Nodes"));
}

void ColorNodeEditor::mouseReleaseEvent(QMouseEvent* event)
{
    const Drag drag = m_drag;
    if (drag == Drag::Pan && event->button() == Qt::MiddleButton) {
        m_drag = Drag::None;
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    if (drag == Drag::Move && m_session && !m_moveDelta.isNull()) {
        QVector<std::pair<quint64, QPointF>> moves;
        QVector<quint64> ids = m_moveIds;
        if (!ids.contains(m_pressNode))
            ids.append(m_pressNode);
        for (const quint64 id : ids) {
            if (const GradeNode* n = m_session->grade().node(id))
                moves.append({id, n->pos + m_moveDelta});
        }
        m_drag = Drag::None;
        m_session->moveNodes(moves);
    } else if (drag == Drag::Connect || drag == Drag::Reconnect) {
        finishConnect(event->position());
    } else if (drag == Drag::Marquee && m_session
        && distance(event->position(), m_pressPos) >= kDragStartPx) {
        // Rubber band: every shown tile it touches.
        const QRectF band = QRectF(m_pressPos, event->position()).normalized();
        QVector<quint64> ids;
        for (const GradeNode& n : m_session->grade().nodes) {
            if (isVisible(n) && tileRect(n).intersects(band))
                ids.append(n.id);
        }
        if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) {
            for (const quint64 id : m_session->selectedNodes()) {
                if (!ids.contains(id))
                    ids.append(id);
            }
        }
        if (!ids.isEmpty())
            m_session->selectNodes(ids);
    }
    m_drag = Drag::None;
    m_moveDelta = {};
    m_moveIds.clear();
    m_pressNode = 0;
    m_fromKey = false;
    m_pickKey = false;
    update();
}

void ColorNodeEditor::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (!m_session)
        return;
    const Hit hit = hitTest(event->position());
    if (hit.kind == Hit::Kind::Tile) {
        // A Compound node opens: the editor shows its members.
        if (const GradeNode* n = m_session->grade().node(hit.node);
            n && n->type == GradeNodeType::Compound)
            m_session->openCompound(hit.node);
        return;
    }
    if (hit.kind != Hit::Kind::None)
        return;
    // Double-click on the background: close the open compound, else show every node (Fit).
    if (m_session->openCompoundId() != 0) {
        m_session->closeCompound();
        return;
    }
    m_viewSet = false;
    fitToView();
}

void ColorNodeEditor::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && m_session && m_session->openCompoundId() != 0) {
        m_session->closeCompound();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ColorNodeEditor::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0)
        return;
    const QPointF anchor = event->position();
    const QPointF before = toEditor(anchor);
    m_zoom = std::clamp(m_zoom * std::pow(1.15, steps), kMinZoom, kMaxZoom);
    m_origin = before - anchor / m_zoom;
    m_viewSet = true;
    update();
    event->accept();
}

QMenu* ColorNodeEditor::buildContextMenu(QPointF pos, QWidget* parent)
{
    auto* menu = new QMenu(parent);
    const Hit hit = hitTest(pos);
    if (m_session
        && (hit.kind == Hit::Kind::Tile || hit.kind == Hit::Kind::Output
            || hit.kind == Hit::Kind::Input || hit.kind == Hit::Kind::KeyInput
            || hit.kind == Hit::Kind::KeyOutput)
        && !(hit.kind == Hit::Kind::Tile && m_session->selectedNodes().contains(hit.node)))
        m_session->selectNode(hit.node);
    const bool onNode = hit.node != 0;
    auto add = [&](const char* id, const QString& fallback, auto&& fn) {
        QAction* a = nullptr;
        if (QAction* reg = registryAction(m_actions, id)) {
            a = menu->addAction(reg->text());
            a->setShortcut(reg->shortcut());
            connect(a, &QAction::triggered, reg, &QAction::trigger);
        } else {
            a = menu->addAction(fallback);
            connect(a, &QAction::triggered, this, fn);
        }
        a->setObjectName(QLatin1String(id));
        return a;
    };
    QPointer<ColorSession> s = m_session;
    // The key link under the cursor: invert it, change its gain, remove it.
    if (s && hit.kind == Hit::Kind::KeyInput) {
        const GradeNode* n = s->grade().node(hit.node);
        const bool mixer = n && n->type == GradeNodeType::KeyMixer;
        const bool linked
            = n && (mixer ? hit.index < n->inputs.size() && n->inputs[hit.index] != 0
                          : n->keyInput != 0);
        if (linked) {
            const quint64 node = hit.node;
            const int index = hit.index;
            const bool inverted = mixer ? n->keyInverted(index) : n->keyInputInvert;
            QAction* invert = menu->addAction(tr("Invert Key"));
            invert->setObjectName(QStringLiteral("color.key_invert"));
            invert->setCheckable(true);
            invert->setChecked(inverted);
            connect(invert, &QAction::triggered, this, [s, node, index, inverted] {
                if (s)
                    s->setKeyInverted(node, index, !inverted);
            });
            if (mixer) {
                QAction* gain = menu->addAction(tr("Key Gain…"));
                gain->setObjectName(QStringLiteral("color.key_gain"));
                const double current = n->keyGain(index);
                connect(gain, &QAction::triggered, this, [this, s, node, index, current] {
                    bool ok = false;
                    const double v = QInputDialog::getDouble(this, tr("Key Gain"),
                        tr("Gain of this key:"), current, -4.0, 4.0, 2, &ok);
                    if (ok && s)
                        s->setKeyGain(node, index, v);
                });
            }
            QAction* cut = menu->addAction(tr("Disconnect Key"));
            cut->setObjectName(QStringLiteral("color.key_disconnect"));
            connect(cut, &QAction::triggered, this, [s, node, index] {
                if (s)
                    s->disconnectKeyInput(node, index);
            });
            menu->addSeparator();
        }
    }
    add("color.add_serial", tr("Add Serial Node"), [s] {
        if (s)
            s->addSerialNode();
    });
    add("color.add_serial_before", tr("Add Serial Node Before"), [s] {
        if (s)
            s->addSerialNodeBefore();
    });
    add("color.add_parallel", tr("Add Parallel Node"), [s] {
        if (s)
            s->addParallelNode();
    });
    add("color.add_layer", tr("Add Layer Node"), [s] {
        if (s)
            s->addLayerNode();
    });
    add("color.add_outside", tr("Add Outside Node"), [s] {
        if (s)
            s->addOutsideNode();
    });
    add("color.add_key_mixer", tr("Add Key Mixer"), [s] {
        if (s)
            s->addKeyMixer();
    });
    add("color.add_splitter_combiner", tr("Add Splitter / Combiner"), [s] {
        if (s)
            s->addSplitterCombiner();
    });
    add("color.add_alpha_output", tr("Add Alpha Output"), [s] {
        if (s)
            s->addAlphaOutput();
    });
    menu->addSeparator();
    const GradeNode* current = m_session ? m_session->currentNode() : nullptr;
    const bool isCompound = current && current->type == GradeNodeType::Compound;
    add("color.group_nodes", tr("Create Compound Node"), [s] {
        if (s)
            s->groupNodes();
    })->setEnabled(m_session && !m_session->grade().isEmpty() && !m_session->selectedNodes().isEmpty());
    add("color.ungroup_nodes", tr("Decompose Compound Node"), [s] {
        if (s)
            s->ungroupNode();
    })->setEnabled(isCompound);
    add("color.open_compound", tr("Open Compound Node"), [s] {
        if (s)
            s->openCompound();
    })->setEnabled(isCompound);
    add("color.close_compound", tr("Close Compound Node"), [s] {
        if (s)
            s->closeCompound();
    })->setEnabled(m_session && m_session->openCompoundId() != 0);
    menu->addSeparator();
    add("color.bypass_node", tr("Enable / Disable Node"), [s] {
        if (s)
            s->toggleNodeBypass();
    })->setEnabled(onNode);
    add("color.reset_node", tr("Reset Node Grade"), [s] {
        if (s)
            s->resetNode();
    })->setEnabled(onNode);
    add("color.delete_node", tr("Delete Node"), [s] {
        if (s)
            s->removeNode();
    })->setEnabled(onNode);
    add("color.keyframe", tr("Add / Remove Keyframe"), [s] {
        if (s)
            s->toggleKeyframe();
    })->setEnabled(m_session && m_session->hasGrade() && (!current || !current->isMixer()));
    menu->addSeparator();
    add("color.reset_grade", tr("Reset Clip Grade"), [s] {
        if (s)
            s->resetGrade();
    })->setEnabled(m_session && !m_session->grade().isEmpty());
    QAction* fit = menu->addAction(tr("Fit to View"));
    fit->setObjectName(QStringLiteral("color.fit_nodes"));
    connect(fit, &QAction::triggered, this, [this] {
        m_viewSet = false;
        fitToView();
    });
    return menu;
}

void ColorNodeEditor::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu* menu = buildContextMenu(event->pos(), this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(event->globalPos());
}

} // namespace kk
