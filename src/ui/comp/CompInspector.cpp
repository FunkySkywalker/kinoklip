// SPDX-License-Identifier: GPL-3.0-or-later
// CompInspector implementation: the stand-in clip, the header and the edit protocol.
#include "ui/comp/CompInspector.h"

#include "compositing/tracking/TrackFollow.h"
#include "compositing/tracking/TrackLink.h"
#include "core/CompGroups.h"
#include "core/MediaPool.h"
#include "core/Modifiers.h"
#include "core/NodeRegistry.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "platform/FileDialogs.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelInfo.h"
#include "ui/common/VimRowOps.h"
#include "ui/comp/CompAdapter.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/TrackerPanel.h"
#include "ui/inspector/InspectorGlyphs.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace kk {

CompInspector::CompInspector(AppContext& ctx, CompSession* session, QWidget* parent)
    : PanelFrame(tr("Inspector"), parent)
    , m_ctx(ctx)
    , m_session(session)
{
    markVimForm(this);
    setPanelInfo(this, tr("Inspector"),
        tr("Parameters of the selected node, with an on/off switch and Reset."));
    setObjectName(QStringLiteral("CompInspector"));
    m_proxy.id = kProxyClipId;
    m_proxy.kind = ClipKind::Generator;

    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header: name | enable | reset.
    m_header = new QWidget;
    m_header->setObjectName(QStringLiteral("CompInspectorHeader"));
    auto* header = new QHBoxLayout(m_header);
    header->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceS, metrics::kSpaceM, metrics::kSpaceS);
    m_enable = new QCheckBox;
    m_enable->setObjectName(QStringLiteral("CompNodeEnable"));
    m_enable->setToolTip(tr("Enabled (off: pass-through, the main input goes straight through)"));
    m_name = new QLabel;
    m_name->setObjectName(QStringLiteral("CompNodeName"));
    QFont bold = m_name->font();
    bold.setBold(true);
    m_name->setFont(bold);
    m_reset = new QToolButton;
    m_reset->setObjectName(QStringLiteral("CompNodeReset"));
    m_reset->setIcon(inspectorGlyph(InspectorGlyph::Reset));
    m_reset->setToolTip(tr("Reset all parameters of the node"));
    m_reset->setAutoRaise(true);
    header->addWidget(m_enable);
    header->addWidget(m_name, 1);
    header->addWidget(m_reset);
    layout->addWidget(m_header);

    m_scroll = new QScrollArea;
    m_scroll->setObjectName(QStringLiteral("InspectorScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(m_scroll, 1);
    m_empty = createPlaceholderContent(tr("Select a node to see its parameters"));
    layout->addWidget(m_empty, 1);
    setContentWidget(body);
    m_tracker = new TrackerPanel(ctx, session, this);
    m_tracker->hide();

    connect(m_enable, &QCheckBox::clicked, this, [this](bool on) {
        if (m_session && m_node)
            m_session->setNodeEnabled(m_node, on);
    });
    connect(m_reset, &QToolButton::clicked, this, [this] {
        if (!m_session || !m_node)
            return;
        const quint64 id = m_node;
        const CompNode* n = m_session->composition().node(id);
        m_session->edit(tr("Reset %1").arg(n ? n->name : QString()), [id](Composition& c) {
            CompNode* node = c.node(id);
            const compadapter::NodeTypeInfo* info
                = node ? compadapter::nodeType(node->type) : nullptr;
            if (!info || node->locked)
                return false;
            if (node->macro) { // a macro's controls go back to the values they have by default
                for (const ParamDescriptor& d : compgroups::controlDescriptors(c, *node))
                    node->params.setBase(d.key, d.def);
                return true;
            }
            node->params = {};
            for (const ParamDescriptor& p : info->params) {
                if (p.isScalar())
                    node->params.setBase(p.key, p.def);
                else
                    node->params.setStatic(p.key, p.defStatic);
            }
            return true;
        });
    });
    connect(session, &CompSession::selectionChanged, this, &CompInspector::rebuild);
    connect(session, &CompSession::clipChanged, this, &CompInspector::rebuild);
    connect(session, &CompSession::compositionChanged, this, &CompInspector::rebuild);
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, &CompInspector::refreshRows);
    rebuild();
}

