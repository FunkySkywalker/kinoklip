// SPDX-License-Identifier: GPL-3.0-or-later
// ColorTracker implementation. See the header.
#include "ui/color/ColorTracker.h"

#include "compositing/tracking/StabilizeJob.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/color/ColorSession.h"
#include "ui/common/VimRowOps.h"
#include "ui/inspector/ValueField.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr int kAnalysisWidth = 960; ///< widest picture that is tracked

QToolButton* toolButton(const QString& text, QWidget* parent, bool checkable = false)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setCheckable(checkable);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

namespace st = stabilize;

} // namespace

// ---- TrackGraph ----

TrackGraph::TrackGraph(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("TrackerGraph"));
    setMinimumHeight(60);
    setToolTip(tr("The track: Pan (red), Tilt (green), Zoom (blue), Rotate (yellow)"));
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, qOverload<>(&QWidget::update));
        connect(m_session, &ColorSession::selectedNodeChanged, this, qOverload<>(&QWidget::update));
    }
}

void TrackGraph::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(3, 3, -3, -3);
    p.fillRect(rect(), QColor(24, 24, 24));
    p.setPen(QPen(QColor(70, 70, 70), 1));
    p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
    const GradeTrack t = m_session ? m_session->track() : GradeTrack {};
    if (!t.hasData() || t.samples.size() < 2)
        return;
    struct Curve
    {
        QColor colour;
        double (*value)(const GradeTrackSample&);
        double range; ///< the value that reaches the top / bottom
        bool on;
    };
    const Curve curves[] = {
        {QColor(230, 90, 80), [](const GradeTrackSample& s) { return s.dx; }, 0.5, t.pan},
        {QColor(90, 210, 110), [](const GradeTrackSample& s) { return s.dy; }, 0.5, t.tilt},
        {QColor(90, 150, 240),
            [](const GradeTrackSample& s) { return std::log2(std::max(1e-3, std::sqrt(s.sx * s.sy))); },
            1.0, t.zoom},
        {QColor(240, 210, 80), [](const GradeTrackSample& s) { return s.rot; }, 45.0, t.rotate},
    };
    const int n = int(t.samples.size());
    for (const Curve& c : curves) {
        if (!c.on)
            continue;
        QPainterPath path;
        for (int i = 0; i < n; ++i) {
            const double x = r.left() + r.width() * i / std::max(1, n - 1);
            const double v = std::clamp(c.value(t.samples[i]) / c.range, -1.0, 1.0);
            const double y = r.center().y() - v * r.height() / 2;
            i == 0 ? path.moveTo(x, y) : path.lineTo(x, y);
        }
        p.setPen(QPen(c.colour, 1.3));
        p.drawPath(path);
    }
    if (m_session) {
        const int64_t f = m_session->gradeFrame();
        if (f >= t.firstFrame && f <= t.lastFrame()) {
            const double x = r.left() + r.width() * double(f - t.firstFrame) / std::max(1, n - 1);
            p.setPen(QPen(QColor(255, 255, 255, 200), 1));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
    }
}

// ---- ColorTrackerPanel ----

