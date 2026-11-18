// SPDX-License-Identifier: GPL-3.0-or-later
// CompViewer implementation: header, surface, guides, node drops and the picture source.
#include "ui/comp/CompViewer.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/CompScene3D.h"
#include "render/Compositor.h"
#include "core/NodeRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelInfo.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/CompViewOptions.h"
#include "ui/comp/ViewerCache.h"
#include "ui/comp/ViewerRegion.h"
#include "ui/common/SchemeColors.h"
#include "ui/comp/CompViewerOverlay.h"
#include "ui/viewer/GuideOverlay.h"
#include "ui/viewer/ViewerOverlaySettings.h"
#include "ui/viewer/ViewerRuler.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPolygon>
#include <QRegion>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <functional>
#include <future>
#include <memory>
#include <tuple>
#include <numbers>

namespace kk {

/// The draggable line of the split display: drag near the centre to move it, elsewhere on the
/// line to turn it (Shift snaps to 45 degrees). Only a thin strip around the line takes the
/// mouse, the rest of the picture stays free for the on-viewer controls.
class SplitHandle : public QWidget
{
public:
    std::function<void(QPointF, double)> changed;

    SplitHandle(ViewerSurface* surface, QWidget* parent)
        : QWidget(parent)
        , m_surface(surface)
    {
        setObjectName(QStringLiteral("CompViewerSplit"));
        setAttribute(Qt::WA_NoSystemBackground);
        setCursor(Qt::SizeAllCursor);
        parent->installEventFilter(this);
        setGeometry(parent->rect());
        hide();
    }
    void setLine(QPointF pos, double angle)
    {
        m_pos = pos;
        m_angle = angle;
        updateMask();
        update();
    }
    void updateMask()
    {
        const QPointF c = centre();
        const QPointF d = dir();
        const double len = width() + height();
        QPolygonF poly;
        const QPointF n(-d.y() * 6, d.x() * 6);
        poly << c - d * len + n << c + d * len + n << c + d * len - n << c - d * len - n;
        setMask(QRegion(poly.toPolygon()));
    }
    [[nodiscard]] QPointF centre() const
    {
        const QRect r = m_surface->pictureRect();
        return {r.x() + m_pos.x() * r.width(), r.y() + m_pos.y() * r.height()};
    }
    [[nodiscard]] QPointF dir() const
    {
        const double a = m_angle * std::numbers::pi / 180.0;
        return {std::cos(a), -std::sin(a)};
    }

protected:
    bool eventFilter(QObject* w, QEvent* e) override
    {
        if (w == parent() && e->type() == QEvent::Resize) {
            setGeometry(static_cast<QWidget*>(w)->rect());
            updateMask();
        }
        return QWidget::eventFilter(w, e);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c = centre(), d = dir();
        const double len = width() + height();
        p.setPen(QPen(QColor(0, 0, 0, 160), 3));
        p.drawLine(c - d * len, c + d * len);
        p.setPen(QPen(Qt::white, 1));
        p.drawLine(c - d * len, c + d * len);
        p.setBrush(Qt::white);
        p.drawEllipse(c, 5.0, 5.0);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const QPointF c = centre();
        m_move = std::hypot(e->position().x() - c.x(), e->position().y() - c.y()) < 16;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const QRect r = m_surface->pictureRect();
        if (r.isEmpty() || !(e->buttons() & Qt::LeftButton))
            return;
        if (m_move) {
            m_pos = {std::clamp((e->position().x() - r.x()) / r.width(), 0.0, 1.0),
                std::clamp((e->position().y() - r.y()) / r.height(), 0.0, 1.0)};
        } else {
            const QPointF c = centre();
            double a = std::atan2(-(e->position().y() - c.y()), e->position().x() - c.x())
                * 180.0 / std::numbers::pi;
            if (e->modifiers() & Qt::ShiftModifier)
                a = std::round(a / 45.0) * 45.0;
            m_angle = a < 0 ? a + 180.0 : (a >= 180.0 ? a - 180.0 : a);
        }
        updateMask();
        update();
        if (changed)
            changed(m_pos, m_angle);
    }

private:
    ViewerSurface* m_surface;
    QPointF m_pos {0.5, 0.5};
    double m_angle = 90.0;
    bool m_move = false;
};

namespace {
/// Zoom choices of the header: label, surface zoom (0 = fit).
const std::pair<const char*, double> kZooms[] = {
    {QT_TRANSLATE_NOOP("kk::CompViewer", "Fit"), 0.0},
    {"50%", 0.5},
    {"100%", 1.0},
    {"200%", 2.0},
};
} // namespace

CompViewer::CompViewer(int index, AppContext& ctx, CompSession* session, QWidget* parent)
    : PanelFrame(QString(), parent)
    , m_ctx(ctx)
    , m_session(session)
    , m_index(index)
{
    setPanelInfo(this, index == 0 ? tr("Left Viewer") : tr("Right Viewer"),
        tr("Shows the output of one node of the open composition."));
    setObjectName(
        index == 0 ? QStringLiteral("CompViewerLeft") : QStringLiteral("CompViewerRight"));
    setTitleCentered(true);
    setAcceptDrops(true);

    m_zoom = new QComboBox;
    m_zoom->setObjectName(QStringLiteral("CompViewerZoom"));
    m_zoom->setMinimumWidth(metrics::kTitleComboMinWidth);
    for (const auto& [label, zoom] : kZooms)
        m_zoom->addItem(tr(label), zoom);
    m_zoom->setFocusPolicy(Qt::NoFocus);
    addTitleBarLeadingWidget(m_zoom);
    connect(m_zoom, &QComboBox::currentIndexChanged, this,
        [this] { setZoom(m_zoom->currentData().toDouble()); });
    m_frameLabel = new QLabel;
    m_frameLabel->setObjectName(QStringLiteral("CompViewerFrame"));
    addTitleBarWidget(m_frameLabel);

    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    buildToolBar();
    layout->addWidget(m_bar);
    m_surface = ViewerSurface::create(body);
    // The picture with the optional rulers along its top and left edges (outside the picture).
    auto* area = new QWidget(body);
    auto* grid = new QGridLayout(area);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    grid->addWidget(m_surface->widget(), 1, 1);
    grid->setRowStretch(1, 1);
    grid->setColumnStretch(1, 1);
    layout->addWidget(area, 1);
    setContentWidget(body);
    const TimelineData& data = ctx.project->timeline()->data();
    m_surface->setAspect(QSize(data.width, data.height));
    m_guides = new GuideOverlay(ctx, m_surface, true);
    m_guides->setFrameSize(QSize(data.width, data.height));
    m_rulerCorner = new QWidget(area);
    m_rulerCorner->setObjectName(QStringLiteral("CompViewerRulerCorner"));
    m_rulerCorner->setFixedSize(ViewerRuler::kThickness, ViewerRuler::kThickness);
    m_rulerCorner->setAutoFillBackground(true);
    QPalette cornerPalette = m_rulerCorner->palette();
    cornerPalette.setColor(QPalette::Window, schemeColors().ruler);
    m_rulerCorner->setPalette(cornerPalette);
    m_rulerTop = new ViewerRuler(Qt::Horizontal, m_guides, area);
    m_rulerLeft = new ViewerRuler(Qt::Vertical, m_guides, area);
    m_rulerTop->setObjectName(QStringLiteral("CompViewerRulerTop"));
    m_rulerLeft->setObjectName(QStringLiteral("CompViewerRulerLeft"));
    grid->addWidget(m_rulerCorner, 0, 0);
    grid->addWidget(m_rulerTop, 0, 1);
    grid->addWidget(m_rulerLeft, 1, 0);
    auto applyRulers = [this] {
        const bool on = ViewerOverlaySettings::instance().options().showRulers;
        for (QWidget* w : {m_rulerCorner, static_cast<QWidget*>(m_rulerTop), static_cast<QWidget*>(m_rulerLeft)})
            w->setVisible(on);
        syncToolBar();
    };
    applyRulers();
    connect(&ViewerOverlaySettings::instance(), &ViewerOverlaySettings::changed, this, applyRulers);
    m_controls = new CompViewerOverlay(session, m_surface, m_index);
    m_regionOverlay = new ViewerRegion(m_surface);
    m_regionOverlay->raise();
    for (QObject* o : {static_cast<QObject*>(m_surface->widget()), static_cast<QObject*>(m_controls),
             static_cast<QObject*>(m_regionOverlay)})
        o->installEventFilter(this);
    connect(m_regionOverlay, &ViewerRegion::changed, this, [this] { requestRender(); });
    connect(m_regionOverlay, &ViewerRegion::dragFinished, this, [this] { refresh(); });
    connect(&CompViewOptions::instance(), &CompViewOptions::changed, this, [this] { refresh(); });
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    m_idle->setInterval(600);
    connect(m_idle, &QTimer::timeout, this, [this] {
        // The interaction is over: a proxy that Auto Proxy showed is replaced by the full picture.
        if (m_proxyShown)
            refresh();
    });
    // Roto Assist reads the viewer's picture: rendered once, on the GUI thread, when a point is
    // pressed (not per mouse move).
    m_controls->setSnapSource([this]() -> QImage {
        if (!rendersNodes() || !node())
            return {};
        const QSize logical = m_surface->pictureRect().size();
        if (logical.isEmpty())
            return {};
        const Composition& comp = m_session->composition();
        const quint64 id = node();
        auto future = m_engine->renderCompositionNode(m_ctx.project->timeline()->snapshot(),
            m_session->clipId(), id == comp.mediaOutId ? 0 : id, m_session->compFrame(), logical,
            OutputImage, m_session->transition());
        const RenderedFramePtr frame = future.get();
        return frame && frame->ok ? frame->image : QImage();
    });
    m_handle = new SplitHandle(m_surface, m_surface->widget());
    m_handle->changed = [this](QPointF pos, double angle) { setSplitLine(pos, angle); };
    setFocusPolicy(Qt::ClickFocus);
    syncToolBar();

    connect(session, &CompSession::viewersChanged, this, &CompViewer::onSessionViewers);
    connect(session, &CompSession::viewersChanged, this, &CompViewer::refresh);
    connect(session, &CompSession::clipChanged, this, [this] { m_empty = false; });
    connect(session, &CompSession::clipChanged, this, &CompViewer::refresh);
    connect(session, &CompSession::compositionChanged, this, [this] { noteInteraction(); });
    connect(session, &CompSession::compositionChanged, this, &CompViewer::refresh);
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, [this] { noteInteraction(); });
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, &CompViewer::refresh);
    m_watcher = new QFutureWatcher<RenderedFramePtr>(this);
    connect(m_watcher, &QFutureWatcher<RenderedFramePtr>::finished, this, &CompViewer::onRendered);
    connect(ctx.project->timeline(), &Timeline::changed, this, [this] {
        const TimelineData& d = m_ctx.project->timeline()->data();
        m_surface->setAspect(QSize(d.width, d.height));
        m_guides->setFrameSize(QSize(d.width, d.height));
    });
    m_engine = ctx.playback;
    if (m_engine)
        connect(m_engine, &PlaybackEngine::frameReady, this, &CompViewer::showFrame);
    refresh();
}