void CompInspector::syncProxy()
{
    const CompNode* n = m_session ? m_session->composition().node(m_node) : nullptr;
    m_proxy.duration = std::max<int64_t>(1, m_session ? m_session->compFrameCount() : 1);
    m_proxy.effects.clear();
    if (!n)
        return;
    EffectInstance fx;
    fx.id = n->id;
    fx.type = n->type;
    fx.enabled = n->enabled;
    fx.params = n->params;
    m_proxy.effects.append(fx);
}

void CompInspector::rebuild()
{
    const quint64 node = m_session ? m_session->currentNode() : 0;
    const CompNode* n = m_session ? m_session->composition().node(node) : nullptr;
    const QString type = n ? n->type : QString();
    syncProxy();
    // The Modifiers sections follow the modifiers (added, removed, connected elsewhere).
    QString signature;
    if (n) {
        signature += QString::number(n->instanceOf) + QLatin1Char('#') + (n->macro ? QLatin1Char('M') : QLatin1Char('-'));
        for (const MacroControl& c : n->controls)
            signature += c.key + c.label + QString::number(c.node) + QLatin1Char(';');
        signature += QString::number(compgroups::members(m_session->composition(), n->id, false).size());
        for (const QString& key : modifiers::modifiedKeys(n->params)) {
            for (const ParamModifier& m : n->params.values.value(key).modifiers)
                signature += key + QLatin1Char('/') + m.id + QLatin1Char('/') + modifierKindId(m.kind)
                    + QLatin1Char('/') + QString::number(m.targetNode) + m.targetKey
                    + (m.enabled ? QLatin1Char('+') : QLatin1Char('-')) + QLatin1Char(';');
        }
    }
    if (node != m_node || type != m_nodeType || (n && !m_group) || signature != m_modSignature) {
        m_modSignature = signature;
        m_node = n ? node : 0;
        m_nodeType = type;
        m_proxy.effects.clear();
        syncProxy();
        m_tracker->setParent(this); // survives the rebuild: a track may be running
        m_tracker->hide();
        delete m_scroll->takeWidget();
        m_group = nullptr;
        m_groupSection = nullptr;
        m_modifierBoxes.clear();
        m_settings = nullptr;
        m_source = nullptr;
        m_audioMode = nullptr;
        m_audioGroup = nullptr;
        m_soundOffset = nullptr;
        m_follow = nullptr;
        m_followButton = nullptr;
        m_followPosition = m_followSize = m_followAngle = m_followPoints = nullptr;
        if (n) {
            const compadapter::NodeTypeInfo* info = compadapter::nodeType(n->type);
            m_group = new ParamGroupBox(info ? info->name : n->type);
            m_group->setObjectName(QStringLiteral("CompNodeParams"));
            QVector<ParamDescriptor> descs;
            if (info)
                descs = info->params;
            if (n->macro) // a macro shows its controls instead of parameters of its own
                descs = compgroups::controlDescriptors(m_session->composition(), *n);
            if (info) {
                for (const ParamDescriptor& desc : descs) {
                    if (desc.hidden)
                        continue;
                    ParamRef ref;
                    ref.clipId = kProxyClipId;
                    ref.scope = ParamScope::Effect;
                    ref.effectId = n->id;
                    ref.key = desc.key;
                    m_group->addParam(ref, desc, *this);
                }
            }
            auto* holder = new QWidget;
            auto* l = new QVBoxLayout(holder);
            l->setContentsMargins(0, 0, 0, 0);
            if (n->type == nodetypes::Tracker || n->type == nodetypes::PlanarTracker) {
                m_tracker->setNode(n->id);
                l->addWidget(m_tracker);
                m_tracker->show();
            }
            l->addWidget(m_group);
            buildGroupSection(l, *n);
            buildModifiers(l, *n);
            const QVector<ParamDescriptor> common
                = NodeRegistry::instance().commonControls(n->type);
            if (!common.isEmpty()) {
                // Settings > Common Controls: Blend, channels, mask options (spec 91).
                m_settings = new ParamGroupBox(tr("Settings"));
                m_settings->setObjectName(QStringLiteral("CompNodeSettings"));
                for (const ParamDescriptor& desc : common) {
                    ParamRef ref;
                    ref.clipId = kProxyClipId;
                    ref.scope = ParamScope::Effect;
                    ref.effectId = n->id;
                    ref.key = desc.key;
                    m_settings->addParam(ref, desc, *this);
                }
                m_settings->setCollapsed(true);
                l->addWidget(m_settings);
            }
            if (n->type == nodetypes::MediaIn) {
                // The picture it reads: the clip's own, or another media pool item (a logo).
                auto* row = new QWidget;
                auto* h = new QHBoxLayout(row);
                h->setContentsMargins(
                    metrics::kSpaceM, metrics::kSpaceS, metrics::kSpaceM, metrics::kSpaceS);
                h->addWidget(new QLabel(tr("Source")));
                m_source = new QComboBox;
                m_source->setObjectName(QStringLiteral("CompMediaInSource"));
                m_source->setToolTip(tr("The picture this node reads. Media pool items can "
                                        "also be dragged onto the node editor."));
                h->addWidget(m_source, 1);
                l->addWidget(row);
                connect(m_source, &QComboBox::activated, this, [this](int index) {
                    if (m_session && m_source)
                        m_session->setMediaInSource(m_node, m_source->itemData(index).toString());
                });
                // Audio (Resolve: MediaIn Inspector > Audio tab): which sound the Compositing
                // page plays and a Sound Offset; both live in the composition.
                auto* audio = new ParamGroupBox(tr("Audio"));
                audio->setObjectName(QStringLiteral("CompMediaInAudio"));
                m_audioMode = new QComboBox;
                m_audioMode->setObjectName(QStringLiteral("CompMediaInAudioMode"));
                m_audioMode->addItem(tr("Timeline"), false);
                m_audioMode->addItem(tr("Solo this clip"), true);
                m_audioMode->setToolTip(tr("Timeline: the page plays the timeline's sound. Solo "
                                           "this clip: only the sound of this clip (its linked "
                                           "audio). Applies on this page only."));
                audio->addWidgetRow(tr("Audio"), m_audioMode);
                m_soundOffset = new QDoubleSpinBox;
                m_soundOffset->setObjectName(QStringLiteral("CompMediaInSoundOffset"));
                m_soundOffset->setRange(-1000.0, 1000.0);
                m_soundOffset->setDecimals(2);
                m_soundOffset->setSingleStep(0.25);
                m_soundOffset->setSuffix(tr(" frames"));
                m_soundOffset->setToolTip(tr("Moves the clip's sound against its picture, in "
                                             "frames (fractions allowed; positive = later). "
                                             "Applies on this page only."));
                audio->addWidgetRow(tr("Sound Offset"), m_soundOffset);
                m_audioGroup = audio;
                l->addWidget(audio);
                connect(m_audioMode, &QComboBox::activated, this, [this](int index) {
                    if (!m_session || !m_audioMode)
                        return;
                    const bool solo = m_audioMode->itemData(index).toBool();
                    m_session->edit(tr("Audio"), [solo](Composition& c) {
                        if (c.soloAudio == solo)
                            return false;
                        c.soloAudio = solo;
                        return true;
                    });
                });
                connect(m_soundOffset, &QDoubleSpinBox::editingFinished, this, [this] {
                    if (!m_session || !m_soundOffset)
                        return;
                    const double value = m_soundOffset->value();
                    m_session->edit(tr("Sound Offset"), [value](Composition& c) {
                        if (c.soundOffset == value)
                            return false;
                        c.soundOffset = value;
                        return true;
                    });
                });
            } else if (descs.isEmpty()) {
                // MediaIn / MediaOut: no empty group to unfold, a plain note instead.
                m_group->hide();
                l->addWidget(createPlaceholderContent(tr("This node has no parameters")));
            }
            if (tracking::canFollowTrack(*n)) {
                // "Connect to > Tracker n": the track of a Tracker / Planar Tracker is copied
                // into this node's Center keyframes (there are no live parameter links).
                auto* row = new QWidget;
                auto* h = new QHBoxLayout(row);
                h->setContentsMargins(
                    metrics::kSpaceM, metrics::kSpaceS, metrics::kSpaceM, metrics::kSpaceS);
                h->addWidget(new QLabel(tr("Follow Track")));
                m_follow = new QComboBox;
                m_follow->setObjectName(QStringLiteral("CompFollowTrack"));
                m_follow->setToolTip(tr("A track made by a Tracker or Planar Tracker node"));
                h->addWidget(m_follow, 1);
                m_followButton = new QToolButton;
                m_followButton->setObjectName(QStringLiteral("CompFollowConnect"));
                m_followButton->setText(tr("Connect"));
                m_followButton->setToolTip(tr("Copy the track into this node's Center keyframes, "
                                              "keeping its place at the playhead and following "
                                              "the track's movement"));
                h->addWidget(m_followButton);
                l->addWidget(row);
                // What the node takes from the track: its position, and where it has them its
                // size, angle and (path shapes) the points of the shape.
                auto* what = new QWidget;
                auto* wh = new QHBoxLayout(what);
                wh->setContentsMargins(
                    metrics::kSpaceM, 0, metrics::kSpaceM, metrics::kSpaceS);
                const auto tick = [&](const QString& name, const QString& text, const QString& tip,
                                      bool on) {
                    auto* box = new QCheckBox(text);
                    box->setObjectName(name);
                    box->setToolTip(tip);
                    box->setChecked(on);
                    wh->addWidget(box);
                    return box;
                };
                m_followPosition = tick(QStringLiteral("CompFollowPosition"), tr("Position"),
                    tr("Follow the position of the track"), true);
                if (tracking::canFollowSize(*n))
                    m_followSize = tick(QStringLiteral("CompFollowSize"), tr("Size"),
                        tr("Follow the scale of the track (a Planar Tracker, or a Tracker with "
                           "two or more patterns)"),
                        false);
                if (tracking::canFollowAngle(*n))
                    m_followAngle = tick(QStringLiteral("CompFollowAngle"), tr("Angle"),
                        tr("Follow the rotation of the track (a Planar Tracker, or a Tracker with "
                           "two or more patterns)"),
                        false);
                if (tracking::hasPathPoints(*n))
                    m_followPoints = tick(QStringLiteral("CompFollowPoints"), tr("Points"),
                        tr("Move the points of the shape (shape keyframes) and leave the node's "
                           "own Center, Size and Angle alone"),
                        false);
                wh->addStretch(1);
                l->addWidget(what);
                connect(m_followButton, &QToolButton::clicked, this, [this] {
                    if (!m_session || !m_follow || m_follow->currentData().isNull())
                        return;
                    const quint64 target = m_node;
                    const quint64 source = m_follow->currentData().toULongLong();
                    const int64_t frame = m_session->compFrame();
                    const CompNode* from = m_session->composition().node(source);
                    tracking::FollowOptions o;
                    o.position = !m_followPosition || m_followPosition->isChecked();
                    o.size = m_followSize && m_followSize->isChecked();
                    o.angle = m_followAngle && m_followAngle->isChecked();
                    o.points = m_followPoints && m_followPoints->isChecked();
                    const QSize frameSize = m_session->frameSize();
                    m_session->edit(tr("Connect to %1").arg(from ? from->name : QString()),
                        [=](Composition& c) {
                            CompNode* t = c.node(target);
                            const CompNode* s = c.node(source);
                            return t && s
                                && tracking::followTrack(*t, *s, frame, QSizeF(frameSize), o);
                        });
                });
            }
            l->addStretch(1);
            m_scroll->setWidget(holder);
        }
    }
    m_header->setVisible(n != nullptr);
    m_scroll->setVisible(n != nullptr);
    m_empty->setVisible(n == nullptr);
    if (n) {
        m_name->setText(n->name);
        const QSignalBlocker block(m_enable);
        m_enable->setChecked(n->enabled);
        m_enable->setEnabled(n->id != m_session->composition().mediaOutId);
        m_reset->setEnabled(!n->locked);
        if (m_group)
            m_group->setEnabled(!n->locked && n->instanceOf == 0); // an instance's settings are the original's
        if (m_settings)
            m_settings->setEnabled(!n->locked);
        if (m_source)
            fillSourcePicker(*n);
        if (m_audioMode) {
            const Composition& comp = m_session->composition();
            const QSignalBlocker b1(m_audioMode), b2(m_soundOffset);
            m_audioMode->setCurrentIndex(comp.soloAudio ? 1 : 0);
            m_soundOffset->setValue(comp.soundOffset);
            // The Audio section belongs to the MediaIn of the clip's own picture.
            m_audioGroup->setVisible(n->mediaId.isEmpty() && n->transitionSide == 0);
            m_audioMode->setEnabled(!n->locked);
            m_soundOffset->setEnabled(!n->locked);
        }
        if (m_follow)
            fillFollowPicker(*n);
    }
    refreshRows();
}

