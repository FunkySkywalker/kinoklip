// SPDX-License-Identifier: GPL-3.0-or-later
// Color page viewer features (docs/color-page.md, "Split Screen, Lightbox, Output Blanking"):
// View > Split Screen (Selected Clips, Timeline; one comparison at a time with the gallery
// wipe; click a tile to grade it), View > Lightbox (a grid of every clip, click to go to it,
// size / labels / grade info options) and Timeline > Output Blanking (the presets as checked
// menu entries following the timeline, one undo step each). The bars in a rendered frame are
// tested in test_outputblanking.
#include "app/MainWindow.h"
#include "app/pages/ColorPage.h"
#include "core/EditOps.h"
#include "core/GradeGallery.h"
#include "core/GradeMath.h"
#include "core/OutputBlanking.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/color/ColorFrameSource.h"
#include "ui/color/ColorLightbox.h"
#include "ui/color/ColorSession.h"
#include "ui/color/ColorSplitScreen.h"
#include "ui/color/ColorViewerOverlay.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QPainter>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

const QColor kColours[] = {QColor(200, 60, 40), QColor(40, 90, 200), QColor(60, 180, 70),
    QColor(180, 180, 60)};

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QVector<quint64> clips;

    explicit Fixture(int clipCount = 4)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        project.beginEdit(QStringLiteral("Add Clips"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 320;
        tl.height = 180;
        for (int i = 0; i < clipCount; ++i) {
            ClipData clip;
            clip.kind = ClipKind::Solid;
            clip.solidColor = kColours[i % 4];
            clip.name = QStringLiteral("Clip %1").arg(i + 1);
            clip.start = i * 50;
            clip.duration = 50;
            clips.append(editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip));
        }
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    ColorPage* page() const { return window->colorPage(); }
    ColorSession* session() const { return page()->session(); }
    void showPage()
    {
        window->resize(QSize(1600, 900));
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        action("view.page_color")->trigger();
        QCoreApplication::processEvents();
    }
    /// Gives clip @p i (index in `clips`) a detail picture of its solid colour.
    void paintDetail(int i)
    {
        QImage img(480, 270, QImage::Format_ARGB32_Premultiplied);
        img.fill(kColours[i % 4]);
        page()->frames()->setDetailImage(clips[i], img);
    }
};

QColor pixelAt(QWidget* w, QPointF p)
{
    QImage img(w->size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    w->render(&img);
    return img.pixelColor(p.toPoint());
}

} // namespace

