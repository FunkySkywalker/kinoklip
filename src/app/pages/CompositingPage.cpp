// SPDX-License-Identifier: GPL-3.0-or-later
// CompositingPage layout and wiring of the comp.* commands.
#include "app/pages/CompositingPage.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "render/CompSaver.h"
#include "core/SelectionModel.h"
#include "core/TransitionPresets.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelInfo.h"
#include "ui/common/PanelSplitter.h"
#include "ui/common/StandardActions.h"
#include "ui/common/TimelineCommandSuspension.h"
#include "core/CompGroups.h"
#include "ui/comp/CompAdapter.h"
#include "ui/comp/MakeMacroDialog.h"
#include "ui/comp/CompInspector.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/CompTransport.h"
#include "ui/comp/CompViewer.h"
#include "ui/comp/NodeEditor.h"
#include "ui/comp/NodeLibrary.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/keyframes/KeyframesPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QHideEvent>
#include <QSettings>
#include <QShowEvent>
#include <QSplitter>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {

// Default sizes for a 1920x1080 window (splitters scale them).
constexpr int kLibraryWidth = 260;
constexpr int kCentreWidth = 1260;
constexpr int kInspectorWidth = 400;
constexpr int kViewersHeight = 470;
constexpr int kNodeAreaHeight = 420;
/// Layout version of the saved splitter states (bump when the structure changes).
constexpr int kLayoutVersion = 2;

QAction* registryAction(const AppContext& ctx, const QString& id)
{
    return ctx.actions && ctx.actions->contains(id) ? ctx.actions->action(id) : nullptr;
}

} // namespace