void CompInspector::fillSourcePicker(const CompNode& node)
{
    const QSignalBlocker block(m_source);
    m_source->clear();
    if (node.transitionSide != 0) {
        // MediaIn1 / MediaIn2 of a transition: always the two clips of the transition.
        m_source->addItem(node.transitionSide == 1 ? tr("Outgoing clip") : tr("Incoming clip"), QString());
        m_source->setCurrentIndex(0);
        m_source->setEnabled(false);
        return;
    }
    m_source->addItem(tr("This clip"), QString());
    for (const MediaItem& item : m_ctx.project->mediaPool()->items()) {
        if (m_session->canReadIntoMediaIn(item.id))
            m_source->addItem(item.name, item.id);
    }
    int index = m_source->findData(node.mediaId);
    if (index < 0) {
        // An item no longer in the media pool: keep it visible as what is read.
        m_source->addItem(tr("Missing media"), node.mediaId);
        index = m_source->count() - 1;
    }
    m_source->setCurrentIndex(index);
    m_source->setEnabled(!node.locked);
}

void CompInspector::fillFollowPicker(const CompNode& node)
{
    const quint64 keep = m_follow->currentData().toULongLong();
    const QSignalBlocker block(m_follow);
    m_follow->clear();
    for (const quint64 id : tracking::trackSources(m_session->composition())) {
        if (const CompNode* src = m_session->composition().node(id))
            m_follow->addItem(src->name, id);
    }
    if (m_follow->count() == 0)
        m_follow->addItem(tr("No tracks yet"), QVariant());
    const int index = m_follow->findData(keep);
    m_follow->setCurrentIndex(index >= 0 ? index : 0);
    const bool usable = !m_follow->currentData().isNull() && !node.locked;
    m_follow->setEnabled(usable);
    m_followButton->setEnabled(usable);
}