CompViewer::~CompViewer()
{
    if (m_engine) {
        m_engine->clearCompositionView(this);
        m_engine->removeOutputClient(this);
    }
}

bool CompViewer::followsEngine() const
{
    return m_engine && m_engine->compositionViewOwner() == this;
}

void CompViewer::updateEngineView()
{
    if (!m_engine)
        return;
    // The program engine renders this viewer's node itself (issue #7): playback then runs
    // through the Edit page's pipeline. Not with the viewer tools, which work on pictures.
    const bool wanted = isVisible() && m_engine->compositor() && m_session && m_session->clipId()
        && node() && !needsFx();
    if (!wanted) {
        m_engine->clearCompositionView(this);
        return;
    }
    const quint64 id = node() == m_session->composition().mediaOutId ? 0 : node();
    m_engine->setCompositionView(this, m_index, m_session->clipId(), id, m_session->transition());
}

quint64 CompViewer::node() const
{
    return shownNode();
}

quint64 CompViewer::shownNode() const
{
    if (!m_session)
        return 0;
    return m_locked ? m_locked : bufferNode(m_active);
}

quint64 CompViewer::bufferNode(int buffer) const
{
    if (m_locked && buffer == m_active)
        return m_locked;
    if (buffer == 1)
        return m_nodeB;
    if (m_empty)
        return 0;
    return m_active == 0 && m_session ? m_session->viewerNode(m_index) : m_nodeA;
}