ColorTrackerPanel::ColorTrackerPanel(
    ColorSession* session, PlaybackEngine* engine, Project* project, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_engine(engine)
    , m_project(project)
{
    markVimForm(this);
    setObjectName(QStringLiteral("ColorTrackerPanel"));
    m_runner = new tracking::TrackerRunner(this);
    m_stabilizer = new tracking::StabilizeRunner(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(3);
    auto* modes = new QHBoxLayout;
    auto* group = new QButtonGroup(this);
    const QString names[2] = {tr("Window"), tr("Stabilizer")};
    for (int i = 0; i < 2; ++i) {
        m_modeButtons[i] = toolButton(names[i], this, true);
        m_modeButtons[i]->setProperty("kkTab", true); // Kinoklip style: red underline when checked
        m_modeButtons[i]->setObjectName(QStringLiteral("TrackerMode_") + QString::number(i));
        group->addButton(m_modeButtons[i], i);
        modes->addWidget(m_modeButtons[i]);
    }
    m_modeButtons[0]->setChecked(true);
    modes->addStretch(1);
    layout->addLayout(modes);
    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);
    connect(group, &QButtonGroup::idClicked, m_stack, &QStackedWidget::setCurrentIndex);

    // ---- Window mode ----
    auto* window = new QWidget;
    auto* wl = new QVBoxLayout(window);
    wl->setContentsMargins(0, 2, 0, 0);
    wl->setSpacing(3);
    auto* boxes = new QHBoxLayout;
    auto makeBox = [&](const QString& text, const QString& name, bool on, const QString& tip) {
        auto* b = new QCheckBox(text, window);
        b->setObjectName(name);
        b->setChecked(on);
        b->setToolTip(tip);
        boxes->addWidget(b);
        return b;
    };
    m_pan = makeBox(tr("Pan"), QStringLiteral("TrackerPan"), true,
        tr("The windows follow the subject sideways"));
    m_tilt = makeBox(tr("Tilt"), QStringLiteral("TrackerTilt"), true,
        tr("The windows follow the subject up and down"));
    m_zoom = makeBox(tr("Zoom"), QStringLiteral("TrackerZoom"), false,
        tr("The windows grow and shrink with the subject (track again after switching on)"));
    m_rotate = makeBox(tr("Rotate"), QStringLiteral("TrackerRotate"), false,
        tr("The windows turn with the subject (track again after switching on)"));
    m_perspective = makeBox(tr("3D"), QStringLiteral("TrackerPerspective"), false,
        tr("Width and height follow the subject on their own (track again after switching on)"));
    boxes->addStretch(1);
    wl->addLayout(boxes);

    auto* row = new QHBoxLayout;
    auto* kind = new QButtonGroup(this);
    m_clipMode = toolButton(tr("Clip"), window, true);
    m_clipMode->setObjectName(QStringLiteral("TrackerClipMode"));
    m_clipMode->setToolTip(tr("A run tracks to the end (or start) of the clip"));
    m_frameMode = toolButton(tr("Frame"), window, true);
    m_frameMode->setObjectName(QStringLiteral("TrackerFrameMode"));
    m_frameMode->setToolTip(tr("A run tracks one frame, to correct a track step by step"));
    kind->addButton(m_clipMode);
    kind->addButton(m_frameMode);
    m_clipMode->setChecked(true);
    row->addWidget(m_clipMode);
    row->addWidget(m_frameMode);
    row->addSpacing(10);
    m_reverse = toolButton(tr("Track Reverse"), window);
    m_reverse->setObjectName(QStringLiteral("TrackerReverse"));
    m_forward = toolButton(tr("Track Forward"), window);
    m_forward->setObjectName(QStringLiteral("TrackerForward"));
    m_stop = toolButton(tr("Stop"), window);
    m_stop->setObjectName(QStringLiteral("TrackerStop"));
    m_clear = toolButton(tr("Clear Track"), window);
    m_clear->setObjectName(QStringLiteral("TrackerClear"));
    row->addWidget(m_reverse);
    row->addWidget(m_forward);
    row->addWidget(m_stop);
    row->addWidget(m_clear);
    row->addStretch(1);
    wl->addLayout(row);
    m_progress = new QProgressBar(window);
    m_progress->setObjectName(QStringLiteral("TrackerProgress"));
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(6);
    wl->addWidget(m_progress);
    m_status = new QLabel(window);
    m_status->setObjectName(QStringLiteral("TrackerStatus"));
    m_status->setWordWrap(true);
    wl->addWidget(m_status);
    m_graph = new TrackGraph(m_session, window);
    wl->addWidget(m_graph, 1);
    m_stack->addWidget(window);

    // ---- Stabilizer mode ----
    auto* stab = new QWidget;
    auto* grid = new QGridLayout(stab);
    grid->setContentsMargins(0, 2, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(3);
    grid->addWidget(new QLabel(tr("Mode"), stab), 0, 0);
    m_stabMode = new QComboBox(stab);
    m_stabMode->setObjectName(QStringLiteral("StabilizerMode"));
    m_stabMode->addItem(tr("Perspective"), int(st::Mode::Perspective));
    m_stabMode->addItem(tr("Similarity"), int(st::Mode::Similarity));
    m_stabMode->addItem(tr("Translation"), int(st::Mode::Translation));
    grid->addWidget(m_stabMode, 0, 1, 1, 2);
    m_stabZoom = new QCheckBox(tr("Zoom"), stab);
    m_stabZoom->setObjectName(QStringLiteral("StabilizerZoom"));
    m_stabZoom->setToolTip(tr("Scale up to hide the empty edges the correction leaves"));
    grid->addWidget(m_stabZoom, 1, 1);
    grid->addWidget(new QLabel(tr("Smooth"), stab), 2, 0);
    m_smooth = new ValueField(stab);
    m_smooth->setObjectName(QStringLiteral("StabilizerSmooth"));
    m_smooth->setRange(st::kMinSmooth, st::kMaxSmooth);
    m_smooth->setStep(0.01);
    m_smooth->setDecimals(2);
    m_smooth->setToolTip(tr("0 keeps the camera movement fine, 1 follows only the slow drift"));
    grid->addWidget(m_smooth, 2, 1);
    grid->addWidget(new QLabel(tr("Strength"), stab), 3, 0);
    m_strength = new ValueField(stab);
    m_strength->setObjectName(QStringLiteral("StabilizerStrength"));
    m_strength->setRange(st::kMinStrength, st::kMaxStrength);
    m_strength->setStep(0.05);
    m_strength->setDecimals(2);
    grid->addWidget(m_strength, 3, 1);
    m_stabilize = toolButton(tr("Stabilize"), stab);
    m_stabilize->setObjectName(QStringLiteral("StabilizerStabilize"));
    m_stabCancel = toolButton(tr("Cancel"), stab);
    m_stabCancel->setObjectName(QStringLiteral("StabilizerCancel"));
    m_stabReset = toolButton(tr("Reset Stabilization"), stab);
    m_stabReset->setObjectName(QStringLiteral("StabilizerReset"));
    auto* brow = new QHBoxLayout;
    brow->addWidget(m_stabilize);
    brow->addWidget(m_stabCancel);
    brow->addWidget(m_stabReset);
    brow->addStretch(1);
    grid->addLayout(brow, 4, 0, 1, 4);
    m_stabProgress = new QProgressBar(stab);
    m_stabProgress->setObjectName(QStringLiteral("StabilizerProgress"));
    m_stabProgress->setTextVisible(false);
    m_stabProgress->setMaximumHeight(6);
    grid->addWidget(m_stabProgress, 5, 0, 1, 4);
    m_stabStatus = new QLabel(stab);
    m_stabStatus->setObjectName(QStringLiteral("StabilizerStatus"));
    m_stabStatus->setWordWrap(true);
    grid->addWidget(m_stabStatus, 6, 0, 1, 4);
    grid->setColumnStretch(3, 1);
    grid->setRowStretch(7, 1);
    m_stack->addWidget(stab);

    // ---- Window mode wiring ----
    auto bindBox = [this](QCheckBox* box, bool GradeTrack::*member, const QString& label) {
        connect(box, &QCheckBox::clicked, this, [this, member, label](bool on) {
            setOption(label, [member, on](GradeTrack& t) { t.*member = on; });
        });
    };
    bindBox(m_pan, &GradeTrack::pan, tr("Tracker Pan"));
    bindBox(m_tilt, &GradeTrack::tilt, tr("Tracker Tilt"));
    bindBox(m_zoom, &GradeTrack::zoom, tr("Tracker Zoom"));
    bindBox(m_rotate, &GradeTrack::rotate, tr("Tracker Rotate"));
    bindBox(m_perspective, &GradeTrack::perspective, tr("Tracker 3D"));
    connect(m_forward, &QToolButton::clicked, this, [this] { trackFromPlayhead(true); });
    connect(m_reverse, &QToolButton::clicked, this, [this] { trackFromPlayhead(false); });
    connect(m_stop, &QToolButton::clicked, this, [this] { stopTracking(); });
    connect(m_clear, &QToolButton::clicked, this, [this] { clearTrack(); });
    connect(m_runner, &tracking::TrackerRunner::progress, this, [this](int done, int total) {
        m_progress->setMaximum(std::max(1, total));
        m_progress->setValue(done);
    });
    connect(m_runner, &tracking::TrackerRunner::finished, this, &ColorTrackerPanel::onFinished);

    // ---- Stabilizer wiring ----
    connect(m_stabMode, &QComboBox::activated, this, [this](int index) {
        const auto mode = st::Mode(m_stabMode->itemData(index).toInt());
        editStabilization(tr("Stabilizer Mode"), [mode](ClipData& c) {
            c.stabilization.settings.mode = mode;
        });
    });
    connect(m_stabZoom, &QCheckBox::clicked, this, [this](bool on) {
        editStabilization(tr("Stabilizer Zoom"),
            [on](ClipData& c) { c.stabilization.settings.zoom = on; });
    });
    connect(m_smooth, &ValueField::valueCommitted, this, [this](double v) {
        editStabilization(tr("Stabilizer Smooth"),
            [v](ClipData& c) { c.stabilization.settings.smooth = v; });
    });
    connect(m_strength, &ValueField::valueCommitted, this, [this](double v) {
        editStabilization(tr("Stabilizer Strength"),
            [v](ClipData& c) { c.stabilization.settings.strength = v; });
    });
    connect(m_stabilize, &QToolButton::clicked, this, [this] { startStabilize(); });
    connect(m_stabCancel, &QToolButton::clicked, this, [this] { m_stabilizer->cancel(); });
    connect(m_stabReset, &QToolButton::clicked, this, [this] {
        editStabilization(tr("Reset Stabilization"),
            [](ClipData& c) { c.stabilization = st::ClipStabilization {}; });
    });
    connect(m_stabilizer, &tracking::StabilizeRunner::progress, this, [this](int, int) {
        m_stabProgress->setMaximum(std::max(1, m_stabilizer->total()));
        m_stabProgress->setValue(m_stabilizer->done());
    });
    connect(m_stabilizer, &tracking::StabilizeRunner::finished, this,
        &ColorTrackerPanel::onStabilizeFinished);

    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &ColorTrackerPanel::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &ColorTrackerPanel::refresh);
        connect(m_session, &ColorSession::selectedWindowChanged, this, &ColorTrackerPanel::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &ColorTrackerPanel::refresh);
    }
    if (m_project)
        connect(m_project->timeline(), &Timeline::changed, this, &ColorTrackerPanel::refresh);
    refresh();
}

