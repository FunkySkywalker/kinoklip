// SPDX-License-Identifier: GPL-3.0-or-later
// Spec 131: drawing on the Paint node in a Compositing page viewer. Mouse events go to the viewer's
// control overlay; one drag is one stroke and one undo step.
#include "app/MainWindow.h"
#include "app/pages/CompositingPage.h"
#include "core/EditOps.h"
#include "core/NodeRegistry.h"
#include "core/PaintStrokes.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/StandardActions.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/CompViewer.h"
#include "ui/comp/CompViewerOverlay.h"
#include "ui/viewer/ViewerSurface.h"

#include <QApplication>
#include <QMouseEvent>
#include <QTest>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    quint64 clipId = 0;
    quint64 paintId = 0;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.name = QStringLiteral("Plate");
        clip.start = 10;
        clip.duration = 50;
        project.beginEdit(QStringLiteral("Add Clip"));
        clipId = editops::overwriteClip(
            project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    CompositingPage* page() const { return window->compositingPage(); }
    CompSession* session() const { return page()->session(); }
    const Composition& comp() const { return session()->composition(); }
    int steps() const { return project.undoStack()->count(); }

    /// Opens the page with a Paint node after MediaIn1, selected and shown in the right viewer.
    CompViewerOverlay* openPaint()
    {
        project.timeline()->setPlayhead(20);
        window->resize(1800, 1000);
        window->show();
        (void)QTest::qWaitForWindowExposed(window.get());
        actions.action("view.page_compositing")->trigger();
        QCoreApplication::processEvents();
        for (const CompNode& n : comp().nodes)
            if (n.name == QStringLiteral("MediaIn1"))
                session()->setSelection({n.id});
        paintId = session()->addNode(nodetypes::Paint);
        session()->setSelection({paintId});
        session()->setViewerNode(1, paintId);
        QCoreApplication::processEvents();
        return page()->viewer(1)->controls();
    }

    QVector<paint::Stroke> strokes() const
    {
        const CompNode* n = comp().node(paintId);
        return n ? paint::strokesOf(n->params) : QVector<paint::Stroke>();
    }

    void setParam(const QString& key, double v)
    {
        session()->edit(QStringLiteral("Set"), [&](Composition& c) {
            c.node(paintId)->params.setBase(key, v);
            return true;
        });
    }
    void setStatic(const QString& key, const QString& v)
    {
        session()->edit(QStringLiteral("Set"), [&](Composition& c) {
            c.node(paintId)->params.setStatic(key, v);
            return true;
        });
    }
};

void send(CompViewerOverlay* o, QEvent::Type type, QPointF p, Qt::MouseButton b,
    Qt::MouseButtons bs, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent e(type, p, o->mapToGlobal(p), b, bs, mods);
    QApplication::sendEvent(o, &e);
}

/// A drag of @p count moves from @p from by @p step each.
void drag(CompViewerOverlay* o, QPointF from, QPointF step, int count)
{
    send(o, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= count; ++i)
        send(o, QEvent::MouseMove, from + step * i, Qt::NoButton, Qt::LeftButton);
    send(o, QEvent::MouseButtonRelease, from + step * count, Qt::LeftButton, Qt::NoButton);
}

} // namespace

class TestPaintViewer : public QObject
{
    Q_OBJECT
private slots:
    void aDragIsOneStrokeAndOneUndoStep()
    {
        Fixture f;
        CompViewerOverlay* o = f.openPaint();
        QVERIFY(o->hasControls());
        QCOMPARE(o->handleAt(QPointF(o->width() / 2.0, o->height() / 2.0)),
            CompViewerOverlay::Handle::None);
        f.setParam(nodekeys::BrushSize, 0.1);
        const int before = f.steps();
        const QPointF start(o->width() * 0.3, o->height() * 0.5);
        drag(o, start, QPointF(10, 0), 6);
        QCOMPARE(f.steps(), before + 1);
        const QVector<paint::Stroke> s = f.strokes();
        QCOMPARE(s.size(), 1);
        QVERIFY(s[0].points.size() >= 7);
        QCOMPARE(int(s[0].tool), int(paint::Tool::Stroke));
        QCOMPARE(s[0].size, 0.1);
        QCOMPARE(s[0].points.first().pressure, 1.0f);
        // Left to right along the same height; y 0..1 from the bottom.
        QVERIFY(s[0].points.last().x > s[0].points.first().x);
        QVERIFY(std::abs(s[0].points.last().y - s[0].points.first().y) < 0.001f);
        QVERIFY(s[0].points.first().x > 0.0f && s[0].points.first().x < 1.0f);
        // One frame: the playhead's comp frame (clip starts at 10, playhead 20).
        QCOMPARE(s[0].first, f.session()->compFrame());
        QCOMPARE(s[0].last, f.session()->compFrame());
        // A second stroke adds one more; undo takes strokes back one at a time.
        drag(o, start + QPointF(0, 40), QPointF(0, 10), 4);
        QCOMPARE(f.strokes().size(), 2);
        QCOMPARE(f.steps(), before + 2);
        f.project.undo();
        QCOMPARE(f.strokes().size(), 1);
        f.project.undo();
        QCOMPARE(f.strokes().size(), 0);
        f.project.redo();
        QCOMPARE(f.strokes().size(), 1);
    }

