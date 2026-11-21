// SPDX-License-Identifier: GPL-3.0-or-later
// Timeline > Output Blanking (docs/color-page.md): black bars on the output of a timeline. The
// bar geometry, the setting as one undo step, saved with the timeline (and absent in files
// without it), and the bars in an exported frame (ExportJob, lossless PNG), in the viewer's
// render (Compositor) and in the render cache key. Small frames: llvmpipe.
#include "ExportTestUtil.h"
#include "core/OutputBlanking.h"
#include "core/Timeline.h"
#include "export/ExportJob.h"
#include "render/Compositor.h"
#include "render/TimelineEval.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <memory>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int kW = 160;
constexpr int kH = 90;

/// A project with one bright orange solid clip over [0, 10).
void buildProject(Project& project)
{
    project.beginEdit(QStringLiteral("Setup"));
    TimelineData& tl = project.timeline()->mutableData();
    tl.width = kW;
    tl.height = kH;
    tl.fps = {25, 1};
    tl.videoTracks[0].clips.clear();
    addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(230, 140, 30), 0, 10));
    project.endEdit();
}

bool isBlack(const QImage& image, int x, int y)
{
    const QColor c = image.pixelColor(x, y);
    return c.red() < 6 && c.green() < 6 && c.blue() < 6;
}

bool isOrange(const QImage& image, int x, int y)
{
    const QColor c = image.pixelColor(x, y);
    return c.red() > 200 && c.green() > 110 && c.blue() < 60;
}

} // namespace

class TestOutputBlanking : public QObject
{
    Q_OBJECT
private slots:
    void presetsAndGeometry()
    {
        const auto presets = outputblanking::presets();
        QCOMPARE(presets.size(), size_t(8));
        QCOMPARE(QString::fromLatin1(presets.front().label), QStringLiteral("1.33"));
        QCOMPARE(QString::fromLatin1(presets.back().label), QStringLiteral("2.40"));
        // Off, and the frame's own aspect: nothing is blanked.
        QCOMPARE(outputblanking::pictureRect({1920, 1080}, 0.0), QRect(0, 0, 1920, 1080));
        QVERIFY(!outputblanking::blanks({1920, 1080}, 16.0 / 9.0));
        // Wider than the frame: bars above and below, centred.
        const QRect scope = outputblanking::pictureRect({1920, 1080}, 2.39);
        QCOMPARE(scope.width(), 1920);
        QCOMPARE(scope.height(), 803);
        QCOMPARE(scope.top(), (1080 - 803) / 2);
        // Narrower: bars left and right.
        const QRect four3 = outputblanking::pictureRect({1920, 1080}, 4.0 / 3.0);
        QCOMPARE(four3.height(), 1080);
        QCOMPARE(four3.width(), 1440);
        QCOMPARE(four3.left(), 240);
        QCOMPARE(outputblanking::aspectLabel(0.0), QStringLiteral("Off"));
        QCOMPARE(outputblanking::aspectLabel(2.39), QStringLiteral("2.39"));
    }

    void settingIsUndoableAndSaved()
    {
        QTemporaryDir dir;
        Project project;
        buildProject(project);
        const quint64 uid = project.activeTimelineUid();
        QCOMPARE(project.outputBlanking(uid), 0.0);
        const int base = project.undoStack()->index();
        QVERIFY(project.setOutputBlanking(uid, 2.39, QStringLiteral("Output Blanking 2.39")));
        QCOMPARE(project.outputBlanking(uid), 2.39);
        QCOMPARE(project.undoStack()->index(), base + 1); // one undo step
        // The same value again is no step.
        QVERIFY(project.setOutputBlanking(uid, 2.39, QStringLiteral("again")));
        QCOMPARE(project.undoStack()->index(), base + 1);
        // Saved with the timeline and loaded back.
        const QString path = dir.filePath(QStringLiteral("blank.kinoklip"));
        QString error;
        QVERIFY2(project.save(path, &error), qPrintable(error));
        Project loaded;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        QCOMPARE(loaded.outputBlanking(loaded.activeTimelineUid()), 2.39);
        // Undo and redo.
        project.undoStack()->undo();
        QCOMPARE(project.outputBlanking(uid), 0.0);
        project.undoStack()->redo();
        QCOMPARE(project.outputBlanking(uid), 2.39);
        // Off is saved as an absent key, so files without blanking stay as they were.
        QVERIFY(project.setOutputBlanking(uid, 0.0, QStringLiteral("Off")));
        QVERIFY2(project.save(path, &error), qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("outputBlanking"));
        Project off;
        QVERIFY2(off.load(path, &error), qPrintable(error));
        QCOMPARE(off.outputBlanking(off.activeTimelineUid()), 0.0);
        // Unknown timeline: refused.
        QVERIFY(!project.setOutputBlanking(0xdeadbeef, 2.0, QStringLiteral("x")));
    }