ColorTrackerPanel::~ColorTrackerPanel()
{
    m_runner->stop();
    m_stabilizer->cancel();
}

ColorTrackerPanel::Mode ColorTrackerPanel::mode() const
{
    return Mode(m_stack->currentIndex());
}

void ColorTrackerPanel::setMode(Mode mode)
{
    m_stack->setCurrentIndex(int(mode));
    m_modeButtons[int(mode)]->setChecked(true);
}

bool ColorTrackerPanel::isTracking() const
{
    return m_runner->isRunning();
}

QString ColorTrackerPanel::status() const
{
    return m_status->text();
}

void ColorTrackerPanel::setStatus(const QString& text)
{
    m_status->setText(text);
}

void ColorTrackerPanel::setOption(const QString& label, const std::function<void(GradeTrack&)>& edit)
{
    if (m_refreshing || !m_session)
        return;
    GradeTrack t = m_session->track();
    edit(t);
    m_session->setTrack(0, t, label);
}

void ColorTrackerPanel::clearTrack()
{
    if (m_runner->isRunning())
        stopTracking();
    if (m_session && m_session->clearTrack())
        setStatus(tr("Track cleared."));
}

void ColorTrackerPanel::stopTracking()
{
    m_runner->stop();
}

bool ColorTrackerPanel::trackFromPlayhead(bool forward)
{
    if (m_runner->isRunning() || !m_session)
        return false;
    const GradeNode* n = m_session->currentNode();
    const GradeWindow* w = m_session->window();
    if (!n || n->isMixer() || !w) {
        setStatus(tr("Add a window to the node first (Windows palette), then track it."));
        return false;
    }
    int64_t first = 0, last = 0, offset = 0;
    if (!m_session->trackRange(&first, &last, &offset)) {
        setStatus(tr("There is no clip to track."));
        return false;
    }
    const int64_t frame = m_session->gradeFrame();
    if (frame < first || frame > last) {
        setStatus(tr("Move the playhead onto the clip to track it."));
        return false;
    }
    int64_t end = forward ? last : first;
    if (m_frameMode->isChecked())
        end = std::clamp<int64_t>(frame + (forward ? 1 : -1), first, last);
    if (end == frame) {
        setStatus(forward ? tr("The playhead is on the last frame; there is nothing to track.")
                          : tr("The playhead is on the first frame; there is nothing to track."));
        return false;
    }
    if (!m_override && !m_engine) {
        setStatus(tr("Cannot track: there is no playback engine."));
        return false;
    }
    const TimelineData& data = m_project->timeline()->data();
    const double aspect = m_session->pictureAspect();
    QString why;
    auto plan = tracking::planWindowTrack(*n, *w, frame, end, aspect, &why);
    if (!plan) {
        setStatus(tr("Cannot track: %1.").arg(why));
        return false;
    }
    const double scale = std::min(1.0, double(kAnalysisWidth) / std::max(1, data.width));
    const QSize size(std::max(16, int(data.width * scale)), std::max(16, int(data.height * scale)));
    tracking::TrackJob job = plan->job;
    PlaybackEngine* engine = m_engine;
    const FrameSource override = m_override;
    const auto grades = m_session->target() == ColorSession::Target::Timeline
        ? PlaybackEngine::ImageGrades::ClipsOnly
        : PlaybackEngine::ImageGrades::None;
    job.source = [engine, override, offset, size, grades](int64_t f) {
        if (override)
            return override(f + offset, size);
        return engine->renderImage(f + offset, size, grades);
    };
    m_plan = std::move(plan);
    m_runNode = n->id;
    m_runOffset = offset;
    m_progress->setValue(0);
    if (!m_runner->start(std::move(job))) {
        m_plan.reset();
        setStatus(tr("Cannot track: nothing to track."));
        return false;
    }
    setStatus(forward ? tr("Tracking forward...") : tr("Tracking in reverse..."));
    refresh();
    return true;
}

