// SPDX-License-Identifier: GPL-3.0-or-later
// Spec 128.10: a transition built on the Compositing page saved as a preset in the Effects
// Library (User Presets, under Video Transitions): files, list / rename / delete, the library
// folder, applying it to a cut, and a project that holds a preset transition.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/CompOps.h"
#include "core/EffectRegistry.h"
#include "core/NodeRegistry.h"
#include "core/Serialization.h"
#include "core/TransitionPresets.h"
#include "ui/common/DragMime.h"
#include "ui/common/Resources.h"
#include "ui/effects/EffectActions.h"
#include "ui/effects/EffectsLibraryPanel.h"

#include <QApplication>
#include <QMenu>
#include <QMimeData>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

using namespace kk;
using namespace kk::test;
namespace tp = kk::transitionpresets;

namespace {

/// A graph that is not the default one: an extra Transform node between MediaIn2 and the Merge.
Composition customGraph(int64_t duration)
{
    Composition c = compops::defaultTransitionComposition(duration);
    const quint64 blur = compops::addNode(c, nodetypes::Transform, QPointF(1, 3));
    c.node(blur)->name = QStringLiteral("Soften");
    return c;
}

tp::Preset makePreset(const QString& name, int64_t duration = 20)
{
    tp::Preset p;
    p.name = name;
    p.duration = duration;
    p.composition = customGraph(duration);
    return p;
}

} // namespace