    void theToolAndDurationComeFromTheNode()
    {
        Fixture f;
        CompViewerOverlay* o = f.openPaint();
        f.setStatic(nodekeys::PaintTool, painttools::Eraser);
        f.setStatic(nodekeys::StrokeDuration, paintdurations::Whole);
        drag(o, QPointF(o->width() * 0.4, o->height() * 0.4), QPointF(8, 3), 4);
        const QVector<paint::Stroke> s = f.strokes();
        QCOMPARE(s.size(), 1);
        QCOMPARE(int(s[0].tool), int(paint::Tool::Eraser));
        QVERIFY(s[0].visibleAt(0) && s[0].visibleAt(1000));
        f.setStatic(nodekeys::StrokeDuration, paintdurations::Frames);
        f.setParam(nodekeys::StrokeFrames, 5);
        drag(o, QPointF(o->width() * 0.4, o->height() * 0.6), QPointF(8, 3), 4);
        const paint::Stroke last = f.strokes().last();
        QCOMPARE(last.last - last.first, int64_t(4));
    }

    void altClickSetsTheCloneSourceAndTheNextStrokeItsOffset()
    {
        Fixture f;
        CompViewerOverlay* o = f.openPaint();
        f.setStatic(nodekeys::PaintTool, painttools::Clone);
        const int before = f.steps();
        const QPointF source(o->width() * 0.25, o->height() * 0.5);
        send(o, QEvent::MouseButtonPress, source, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
        send(o, QEvent::MouseButtonRelease, source, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
        QVERIFY(o->cloneSource());
        QCOMPARE(f.steps(), before); // setting the source paints nothing
        QCOMPARE(f.strokes().size(), 0);
        // The brush goes 100 px right and 20 px down: the source is to its left and above.
        const QRectF pic = f.page()->viewer(1)->surface()->pictureRect();
        const QPointF start = source + QPointF(100, 20);
        drag(o, start, QPointF(5, 0), 3);
        const QVector<paint::Stroke> s = f.strokes();
        QCOMPARE(s.size(), 1);
        QCOMPARE(int(s[0].tool), int(paint::Tool::Clone));
        QVERIFY(std::abs(s[0].cloneX + 100.0 / pic.width()) < 0.002);
        QVERIFY(std::abs(s[0].cloneY - 20.0 / pic.height()) < 0.002);
        // The Inspector's offset shows the same.
        QVERIFY(std::abs(f.comp().node(f.paintId)->params.value(nodekeys::CloneOffsetX, 0, 0) - s[0].cloneX) < 1e-9);
    }

    void aLockedNodeTakesNoStrokes()
    {
        Fixture f;
        CompViewerOverlay* o = f.openPaint();
        f.session()->toggleLock();
        drag(o, QPointF(o->width() * 0.4, o->height() * 0.4), QPointF(8, 3), 4);
        QCOMPARE(f.strokes().size(), 0);
    }

    void escapeCancelsTheStroke()
    {
        Fixture f;
        CompViewerOverlay* o = f.openPaint();
        const QPointF p(o->width() * 0.4, o->height() * 0.4);
        send(o, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        send(o, QEvent::MouseMove, p + QPointF(10, 0), Qt::NoButton, Qt::LeftButton);
        QVERIFY(o->painting());
        QCOMPARE(f.strokes().size(), 1);
        const int stepsBefore = f.steps();
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(o, &esc);
        QVERIFY(!o->painting());
        QCOMPARE(f.strokes().size(), 0);
        QCOMPARE(f.steps(), stepsBefore); // no undo step is left
    }
};

QTEST_MAIN(TestPaintViewer)
#include "test_paintviewer.moc"