void ColorTrackerPanel::onFinished(tracking::TrackOutcome outcome, const QString& reason)
{
    if (!m_plan) {
        refresh();
        return;
    }
    const tracking::WindowTrackPlan plan = *m_plan;
    m_plan.reset();
    const std::vector<tracking::TrackSample>& samples = m_runner->samples();
    QString message;
    if (samples.size() < 2) {
        message = outcome == tracking::TrackOutcome::Stopped
            ? tr("Tracking cancelled. The track is unchanged.")
            : tr("Tracking failed: %1")
                  .arg(reason.isEmpty() ? tr("nothing was found.") : reason);
    } else if (!m_session || !m_session->grade().node(m_runNode)) {
        message = tr("The node was removed; the track was dropped.");
    } else {
        GradeTrack t = m_session->grade().node(m_runNode)->track;
        tracking::mergeSamples(t, plan, samples);
        m_session->setTrack(m_runNode, t, tr("Track Window"));
        message = outcome == tracking::TrackOutcome::Finished
            ? tr("Tracked %1 frames.").arg(samples.size())
            : outcome == tracking::TrackOutcome::Stopped
            ? tr("Tracking stopped after %1 frames.").arg(samples.size())
            : tr("Tracked %1 frames, then stopped: %2").arg(samples.size()).arg(reason);
    }
    setStatus(message);
    refresh();
}