class TestTransitionPresets : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
    }

    void init()
    {
        tp::setDirectory(m_dir.path());
        for (const tp::Entry& e : tp::list())
            tp::remove(e.name);
    }

    void cleanupTestCase() { tp::setDirectory({}); }

    void saveListLoadRenameDelete()
    {
        QVERIFY(tp::list().isEmpty());
        QString error;
        const QString path = tp::save(makePreset(QStringLiteral("Spin Blur")), &error);
        QVERIFY2(!path.isEmpty(), qPrintable(error));
        QVERIFY(path.endsWith(QStringLiteral(".kktransition")));
        QVERIFY(!tp::save(makePreset(QStringLiteral("Alpha"), 10)).isEmpty());

        const QVector<tp::Entry> all = tp::list();
        QCOMPARE(all.size(), 2);
        QCOMPARE(all[0].name, QStringLiteral("Alpha"));
        QCOMPARE(all[1].id(), QStringLiteral("tpreset:Spin-Blur"));

        const auto loaded = tp::loadById(all[1].id());
        QVERIFY(loaded);
        QCOMPARE(loaded->name, QStringLiteral("Spin Blur"));
        QCOMPARE(loaded->duration, int64_t(20));
        QVERIFY(loaded->composition.nodes.size() == customGraph(20).nodes.size());
        bool soften = false;
        for (const CompNode& n : loaded->composition.nodes)
            soften = soften || n.name == QStringLiteral("Soften");
        QVERIFY(soften);

        // Saved presets survive a restart: a fresh read of the folder finds them.
        QCOMPARE(tp::list().size(), 2);

        QVERIFY(tp::rename(QStringLiteral("Spin Blur"), QStringLiteral("Soft Spin")));
        QVERIFY(!tp::loadById(QStringLiteral("tpreset:Spin-Blur")));
        const auto renamed = tp::loadById(QStringLiteral("tpreset:Soft-Spin"));
        QVERIFY(renamed);
        QCOMPARE(renamed->name, QStringLiteral("Soft Spin"));
        // Another preset's name is not taken over.
        QVERIFY(!tp::rename(QStringLiteral("Soft Spin"), QStringLiteral("Alpha")));
        QVERIFY(!tp::rename(QStringLiteral("Nothing"), QStringLiteral("Beta")));
        QVERIFY(!tp::rename(QStringLiteral("Soft Spin"), QStringLiteral("  ")));

        QVERIFY(tp::remove(QStringLiteral("Soft Spin")));
        QVERIFY(!tp::remove(QStringLiteral("Soft Spin")));
        QCOMPARE(tp::list().size(), 1);
    }

    void stretchKeepsTheDefaultBlendOnTheWindow()
    {
        const Composition base = compops::defaultTransitionComposition(10);
        const Composition longer = tp::stretched(base, 10, 20);
        const CompNode* merge = nullptr;
        for (const CompNode& n : longer.nodes)
            merge = n.type == nodetypes::Merge ? &n : merge;
        QVERIFY(merge);
        const AnimatedValue& blend = merge->params.values.value(nodekeys::Opacity);
        QCOMPARE(blend.keys.size(), 2);
        QCOMPARE(blend.keys[0].frame, int64_t(-1));
        QCOMPARE(blend.keys[1].frame, int64_t(20));
        // Same length: untouched. Shorter: the end key follows.
        QVERIFY(tp::stretched(base, 10, 10) == base);
        const Composition shorter = tp::stretched(base, 10, 5);
        for (const CompNode& n : shorter.nodes) {
            if (n.type == nodetypes::Merge)
                QCOMPARE(n.params.values.value(nodekeys::Opacity).keys.last().frame, int64_t(5));
        }
    }

    void theRegistryKnowsPresets()
    {
        QVERIFY(!tp::save(makePreset(QStringLiteral("Whirl"), 20)).isEmpty());
        const EffectRegistry& reg = EffectRegistry::instance();
        const EffectDescriptor* d = reg.find(QStringLiteral("tpreset:Whirl"));
        QVERIFY(d);
        QCOMPARE(d->name, QStringLiteral("Whirl"));
        QCOMPARE(d->category, EffectCategory::Transition);
        QCOMPARE(d->group, QString::fromLatin1(effectgroups::UserTransitions));
        QVERIFY(!reg.find(QStringLiteral("tpreset:Missing")));

        const TransitionData t = reg.createTransition(QStringLiteral("tpreset:Whirl"), 40);
        QCOMPARE(t.type, kCompositionTransition);
        QCOMPARE(t.duration, int64_t(40));
        QVERIFY(t.composition);
        // The graph's blend was stretched from 20 to 40 frames.
        for (const CompNode& n : t.composition->nodes) {
            if (n.type == nodetypes::Merge)
                QCOMPARE(n.params.values.value(nodekeys::Opacity).keys.last().frame, int64_t(40));
        }
        // A preset that is gone makes a plain dissolve.
        QVERIFY(tp::remove(QStringLiteral("Whirl")));
        QVERIFY(!reg.find(QStringLiteral("tpreset:Whirl")));
        QCOMPARE(reg.createTransition(QStringLiteral("tpreset:Whirl"), 12).type,
            QStringLiteral("cross_dissolve"));
    }

    void libraryShowsRenamesAndDeletesPresets()
    {
        PanelFixture f;
        EffectsLibraryPanel panel(f.ctx);
        panel.resize(320, 600);
        QVERIFY(!panel.visibleDescriptorIds().contains(QStringLiteral("tpreset:Whirl")));
        QVERIFY(!tp::save(makePreset(QStringLiteral("Whirl"))).isEmpty());
        panel.refreshPresets();
        QVERIFY(panel.visibleDescriptorIds().contains(QStringLiteral("tpreset:Whirl")));

        // It sits in the folder "User Presets" of Video Transitions, and a search finds it.
        QTreeWidgetItem* transitions = panel.tree()->topLevelItem(0);
        QCOMPARE(transitions->text(0), QStringLiteral("Video Transitions"));
        bool inFolder = false;
        for (int i = 0; i < transitions->childCount(); ++i) {
            if (transitions->child(i)->text(0) == QStringLiteral("User Presets"))
                inFolder = transitions->child(i)->childCount() == 1;
        }
        QVERIFY(inFolder);
        panel.setFilterText(QStringLiteral("whirl"));
        QCOMPARE(panel.visibleDescriptorIds(), QStringList {QStringLiteral("tpreset:Whirl")});
        panel.setFilterText({});

        // Context menu: the usual entries plus Rename and Delete.
        std::unique_ptr<QMenu> menu(panel.createContextMenu(QStringLiteral("tpreset:Whirl")));
        QVERIFY(menu);
        QVERIFY(menu->findChild<QAction*>(QStringLiteral("ApplyLibraryEntry")));
        QVERIFY(menu->findChild<QAction*>(QStringLiteral("RenameTransitionPreset")));
        QVERIFY(menu->findChild<QAction*>(QStringLiteral("DeleteTransitionPreset")));

        QVERIFY(panel.renamePreset(QStringLiteral("tpreset:Whirl"), QStringLiteral("Twirl")));
        QVERIFY(!panel.visibleDescriptorIds().contains(QStringLiteral("tpreset:Whirl")));
        QVERIFY(panel.visibleDescriptorIds().contains(QStringLiteral("tpreset:Twirl")));
        QVERIFY(panel.deletePreset(QStringLiteral("tpreset:Twirl")));
        QVERIFY(!panel.visibleDescriptorIds().contains(QStringLiteral("tpreset:Twirl")));
        QVERIFY(tp::list().isEmpty());
    }

    void aPresetDragsOntoACutAndSurvivesSaveAndLoad()
    {
        QVERIFY(!tp::save(makePreset(QStringLiteral("Whirl"), 20)).isEmpty());
        PanelFixture f;
        EffectsLibraryPanel panel(f.ctx);
        panel.refreshPresets();
        // The drag payload is the id, like every library entry.
        QTreeWidgetItem* item = nullptr;
        QTreeWidgetItemIterator it(panel.tree());
        for (; *it; ++it) {
            if ((*it)->text(0) == QStringLiteral("Whirl"))
                item = *it;
        }
        QVERIFY(item);
        QVERIFY(item->flags().testFlag(Qt::ItemIsDragEnabled));

        const quint64 a = f.addClip({TrackType::Video, 0}, 0, 100);
        const quint64 b = f.addClip({TrackType::Video, 0}, 100, 100);
        QVERIFY(effectactions::applyTransition(
            f.project, b, QStringLiteral("tpreset:Whirl"), true, 20));
        const ClipData* head = f.clip(b);
        QVERIFY(head && head->transitionIn);
        QCOMPARE(head->transitionIn->type, kCompositionTransition);
        QCOMPARE(head->transitionIn->duration, int64_t(20));
        QVERIFY(head->transitionIn->composition);
        QCOMPARE(head->transitionIn->composition->nodes.size(), customGraph(20).nodes.size());
        Q_UNUSED(a);

        // The project file holds the graph itself: deleting the preset changes nothing, and the
        // file loads without the preset.
        const TransitionData stored = *head->transitionIn;
        QVERIFY(tp::remove(QStringLiteral("Whirl")));
        const QJsonObject json = serialization::timelineToJson(f.project.timeline()->data());
        TimelineData back;
        QString error;
        QVERIFY2(serialization::timelineFromJson(json, back, &error), qPrintable(error));
        const ClipData* loaded = nullptr;
        for (const ClipData& c : back.videoTracks[0].clips)
            loaded = c.id == b ? &c : loaded;
        QVERIFY(loaded && loaded->transitionIn);
        QVERIFY(*loaded->transitionIn == stored);
    }
};

QTEST_MAIN(TestTransitionPresets)
#include "test_transitionpresets.moc"