QString CompViewer::headerText() const
{
    return title();
}

void CompViewer::setZoom(double zoom)
{
    m_surface->setZoom(zoom);
    for (int i = 0; i < m_zoom->count(); ++i) {
        if (qFuzzyCompare(m_zoom->itemData(i).toDouble() + 1, zoom + 1)) {
            const QSignalBlocker block(m_zoom);
            m_zoom->setCurrentIndex(i);
        }
    }
    m_controls->update();
    updateOutputClient();
}

void CompViewer::updateHeader()
{
    const QString side = m_index == 0 ? tr("Left") : tr("Right");
    const CompNode* n = m_session ? m_session->composition().node(node()) : nullptr;
    setTitle(n ? tr("%1 — %2").arg(side, n->name) : tr("%1 — No Node").arg(side));
    if (m_session && m_session->clipId()) {
        m_frameLabel->setText(tr("Frame %1").arg(m_session->compFrame()));
    } else {
        m_frameLabel->clear();
    }
    m_controls->refresh();
}

void CompViewer::refresh()
{
    updateEngineView();
    updateHeader();
    if (!m_session || !m_session->clipId()) {
        m_surface->clear(tr("Select a clip on the Edit page"));
        return;
    }
    if (!node()) {
        m_surface->clear(
            m_index == 0 ? tr("Select a node and press 1") : tr("Select a node and press 2"));
        return;
    }
    if (followsEngine()) {
        // The engine's frame for this node arrives with frameReady(); show the current one now
        // if it already is that.
        return;
    }
    if (rendersNodes()) {
        requestRender();
        return;
    }
    // No stored composition: the default MediaIn -> MediaOut shows exactly the clip's picture,
    // which the program engine already renders (no second render needed).
    if (m_engine && m_engine->currentFrame())
        m_surface->setRenderedFrame(m_engine->currentFrame());
    else
        m_surface->clear();
}

bool CompViewer::shows3D() const
{
    if (!m_session)
        return false;
    const CompNode* n = m_session->composition().node(shownNode());
    const NodeDescriptor* d = n ? NodeRegistry::instance().find(n->type) : nullptr;
    return d && d->outputKind == PortKind::Scene3D;
}