void ColorTrackerPanel::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    const GradeTrack t = n ? n->track : GradeTrack {};
    const bool running = m_runner->isRunning();
    const bool corrector = n && !n->isMixer();
    for (auto [box, value] : {std::pair {m_pan, t.pan}, std::pair {m_tilt, t.tilt},
             std::pair {m_zoom, t.zoom}, std::pair {m_rotate, t.rotate},
             std::pair {m_perspective, t.perspective}}) {
        if (box->isChecked() != value)
            box->setChecked(value);
        box->setEnabled(corrector && !running);
    }
    const bool hasWindow = corrector && m_session->window();
    m_forward->setEnabled(hasWindow && !running);
    m_reverse->setEnabled(hasWindow && !running);
    m_stop->setEnabled(running);
    m_clear->setEnabled(t.hasData() && !running);
    m_progress->setVisible(running);
    m_graph->update();

    // Stabilizer: the settings of the graded clip.
    const ClipData* clip = m_session && m_project
        ? editops::clipById(m_project->timeline()->data(), m_session->clipId())
        : nullptr;
    const bool canStab = clip && clip->kind == ClipKind::Media;
    const bool stabRunning = m_stabilizer->isRunning();
    m_stabMode->setEnabled(canStab && !stabRunning);
    m_stabZoom->setEnabled(canStab);
    m_smooth->setEnabled(canStab);
    m_strength->setEnabled(canStab);
    m_stabilize->setEnabled(canStab && !stabRunning);
    m_stabCancel->setVisible(stabRunning);
    m_stabProgress->setVisible(stabRunning);
    m_stabReset->setEnabled(canStab && !stabRunning && !clip->stabilization.isDefault());
    if (clip) {
        const st::Settings& s = clip->stabilization.settings;
        const int index = m_stabMode->findData(int(s.mode));
        if (index >= 0 && m_stabMode->currentIndex() != index)
            m_stabMode->setCurrentIndex(index);
        if (m_stabZoom->isChecked() != s.zoom)
            m_stabZoom->setChecked(s.zoom);
        if (!m_smooth->isScrubbing() && std::abs(m_smooth->value() - s.smooth) > 1e-9)
            m_smooth->setValue(s.smooth);
        if (!m_strength->isScrubbing() && std::abs(m_strength->value() - s.strength) > 1e-9)
            m_strength->setValue(s.strength);
        if (!stabRunning && st::needsAnalysis(clip->stabilization))
            m_stabStatus->setText(tr("The Mode changed since the analysis. Press Stabilize again."));
    }
    if (!canStab && !stabRunning)
        m_stabStatus->setText(clip ? tr("Only clips of video files can be stabilized.")
                                   : tr("Select a clip to stabilize it."));
    m_refreshing = false;
}