    void barsInTheExportedFrame()
    {
        QTemporaryDir dir;
        Project project;
        buildProject(project);
        const quint64 uid = project.activeTimelineUid();
        QVERIFY(project.setOutputBlanking(uid, 2.39, QStringLiteral("Output Blanking 2.39")));
        const auto snapshot = project.timeline()->snapshot();
        auto media = std::make_shared<MediaPathMap>();
        auto compositor = std::make_shared<Compositor>();
        compositor->setMediaPaths(media);

        ExportSettings s;
        s.container = QStringLiteral("png_still");
        s.video.codec = QStringLiteral("png");
        s.range = RangeMode::Custom;
        s.rangeIn = 5;
        s.rangeOut = 6;
        s.outputPath = dir.filePath(QStringLiteral("frame.png"));
        ExportJob job(s, snapshot, media);
        job.setCompositor(compositor.get());
        const ExportResult r = job.run();
        QVERIFY2(r.ok, qPrintable(r.error));
        const QImage exported = QImage(s.outputPath).convertToFormat(QImage::Format_RGB32);
        QCOMPARE(exported.size(), QSize(kW, kH));
        const QRect keep = outputblanking::pictureRect({kW, kH}, 2.39);
        QVERIFY(keep.top() > 5); // a real letterbox on this frame
        for (const int x : {0, kW / 2, kW - 1}) {
            QVERIFY(isBlack(exported, x, 0));
            QVERIFY(isBlack(exported, x, keep.top() - 1));
            QVERIFY(isOrange(exported, x, keep.top()));
            QVERIFY(isOrange(exported, x, kH / 2));
            QVERIFY(isOrange(exported, x, keep.bottom()));
            QVERIFY(isBlack(exported, x, keep.bottom() + 1));
            QVERIFY(isBlack(exported, x, kH - 1));
        }

        // The viewer's render shows the same bars.
        const QImage viewer = compositor->renderImage(snapshot, 5).convertToFormat(QImage::Format_RGB32);
        QCOMPARE(viewer.size(), QSize(kW, kH));
        QVERIFY(isBlack(viewer, kW / 2, 0));
        QVERIFY(isOrange(viewer, kW / 2, kH / 2));
        QVERIFY(isBlack(viewer, kW / 2, kH - 1));

        // Narrower than the frame: bars at the sides (4:3 on a 16:9 frame).
        QVERIFY(project.setOutputBlanking(uid, 1.33, QStringLiteral("Output Blanking 1.33")));
        const QImage pillar = compositor->renderImage(project.timeline()->snapshot(), 5)
                                  .convertToFormat(QImage::Format_RGB32);
        const QRect side = outputblanking::pictureRect({kW, kH}, 1.33);
        QVERIFY(side.left() > 5);
        QVERIFY(isBlack(pillar, 0, kH / 2));
        QVERIFY(isBlack(pillar, side.left() - 1, kH / 2));
        QVERIFY(isOrange(pillar, side.left(), kH / 2));
        QVERIFY(isOrange(pillar, kW / 2, 0));
        QVERIFY(isBlack(pillar, kW - 1, kH / 2));

        // Off again: the whole frame is picture, as an unblanked timeline renders it.
        QVERIFY(project.setOutputBlanking(uid, 0.0, QStringLiteral("Off")));
        const QImage plain = compositor->renderImage(project.timeline()->snapshot(), 5)
                                 .convertToFormat(QImage::Format_RGB32);
        QVERIFY(isOrange(plain, 0, 0));
        QVERIFY(isOrange(plain, kW - 1, kH - 1));
    }

    void renderCacheKeyFollowsTheBlanking()
    {
        Project project;
        buildProject(project);
        const quint64 uid = project.activeTimelineUid();
        const auto plain = project.timeline()->snapshot();
        QVERIFY(project.setOutputBlanking(uid, 2.39, QStringLiteral("Output Blanking 2.39")));
        const auto blanked = project.timeline()->snapshot();
        QVERIFY(!render::samePictures(*plain, *blanked));
        QVERIFY(project.setOutputBlanking(uid, 0.0, QStringLiteral("Off")));
        QVERIFY(render::samePictures(*plain, *project.timeline()->snapshot()));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestOutputBlanking>(argc, argv);
}
#include "test_outputblanking.moc"
