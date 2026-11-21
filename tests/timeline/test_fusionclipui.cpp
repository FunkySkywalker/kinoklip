// SPDX-License-Identifier: GPL-3.0-or-later
// Fusion clips in the UI: New Fusion Clip in the timeline clip menu (right after New Compound
// Clip, opening the Compositing page), the media pool entries (New Fusion Composition, New
// Fusion Clip, row kind, icon, tooltip), dropping two instances on the timeline, renaming and
// deleting from the pool. Writes screenshots when KK_SCREENSHOT_DIR is set.
#include "TimelineTestUtils.h"
#include "core/CompoundOps.h"
#include "core/MediaPool.h"
#include "core/MediaUsage.h"
#include "ui/mediapool/MediaPoolModel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

namespace {

QString addMedia(Project& project, const QString& name, double seconds)
{
    MediaItem item;
    item.path = QCoreApplication::applicationFilePath();
    item.name = name;
    item.info.valid = item.info.hasVideo = item.info.hasAudio = true;
    item.info.width = item.info.displayWidth = 1280;
    item.info.height = item.info.displayHeight = 720;
    item.info.frameRate = {25, 1};
    item.info.durationUs = int64_t(seconds * 1e6);
    project.beginEdit(QStringLiteral("add"));
    const QString id = project.mediaPool()->addItem(item);
    project.endEdit();
    return id;
}

QString plain(QString s)
{
    return s.remove(QLatin1Char('&'));
}

/// Opens the context menu of @p w at @p pos; returns the texts of its entries (separators as
/// "-"), reads the submenu "More" too, and grabs the popup to <dir>/<shot>.png when asked.
struct MenuRead
{
    QStringList top;
    QStringList more;
};
MenuRead readMenu(QWidget& w, QPoint pos, const QString& shot = {})
{
    MenuRead r;
    QTimer::singleShot(0, [&r, shot] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            for (QAction* a : menu->actions())
                r.top.append(a->isSeparator() ? QStringLiteral("-") : plain(a->text()));
            if (auto* more = menu->findChild<QMenu*>(QStringLiteral("MoreClipMenu"))) {
                for (QAction* a : more->actions())
                    r.more.append(a->isSeparator() ? QStringLiteral("-") : plain(a->text()));
            }
            const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR");
            if (!shot.isEmpty() && !dir.isEmpty()) {
                QDir().mkpath(dir);
                menu->grab().save(QDir(dir).filePath(shot + QStringLiteral(".png")));
            }
            menu->close();
        }
    });
    QContextMenuEvent event(QContextMenuEvent::Mouse, pos, w.mapToGlobal(pos));
    QCoreApplication::sendEvent(&w, &event);
    return r;
}

} // namespace