void ColorTrackerPanel::editStabilization(
    const QString& label, const std::function<void(ClipData&)>& edit)
{
    if (m_refreshing || !m_session || !m_project)
        return;
    const quint64 id = m_session->clipId();
    timelineedit::runEdit(m_project, label, [&](TimelineData& d) {
        ClipData* c = editops::clipById(d, id);
        if (!c)
            return false;
        const auto before = c->stabilization;
        edit(*c);
        return !(before == c->stabilization);
    });
}

void ColorTrackerPanel::startStabilize()
{
    if (!m_session || !m_project || m_stabilizer->isRunning())
        return;
    const quint64 id = m_session->clipId();
    const TimelineData& d = m_project->timeline()->data();
    const ClipData* clip = editops::clipById(d, id);
    const MediaItem* item = clip ? m_project->mediaPool()->item(clip->mediaId) : nullptr;
    QString why;
    std::optional<tracking::StabilizeJob> job;
    if (clip && item)
        job = tracking::planStabilizeJob(*clip, *item, d.fps, clip->stabilization.settings.mode, &why);
    else
        why = tr("the media of the clip is missing");
    if (!job) {
        m_stabStatus->setText(tr("Cannot stabilize: %1.").arg(why));
        return;
    }
    m_stabStatus->setText(tr("Analysing the clip..."));
    m_stabilizer->start(id, std::move(*job));
    refresh();
}

void ColorTrackerPanel::onStabilizeFinished(quint64 clipId)
{
    const tracking::StabilizeOutcome& result = m_stabilizer->outcome();
    if (result.cancelled) {
        m_stabStatus->setText(tr("Stabilization cancelled. The clip is unchanged."));
    } else if (!result.data) {
        m_stabStatus->setText(tr("Stabilize failed: %1.").arg(result.error));
    } else if (!m_project || !editops::clipById(m_project->timeline()->data(), clipId)) {
        m_stabStatus->setText(tr("The clip was removed; the result was dropped."));
    } else {
        const auto data = result.data;
        timelineedit::runEdit(m_project, tr("Stabilize"), [&](TimelineData& d) {
            ClipData* c = editops::clipById(d, clipId);
            if (!c)
                return false;
            stabilize::Settings s = c->stabilization.settings;
            s.enabled = true;
            tracking::applyStabilization(*c, s, data);
            return true;
        });
        m_stabStatus->setText(tr("Stabilized."));
    }
    refresh();
}

void ColorTrackerPanel::resetAll()
{
    if (m_session) {
        GradeTrack t;
        const GradeNode* n = m_session->currentNode();
        if (n && n->track != t)
            m_session->setTrack(0, t, tr("Reset Tracker"));
    }
}

} // namespace kk