CompositingPage::CompositingPage(AppContext& ctx, QWidget* parent)
    : Page(parent)
    , m_ctx(ctx)
{
    m_session = new CompSession(ctx, this);

    m_left = new CompViewer(0, ctx, m_session);
    m_right = new CompViewer(1, ctx, m_session);
    // Clear Viewers (the ~ key or button of either viewer) empties both.
    connect(m_left, &CompViewer::clearRequested, m_right, &CompViewer::clearViewer);
    connect(m_right, &CompViewer::clearRequested, m_left, &CompViewer::clearViewer);
    m_viewers = new PanelSplitter(Qt::Horizontal);
    m_viewers->setObjectName(QStringLiteral("CompViewersSplitter"));
    m_viewers->addWidget(m_left);
    m_viewers->addWidget(m_right);
    m_transport = new CompTransport(ctx, m_session);
    setPanelInfo(m_transport, tr("Transport"),
        tr("Play and stop, and a ruler over the frames of the open clip."));
    auto* viewerArea = new QWidget;
    auto* va = new QVBoxLayout(viewerArea);
    va->setContentsMargins(0, 0, 0, 0);
    va->setSpacing(0);
    va->addWidget(m_viewers, 1);
    va->addWidget(m_transport);

    m_editor = new NodeEditor(m_session, ctx.actions);
    // Tile pictures: the node's own image at the playhead, rendered like the viewers do.
    m_editor->setThumbnailSource(
        [this](quint64 nodeId, QSize px) -> std::future<RenderedFramePtr> {
            PlaybackEngine* engine = m_ctx.playback;
            if (!engine || !engine->compositor() || !m_session->isStored() || !m_ctx.project
                || !m_ctx.project->timeline())
                return {};
            const bool out = nodeId == m_session->composition().mediaOutId;
            return engine->renderCompositionNode(m_ctx.project->timeline()->snapshot(),
                m_session->clipId(), out ? 0 : nodeId, m_session->compFrame(), px, OutputImage,
                m_session->transition());
        });
    setPanelInfo(m_editor, tr("Node Editor"),
        tr("The node graph of the open clip: select, move, connect and edit nodes."));
    m_toolBar = buildToolBar();
    m_nodeArea = new QWidget;
    m_nodeArea->setObjectName(QStringLiteral("CompNodeArea"));
    auto* na = new QVBoxLayout(m_nodeArea);
    na->setContentsMargins(0, 0, 0, 0);
    na->setSpacing(0);
    na->addWidget(m_toolBar);
    na->addWidget(m_editor, 1);

    // Spline and Keyframes (interface bar, off by default) sit beside the node editor like
    // Resolve's Fusion page; they show the animated parameters of the selected nodes.
    m_spline = new KeyframesPanel(ctx);
    m_spline->attachComposition(m_session, KeyframesPanel::View::Curves);
    m_keyframes = new KeyframesPanel(ctx);
    m_keyframes->attachComposition(m_session, KeyframesPanel::View::Keyframes);
    m_spline->hide();
    m_keyframes->hide();
    m_nodeRow = new PanelSplitter(Qt::Horizontal);
    m_nodeRow->setObjectName(QStringLiteral("CompNodeRow"));
    m_nodeRow->addWidget(m_nodeArea);
    m_nodeRow->addWidget(m_spline);
    m_nodeRow->addWidget(m_keyframes);
    m_nodeRow->setStretchFactor(0, 3);
    m_nodeRow->setStretchFactor(1, 2);
    m_nodeRow->setStretchFactor(2, 2);
    m_nodeRow->setChildrenCollapsible(false);

    m_centre = new PanelSplitter(Qt::Vertical);
    m_centre->setObjectName(QStringLiteral("CompCentreSplitter"));
    m_centre->addWidget(viewerArea);
    m_centre->addWidget(m_nodeRow);
    m_centre->setStretchFactor(1, 1);
    m_centre->setSizes({kViewersHeight, kNodeAreaHeight});

    m_library = new NodeLibraryPanel(m_session);
    m_mediaPool = new MediaPoolPanel(ctx);
    m_mediaPool->setObjectName(QStringLiteral("CompMediaPool"));
    m_leftColumn = new PanelSplitter(Qt::Vertical);
    m_leftColumn->setObjectName(QStringLiteral("CompLeftColumn"));
    m_leftColumn->addWidget(m_mediaPool);
    m_leftColumn->addWidget(m_library);
    m_inspector = new CompInspector(ctx, m_session);
    m_columns = new PanelSplitter(Qt::Horizontal);
    m_columns->setObjectName(QStringLiteral("CompColumnsSplitter"));
    m_columns->addWidget(m_leftColumn);
    m_columns->addWidget(m_centre);
    m_columns->addWidget(m_inspector);
    m_columns->setStretchFactor(1, 1);
    m_columns->setSizes({kLibraryWidth, kCentreWidth, kInspectorWidth});
    // Toggles (interface bar "Media Pool" / "Effects"), off by default.
    m_mediaPool->hide();
    m_library->hide();
    updateLeftColumn();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_columns);

    m_selectNode = new SelectNodePopup(m_session, this);
    // Disables the hidden Edit page timeline's commands while the page is shown (spec 42b.22).
    new TimelineCommandSuspension(this, ctx.actions, keptCommandIds());
    connect(m_editor, &NodeEditor::viewChanged, m_session, &CompSession::setViewState);
    connect(m_editor, &NodeEditor::makeMacroRequested, this, &CompositingPage::makeMacro);
    connect(m_editor, &NodeEditor::saveMacroRequested, this, &CompositingPage::saveMacro);
    connect(m_inspector, &CompInspector::makeMacroRequested, this, &CompositingPage::makeMacro);
    connect(m_inspector, &CompInspector::saveMacroRequested, this, &CompositingPage::saveMacro);
    connectActions();
}

CompositingPage::~CompositingPage() = default;

void CompositingPage::makeMacro(quint64 group)
{
    const CompNode* g = m_session->composition().node(group);
    if (!g || !compgroups::isGroup(*g))
        return;
    auto* dialog = new MakeMacroDialog(m_session->composition(), group, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, group, dialog] {
        m_session->makeMacro(group, dialog->macroName(), dialog->picks());
    });
    dialog->open();
}