bool CompViewer::eventFilter(QObject* watched, QEvent* event)
{
    namespace s3 = render::scene3d;
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (shows3D() && (me->button() == Qt::LeftButton || me->button() == Qt::MiddleButton)) {
            m_nav3D = me->button();
            m_navPos = me->globalPosition();
            return true;
        }
        break;
    }
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (m_nav3D != Qt::NoButton) {
            const QPointF d = me->globalPosition() - m_navPos;
            m_navPos = me->globalPosition();
            const s3::Orbit o = s3::viewerOrbit();
            s3::setViewerOrbit(m_nav3D == Qt::MiddleButton
                    ? s3::panned(o, d.x(), d.y(), m_surface->pictureRect().width())
                    : s3::orbited(o, d.x(), d.y()));
            requestRender();
            return true;
        }
        break;
    }
    case QEvent::MouseButtonRelease:
        if (m_nav3D != Qt::NoButton) {
            m_nav3D = Qt::NoButton;
            refresh();
            return true;
        }
        break;
    case QEvent::Wheel: {
        auto* we = static_cast<QWheelEvent*>(event);
        if (shows3D() && we->angleDelta().y() != 0) {
            s3::setViewerOrbit(s3::zoomed(s3::viewerOrbit(), we->angleDelta().y() / 120.0));
            requestRender();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return PanelFrame::eventFilter(watched, event);
}

bool CompViewer::rendersNodes() const
{
    return m_engine && m_engine->compositor() && m_session && m_session->isStored();
}

void CompViewer::requestRender()
{
    if (!isVisible() || !rendersNodes() || !node() || followsEngine())
        return;
    // One render in flight per viewer: requests while it runs (scrubbing, playback, slider
    // drags) collapse into one more render of the newest state when it finishes.
    if (m_inFlight) {
        m_renderAgain = true;
        return;
    }
    m_inFlight = true;
    m_renderAgain = false;
    const CompViewOptions& options = CompViewOptions::instance();
    const qreal dpr = m_surface->widget()->devicePixelRatioF();
    const QSize logical = m_surface->pictureRect().size();
    const QSize full(int(logical.width() * dpr), int(logical.height() * dpr));
    // Proxy (spec 121): a fraction of the viewer's size, scaled up by the surface.
    const QSize pixels = options.renderSize(full, interacting());
    m_proxyShown = pixels != full;
    m_lastRenderSize = pixels;
    const bool fx = needsFx();
    const Composition& comp = m_session->composition();
    // High Quality / Motion Blur off: a draft of the composition (the viewer only).
    const std::shared_ptr<const TimelineData> timeline
        = options.viewerTimeline(m_ctx.project->timeline()->snapshot(), m_session->clipId());
    const bool useCache = options.options().diskCache && !shows3D(); // the orbit view is not saved
    const QByteArray content = useCache ? cacheContent() : QByteArray();
    const int cacheFlags = options.cacheFlags();
    const int64_t compFrame = m_session->compFrame();
    const int64_t frameCount = m_session->compFrameCount();

    // One picture the viewer needs: a node at a frame, from the disk cache or rendered now.
    struct Pending
    {
        std::shared_ptr<std::future<RenderedFramePtr>> future;
        RenderedFramePtr ready;
        QByteArray key; ///< disk cache key when it is to be stored
    };
    int hits = 0, misses = 0;
    auto submit = [&](quint64 id, int64_t frame) {
        Pending pending;
        const quint64 nodeId = id == comp.mediaOutId ? 0 : id;
        if (!content.isEmpty()) {
            pending.key = viewercache::makeKey(content, nodeId, frame, pixels, cacheFlags);
            const QImage cached = viewercache::load(pending.key);
            if (!cached.isNull()) {
                auto f = std::make_shared<RenderedFrame>();
                f->ok = true;
                f->frame = frame;
                f->size = cached.size();
                f->image = cached;
                f->fromCache = true;
                pending.ready = f;
                pending.key.clear();
                ++hits;
                return pending;
            }
            ++misses;
        }
        pending.future = std::make_shared<std::future<RenderedFramePtr>>(m_engine->renderCompositionNode(
            timeline, m_session->clipId(), nodeId, frame, pixels,
            fx || !m_surface->canDrawTextures() ? OutputImage : OutputTexture,
            m_session->transition()));
        return pending;
    };
    const bool split = fx && m_split;
    Pending fa = submit(split ? bufferNode(0) : node(), compFrame);
    Pending fb;
    if (split)
        fb = submit(bufferNode(1) ? bufferNode(1) : bufferNode(0), compFrame);
    // Onion skin: the frames around this one, not with the split display.
    QVector<Pending> before, after;
    if (m_onion && !split) {
        for (int k = 1; k <= m_onionFrames; ++k) {
            if (compFrame - k >= 0)
                before.append(submit(node(), compFrame - k));
            if (compFrame + k < frameCount)
                after.append(submit(node(), compFrame + k));
        }
    }
    m_cacheHits = hits;
    m_cacheMisses = misses;
    const viewerfx::Channel channel = m_channel;
    const bool checker = m_checker;
    const QPointF pos = m_splitPos;
    const double angle = m_splitAngle;
    const QRectF region = m_roi ? m_regionOverlay->region() : QRectF(0, 0, 1, 1);
    // Wait on a pool thread, deliver on the GUI thread (the compositor's futures are std::).
    auto keep = std::make_shared<std::tuple<Pending, Pending, QVector<Pending>, QVector<Pending>>>(
        std::move(fa), std::move(fb), std::move(before), std::move(after));
    m_watcher->setFuture(QtConcurrent::run([=] {
        const auto get = [](Pending& p) -> RenderedFramePtr {
            RenderedFramePtr frame = p.ready ? p.ready : (p.future ? p.future->get() : RenderedFramePtr());
            if (!p.key.isEmpty() && frame && frame->ok && !frame->image.isNull()) {
                if (viewercache::store(p.key, frame->image))
                    viewercache::trimSometimes();
            }
            return frame;
        };
        RenderedFramePtr frame = get(std::get<0>(*keep));
        if (!fx || !frame || !frame->ok || frame->image.isNull())
            return frame;
        QImage image = frame->image;
        if (std::get<1>(*keep).future || std::get<1>(*keep).ready) {
            const RenderedFramePtr other = get(std::get<1>(*keep));
            if (other && other->ok && !other->image.isNull())
                image = viewerfx::splitWipe(image, other->image, pos, angle);
        }
        if (!std::get<2>(*keep).isEmpty() || !std::get<3>(*keep).isEmpty()) {
            QVector<QImage> b, a;
            for (Pending& p : std::get<2>(*keep)) {
                const RenderedFramePtr f = get(p);
                b.append(f && f->ok ? f->image : QImage());
            }
            for (Pending& p : std::get<3>(*keep)) {
                const RenderedFramePtr f = get(p);
                a.append(f && f->ok ? f->image : QImage());
            }
            image = viewerfx::onionSkin(image, b, a);
        }
        image = viewerfx::showChannel(image, channel);
        image = viewerfx::applyRegion(image, region);
        if (checker)
            image = viewerfx::overChecker(image);
        auto out = std::make_shared<RenderedFrame>(*frame);
        out->texture = 0;
        out->fence = nullptr;
        out->image = image;
        out->size = image.size();
        return RenderedFramePtr(out);
    }));
}

void CompViewer::onRendered()
{
    m_inFlight = false;
    const RenderedFramePtr frame = m_watcher->result();
    if (frame && frame->ok && rendersNodes() && node())
        m_surface->setRenderedFrame(frame);
    if (m_renderAgain)
        requestRender();
}

bool CompViewer::needsFx() const
{
    // Anything that changes the picture the engine would show makes the viewer render it itself.
    const CompViewOptionsData& o = CompViewOptions::instance().options();
    return m_channel != viewerfx::Channel::Color || m_checker || m_split || m_onion || m_roi
        || o.proxy || o.autoProxy || !o.highQuality || !o.motionBlur || o.diskCache;
}

void CompViewer::noteInteraction()
{
    m_interaction.restart();
    if (m_idle && CompViewOptions::instance().options().autoProxy)
        m_idle->start();
}

bool CompViewer::interacting() const
{
    return (m_engine && m_engine->isPlaying())
        || (m_interaction.isValid() && m_interaction.elapsed() < 500);
}

QByteArray CompViewer::cacheContent() const
{
    // The composition's picture-relevant JSON, the clip's source, the timeline's format and the
    // files the composition reads (path, size, time).
    if (m_session->transition())
        return {}; // a transition's pictures are not cached on disk
    const TimelineData& data = m_ctx.project->timeline()->data();
    QByteArray content;
    const ClipData* clip = nullptr;
    for (const TrackData& t : data.videoTracks)
        for (const ClipData& c : t.clips)
            if (c.id == m_session->clipId())
                clip = &c;
    if (!clip || !clip->composition)
        return {};
    content += QJsonDocument(serialization::compositionToJson(*clip->composition, true)).toJson(QJsonDocument::Compact);
    content += QByteArray::number(clip->sourceInUs) + ' ' + QByteArray::number(clip->speed, 'g', 12) + ' '
        + clip->mediaId.toUtf8() + ' ' + QByteArray::number(data.width) + 'x' + QByteArray::number(data.height)
        + ' ' + QByteArray::number(data.fps.toDouble(), 'g', 12);
    QStringList media {clip->mediaId};
    for (const CompNode& n : clip->composition->nodes)
        if (!n.mediaId.isEmpty())
            media.append(n.mediaId);
    for (const QString& id : media) {
        if (id.isEmpty())
            continue;
        if (const MediaItem* item = m_ctx.project->mediaPool()->item(id)) {
            const QFileInfo fi(item->path);
            content += ' ' + item->path.toUtf8() + ':' + QByteArray::number(fi.size()) + ':'
                + QByteArray::number(fi.lastModified().toMSecsSinceEpoch());
        }
    }
    return content;
}

void CompViewer::onSessionViewers()
{
    if (m_capturing || !m_session)
        return;
    const quint64 now = m_session->viewerNode(m_index);
    if (now != m_seen)
        m_empty = false; // a node was assigned to this viewer
    m_seen = now;
    if (m_active == 1 && !m_locked && now != m_nodeA) {
        // Buffer B is shown: the node assigned now goes to B and A keeps its node.
        m_nodeB = now;
        m_capturing = true;
        m_session->setViewerNode(m_index, m_nodeA);
        m_capturing = false;
    }
    syncToolBar();
}

void CompViewer::setActiveBuffer(int buffer)
{
    buffer = buffer == 1 ? 1 : 0;
    if (buffer == m_active)
        return;
    if (buffer == 1) {
        m_nodeA = bufferNode(0);
        if (!m_nodeB)
            m_nodeB = m_nodeA;
        m_active = 1;
    } else {
        m_active = 0;
        m_capturing = true;
        if (m_session)
            m_session->setViewerNode(m_index, m_nodeA);
        m_capturing = false;
    }
    syncToolBar();
    refresh();
}

void CompViewer::setBufferNode(int buffer, quint64 nodeId)
{
    m_empty = false;
    if (buffer == 1) {
        m_nodeB = nodeId;
        refresh();
    } else if (m_session) {
        m_nodeA = nodeId;
        m_capturing = true;
        if (m_active == 0)
            m_session->setViewerNode(m_index, nodeId);
        m_capturing = false;
        refresh();
    }
}

void CompViewer::setSplit(bool on)
{
    if (on == m_split)
        return;
    m_split = on;
    if (on && !m_nodeB)
        m_nodeB = bufferNode(0);
    syncToolBar();
    refresh();
}

void CompViewer::setSplitLine(QPointF pos, double angleDeg)
{
    m_splitPos = pos;
    m_splitAngle = angleDeg;
    m_handle->setLine(pos, angleDeg);
    syncToolBar();
    requestRender();
}

void CompViewer::setChannel(viewerfx::Channel channel)
{
    m_channel = channel;
    syncToolBar();
    refresh();
}

void CompViewer::setChecker(bool on)
{
    m_checker = on;
    syncToolBar();
    refresh();
}

void CompViewer::setOnion(bool on)
{
    if (on == m_onion)
        return;
    m_onion = on;
    syncToolBar();
    refresh();
}

void CompViewer::setOnionFrames(int frames)
{
    frames = std::clamp(frames, 1, 3);
    if (frames == m_onionFrames)
        return;
    m_onionFrames = frames;
    syncToolBar();
    if (m_onion)
        refresh();
}

void CompViewer::setRegionShown(bool on)
{
    if (on == m_roi)
        return;
    m_roi = on;
    m_regionOverlay->setVisible(on);
    if (on)
        m_regionOverlay->raise();
    syncToolBar();
    refresh();
}

QRectF CompViewer::region() const
{
    return m_regionOverlay->region();
}

void CompViewer::setRegion(const QRectF& region)
{
    m_regionOverlay->setRegion(region);
    if (m_roi)
        refresh();
}

bool CompViewer::rulersShown() const
{
    return ViewerOverlaySettings::instance().options().showRulers;
}

void CompViewer::setRulersShown(bool on)
{
    ViewerOverlayOptions o = ViewerOverlaySettings::instance().options();
    o.showRulers = on;
    ViewerOverlaySettings::instance().setOptions(o);
}

bool CompViewer::safeAreaShown() const
{
    const ViewerOverlayOptions& o = ViewerOverlaySettings::instance().options();
    return o.actionSafe || o.titleSafe;
}

void CompViewer::setSafeAreaShown(bool on)
{
    ViewerOverlayOptions o = ViewerOverlaySettings::instance().options();
    o.actionSafe = on;
    o.titleSafe = on;
    ViewerOverlaySettings::instance().setOptions(o);
}

void CompViewer::setControlsShown(bool on)
{
    m_controlsShown = on;
    m_controls->setVisible(on);
    syncToolBar();
}

void CompViewer::setLocked(bool on)
{
    if (on == locked())
        return;
    m_locked = on ? shownNode() : 0;
    syncToolBar();
    refresh();
}

void CompViewer::clearViewer()
{
    m_locked = 0;
    m_nodeB = 0;
    m_nodeA = 0;
    m_active = 0;
    m_split = false;
    m_empty = true;
    m_capturing = true;
    if (m_session)
        m_session->setViewerNode(m_index, 0);
    m_capturing = false;
    m_seen = m_session ? m_session->viewerNode(m_index) : 0;
    syncToolBar();
    refresh();
}

void CompViewer::zoomTo100()
{
    setZoom(1.0);
    m_surface->setView(1.0, QPointF());
}

QAction* CompViewer::toolAction(const QString& id) const
{
    return findChild<QAction*>(QStringLiteral("CompViewerTool:") + id);
}

void CompViewer::buildToolBar()
{
    // A QToolBar: buttons that do not fit move, in order, into its "»" menu (1366 px wide
    // screens split in two viewers), so they never overlap or get cut off.
    m_bar = new QToolBar;
    m_bar->setObjectName(QStringLiteral("CompViewerToolBar"));
    m_bar->setMovable(false);
    m_bar->setFloatable(false);
    m_bar->setContextMenuPolicy(Qt::PreventContextMenu);
    m_bar->layout()->setContentsMargins(
        metrics::kSpaceS, metrics::kSpaceXs, metrics::kSpaceS, metrics::kSpaceXs);
    m_bar->layout()->setSpacing(metrics::kSpaceXs);
    auto add = [this](const char* id, const QString& text, const QString& tip, bool checkable) {
        auto* a = new QAction(text, this);
        a->setObjectName(QStringLiteral("CompViewerTool:") + QLatin1String(id));
        a->setToolTip(tip);
        a->setCheckable(checkable);
        m_bar->addAction(a);
        const QString key = QLatin1String(id);
        connect(a, &QAction::triggered, this, [this, key] { runTool(key); });
        return a;
    };
    add("a", tr("A"), tr("Buffer A (,)"), true);
    add("b", tr("B"), tr("Buffer B (.)"), true);
    add("split", tr("A|B"), tr("Split display of A and B (/)"), true);
    m_bar->addSeparator();
    add("color", tr("Color"), tr("Show colour (C)"), true);
    add("red", tr("Red"), tr("Show red (R)"), true);
    add("green", tr("Green"), tr("Show green (G)"), true);
    add("blue", tr("Blue"), tr("Show blue (B)"), true);
    add("alpha", tr("Alpha"), tr("Show alpha (A)"), true);
    m_bar->addSeparator();
    add("checker", tr("Checker"), tr("Checker under the picture"), true);
    add("onion", tr("Onion Skin"),
        tr("Onion Skin (O): the frames before the current one in blue and the frames after it in "
           "orange, laid faintly over the picture"),
        true);
    add("onionframes", tr("Onion: 1"),
        tr("How many frames on each side Onion Skin shows (click: 1, 2, 3)"), false);
    add("roi", tr("ROI"),
        tr("Region of Interest: show only a region of the picture; drag over the picture to draw "
           "it, double-click to use the whole picture again"),
        true);
    add("rulers", tr("Rulers"), tr("Rulers along the top and left edge of the picture"), true);
    add("safe", tr("Safe Area"), tr("Title and action safe areas over the picture"), true);
    add("controls", tr("Controls"), tr("Show on-screen controls (Ctrl+K)"), true);
    add("lock", tr("Lock"), tr("Lock the viewer on this node (Ctrl+L)"), true);
    add("clear", tr("Clear"), tr("Clear the viewers (~)"), false);
    add("zoom100", tr("100%"), tr("Zoom to 100% (Ctrl+1)"), false);
}

void CompViewer::syncToolBar()
{
    auto mark = [this](const char* id, bool on) {
        if (QAction* a = toolAction(QLatin1String(id))) {
            const QSignalBlocker block(a);
            a->setChecked(on);
        }
    };
    mark("a", m_active == 0);
    mark("b", m_active == 1);
    mark("split", m_split);
    mark("color", m_channel == viewerfx::Channel::Color);
    mark("red", m_channel == viewerfx::Channel::Red);
    mark("green", m_channel == viewerfx::Channel::Green);
    mark("blue", m_channel == viewerfx::Channel::Blue);
    mark("alpha", m_channel == viewerfx::Channel::Alpha);
    mark("checker", m_checker);
    mark("onion", m_onion);
    mark("roi", m_roi);
    mark("rulers", rulersShown());
    mark("safe", safeAreaShown());
    if (QAction* a = toolAction(QStringLiteral("onionframes")))
        a->setText(tr("Onion: %1").arg(m_onionFrames));
    mark("controls", m_controlsShown);
    mark("lock", locked());
    if (m_handle) {
        m_handle->setVisible(m_split);
        m_handle->setLine(m_splitPos, m_splitAngle);
    }
}

void CompViewer::runTool(const QString& id)
{
    using viewerfx::Channel;
    if (id == QLatin1String("a")) {
        setActiveBuffer(0);
    } else if (id == QLatin1String("b")) {
        setActiveBuffer(1);
    } else if (id == QLatin1String("split")) {
        setSplit(!m_split);
    } else if (id == QLatin1String("color")) {
        setChannel(Channel::Color);
    } else if (id == QLatin1String("red")) {
        setChannel(m_channel == Channel::Red ? Channel::Color : Channel::Red);
    } else if (id == QLatin1String("green")) {
        setChannel(m_channel == Channel::Green ? Channel::Color : Channel::Green);
    } else if (id == QLatin1String("blue")) {
        setChannel(m_channel == Channel::Blue ? Channel::Color : Channel::Blue);
    } else if (id == QLatin1String("alpha")) {
        setChannel(m_channel == Channel::Alpha ? Channel::Color : Channel::Alpha);
    } else if (id == QLatin1String("checker")) {
        setChecker(!m_checker);
    } else if (id == QLatin1String("onion")) {
        setOnion(!m_onion);
    } else if (id == QLatin1String("onionframes")) {
        setOnionFrames(m_onionFrames % 3 + 1);
    } else if (id == QLatin1String("roi")) {
        setRegionShown(!m_roi);
    } else if (id == QLatin1String("rulers")) {
        setRulersShown(!rulersShown());
    } else if (id == QLatin1String("safe")) {
        setSafeAreaShown(!safeAreaShown());
    } else if (id == QLatin1String("controls")) {
        setControlsShown(!m_controlsShown);
    } else if (id == QLatin1String("lock")) {
        setLocked(!locked());
    } else if (id == QLatin1String("clear")) {
        emit clearRequested();
        clearViewer(); // also when no page listens
    } else if (id == QLatin1String("zoom100")) {
        zoomTo100();
    }
}

QString CompViewer::keyTool(const QKeyEvent* e) const
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    if (mods == Qt::ControlModifier) {
        switch (e->key()) {
        case Qt::Key_K: return QStringLiteral("controls");
        case Qt::Key_L: return QStringLiteral("lock");
        case Qt::Key_1: return QStringLiteral("zoom100");
        default: return {};
        }
    }
    if (mods == Qt::ShiftModifier)
        return e->key() == Qt::Key_AsciiTilde || e->key() == Qt::Key_QuoteLeft
            ? QStringLiteral("clear")
            : QString();
    if (mods != Qt::NoModifier)
        return {};
    switch (e->key()) {
    case Qt::Key_Comma: return QStringLiteral("a");
    case Qt::Key_Period: return QStringLiteral("b");
    case Qt::Key_Slash: return QStringLiteral("split");
    case Qt::Key_C: return QStringLiteral("color");
    case Qt::Key_R: return QStringLiteral("red");
    case Qt::Key_G: return QStringLiteral("green");
    case Qt::Key_B: return QStringLiteral("blue");
    case Qt::Key_A: return QStringLiteral("alpha");
    case Qt::Key_O: return QStringLiteral("onion");
    case Qt::Key_AsciiTilde: return QStringLiteral("clear");
    default: return {};
    }
}