class TestColorViewer : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-test"));
        QCoreApplication::setApplicationName(QStringLiteral("test_colorviewer"));
        QSettings().clear();
        initResources();
    }

    void keysAndMenuActions()
    {
        Fixture f;
        QVERIFY(f.action("color.split_screen")->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+Alt+W"))));
        QVERIFY(f.action("color.lightbox")->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+Alt+L"))));
        for (const char* id : {"color.split_screen", "color.split_selected", "color.split_timeline",
                 "color.lightbox"})
            QVERIFY(f.action(id)->isCheckable());
        QCOMPARE(f.actions.menuEntries(QStringLiteral("color")).contains(QStringLiteral("color.lightbox")), true);
    }

    void splitSelectedClips()
    {
        Fixture f;
        f.showPage();
        ColorSplitScreen* split = f.page()->splitScreen();
        QCOMPARE(split->mode(), ColorSplitScreen::Mode::Off);
        QVERIFY(split->tiles().isEmpty());
        f.session()->setSelectedClips({f.clips[0], f.clips[2]});
        f.action("color.split_selected")->trigger();
        QCOMPARE(f.page()->splitMode(), ColorSplitScreen::Mode::SelectedClips);
        QVERIFY(f.action("color.split_selected")->isChecked());
        QVERIFY(f.action("color.split_screen")->isChecked());
        QVERIFY(!f.action("color.split_timeline")->isChecked());
        QCOMPARE(split->clips(), (QVector<quint64> {f.clips[0], f.clips[2]}));
        // Two tiles side by side over the viewer picture, whole pictures each.
        auto tiles = split->tiles();
        QCOMPARE(tiles.size(), 2);
        QCOMPARE(tiles[0].source, QRectF(0, 0, 1, 1));
        QVERIFY(tiles[0].rect.right() <= tiles[1].rect.left() + 1);
        QCOMPARE(tiles[0].rect.top(), tiles[1].rect.top());
        // The selected clips' pictures are drawn in their tiles (the clip colours).
        f.paintDetail(0);
        f.paintDetail(2);
        QCOMPARE(split->clips().size(), 2);
        const QColor a = pixelAt(split, tiles[0].rect.center());
        const QColor b = pixelAt(split, tiles[1].rect.center());
        QVERIFY2(qAbs(a.red() - kColours[0].red()) < 6 && qAbs(a.blue() - kColours[0].blue()) < 6,
            qPrintable(a.name()));
        QVERIFY2(qAbs(b.green() - kColours[2].green()) < 6 && qAbs(b.red() - kColours[2].red()) < 6,
            qPrintable(b.name()));
        // Four clips make a 2 x 2 grid.
        f.session()->setSelectedClips(f.clips);
        tiles = split->tiles();
        QCOMPARE(tiles.size(), 4);
        QCOMPARE(tiles[0].rect.top(), tiles[1].rect.top());
        QVERIFY(tiles[2].rect.top() > tiles[0].rect.top());
        // One selected clip: the clip under the playhead joins it (something to compare).
        f.session()->setSelectedClips({f.clips[3]});
        QCOMPARE(split->clips().size(), 2);
        QVERIFY(split->clips().contains(f.session()->clipId()));
        // Clicking a tile grades that clip.
        f.session()->setSelectedClips({f.clips[0], f.clips[1], f.clips[2]});
        tiles = split->tiles();
        QTest::mouseClick(split, Qt::LeftButton, {}, tiles[2].rect.center().toPoint());
        QCOMPARE(f.session()->clipId(), f.clips[2]);
        // Ctrl+Alt+W (color.split_screen) ends it and brings it back in the mode last used.
        f.action("color.split_screen")->trigger();
        QCOMPARE(f.page()->splitMode(), ColorSplitScreen::Mode::Off);
        QVERIFY(split->tiles().isEmpty());
        QVERIFY(!f.action("color.split_selected")->isChecked());
        f.action("color.split_screen")->trigger();
        QCOMPARE(f.page()->splitMode(), ColorSplitScreen::Mode::SelectedClips);
        // The header button is the same switch.
        QToolButton* button = f.page()->findChild<QToolButton*>(QStringLiteral("ColorSplitButton"));
        QVERIFY(button);
        QVERIFY(button->isChecked());
        button->click();
        QCOMPARE(f.page()->splitMode(), ColorSplitScreen::Mode::Off);
        QVERIFY(!f.action("color.split_screen")->isChecked());
    }

    void splitTimelineNeighbours()
    {
        Fixture f(6);
        f.showPage();
        ColorSplitScreen* split = f.page()->splitScreen();
        f.session()->gradeClip(f.clips[3]);
        f.action("color.split_timeline")->trigger();
        QCOMPARE(f.page()->splitMode(), ColorSplitScreen::Mode::Timeline);
        QVERIFY(f.action("color.split_timeline")->isChecked());
        QVERIFY(!f.action("color.split_selected")->isChecked());
        // The clip under the playhead and two neighbours on each side.
        QCOMPARE(split->clips(),
            (QVector<quint64> {f.clips[1], f.clips[2], f.clips[3], f.clips[4], f.clips[5]}));
        const auto tiles = split->tiles();
        QCOMPARE(tiles.size(), 5);
        // Vertical strips side by side, each showing its fifth of its clip's picture.
        for (int i = 0; i < 5; ++i) {
            QCOMPARE(tiles[i].source, QRectF(i / 5.0, 0, 0.2, 1));
            if (i > 0) {
                QVERIFY(qAbs(tiles[i].rect.left() - tiles[i - 1].rect.right()) < 0.01);
                QCOMPARE(tiles[i].rect.top(), tiles[0].rect.top());
                QCOMPARE(tiles[i].rect.height(), tiles[0].rect.height());
            }
        }
        // Near the start of the timeline there are fewer neighbours.
        f.session()->gradeClip(f.clips[0]);
        QCOMPARE(split->clips(), (QVector<quint64> {f.clips[0], f.clips[1], f.clips[2]}));
        // Switching the mode keeps one split screen.
        f.action("color.split_selected")->trigger();
        QCOMPARE(f.page()->splitMode(), ColorSplitScreen::Mode::SelectedClips);
        QVERIFY(!f.action("color.split_timeline")->isChecked());
        // Unchecking the mode ends the split screen.
        f.action("color.split_selected")->trigger();
        QCOMPARE(f.page()->splitMode(), ColorSplitScreen::Mode::Off);
    }

    void splitAndWipeExcludeEachOther()
    {
        Fixture f;
        f.showPage();
        // A gallery still to wipe with.
        QImage img(320, 180, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(90, 90, 200));
        ClipGrade still;
        gradeops::initialize(still);
        f.project.gallery()->addStill(img, still);
        f.action("color.wipe")->trigger();
        QVERIFY(f.page()->overlay()->isWiping());
        QVERIFY(f.action("color.wipe")->isChecked());
        // Split screen on: the wipe ends.
        f.action("color.split_timeline")->trigger();
        QVERIFY(!f.page()->overlay()->isWiping());
        QVERIFY(!f.action("color.wipe")->isChecked());
        QCOMPARE(f.page()->splitMode(), ColorSplitScreen::Mode::Timeline);
        // Wipe on: the split screen ends.
        f.action("color.wipe")->trigger();
        QVERIFY(f.page()->overlay()->isWiping());
        QCOMPARE(f.page()->splitMode(), ColorSplitScreen::Mode::Off);
        QVERIFY(!f.action("color.split_screen")->isChecked());
    }

    void lightboxGridAndOptions()
    {
        QSettings().clear();
        Fixture f(5);
        f.showPage();
        ColorLightbox* box = f.page()->lightbox();
        ColorLightboxGrid* grid = box->grid();
        QVERIFY(!f.page()->isLightbox());
        f.action("color.lightbox")->trigger();
        QVERIFY(f.page()->isLightbox());
        QVERIFY(f.action("color.lightbox")->isChecked());
        QVERIFY(box->isVisible());
        QCOMPARE(grid->clips(), f.clips);
        for (int i = 0; i < 5; ++i)
            QVERIFY(grid->cellRect(f.clips[i]).isValid());
        QVERIFY(grid->columns() >= 2);
        // Cells in strip order, left to right.
        QVERIFY(grid->cellRect(f.clips[1]).left() > grid->cellRect(f.clips[0]).left());
        // The graded pictures show in the cells.
        f.paintDetail(2);
        const QRect cell = grid->cellRect(f.clips[2]);
        const QColor c = pixelAt(grid->viewport(), QPointF(cell.center().x(), cell.top() + cell.height() / 3));
        QVERIFY2(qAbs(c.green() - kColours[2].green()) < 8 && qAbs(c.red() - kColours[2].red()) < 8,
            qPrintable(c.name()));
        // A click goes to the clip; Ctrl-click adds it to the selected clips.
        QTest::mouseClick(grid->viewport(), Qt::LeftButton, {}, grid->cellRect(f.clips[3]).center());
        QCOMPARE(f.session()->clipId(), f.clips[3]);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(3 * 50));
        QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::ControlModifier, grid->cellRect(f.clips[1]).center());
        QVERIFY(f.session()->selectedClips().contains(f.clips[1]));
        QVERIFY(f.session()->selectedClips().contains(f.clips[3]));
        QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::ControlModifier, grid->cellRect(f.clips[1]).center());
        QVERIFY(!f.session()->selectedClips().contains(f.clips[1]));
        // Size option: Large cells are bigger than Small ones.
        box->sizeBox()->setCurrentIndex(0);
        QCOMPARE(grid->thumbnailSize(), ColorLightboxGrid::Size::Small);
        const int small = grid->cellRect(f.clips[0]).width();
        box->sizeBox()->setCurrentIndex(2);
        QCOMPARE(grid->thumbnailSize(), ColorLightboxGrid::Size::Large);
        QVERIFY(grid->cellRect(f.clips[0]).width() > small * 2);
        // Labels and grade info change the cell height.
        box->sizeBox()->setCurrentIndex(1);
        const int full = grid->cellRect(f.clips[0]).height();
        box->infoBox()->setChecked(false);
        const int noInfo = grid->cellRect(f.clips[0]).height();
        QVERIFY(noInfo < full);
        box->labelsBox()->setChecked(false);
        QVERIFY(grid->cellRect(f.clips[0]).height() < noInfo);
        QVERIFY(!grid->showLabels());
        QVERIFY(!grid->showGradeInfo());
        // The options are remembered.
        QCOMPARE(QSettings().value(QStringLiteral("color/lightboxLabels")).toBool(), false);
        box->labelsBox()->setChecked(true);
        box->infoBox()->setChecked(true);
        // Double-click returns to the viewer.
        QTest::mouseClick(grid->viewport(), Qt::LeftButton, {}, grid->cellRect(f.clips[0]).center());
        QTest::mouseDClick(grid->viewport(), Qt::LeftButton, {}, grid->cellRect(f.clips[0]).center());
        QVERIFY(!f.page()->isLightbox());
        QVERIFY(!f.action("color.lightbox")->isChecked());
        QCOMPARE(f.session()->clipId(), f.clips[0]);
        // The Lightbox and the split screen exclude each other.
        f.action("color.split_selected")->trigger();
        f.action("color.lightbox")->trigger();
        QVERIFY(f.page()->isLightbox());
        QCOMPARE(f.page()->splitMode(), ColorSplitScreen::Mode::Off);
        f.action("color.split_timeline")->trigger();
        QVERIFY(!f.page()->isLightbox());
        QCOMPARE(f.page()->splitMode(), ColorSplitScreen::Mode::Timeline);
    }

    void outputBlankingMenu()
    {
        Fixture f;
        const quint64 uid = f.project.activeTimelineUid();
        QVERIFY(f.action("timeline.blanking_off")->isChecked());
        const int base = f.project.undoStack()->index();
        f.action("timeline.blanking_2_39")->trigger();
        QCOMPARE(f.project.outputBlanking(uid), 2.39);
        QCOMPARE(f.project.undoStack()->index(), base + 1);
        QVERIFY(f.action("timeline.blanking_2_39")->isChecked());
        QVERIFY(!f.action("timeline.blanking_off")->isChecked());
        QVERIFY(!f.action("timeline.blanking_1_85")->isChecked());
        // Another preset replaces it; every preset of the menu works.
        for (const outputblanking::Preset& p : outputblanking::presets()) {
            const QString id = QStringLiteral("timeline.blanking_") + QLatin1String(p.id);
            f.actions.action(id)->trigger();
            QCOMPARE(f.project.outputBlanking(uid), p.aspect);
            QVERIFY(f.actions.action(id)->isChecked());
        }
        // Undo and redo move the check mark with the setting.
        f.action("edit.undo")->trigger();
        QCOMPARE(f.project.outputBlanking(uid), 2.39);
        QVERIFY(f.action("timeline.blanking_2_39")->isChecked());
        QVERIFY(!f.action("timeline.blanking_2_40")->isChecked());
        while (f.project.undoStack()->canUndo() && f.project.outputBlanking(uid) != 0.0)
            f.action("edit.undo")->trigger();
        QCOMPARE(f.project.outputBlanking(uid), 0.0);
        QVERIFY(f.action("timeline.blanking_off")->isChecked());
        // Off removes the bars again; clicking the checked preset keeps it chosen.
        f.action("timeline.blanking_1_85")->trigger();
        f.action("timeline.blanking_1_85")->trigger();
        QCOMPARE(f.project.outputBlanking(uid), 1.85);
        QVERIFY(f.action("timeline.blanking_1_85")->isChecked());
        f.action("timeline.blanking_off")->trigger();
        QCOMPARE(f.project.outputBlanking(uid), 0.0);
        QVERIFY(f.action("timeline.blanking_off")->isChecked());
    }
};

QTEST_MAIN(TestColorViewer)
#include "test_colorviewer.moc"