void CompositingPage::saveMacro(quint64 group)
{
    const CompNode* g = m_session->composition().node(group);
    if (!g || !compgroups::isGroup(*g))
        return;
    auto* dialog = new QInputDialog(this);
    dialog->setObjectName(QStringLiteral("SaveMacroDialog"));
    dialog->setWindowTitle(tr("Save Macro as Title"));
    dialog->setLabelText(tr("Name of the title in the Effects Library"));
    dialog->setTextValue(g->name);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QInputDialog::accepted, this, [this, group, dialog] {
        QString error;
        if (m_session->saveMacro(group, dialog->textValue(), &error).isEmpty())
            QMessageBox::warning(this, tr("Save Macro as Title"), error);
    });
    dialog->open();
}

QWidget* CompositingPage::buildToolBar()
{
    // A QToolBar: the buttons that do not fit move, in order, into its "»" menu, so they never
    // overlap however narrow the node row is (Spline / Keyframes beside it at 1366 px).
    auto* bar = new QToolBar;
    bar->setObjectName(QStringLiteral("CompNodeToolBar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);
    // However many tools there are, the row takes the width it is given (extra buttons fold
    // into "»") and never widens the panels beside the node editor.
    bar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    bar->layout()->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceXs, metrics::kSpaceM, metrics::kSpaceXs);
    bar->layout()->setSpacing(metrics::kSpaceXs);
    for (const QString& type : compadapter::toolbarTypes()) {
        const compadapter::NodeTypeInfo* info = compadapter::nodeType(type);
        auto* b = new QToolButton(bar);
        b->setObjectName(QStringLiteral("CompTool:") + type);
        b->setText(info->name);
        b->setToolTip(tr("Add %1 after the selected node").arg(info->name));
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QToolButton::clicked, this, [this, type] {
            m_session->addNode(type);
            m_editor->setFocus();
        });
        bar->addWidget(b);
    }
    bar->addSeparator();
    m_renderSavers = new QToolButton(bar);
    m_renderSavers->setObjectName(QStringLiteral("CompRenderSavers"));
    m_renderSavers->setText(tr("Render Savers"));
    m_renderSavers->setToolTip(
        tr("Renders every frame of the open composition and writes the pictures of its Saver nodes to their image files. Playing and scrubbing never write files"));
    m_renderSavers->setAutoRaise(true);
    m_renderSavers->setFocusPolicy(Qt::NoFocus);
    connect(m_renderSavers, &QToolButton::clicked, this, [this] { renderSavers(); });
    bar->addWidget(m_renderSavers);
    m_savePreset = new QToolButton(bar);
    m_savePreset->setObjectName(QStringLiteral("CompSaveTransitionPreset"));
    m_savePreset->setText(tr("Save Transition Preset"));
    m_savePreset->setToolTip(
        tr("Saves the open transition's node graph in the Effects Library, Video Transitions, User Presets, where it drags onto cuts like any transition"));
    m_savePreset->setAutoRaise(true);
    m_savePreset->setFocusPolicy(Qt::NoFocus);
    m_savePreset->setEnabled(false);
    connect(m_savePreset, &QToolButton::clicked, this, [this] { saveTransitionPreset(); });
    connect(m_session, &CompSession::clipChanged, this,
        [this] { m_savePreset->setEnabled(m_session->transition() != 0); });
    bar->addWidget(m_savePreset);
    return bar;
}