bool CompViewer::event(QEvent* e)
{
    // The keys are local to the viewer: while it (or its overlay) has the focus they win over
    // a global shortcut of the same key; with the focus elsewhere nothing changes.
    if (e->type() == QEvent::ShortcutOverride && !keyTool(static_cast<QKeyEvent*>(e)).isEmpty()) {
        e->accept();
        return true;
    }
    return PanelFrame::event(e);
}

void CompViewer::keyPressEvent(QKeyEvent* e)
{
    const QString tool = keyTool(e);
    if (tool.isEmpty()) {
        PanelFrame::keyPressEvent(e);
        return;
    }
    runTool(tool);
    e->accept();
}

void CompViewer::showFrame(const RenderedFramePtr& frame)
{
    if (followsEngine()) {
        if (isVisible() && node() && frame && frame->ok)
            m_surface->setRenderedFrame(frame);
        return;
    }
    if (rendersNodes()) {
        requestRender(); // the program moved on (playback): render the node at the new frame
        return;
    }
    if (isVisible() && node() && frame)
        m_surface->setRenderedFrame(frame);
}

void CompViewer::updateOutputClient()
{
    if (!m_engine)
        return;
    if (!isVisible()) {
        m_engine->removeOutputClient(this);
        return;
    }
    const qreal dpr = m_surface->widget()->devicePixelRatioF();
    const QSize logical = m_surface->pictureRect().size();
    m_engine->setOutputClient(this, QSize(int(logical.width() * dpr), int(logical.height() * dpr)),
        m_surface->canDrawTextures());
}

void CompViewer::showEvent(QShowEvent* event)
{
    PanelFrame::showEvent(event);
    updateOutputClient();
    refresh();
}

void CompViewer::hideEvent(QHideEvent* event)
{
    PanelFrame::hideEvent(event);
    if (m_engine) {
        m_engine->clearCompositionView(this);
        m_engine->removeOutputClient(this);
    }
}

void CompViewer::resizeEvent(QResizeEvent* event)
{
    PanelFrame::resizeEvent(event);
    updateOutputClient();
    requestRender();
}

void CompViewer::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat(QLatin1String(mime::CompNode)))
        event->acceptProposedAction();
}

void CompViewer::dropEvent(QDropEvent* event)
{
    const quint64 id = event->mimeData()->data(QLatin1String(mime::CompNode)).toULongLong();
    if (m_locked) {
        event->ignore();
        return;
    }
    if (m_session && id) {
        m_session->setViewerNode(m_index, id);
        event->acceptProposedAction();
    }
}

} // namespace kk
