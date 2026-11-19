// SPDX-License-Identifier: GPL-3.0-or-later
// AudioPage layout and wiring.
#include "app/pages/AudioPage.h"

#include "core/EffectRegistry.h"
#include "core/MixerOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/audio/AutomationBar.h"
#include "ui/audio/AnalyzerPanel.h"
#include "ui/audio/LoudnessPanel.h"
#include "ui/audio/MeterModel.h"
#include "ui/audio/MixerController.h"
#include "ui/audio/MixerPanel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelInfo.h"
#include "ui/common/PanelSplitter.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QHideEvent>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

namespace kk {

namespace {

// Default sizes for a 1920x1080 window (splitters scale them).
constexpr int kViewerWidth = 420;
constexpr int kMetersWidth = 1000;
constexpr int kTopRowHeight = 250;
constexpr int kBottomRowHeight = 710;
constexpr int kTimelineWidth = 1080;
constexpr int kMixerWidth = 840;
constexpr int kAnalyzerWidth = 760;
const QSize kFloatingViewerSize(480, 300);
constexpr int kEffectsWidth = 320;
constexpr int kInspectorWidth = 380;
constexpr int kCentreWidth = 2300;

/// Page-only commands (enabled while the Audio page is shown).
constexpr const char* kPageActions[] = {"audio.float_viewer"};

QAction* registryAction(const AppContext& ctx, const char* id)
{
    return ctx.actions && ctx.actions->contains(QLatin1String(id))
        ? ctx.actions->action(QLatin1String(id))
        : nullptr;
}

} // namespace

AudioPage::AudioPage(AppContext& ctx, TimelineController* controller, QWidget* parent)
    : Page(parent)
    , m_ctx(ctx)
{
    m_mixerController = new MixerController(ctx, this);
    m_mixerController->setWritePassesEnabled(false); // on while the page is shown
    m_meterModel = new MeterModel(this);
    m_meterModel->setEngine(ctx.playback);

    m_viewer = new ViewerPanel(ViewerPanel::Mode::Timeline, ctx);
    m_viewer->setObjectName(QStringLiteral("AudioViewer"));
    m_viewerDock = new QWidget;
    auto* dockLayout = new QVBoxLayout(m_viewerDock);
    dockLayout->setContentsMargins(0, 0, 0, 0);
    dockLayout->addWidget(m_viewer);
    m_meters = new LoudnessPanel(ctx, m_mixerController, m_meterModel);

    m_timeline = new TimelineWidget(ctx, controller);
    setPanelInfo(m_timeline, tr("Audio Timeline"),
        tr("The timeline with tall audio tracks, level lines and fade handles."));
    m_timeline->setObjectName(QStringLiteral("AudioTimeline"));
    TimelineViewOptions options;
    options.audioHeightScale = kAudioRowScale;
    options.videoHeightScale = kVideoRowScale;
    options.rowsFromTop = true; // audio tracks from the top, under a thin strip of video
    options.audioPageHeaders = true;
    options.keepsViewState = false; // the Edit page's view owns the per-timeline zoom
    options.doubleClickOpensSource = false;
    options.headerMeter = [this](quint64 trackId) {
        const MeterReading r = m_meterModel->track(trackId);
        return std::pair<float, float> {r.levelDb[0], r.mono ? r.levelDb[0] : r.levelDb[1]};
    };
    m_timeline->setViewOptions(options);
    m_timeline->setProperty("kkProgramFocus", true);
    // The header meters repaint with the meters (only the header column).
    connect(m_meterModel, &MeterModel::changed, this, [this] {
        m_timeline->viewport()->update(
            QRect(0, 0, timelinemetrics::kHeaderWidth, m_timeline->viewport()->height()));
    });

    m_mixer = new MixerPanel(ctx, m_mixerController, m_meterModel);

    m_topRow = new PanelSplitter(Qt::Horizontal);
    m_topRow->addWidget(m_viewerDock);
    m_topRow->addWidget(m_meters);
    m_analyzer = new AnalyzerPanel(ctx, m_mixerController, m_meterModel);
    m_topRow->addWidget(m_analyzer);
    m_topRow->setStretchFactor(1, 1);
    m_topRow->setStretchFactor(2, 1);
    m_topRow->setSizes({kViewerWidth, kMetersWidth, kAnalyzerWidth});

    m_bottomRow = new PanelSplitter(Qt::Horizontal);
    m_bottomRow->addWidget(m_timeline);
    m_bottomRow->addWidget(m_mixer);
    m_bottomRow->setStretchFactor(0, 1);
    m_bottomRow->setSizes({kTimelineWidth, kMixerWidth});

    m_main = new PanelSplitter(Qt::Vertical);
    m_main->addWidget(m_topRow);
    // The Automation toolbar sits above the timeline and the mixer.
    auto* bottom = new QWidget;
    auto* bottomLayout = new QVBoxLayout(bottom);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(0);
    m_automationBar = new AutomationBar(m_mixerController);
    bottomLayout->addWidget(m_automationBar);
    bottomLayout->addWidget(m_bottomRow, 1);
    m_main->addWidget(bottom);
    m_main->setStretchFactor(1, 1);
    m_main->setSizes({kTopRowHeight, kBottomRowHeight});

    // Effects Library (Audio FX) and Inspector: columns left and right, hidden until toggled.
    m_effects = new EffectsLibraryPanel(ctx);
    m_effects->selectCategory(int(EffectCategory::AudioEffect));
    m_inspector = new InspectorPanel(ctx);
    m_effects->hide();
    m_inspector->hide();
    m_outer = new PanelSplitter(Qt::Horizontal);
    m_outer->addWidget(m_effects);
    m_outer->addWidget(m_main);
    m_outer->addWidget(m_inspector);
    m_outer->setStretchFactor(1, 1);
    m_outer->setSizes({kEffectsWidth, kCentreWidth, kInspectorWidth});

    // A clicked strip shows its track's effects in the Inspector; a double-clicked slot opens
    // its controls there.
    connect(m_mixer, &MixerPanel::stripSelected, this, [this](const MixTarget& target) {
        if (!target.isBus())
            m_inspector->showTrack(target.id);
    });
    connect(m_mixer, &MixerPanel::effectOpenRequested, this,
        [this](const MixTarget& target, quint64 effectId) {
            setPanelVisible(PanelId::Inspector, true);
            m_inspector->inspectTrackEffect(target.id, effectId);
        });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_outer);