class TestFusionClipUi : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_fusionclipui"); }

    void timelineMenuHasNewFusionClipAfterNewCompound()
    {
        TimelineFixture f;
        TimelineController c(f.ctx);
        TimelineWidget w(f.ctx, &c);
        c.connectActions();
        w.setThumbnailProvider({});
        w.setWaveformProvider({});
        w.resize(1200, 500);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setPixelsPerFrame(10.0, kHeaderWidth);
        w.scrollToFrame(0);
        f.project.beginEdit(QStringLiteral("setup"));
        ops::overwriteClip(f.data(), {TrackType::Video, 0}, solidClip(0, 40, QStringLiteral("One")));
        f.project.endEdit();

        const TimelineLayout& L = w.layout();
        const QRect v1 = L.rowRect(L.rowOf({TrackType::Video, 0}));
        const QPoint pos(int(L.frameToX(10)), v1.center().y());
        const MenuRead menu = readMenu(*w.viewport(), pos, QStringLiteral("fusion-clip-menu"));
        const qsizetype compound = menu.top.indexOf(QStringLiteral("New Compound Clip…"));
        QVERIFY2(compound >= 0, qPrintable(menu.top.join('|')));
        QCOMPARE(menu.top.value(compound + 1), QStringLiteral("New Fusion Clip…"));
        QVERIFY(!menu.more.contains(QStringLiteral("New Fusion Clip…")));

        // The rule of the streamlined menu: no submenu holds a single entry.
        QTimer::singleShot(0, [] {
            if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                const auto subs = m->findChildren<QMenu*>();
                for (const QMenu* s : subs)
                    QVERIFY2(s->actions().size() != 1, qPrintable(s->title()));
                m->close();
            }
        });
        QContextMenuEvent event(QContextMenuEvent::Mouse, pos, w.viewport()->mapToGlobal(pos));
        QCoreApplication::sendEvent(w.viewport(), &event);
    }

    void newFusionClipFromSelectionOpensCompositingPage()
    {
        TimelineFixture f;
        TimelineController c(f.ctx);
        c.connectActions();
        f.project.beginEdit(QStringLiteral("setup"));
        const quint64 a = ops::overwriteClip(f.data(), {TrackType::Video, 0}, solidClip(0, 20));
        const quint64 b = ops::overwriteClip(f.data(), {TrackType::Video, 1}, solidClip(5, 20));
        f.project.endEdit();
        f.selection.setClips({a, b});
        QSignalSpy opened(f.actions.action(QStringLiteral("clip.open_in_compositing")),
            &QAction::triggered);
        c.setNamePrompt([](const QString& title, const QString&, const QString& suggestion,
                            const QString&) -> std::optional<QString> {
            return title == QLatin1String("New Fusion Clip") ? std::optional<QString>(suggestion)
                                                             : std::nullopt;
        });
        const int undoCount = f.project.undoStack()->count();
        const quint64 uid = c.newFusionClip();
        QVERIFY(uid);
        QCOMPARE(f.project.undoStack()->count(), undoCount + 1);
        QCOMPARE(opened.count(), 1); // MainWindow turns this into the Compositing page
        QVERIFY(f.project.isFusionClip(uid));
        QCOMPARE(f.project.timelineData(uid)->name, QStringLiteral("Fusion Clip"));
        // The new instance is selected and has the graph.
        const QVector<quint64> sel = f.selection.clips();
        QCOMPARE(sel.size(), 1);
        QVERIFY(ops::clipById(f.view(), sel[0])->composition);

        // Cancelling the name prompt changes nothing.
        f.selection.setClips({sel[0]});
        c.setNamePrompt([](const QString&, const QString&, const QString&, const QString&)
                -> std::optional<QString> { return std::nullopt; });
        QCOMPARE(c.newFusionClip(), quint64(0));
        QCOMPARE(opened.count(), 1);
    }

    void mediaPoolShowsDragsRenamesAndDeletesFusionClips()
    {
        TimelineFixture f;
        MediaPoolPanel pool(f.ctx);
        TimelineController c(f.ctx);
        c.connectActions();
        pool.resize(900, 520);
        pool.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pool));
        const QString m1 = addMedia(f.project, QStringLiteral("Beach.mov"), 4);
        const QString m2 = addMedia(f.project, QStringLiteral("Logo.mov"), 3);

        // New Fusion Composition and New Fusion Clip (from the selected media).
        const quint64 blank = pool.createFusionClip({});
        QVERIFY(blank);
        const quint64 fromMedia = pool.createFusionClip({m1, m2});
        QVERIFY(fromMedia);
        QCOMPARE(f.project.timelineData(fromMedia)->videoTracks.size(), size_t(2));
        const QString blankId = f.project.timelineItemId(blank);
        const QString mediaId = f.project.timelineItemId(fromMedia);
        QCOMPARE(pool.model()->item(blankId)->kind, MediaItemKind::FusionClip);

        // The pool lists them with the Fusion icon, kind and a tooltip.
        const auto rowOf = [&](const QString& id) {
            for (int r = 0; r < pool.model()->rowCount(); ++r) {
                if (pool.model()->index(r, 0).data(MediaPoolModel::IdRole).toString() == id)
                    return pool.model()->index(r, 0);
            }
            return QModelIndex();
        };
        const QModelIndex row = rowOf(mediaId);
        QVERIFY(row.isValid());
        QCOMPARE(row.data(MediaPoolModel::KindRole).toInt(), int(MediaPoolModel::FusionKind));
        QVERIFY(!row.data(Qt::DecorationRole).value<QIcon>().isNull());
        QVERIFY(row.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Fusion clip")));

        // Drag / append twice: two instances with the same graph and media item.
        pool.setSelectedMediaIds({mediaId});
        c.insertSource(mediaId, 0, 0, SourceEditMode::AppendAtEnd);
        c.insertSource(mediaId, 0, 0, SourceEditMode::AppendAtEnd);
        QVector<const ClipData*> inst;
        for (const ClipData& clip : f.view().videoTracks[0].clips) {
            if (clip.kind == ClipKind::Nested && clip.nestedUid == fromMedia)
                inst.append(&clip);
        }
        QCOMPARE(inst.size(), 2);
        QCOMPARE(inst[0]->mediaId, mediaId);
        QVERIFY(inst[0]->composition && inst[1]->composition);
        QCOMPARE(*inst[0]->composition, *inst[1]->composition);
        // Editing one updates both.
        Composition edited = *inst[0]->composition;
        edited.viewZoom = 2.0;
        CompNode n;
        n.id = edited.allocateId();
        n.type = QStringLiteral("transform");
        n.name = QStringLiteral("Transform1");
        edited.nodes.append(n);
        QVERIFY(f.project.setClipComposition(
            f.project.activeTimelineUid(), inst[0]->id, edited, QStringLiteral("Add Node")));
        QCOMPARE(*f.view().videoTracks[0].clips[0].composition, edited);
        QCOMPARE(*f.view().videoTracks[0].clips[1].composition, edited);

        // Rename from the pool.
        f.project.beginEdit(QStringLiteral("Rename"));
        f.project.mediaPool()->renameItem(mediaId, QStringLiteral("Titles"));
        f.project.endEdit();
        QCOMPARE(pool.model()->item(mediaId)->name, QStringLiteral("Titles"));
        QCOMPARE(f.view().videoTracks[0].clips[0].name, QStringLiteral("Titles"));

        // The panel counts the two instances (the delete warning uses this) ...
        QCOMPARE(f.project.timelineUseCount(fromMedia), 2);
        QCOMPARE(usage::compute(f.project).value(mediaId, 0), 2);
        // ... and deleting an unused one needs no warning: the entry goes.
        QCOMPARE(usage::compute(f.project).value(blankId, 0), 0);
        QCOMPARE(pool.deleteItems({blankId}), 1);
        QVERIFY(!f.project.hasTimeline(blank));
        f.project.undo();
        QVERIFY(f.project.isFusionClip(blank));

        // The item menu offers both entries; the right-click menu is shown in the screenshot.
        QAbstractItemView* view = nullptr;
        for (QAbstractItemView* v : pool.findChildren<QAbstractItemView*>()) {
            if (v->isVisible() && v->model() && v->objectName() != QLatin1String("BinTree"))
                view = v;
        }
        QVERIFY(view);
        pool.setSelectedMediaIds({m1});
        QStringList entries;
        QTimer::singleShot(100, [&entries] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                const auto subs = menu->findChildren<QMenu*>();
                for (const QMenu* s : subs) {
                    for (QAction* a : s->actions())
                        entries.append(plain(a->text()));
                }
                for (QAction* a : menu->actions())
                    entries.append(plain(a->text()));
                if (const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR"); !dir.isEmpty()) {
                    for (const QMenu* s : subs) {
                        if (s->objectName() == QLatin1String("AddToPoolMenu")
                            || s->title() == QLatin1String("Add")) {
                            const_cast<QMenu*>(s)->popup(menu->pos() + QPoint(menu->width(), 0));
                            QCoreApplication::processEvents();
                            QDir().mkpath(dir);
                            s->grab().save(QDir(dir).filePath(QStringLiteral("fusion-pool-menu.png")));
                        }
                    }
                }
                menu->close();
            }
        });
        const QModelIndex first = rowOf(m1);
        const QPoint at = view->visualRect(view->model()->index(0, 0)).center();
        Q_UNUSED(first);
        QContextMenuEvent event(QContextMenuEvent::Mouse, at, view->viewport()->mapToGlobal(at));
        QCoreApplication::sendEvent(view->viewport(), &event);
        QVERIFY2(entries.contains(QStringLiteral("New Fusion Composition")),
            qPrintable(entries.join('|')));
        QVERIFY2(entries.contains(QStringLiteral("New Fusion Clip")), qPrintable(entries.join('|')));
        if (const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR"); !dir.isEmpty()) {
            QDir().mkpath(dir);
            QCoreApplication::processEvents();
            pool.grab().save(QDir(dir).filePath(QStringLiteral("fusion-media-pool.png")));
        }
    }
};

QTEST_MAIN(TestFusionClipUi)
#include "test_fusionclipui.moc"