void CompInspector::refreshRows()
{
    if (!m_group || !m_session)
        return;
    syncProxy();
    m_group->refresh(m_proxy, m_session->compFrame(), true);
    for (ParamGroupBox* box : std::as_const(m_modifierBoxes))
        box->refresh(m_proxy, m_session->compFrame(), true);
    if (m_settings)
        m_settings->refresh(m_proxy, m_session->compFrame(), true);
}

// ---- ParamEditHost ----

const ClipData* CompInspector::clip(quint64 clipId) const
{
    return clipId == kProxyClipId && !m_proxy.effects.isEmpty() ? &m_proxy : nullptr;
}

int64_t CompInspector::localFrame(quint64) const
{
    return m_session ? m_session->compFrame() : 0;
}

void CompInspector::beginParamEdit(const QString& label)
{
    ++m_openEdits;
    if (m_session)
        m_session->beginGesture(label);
}

void CompInspector::endParamEdit()
{
    if (m_openEdits <= 0)
        return;
    --m_openEdits;
    if (m_session)
        m_session->endGesture();
}

void CompInspector::modifyClip(quint64 clipId, const std::function<void(ClipData&)>& mutate)
{
    if (clipId != kProxyClipId || m_proxy.effects.isEmpty() || !m_session)
        return;
    mutate(m_proxy);
    if (m_proxy.effects.isEmpty())
        return;
    const ParamSet params = m_proxy.effects.first().params;
    const quint64 id = m_node;
    m_session->modifyLive([&](Composition& c) {
        if (CompNode* n = c.node(id); n && !n->locked)
            n->params = params;
    });
}

void CompInspector::editClip(
    const QString& label, quint64 clipId, const std::function<void(ClipData&)>& mutate)
{
    beginParamEdit(label);
    modifyClip(clipId, mutate);
    endParamEdit();
}

void CompInspector::seekLocalFrame(quint64, int64_t frame)
{
    if (m_session)
        m_session->seekCompFrame(frame);
}

void CompInspector::setActiveParam(const ParamRef& ref, const ParamDescriptor&)
{
    if (m_session && m_node && !modifiers::isControlKey(ref.key))
        m_session->setActiveParam(m_node, ref.key);
}

QColor CompInspector::pickColor(const QColor& initial, const QString& title)
{
    return QColorDialog::getColor(initial, this, title);
}

QString CompInspector::pickFile(const QString& current, const QString& title, const QString& filter)
{
    return platform::getOpenFileName(this, title, filter, QStringLiteral("compositing/paramFile"),
        current.isEmpty() ? QString() : QFileInfo(current).absolutePath());
}

} // namespace kk