    connectActions();
    setPageActionsEnabled(false);
}

AudioPage::~AudioPage()
{
    delete m_viewerWindow; // a floating viewer is not a child of the page
}

void AudioPage::connectActions()
{
    if (QAction* add = registryAction(m_ctx, "audio.add_bus")) {
        connect(add, &QAction::triggered, this, [this] {
            m_mixerController->edit(tr("Add Bus"), [](TimelineData& d) {
                mixerops::addBus(d, false);
                return true;
            });
        });
    }
    if (QAction* reset = registryAction(m_ctx, "audio.reset_loudness")) {
        connect(reset, &QAction::triggered, this, [this] {
            if (m_ctx.playback)
                m_ctx.playback->resetLoudness();
        });
    }
    if (QAction* floating = registryAction(m_ctx, "audio.float_viewer")) {
        connect(floating, &QAction::triggered, this, [this](bool on) {
            if (isVisible())
                setViewerFloating(on);
        });
    }
}

void AudioPage::setPageActionsEnabled(bool enabled)
{
    for (const char* id : kPageActions) {
        if (QAction* a = registryAction(m_ctx, id))
            a->setEnabled(enabled);
    }
}

void AudioPage::setViewerFloating(bool floating)
{
    if (floating == isViewerFloating())
        return;
    if (floating) {
        if (!m_viewerWindow) {
            m_viewerWindow = new QWidget(window(), Qt::Tool);
            m_viewerWindow->setObjectName(QStringLiteral("AudioViewerWindow"));
            m_viewerWindow->setWindowTitle(tr("Viewer"));
            auto* l = new QVBoxLayout(m_viewerWindow);
            l->setContentsMargins(0, 0, 0, 0);
            m_viewerWindow->resize(kFloatingViewerSize);
            // Shortcuts are window-scoped: the transport keys work in the floating viewer.
            m_viewerWindow->addActions(window()->actions());
        }
        m_viewerWindow->layout()->addWidget(m_viewer);
        m_viewerDock->hide();
        m_viewerWindow->show();
    } else {
        m_viewerDock->layout()->addWidget(m_viewer);
        m_viewerDock->show();
        if (m_viewerWindow)
            m_viewerWindow->hide();
    }
    if (QAction* a = registryAction(m_ctx, "audio.float_viewer"))
        a->setChecked(floating);
    m_viewer->activate();
}

