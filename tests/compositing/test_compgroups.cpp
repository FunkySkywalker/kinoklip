// SPDX-License-Identifier: GPL-3.0-or-later
// Group, Macro, Underlay and Instance nodes, and saved macros (spec section 120).
#include "../render/RenderTestUtil.h"
#include "core/CompGroups.h"
#include "core/CompLinks.h"
#include "core/CompOps.h"
#include "core/MacroFiles.h"
#include "core/Modifiers.h"
#include "core/NodeRegistry.h"
#include "core/Serialization.h"
#include "render/Compositor.h"

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

using namespace kk;
using namespace kk::test;
namespace co = kk::compops;
namespace cg = kk::compgroups;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

/// Background (white) -> Crop -> Transform -> out, a small chain to group.
struct Chain
{
    Composition c = co::defaultComposition();
    quint64 bg = 0, crop = 0, transform = 0;
    Chain()
    {
        bg = co::addNode(c, nodetypes::Background, {0, 4});
        c.node(bg)->params.setStatic(nodekeys::Color, QColor(Qt::white));
        crop = co::addNode(c, nodetypes::Crop, {4, 4});
        c.node(crop)->params.setBase(nodekeys::Left, 130);
        c.node(crop)->params.setBase(nodekeys::Top, 70);
        c.node(crop)->params.setBase(nodekeys::Right, kW - 190);
        c.node(crop)->params.setBase(nodekeys::Bottom, kH - 110);
        transform = co::addNode(c, nodetypes::Transform, {8, 4});
        co::connect(c, bg, 0, crop, nodeports::Input);
        co::connect(c, crop, 0, transform, nodeports::Input);
        co::connect(c, transform, 0, c.mediaOutId, nodeports::Input);
    }
};

TimelineData compTimeline(const Composition& comp)
{
    TimelineData tl = emptyTimeline(kW, kH);
    ClipData clip = solidClip(tl, QColor(0, 0, 0), 0, 50);
    clip.composition = comp;
    addClip(tl, TrackType::Video, 0, clip);
    return tl;
}

} // namespace