void CompositingPage::saveTransitionPreset()
{
    if (!m_session->transition())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save Transition Preset"),
        tr("Name of the preset:"), QLineEdit::Normal, tr("My Transition"), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    if (QFileInfo::exists(transitionpresets::pathFor(name))
        && QMessageBox::question(this, tr("Save Transition Preset"),
               tr("A preset named \"%1\" exists. Replace it?").arg(name))
            != QMessageBox::Yes)
        return;
    transitionpresets::Preset preset;
    preset.name = name;
    preset.duration = std::max<int64_t>(1, m_session->compFrameCount());
    preset.composition = m_session->composition();
    QString error;
    if (transitionpresets::save(preset, &error).isEmpty())
        QMessageBox::warning(this, tr("Save Transition Preset"), tr("Cannot save the preset: %1").arg(error));
}

void CompositingPage::renderSavers()
{
    PlaybackEngine* engine = m_ctx.playback;
    const quint64 clipId = m_session->clipId();
    if (!engine || !engine->compositor() || !clipId)
        return;
    const int transition = m_session->transition();
    const auto snapshot = m_ctx.project->timeline()->snapshot();
    const ClipData* clip = editops::clipById(*snapshot, clipId);
    // A clip's graph, or the graph of the open transition (its frames are the transition window).
    const std::optional<Composition>* graph = nullptr;
    int64_t frames = 0;
    if (clip && transition) {
        const std::optional<TransitionData>& slot = transition == 1 ? clip->transitionIn : clip->transitionOut;
        graph = slot ? &slot->composition : nullptr;
        frames = slot ? slot->duration : 0;
    } else if (clip) {
        graph = &clip->composition;
        frames = clip->duration;
    }
    if (!graph || !graph->has_value() || render::compsaver::activeSavers(**graph).isEmpty()) {
        QMessageBox::information(this, tr("Render Savers"),
            tr("The composition has no Saver node with a file name that is switched on."));
        return;
    }
    QProgressDialog progress(tr("Writing the Saver images…"), tr("Cancel"), 0, 100, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    const render::compsaver::Result result = render::compsaver::run(*engine->compositor(), snapshot,
        clipId, 0, std::max<int64_t>(0, frames - 1),
        [&progress](int done, int total) {
            progress.setMaximum(std::max(1, total));
            progress.setValue(done);
            QCoreApplication::processEvents();
            return !progress.wasCanceled();
        },
        m_ctx.playback->mediaPaths(), transition);
    progress.close();
    QString text = tr("%1 image(s) written.").arg(result.files);
    if (result.cancelled)
        text += QLatin1Char(' ') + tr("Cancelled.");
    if (!result.errors.isEmpty())
        text += QLatin1String("\n") + result.errors.mid(0, 5).join(QLatin1Char('\n'));
    QMessageBox::information(this, tr("Render Savers"), text);
}

QToolButton* CompositingPage::toolButton(const QString& type) const
{
    return m_toolBar->findChild<QToolButton*>(QStringLiteral("CompTool:") + type);
}

void CompositingPage::connectActions()
{
    auto on = [this](const char* id, auto handler) {
        if (QAction* a = registryAction(m_ctx, QLatin1String(id)))
            connect(a, &QAction::triggered, this, [this, handler] {
                if (isVisible())
                    handler();
            });
    };
    CompSession* s = m_session;
    // Edit > Compositing Page is in every page's menu bar: its commands are greyed out until
    // the page is shown, like the Cut and Color page commands (spec 42b.32).
    setNodeCommandsEnabled(false);
    on("comp.select_node", [this] { showSelectNode(); });
    on("comp.add_note", [this, s] { s->addNote(m_editor->pastePosition()); });
    on("comp.cut", [s] { s->cutSelection(); });
    on("comp.copy", [s] { s->copySelection(); });
    on("comp.paste", [this, s] { s->paste(m_editor->pastePosition()); });
    on("comp.duplicate", [s] { s->duplicateSelection(); });
    on("comp.delete", [s] { s->deleteSelection(); });
    on("comp.select_all", [s] { s->selectAll(); });
    // The generic Edit commands act on the nodes while the node editor has focus (the
    // timeline leaves them alone then): the Streamlined menu has no node copies of them.
    auto onEdit = [this](const char* id, auto handler) {
        if (QAction* a = registryAction(m_ctx, QLatin1String(id)))
            connect(a, &QAction::triggered, this, [this, handler] {
                if (isVisible() && nodeEditorHasFocus())
                    handler();
            });
    };
    onEdit("edit.cut", [s] { s->cutSelection(); });
    onEdit("edit.copy", [s] { s->copySelection(); });
    onEdit("edit.paste", [this, s] { s->paste(m_editor->pastePosition()); });
    onEdit("edit.delete", [s] { s->deleteSelection(); });
    onEdit("edit.select_all", [s] { s->selectAll(); });
    // The page suspends the timeline's commands (TimelineCommandSuspension); these five are
    // switched on while the node editor has focus, so Edit > Cut ... Select All work there.
    connect(qApp, &QApplication::focusChanged, this, [this] {
        if (!isVisible())
            return;
        const bool focused = nodeEditorHasFocus();
        for (const char* id :
            {"edit.cut", "edit.copy", "edit.paste", "edit.delete", "edit.select_all"}) {
            if (QAction* a = registryAction(m_ctx, QLatin1String(id)))
                a->setEnabled(focused);
        }
    });
    on("comp.rename", [this, s] {
        if (s->currentNode())
            m_editor->startRename(s->currentNode());
    });
    on("comp.pass_through", [s] { s->togglePassThrough(); });
    on("comp.lock", [s] { s->toggleLock(); });
    on("comp.swap_inputs", [s] { s->swapInputs(); });
    on("comp.view_left", [s] { s->setViewerNode(0, s->currentNode()); });
    on("comp.view_right", [s] { s->setViewerNode(1, s->currentNode()); });
    on("comp.fit", [this] { m_editor->fitToView(); });
    if (QAction* snap = registryAction(m_ctx, QStringLiteral("comp.snap_grid"))) {
        snap->setChecked(m_session->snapToGrid());
        connect(snap, &QAction::toggled, m_session, &CompSession::setSnapToGrid);
    }
    // The checkable commands show the current node's state.
    auto syncChecks = [this] {
        const CompNode* n = m_session->composition().node(m_session->currentNode());
        if (QAction* a = registryAction(m_ctx, QStringLiteral("comp.pass_through")))
            a->setChecked(n && !n->enabled);
        if (QAction* a = registryAction(m_ctx, QStringLiteral("comp.lock")))
            a->setChecked(n && n->locked);
    };
    connect(m_session, &CompSession::selectionChanged, this, syncChecks);
    connect(m_session, &CompSession::compositionChanged, this, syncChecks);

    // Clip > Reset Composition (Edit page timeline menu too): the selected clips.
    if (QAction* reset = registryAction(m_ctx, QStringLiteral("clip.reset_composition"))) {
        connect(reset, &QAction::triggered, this, [this] {
            QVector<quint64> clips
                = m_ctx.selection ? m_ctx.selection->clips() : QVector<quint64> {};
            if (isVisible() && m_session->clipId())
                clips = {m_session->clipId()};
            compadapter::resetCompositions(*m_ctx.project, clips);
        });
    }
}

void CompositingPage::showSelectNode()
{
    if (!m_session->clipId())
        return;
    const QPoint local = m_editor->mapFromGlobal(QCursor::pos());
    const QPoint at = m_editor->rect().contains(local)
        ? QCursor::pos()
        : m_editor->mapToGlobal(m_editor->rect().center());
    m_selectNode->popup(at);
}

void CompositingPage::openClip(quint64 clipId)
{
    m_session->openClip(clipId ? clipId : m_session->clipForPage());
}

void CompositingPage::openTransition(quint64 clipId, bool tail)
{
    if (isVisible()) {
        m_session->openTransition(clipId, tail);
        return;
    }
    m_pendingTransitionClip = clipId;
    m_pendingTransitionTail = tail;
}

QStringList CompositingPage::keptCommandIds()
{
    // Besides TimelineCommandSuspension::alwaysKept(): the page's own clip commands and the
    // in / out marks it shows on its time ruler.
    // The keyframe commands of the Spline and Keyframes panels edit the open clip's nodes.
    QStringList ids {QStringLiteral("clip.open_in_compositing"),
        QStringLiteral("clip.reset_composition"), QStringLiteral("mark.go_to_in"),
        QStringLiteral("mark.go_to_out"), QStringLiteral("clip.keyframe_fit"),
        QStringLiteral("clip.delete_keyframe"), QStringLiteral("clip.copy_keyframes"),
        QStringLiteral("clip.paste_keyframes")};
    for (const auto& a : keyframecommands::splineActions())
        ids << QLatin1String(a.id);
    for (const auto& a : keyframecommands::easingActions())
        ids << QLatin1String(a.id);
    return ids;
}

QStringList CompositingPage::suspendedCommandIds(const ActionRegistry& registry)
{
    return TimelineCommandSuspension::commandIds(registry, keptCommandIds());
}

void CompositingPage::showEvent(QShowEvent* event)
{
    Page::showEvent(event);
    if (event->spontaneous())
        return;
    setNodeCommandsEnabled(true);
    if (m_pendingTransitionClip) {
        m_session->openTransition(m_pendingTransitionClip, m_pendingTransitionTail);
        m_pendingTransitionClip = 0;
    } else {
        openClip(); // the timeline commands are already suspended (TimelineCommandSuspension)
    }
    // The node editor's keys act while it has the focus (1 / 2, Shift+Space, Delete right
    // after Shift+5, as in Resolve): when the page that had the focus went away, it takes it.
    QTimer::singleShot(0, this, [this] {
        const QWidget* focus = QApplication::focusWidget();
        const bool elsewhereInWindow
            = focus && focus->window() == window() && focus != this && !isAncestorOf(focus);
        if (isVisible() && m_editor->isVisible()
            && (!focus || !focus->isVisible() || elsewhereInWindow))
            m_editor->setFocus(Qt::OtherFocusReason);
    });
}

void CompositingPage::hideEvent(QHideEvent* event)
{
    Page::hideEvent(event);
    if (!event->spontaneous())
        setNodeCommandsEnabled(false);
}

void CompositingPage::setNodeCommandsEnabled(bool enabled)
{
    if (!m_ctx.actions)
        return;
    for (const QString& id : m_ctx.actions->menuEntries(QStringLiteral("comp"))) {
        if (m_ctx.actions->contains(id))
            m_ctx.actions->action(id)->setEnabled(enabled);
    }
}

// ---- Panels and layout ----

bool CompositingPage::hasPanel(PanelId id) const
{
    return id == PanelId::MediaPool || id == PanelId::Effects || id == PanelId::Nodes
        || id == PanelId::Inspector || id == PanelId::Spline || id == PanelId::Keyframes;
}

void CompositingPage::setPanelVisible(PanelId id, bool visible)
{
    QWidget* panel = id == PanelId::MediaPool ? static_cast<QWidget*>(m_mediaPool)
        : id == PanelId::Effects              ? static_cast<QWidget*>(m_library)
        : id == PanelId::Nodes                ? m_nodeArea
        : id == PanelId::Spline               ? static_cast<QWidget*>(m_spline)
        : id == PanelId::Keyframes            ? static_cast<QWidget*>(m_keyframes)
        : id == PanelId::Inspector            ? static_cast<QWidget*>(m_inspector)
                                              : nullptr;
    if (!panel || panel->isHidden() == !visible)
        return;
    panel->setVisible(visible);
    updateLeftColumn();
    updateNodeRow();
    emit panelVisibilityChanged(id, visible);
}

void CompositingPage::updateNodeRow()
{
    const bool any = !m_nodeArea->isHidden() || !m_spline->isHidden() || !m_keyframes->isHidden();
    m_nodeRow->setVisible(any);
    const bool curves = !m_spline->isHidden() || !m_keyframes->isHidden();
    // After the layout has seen the shown panel: widths 3 : 3 : 3 of the visible parts, and
    // the node row as tall as the viewers (curves need height) while Spline / Keyframes show.
    QTimer::singleShot(0, this, [this, curves] {
        const int total = std::max(1, m_nodeRow->width());
        const int weights[] = {m_nodeArea->isHidden() ? 0 : 3, m_spline->isHidden() ? 0 : 3,
            m_keyframes->isHidden() ? 0 : 3};
        const int sum = weights[0] + weights[1] + weights[2];
        if (sum > 0)
            m_nodeRow->setSizes(
                {total * weights[0] / sum, total * weights[1] / sum, total * weights[2] / sum});
        if (curves) {
            const int h = m_centre->height();
            m_centre->setSizes({h * 2 / 5, h * 3 / 5});
        }
    });
}

void CompositingPage::updateLeftColumn()
{
    m_leftColumn->setVisible(!m_mediaPool->isHidden() || !m_library->isHidden());
}

bool CompositingPage::isPanelVisible(PanelId id) const
{
    switch (id) {
    case PanelId::MediaPool:
        return !m_mediaPool->isHidden();
    case PanelId::Effects:
        return !m_library->isHidden();
    case PanelId::Nodes:
        return !m_nodeArea->isHidden();
    case PanelId::Spline:
        return !m_spline->isHidden();
    case PanelId::Keyframes:
        return !m_keyframes->isHidden();
    case PanelId::Inspector:
        return !m_inspector->isHidden();
    default:
        return false;
    }
}

void CompositingPage::saveLayout(QSettings& settings) const
{
    settings.setValue(QStringLiteral("layoutVersion"), kLayoutVersion);
    settings.setValue(QStringLiteral("columns"), m_columns->saveState());
    settings.setValue(QStringLiteral("centre"), m_centre->saveState());
    settings.setValue(QStringLiteral("nodeRow"), m_nodeRow->saveState());
    settings.setValue(QStringLiteral("viewers"), m_viewers->saveState());
    settings.setValue(QStringLiteral("leftColumn"), m_leftColumn->saveState());
    m_mediaPool->saveSplitterSizes(settings, QStringLiteral("mediaPoolPanel/"));
    for (PanelId id : {PanelId::MediaPool, PanelId::Effects, PanelId::Nodes, PanelId::Inspector,
             PanelId::Spline, PanelId::Keyframes})
        settings.setValue(QStringLiteral("visible/") + panelIdToString(id), isPanelVisible(id));
}

void CompositingPage::restoreLayout(const QSettings& settings)
{
    if (settings.value(QStringLiteral("layoutVersion"), 0).toInt() == kLayoutVersion) {
        m_columns->restoreState(settings.value(QStringLiteral("columns")).toByteArray());
        m_centre->restoreState(settings.value(QStringLiteral("centre")).toByteArray());
        m_nodeRow->restoreState(settings.value(QStringLiteral("nodeRow")).toByteArray());
        m_viewers->restoreState(settings.value(QStringLiteral("viewers")).toByteArray());
        m_leftColumn->restoreState(settings.value(QStringLiteral("leftColumn")).toByteArray());
    }
    m_mediaPool->restoreSplitterSizes(settings, QStringLiteral("mediaPoolPanel/"));
    // Splitter states carry visibility too; the explicit flags win.
    for (PanelId id : {PanelId::MediaPool, PanelId::Effects, PanelId::Nodes, PanelId::Inspector,
             PanelId::Spline, PanelId::Keyframes}) {
        const QString key = QStringLiteral("visible/") + panelIdToString(id);
        if (settings.contains(key))
            setPanelVisible(id, settings.value(key).toBool());
    }
}

} // namespace kk