bool AudioPage::isViewerFloating() const
{
    return m_viewerWindow && m_viewer->parentWidget() == m_viewerWindow;
}

void AudioPage::showEvent(QShowEvent* event)
{
    Page::showEvent(event);
    setPageActionsEnabled(true);
    m_mixerController->setWritePassesEnabled(true);
    m_viewer->activate(); // the transport keys drive the program here
    if (isViewerFloating())
        m_viewerWindow->show();
}

void AudioPage::hideEvent(QHideEvent* event)
{
    Page::hideEvent(event);
    if (event->spontaneous())
        return; // minimised: keep the state
    setPageActionsEnabled(false);
    m_mixerController->setWritePassesEnabled(false);
    if (m_viewerWindow)
        m_viewerWindow->hide();
}

QWidget* AudioPage::panelWidget(PanelId id) const
{
    switch (id) {
    case PanelId::Mixer:
        return m_mixer;
    case PanelId::Effects:
        return m_effects;
    case PanelId::Inspector:
        return m_inspector;
    default:
        return nullptr;
    }
}

void AudioPage::setPanelVisible(PanelId id, bool visible)
{
    QWidget* panel = panelWidget(id);
    if (!panel || panel->isHidden() == !visible)
        return;
    panel->setVisible(visible);
    emit panelVisibilityChanged(id, visible);
}

bool AudioPage::isPanelVisible(PanelId id) const
{
    const QWidget* panel = panelWidget(id);
    return panel && !panel->isHidden();
}

void AudioPage::saveLayout(QSettings& settings) const
{
    settings.setValue(QStringLiteral("outer"), m_outer->saveState());
    settings.setValue(QStringLiteral("main"), m_main->saveState());
    settings.setValue(QStringLiteral("topRow"), m_topRow->saveState());
    settings.setValue(QStringLiteral("bottomRow"), m_bottomRow->saveState());
    for (PanelId id : {PanelId::Mixer, PanelId::Effects, PanelId::Inspector})
        settings.setValue(QStringLiteral("visible/") + panelIdToString(id), isPanelVisible(id));
}

void AudioPage::restoreLayout(const QSettings& settings)
{
    m_outer->restoreState(settings.value(QStringLiteral("outer")).toByteArray());
    m_main->restoreState(settings.value(QStringLiteral("main")).toByteArray());
    m_topRow->restoreState(settings.value(QStringLiteral("topRow")).toByteArray());
    m_bottomRow->restoreState(settings.value(QStringLiteral("bottomRow")).toByteArray());
    // Splitter states carry visibility too; the explicit flags win.
    for (PanelId id : {PanelId::Mixer, PanelId::Effects, PanelId::Inspector}) {
        const QString key = QStringLiteral("visible/") + panelIdToString(id);
        if (settings.contains(key))
            setPanelVisible(id, settings.value(key).toBool());
    }
}

} // namespace kk