class TestCompGroups : public QObject
{
    Q_OBJECT
    std::unique_ptr<Compositor> m_gpu, m_cpu;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_compgroups"));
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
        QDir(macrofiles::directory()).removeRecursively();
    }
    void cleanupTestCase()
    {
        QDir(macrofiles::directory()).removeRecursively();
        m_gpu.reset();
        m_cpu.reset();
    }

    void groupingAndUngrouping()
    {
        Chain ch;
        Composition& c = ch.c;
        const quint64 g = cg::makeGroup(c, {ch.crop, ch.transform});
        QVERIFY(g);
        QCOMPARE(c.node(g)->type, nodetypes::Group);
        QCOMPARE(c.node(ch.crop)->group, g);
        QCOMPARE(c.node(ch.transform)->group, g);
        QCOMPARE(c.node(ch.bg)->group, quint64(0));
        QCOMPARE(cg::members(c, g), (QVector<quint64> {ch.crop, ch.transform}));
        // Its tile sits in the middle of the members.
        QCOMPARE(c.node(g)->position, QPointF(6, 4));
        QVERIFY(!c.node(g)->collapsed);
        // The links did not change: the graph is the same flat graph.
        QCOMPARE(c.node(ch.crop)->inputs.value(nodeports::Input).nodeId, ch.bg);
        // Nothing to group: MediaOut and nothing at all.
        QCOMPARE(cg::makeGroup(c, {c.mediaOutId}), quint64(0));
        QCOMPARE(cg::makeGroup(c, {}), quint64(0));
        // A group inside a group: the outer takes the inner whole.
        const quint64 outer = cg::makeGroup(c, {g, ch.bg});
        QVERIFY(outer);
        QCOMPARE(c.node(g)->group, outer);
        QCOMPARE(c.node(ch.bg)->group, outer);
        QCOMPARE(c.node(ch.crop)->group, g); // still in the inner one
        QCOMPARE(cg::members(c, outer, true).size(), 4);
        QCOMPARE(cg::members(c, outer, false).size(), 2);
        // withMembers: a move or copy of the group takes everything inside.
        QCOMPARE(cg::withMembers(c, {outer}).size(), 5);
        // Collapsing hides the members behind the outermost collapsed group.
        QVERIFY(cg::setCollapsed(c, g, true));
        QCOMPARE(cg::hiddenBy(c, *c.node(ch.crop))->id, g);
        QVERIFY(!cg::hiddenBy(c, *c.node(g)));
        QVERIFY(cg::setCollapsed(c, outer, true));
        QCOMPARE(cg::hiddenBy(c, *c.node(ch.crop))->id, outer);
        QCOMPARE(cg::hiddenBy(c, *c.node(g))->id, outer);
        QVERIFY(!cg::setCollapsed(c, outer, true)); // no change
        QVERIFY(!cg::setCollapsed(c, ch.bg, true)); // not a group
        QVERIFY(cg::setCollapsed(c, outer, false));
        QVERIFY(cg::setCollapsed(c, g, false));
        // The frame of an expanded group surrounds its members with a margin and a title.
        const QRectF frame = cg::frameOf(c, *c.node(g));
        QVERIFY(frame.contains(QRectF(c.node(ch.crop)->position, QSizeF(3.4, 1.0))));
        QVERIFY(frame.contains(QRectF(c.node(ch.transform)->position, QSizeF(3.4, 1.0))));
        QVERIFY(frame.top() < c.node(ch.crop)->position.y() - 1.0); // room for the title
        // Ungroup.
        QVERIFY(cg::ungroup(c, g, 0));
        QVERIFY(!c.node(g));
        QCOMPARE(c.node(ch.crop)->group, outer);
        QVERIFY(!cg::ungroup(c, 9999, 0));
        QVERIFY(!cg::ungroup(c, ch.bg, 0));
        QVERIFY(cg::ungroup(c, outer, 0));
        QCOMPARE(c.node(ch.crop)->group, quint64(0));
        QCOMPARE(c.nodes.size(), 5); // MediaIn, MediaOut, Background, Crop, Transform
    }

    void theOutputNodeIsTheOneReadFromOutside()
    {
        Chain ch;
        const quint64 g = cg::makeGroup(ch.c, {ch.bg, ch.crop, ch.transform});
        QCOMPARE(cg::outputNode(ch.c, g), ch.transform); // read by MediaOut, outside
        const quint64 g2 = cg::makeGroup(ch.c, {ch.bg, ch.crop});
        // Without anything reading from outside, the one nothing inside reads (the right-most).
        Chain open;
        co::disconnect(open.c, open.c.mediaOutId, nodeports::Input);
        const quint64 og = cg::makeGroup(open.c, {open.bg, open.crop, open.transform});
        QCOMPARE(cg::outputNode(open.c, og), open.transform);
    }

    void groupSavesAndOlderFilesLoad()
    {
        Chain ch;
        const quint64 g = cg::makeGroup(ch.c, {ch.crop, ch.transform});
        cg::setCollapsed(ch.c, g, true);
        const quint64 inst = cg::makeInstance(ch.c, ch.crop, {9, 9});
        QVERIFY(inst);
        CompNote note;
        note.id = ch.c.allocateId();
        note.size = QSizeF(6, 4);
        note.underlay = true;
        ch.c.notes.append(note);
        const Composition back = serialization::compositionFromJson(serialization::compositionToJson(ch.c));
        QCOMPARE(back, ch.c);
        QCOMPARE(back.node(ch.crop)->group, g);
        QVERIFY(back.node(g)->collapsed);
        QCOMPARE(back.node(inst)->instanceOf, ch.crop);
        QVERIFY(back.notes.first().underlay);
        // A file with none of this loads with none of it.
        Composition plain = co::defaultComposition();
        co::addNode(plain, nodetypes::Transform, {});
        const Composition plainBack = serialization::compositionFromJson(serialization::compositionToJson(plain));
        for (const CompNode& n : plainBack.nodes)
            QVERIFY(n.group == 0 && !n.collapsed && !n.macro && n.controls.isEmpty() && n.instanceOf == 0);
        // A group node and its members survive copy and paste, as a group.
        Chain src;
        const quint64 sg = cg::makeGroup(src.c, {src.crop, src.transform});
        const QVector<CompNode> copied = co::copyNodes(src.c, cg::withMembers(src.c, {sg}));
        QCOMPARE(copied.size(), 3);
        const QVector<quint64> pasted = co::pasteNodes(src.c, copied, {0, 6});
        QCOMPARE(pasted.size(), 3);
        // The pasted members belong to the pasted group, not the original.
        quint64 newGroup = 0;
        for (const quint64 id : pasted)
            if (src.c.node(id)->type == nodetypes::Group)
                newGroup = id;
        QVERIFY(newGroup && newGroup != sg);
        for (const quint64 id : pasted)
            if (id != newGroup)
                QCOMPARE(src.c.node(id)->group, newGroup);
        QCOMPARE(cg::members(src.c, sg).size(), 2);
        // Copying only a member leaves it outside any group.
        const QVector<CompNode> one = co::copyNodes(src.c, {src.crop});
        const QVector<quint64> p1 = co::pasteNodes(src.c, one, {0, 9});
        QCOMPARE(src.c.node(p1[0])->group, quint64(0));
        // Removing the group node frees its members, deleting a member leaves the group.
        co::removeNodes(src.c, {sg});
        QCOMPARE(src.c.node(src.crop)->group, quint64(0));
    }

    void aGroupRendersLikeItsNodes()
    {
        Chain ch;
        const TimelineData flat = compTimeline(ch.c);
        const auto clipId = flat.videoTracks[0].clips[0].id;
        const quint64 g = cg::makeGroup(ch.c, {ch.crop, ch.transform});
        for (const bool collapsed : {false, true}) {
            cg::setCollapsed(ch.c, g, collapsed);
            const TimelineData grouped = compTimeline(ch.c);
            for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
                const auto a = comp->renderCompositionNode(snap(flat), clipId, 0, 0, {}, Quality::Full, OutputImage).get();
                const auto b = comp->renderCompositionNode(snap(grouped), grouped.videoTracks[0].clips[0].id, 0, 0, {}, Quality::Full, OutputImage).get();
                QVERIFY(a && b && a->ok && b->ok);
                QVERIFY(px(a->image, 160, 90).a > 240);
                QCOMPARE(a->image, b->image);
            }
        }
    }

    void macroControlsAreNumbersOnTheMacro()
    {
        Chain ch;
        Composition& c = ch.c;
        c.node(ch.transform)->params.setBase(nodekeys::Size, 1.0);
        modifiers::add(c.node(ch.crop)->params, nodekeys::Left, ModifierKind::Publish);
        const quint64 g = cg::makeGroup(c, {ch.crop, ch.transform});
        // Published parameters come first in the list.
        const QVector<cg::ControlCandidate> cands = cg::controlCandidates(c, g);
        QVERIFY(!cands.isEmpty());
        QVERIFY(cands.first().published && cands.first().key == nodekeys::Left && cands.first().node == ch.crop);
        QVERIFY(cands.first().label.contains(c.node(ch.crop)->name));
        // Make Macro with two controls and a name.
        QVERIFY(!cg::makeMacro(c, g, QStringLiteral("Zoom Box"), {}, 0)); // nothing picked
        QVERIFY(!cg::makeMacro(c, g, QStringLiteral("Zoom Box"), {{ch.bg, nodekeys::Color, {}}}, 0)); // outside / not a number
        QVERIFY(cg::makeMacro(c, g, QStringLiteral("Zoom Box"),
            {{ch.transform, nodekeys::Size, QStringLiteral("Zoom")}, {ch.crop, nodekeys::Left, {}},
                {ch.transform, nodekeys::Size, QStringLiteral("again")}},
            0));
        const CompNode& m = *c.node(g);
        QVERIFY(m.macro);
        QCOMPARE(m.name, QStringLiteral("Zoom Box"));
        QCOMPARE(m.controls.size(), 2); // the repeat is skipped
        QCOMPARE(m.controls[0].label, QStringLiteral("Zoom"));
        QCOMPARE(m.controls[1].label, complinks::paramLabel(*c.node(ch.crop), nodekeys::Left));
        // The control holds the value the inner parameter had.
        QCOMPARE(m.params.value(m.controls[0].key, 0, -1), 1.0);
        QCOMPARE(m.params.value(m.controls[1].key, 0, -1), 130.0);
        // The inner parameter is connected to it: change the control, the inner value follows.
        c.node(g)->params.setBase(m.controls[0].key, 2.5);
        QCOMPARE(complinks::resolved(c, 0).node(ch.transform)->params.value(nodekeys::Size, 0, 0), 2.5);
        // Keyframes on the control drive the inner parameter over time.
        c.node(g)->params.animated(m.controls[0].key, 1).setKey(0, 1.0);
        c.node(g)->params.animated(m.controls[0].key, 1).setKey(10, 3.0);
        QCOMPARE(complinks::resolved(c, 5).node(ch.transform)->params.value(nodekeys::Size, 5, 0), 2.0);
        // Descriptors for the Inspector: those of the inner parameters under the control's name.
        const QVector<ParamDescriptor> ds = cg::controlDescriptors(c, *c.node(g));
        QCOMPARE(ds.size(), 2);
        QCOMPARE(ds[0].key, m.controls[0].key);
        QCOMPARE(ds[0].label, QStringLiteral("Zoom"));
        QVERIFY(ds[0].max > ds[0].min);
        // The control can be a Connect To target (it is not a descriptor parameter).
        const quint64 other = co::addNode(c, nodetypes::Transform, {0, 8});
        QCOMPARE(complinks::connectParam(c, other, nodekeys::Size, {g, m.controls[0].key}), complinks::LinkError::None);
        QCOMPARE(complinks::resolved(c, 5).node(other)->params.value(nodekeys::Size, 5, 0), 2.0);
        // Remove a control: the inner parameter keeps what it showed at that frame.
        const QString key = c.node(g)->controls[0].key;
        QVERIFY(cg::removeControl(c, g, key, 5));
        QVERIFY(c.node(ch.transform)->params.values[nodekeys::Size].modifiers.isEmpty());
        QCOMPARE(c.node(ch.transform)->params.value(nodekeys::Size, 5, 0), 2.0);
        QVERIFY(!cg::removeControl(c, g, key, 5));
        QVERIFY(c.node(g)->macro); // one control left
        // Saved and loaded with its controls.
        const Composition back = serialization::compositionFromJson(serialization::compositionToJson(c));
        QCOMPARE(back, c);
        QCOMPARE(back.node(g)->controls.size(), 1);
        // Ungrouping a macro cuts the controls loose.
        QVERIFY(cg::ungroup(c, g, 0));
        QVERIFY(!modifiers::first(c.node(ch.crop)->params, nodekeys::Left, ModifierKind::Connect)); // only its Publish mark is left
        QCOMPARE(c.node(ch.crop)->params.value(nodekeys::Left, 0, 0), 130.0);
    }

    void instancesShareTheOriginalsParameters()
    {
        Chain ch;
        Composition& c = ch.c;
        const quint64 inst = cg::makeInstance(c, ch.crop, {4, 7});
        QVERIFY(inst);
        QCOMPARE(c.node(inst)->type, nodetypes::Crop);
        QCOMPARE(c.node(inst)->instanceOf, ch.crop);
        QVERIFY(c.node(inst)->inputs.isEmpty());
        QVERIFY(c.node(inst)->name != c.node(ch.crop)->name);
        // An edit of the original shows in the instance (when rendering).
        QVERIFY(cg::hasInstances(c));
        c.node(ch.crop)->params.setBase(nodekeys::Left, 10);
        Composition r = c;
        cg::applyInstances(r);
        QCOMPARE(r.node(inst)->params.value(nodekeys::Left, 0, 0), 10.0);
        QCOMPARE(complinks::resolved(c, 0).node(inst)->params.value(nodekeys::Left, 0, 0), 10.0);
        // The instance's own position, links and name stay its own.
        QCOMPARE(r.node(inst)->position, QPointF(4, 7));
        // An instance of an instance is an instance of the original.
        const quint64 inst2 = cg::makeInstance(c, inst, {4, 9});
        QCOMPARE(c.node(inst2)->instanceOf, ch.crop);
        QCOMPARE(cg::instanceSource(c, *c.node(inst2)), ch.crop);
        // MediaIn, MediaOut and groups cannot be instanced.
        QCOMPARE(cg::makeInstance(c, c.mediaOutId, {0, 0}), quint64(0));
        QCOMPARE(cg::makeInstance(c, cg::makeGroup(c, {ch.transform}), {0, 0}), quint64(0));
        // Make Independent keeps a copy.
        QVERIFY(cg::makeIndependent(c, inst));
        QCOMPARE(c.node(inst)->instanceOf, quint64(0));
        c.node(ch.crop)->params.setBase(nodekeys::Left, 77);
        QCOMPARE(complinks::resolved(c, 0).node(inst)->params.value(nodekeys::Left, 0, 0), 10.0);
        QVERIFY(!cg::makeIndependent(c, inst));
        // It renders the same as the original would: the instance feeding the output.
        Chain ch2;
        const quint64 i2 = cg::makeInstance(ch2.c, ch2.crop, {4, 7});
        co::connect(ch2.c, ch2.bg, 0, i2, nodeports::Input);
        co::connect(ch2.c, i2, 0, ch2.transform, nodeports::Input);
        const TimelineData tl = compTimeline(ch2.c);
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            const auto f = comp->renderCompositionNode(snap(tl), tl.videoTracks[0].clips[0].id, 0, 0, {}, Quality::Full, OutputImage).get();
            QVERIFY(f && f->ok);
            QVERIFY(px(f->image, 160, 90).a > 240); // the crop box of the original, through the instance
            QVERIFY(px(f->image, 20, 20).a < 15);
        }
    }

    void macroFilesSaveInsertAndMakeTitles()
    {
        Chain ch;
        Composition& c = ch.c;
        const quint64 g = cg::makeGroup(c, {ch.bg, ch.crop});
        QVERIFY(cg::makeMacro(c, g, QStringLiteral("White Box"), {{ch.crop, nodekeys::Left, QStringLiteral("Left")}}, 0));
        const auto file = macrofiles::fromGroup(c, g);
        QVERIFY(file);
        QCOMPARE(file->name, QStringLiteral("White Box"));
        QCOMPARE(file->nodes.size(), 3);
        QCOMPARE(file->nodes.first().id, g);
        QCOMPARE(file->outputId, ch.crop);
        QVERIFY(macrofiles::fromGroup(c, ch.bg) == std::nullopt);
        QString error;
        const QString path = macrofiles::save(*file, &error);
        QVERIFY2(!path.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(path));
        const auto loaded = macrofiles::load(path);
        QVERIFY(loaded);
        QCOMPARE(loaded->nodes.size(), 3);
        QCOMPARE(loaded->outputId, ch.crop);
        QCOMPARE(macrofiles::list().size(), 1);
        QCOMPARE(macrofiles::list().first().name, QStringLiteral("White Box"));
        QCOMPARE(macrofiles::pathOfId(macrofiles::list().first().id()), path);
        QVERIFY(macrofiles::pathOfId(QStringLiteral("macro:nothing")).isEmpty());
        QVERIFY(!macrofiles::load(path + QStringLiteral(".missing")));
        // Another file in the folder that is not a macro is not listed.
        QFile junk(macrofiles::directory() + QStringLiteral("/junk.kkmacro"));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("{\"hello\": 1}");
        junk.close();
        QCOMPARE(macrofiles::list().size(), 1);
        // Insert into another composition: a group, new ids, controls connected to its own macro.
        Composition other = co::defaultComposition();
        co::addNode(other, nodetypes::Transform, {});
        quint64 out = 0;
        const quint64 ig = macrofiles::insert(other, *loaded, {2, 2}, &out);
        QVERIFY(ig);
        QCOMPARE(other.node(ig)->type, nodetypes::Group);
        QVERIFY(other.node(ig)->macro);
        QCOMPARE(other.node(ig)->controls.size(), 1);
        QCOMPARE(cg::members(other, ig).size(), 2);
        QVERIFY(out && other.node(out)->type == nodetypes::Crop);
        const MacroControl& ctl = other.node(ig)->controls[0];
        QVERIFY(ctl.node != ch.crop || other.node(ctl.node)); // remapped to the new crop
        QCOMPARE(other.node(ctl.node)->type, nodetypes::Crop);
        other.node(ig)->params.setBase(ctl.key, 40.0);
        QCOMPARE(complinks::resolved(other, 0).node(ctl.node)->params.value(nodekeys::Left, 0, 0), 40.0);
        // The same macro twice: both work on their own.
        const quint64 ig2 = macrofiles::insert(other, *loaded, {2, 8});
        QVERIFY(ig2 && ig2 != ig);
        other.node(ig2)->params.setBase(other.node(ig2)->controls[0].key, 5.0);
        const Composition r = complinks::resolved(other, 0);
        QCOMPARE(r.node(other.node(ig)->controls[0].node)->params.value(nodekeys::Left, 0, 0), 40.0);
        QCOMPARE(r.node(other.node(ig2)->controls[0].node)->params.value(nodekeys::Left, 0, 0), 5.0);
        // A title: the macro's output goes to MediaOut; it renders its own picture.
        const Composition title = macrofiles::titleComposition(*loaded);
        const CompNode* mo = title.node(title.mediaOutId);
        QVERIFY(mo->inputs.value(nodeports::Input).isConnected());
        const TimelineData tl = compTimeline(title);
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            const auto f = comp->renderCompositionNode(snap(tl), tl.videoTracks[0].clips[0].id, 0, 0, {}, Quality::Full, OutputImage).get();
            QVERIFY(f && f->ok);
            QVERIFY(px(f->image, 160, 90).a > 240);
            QVERIFY(px(f->image, 20, 20).a < 15);
        }
        // Delete.
        QVERIFY(macrofiles::remove(QStringLiteral("White Box")));
        QVERIFY(!macrofiles::remove(QStringLiteral("White Box")));
        QCOMPARE(macrofiles::list().size(), 0);
        QCOMPARE(macrofiles::baseName(QStringLiteral("A / B: c")), QStringLiteral("A---B--c"));
        QCOMPARE(macrofiles::baseName(QString()), QStringLiteral("macro"));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompGroups>(argc, argv);
}
#include "test_compgroups.moc"
