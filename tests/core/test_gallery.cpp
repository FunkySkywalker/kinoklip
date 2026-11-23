// SPDX-License-Identifier: GPL-3.0-or-later
// Color page gallery (spec 151): still albums, memories A to Z, the PowerGrade store shared by
// every project, saving and loading (optional "galleryExtra" key, no format bump), Append Node
// Graph, and Import Still / Export Still round trips (PNG, TIFF, JPEG, DPX + the grade file).
#include "core/GalleryStillFiles.h"
#include "core/Grade.h"
#include "core/GradeGallery.h"
#include "core/PowerGradeStore.h"
#include "core/Project.h"
#include "core/Serialization.h"

#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
namespace go = kk::gradeops;
namespace gk = kk::gradekeys;

namespace {

ClipGrade gradeWith(double saturation)
{
    ClipGrade g;
    const quint64 a = go::initialize(g);
    g.node(a)->params.setBase(gk::Saturation, saturation);
    g.node(a)->label = QStringLiteral("Look");
    return g;
}

ClipGrade twoNodeGrade()
{
    ClipGrade g = gradeWith(1.4);
    const quint64 b = go::addSerialNode(g, g.output);
    g.node(b)->params.setBase(gk::Contrast, 1.2);
    go::addWindow(g, b, GradeWindowShape::Circle);
    return g;
}

QImage picture(QColor colour, int w = 800, int h = 450)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(colour);
    QPainter p(&img);
    p.fillRect(0, 0, w / 2, h / 2, QColor(250, 20, 20));
    p.end();
    return img;
}

QString fileOf(const QTemporaryDir& dir, const QString& name)
{
    return dir.filePath(name);
}

} // namespace

class TestGallery : public QObject
{
    Q_OBJECT
private slots:
    // 151.1 - 151.4: albums
    void albums()
    {
        GradeGallery g;
        QCOMPARE(g.albums().size(), 1);
        QCOMPARE(g.albumName(GradeGallery::kDefaultAlbum), QStringLiteral("Stills"));
        const quint64 a = g.addStill(picture(Qt::blue), gradeWith(1.1));
        QCOMPARE(g.still(a)->album, GradeGallery::kDefaultAlbum);

        const quint64 night = g.addAlbum(QStringLiteral("Night"));
        QVERIFY(night != GradeGallery::kDefaultAlbum);
        // Names stay unique (case-insensitive): a second "night" becomes "night 2".
        const quint64 night2 = g.addAlbum(QStringLiteral("night"));
        QCOMPARE(g.albumName(night2), QStringLiteral("night 2"));
        QVERIFY(!g.renameAlbum(night2, QStringLiteral("NIGHT"))); // taken
        QVERIFY(!g.renameAlbum(night2, QStringLiteral("  ")));
        QVERIFY(g.renameAlbum(night2, QStringLiteral("Day")));
        QCOMPARE(g.albumName(night2), QStringLiteral("Day"));
        QVERIFY(g.renameAlbum(GradeGallery::kDefaultAlbum, QStringLiteral("Main")));

        const quint64 b = g.addStill(picture(Qt::green), gradeWith(0.8), night);
        QCOMPARE(g.stillsIn(night).size(), 1);
        QCOMPARE(g.stillsIn(GradeGallery::kDefaultAlbum).size(), 1);
        // A still of an album that does not exist goes to the first album.
        const quint64 c = g.addStill(picture(Qt::red), gradeWith(0.5), 99);
        QCOMPARE(g.still(c)->album, GradeGallery::kDefaultAlbum);

        // Move and copy between albums.
        QVERIFY(!g.moveStill(b, night)); // already there
        QVERIFY(!g.moveStill(b, 99));
        QVERIFY(g.moveStill(b, night2));
        QCOMPARE(g.stillsIn(night2).size(), 1);
        const quint64 copy = g.copyStill(b, night);
        QVERIFY(copy && copy != b);
        QCOMPARE(g.still(copy)->name, g.still(b)->name);
        QCOMPARE(g.still(copy)->grade, g.still(b)->grade);
        QCOMPARE(g.still(copy)->album, night);
        QCOMPARE(g.stillsIn(night2).size(), 1); // the original stays
        QCOMPARE(g.copyStill(b, 99), quint64(0));
        QCOMPARE(g.copyStill(12345, night), quint64(0));

        // Deleting an album deletes its stills; the first album stays.
        QVERIFY(!g.removeAlbum(GradeGallery::kDefaultAlbum));
        QVERIFY(!g.removeAlbum(99));
        QVERIFY(g.removeAlbum(night2));
        QVERIFY(!g.still(b));
        QVERIFY(g.still(copy));
        QCOMPARE(g.albums().size(), 2);
    }

    // 151.5 - 151.6: memories A to Z
    void memories()
    {
        GradeGallery g;
        QCOMPARE(GradeGallery::memoryLetter(0), QStringLiteral("A"));
        QCOMPARE(GradeGallery::memoryLetter(25), QStringLiteral("Z"));
        QCOMPARE(GradeGallery::memoryLetter(26), QString());
        QSignalSpy changed(&g, &GradeGallery::changed);
        QVERIFY(!g.memory(0).used);
        QVERIFY(g.setMemory(0, gradeWith(1.3), picture(Qt::blue)));
        QVERIFY(g.setMemory(25, twoNodeGrade()));
        QVERIFY(!g.setMemory(26, gradeWith(1.0)));
        QVERIFY(!g.setMemory(-1, gradeWith(1.0)));
        QCOMPARE(changed.count(), 2);
        QCOMPARE(g.usedMemories(), 2);
        QVERIFY(g.memory(0).used);
        QCOMPARE(g.memory(0).grade, gradeWith(1.3));
        QVERIFY(g.memory(0).image.width() <= 160);
        QVERIFY(g.memory(25).image.isNull());
        QVERIFY(g.clearMemory(0));
        QVERIFY(!g.clearMemory(0));
        QVERIFY(!g.memory(0).used);
        QVERIFY(g.hasExtra());
        g.clear();
        QCOMPARE(g.usedMemories(), 0);
        QVERIFY(!g.hasExtra());
    }

    // 151.7: albums, memories and the album of each still survive JSON
    void jsonRoundTrip()
    {
        GradeGallery g;
        const quint64 album = g.addAlbum(QStringLiteral("Looks"));
        g.renameAlbum(GradeGallery::kDefaultAlbum, QStringLiteral("Main"));
        const quint64 a = g.addStill(picture(Qt::blue), twoNodeGrade(), album);
        g.addStill(picture(Qt::green), gradeWith(0.7));
        g.setMemory(3, gradeWith(1.6), picture(Qt::yellow));

        GradeGallery back;
        back.extraFromJson(g.extraToJson());
        back.fromJson(g.toJson());
        QCOMPARE(back.albums().size(), 2);
        QCOMPARE(back.albumName(GradeGallery::kDefaultAlbum), QStringLiteral("Main"));
        QCOMPARE(back.albumName(album), QStringLiteral("Looks"));
        QCOMPARE(back.stills().size(), 2);
        QCOMPARE(back.still(a)->album, album);
        QCOMPARE(back.still(a)->grade, twoNodeGrade());
        QVERIFY(!back.still(a)->image.isNull());
        QVERIFY(back.memory(3).used);
        QCOMPARE(back.memory(3).grade, gradeWith(1.6));
        QVERIFY(!back.memory(3).image.isNull());
        QVERIFY(!back.memory(2).used);

        // A gallery with nothing extra writes nothing extra.
        GradeGallery plain;
        plain.addStill(picture(Qt::red), ClipGrade {});
        QVERIFY(plain.extraToJson().isEmpty());
        QVERIFY(!plain.toJson().at(0).toObject().contains(QStringLiteral("album")));
        // A still of an unknown album (damaged file) lands in the first album.
        QJsonArray stills = g.toJson();
        QJsonObject o = stills.at(0).toObject();
        o[QStringLiteral("album")] = 77;
        stills[0] = o;
        GradeGallery damaged;
        damaged.fromJson(stills);
        QCOMPARE(damaged.still(a)->album, GradeGallery::kDefaultAlbum);
    }

    // 151.8: the project file: optional keys, no format bump, old files load
    void projectSaveAndLoad()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = fileOf(dir, QStringLiteral("gallery.kkproj"));
        quint64 stillId = 0;
        quint64 albumId = 0;
        {
            Project p;
            p.setPowerGradeFile(fileOf(dir, QStringLiteral("pg.json")));
            albumId = p.gallery()->addAlbum(QStringLiteral("Grades"));
            stillId = p.gallery()->addStill(picture(Qt::blue), twoNodeGrade(), albumId);
            p.gallery()->setMemory(1, gradeWith(0.6), picture(Qt::green));
            QVERIFY(p.isDirty());
            QVERIFY(p.save(path));
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QCOMPARE(root.value(QStringLiteral("formatVersion")).toInt(), serialization::kFormatVersion);
        QVERIFY(root.contains(QStringLiteral("galleryExtra")));
        Project back;
        back.setPowerGradeFile(fileOf(dir, QStringLiteral("pg.json")));
        QVERIFY(back.load(path));
        QCOMPARE(back.gallery()->albumName(albumId), QStringLiteral("Grades"));
        QCOMPARE(back.gallery()->still(stillId)->album, albumId);
        QCOMPARE(back.gallery()->still(stillId)->grade, twoNodeGrade());
        QVERIFY(back.gallery()->memory(1).used);
        QCOMPARE(back.gallery()->memory(1).grade, gradeWith(0.6));
        QVERIFY(!back.isDirty());
        // A new project starts without albums and memories.
        back.newProject();
        QCOMPARE(back.gallery()->albums().size(), 1);
        QCOMPARE(back.gallery()->usedMemories(), 0);

        // Without albums and memories the key is not written (older builds read the file).
        const QString plainPath = fileOf(dir, QStringLiteral("plain.kkproj"));
        {
            Project p;
            p.setPowerGradeFile(fileOf(dir, QStringLiteral("pg.json")));
            p.gallery()->addStill(picture(Qt::red), gradeWith(1.2));
            QVERIFY(p.save(plainPath));
        }
        QFile plainFile(plainPath);
        QVERIFY(plainFile.open(QIODevice::ReadOnly));
        QVERIFY(!QJsonDocument::fromJson(plainFile.readAll()).object().contains(QStringLiteral("galleryExtra")));
    }

    // 151.9 - 151.11: PowerGrade albums live outside the project, in every project
    void powerGradeStore()
    {
        QTemporaryDir dir;
        const QString file = fileOf(dir, QStringLiteral("powergrades.json"));
        quint64 album = 0;
        quint64 id = 0;
        {
            PowerGradeStore store(file);
            QCOMPARE(store.gallery()->albumName(GradeGallery::kDefaultAlbum), QStringLiteral("PowerGrade"));
            album = store.gallery()->addAlbum(QStringLiteral("Film Looks"));
            id = store.gallery()->addStill(picture(Qt::magenta), twoNodeGrade(), album);
            QVERIFY(QFile::exists(file)); // saved at once, not when closing
        }
        {
            PowerGradeStore store(file);
            QCOMPARE(store.gallery()->albumName(album), QStringLiteral("Film Looks"));
            QCOMPARE(store.gallery()->still(id)->grade, twoNodeGrade());
            QVERIFY(!store.gallery()->still(id)->image.isNull());
            store.gallery()->renameStill(id, QStringLiteral("Teal"));
        }
        // Two projects share it; a project file holds none of it.
        Project a;
        a.setPowerGradeFile(file);
        Project b;
        b.setPowerGradeFile(file);
        QCOMPARE(a.powerGrades()->gallery()->still(id)->name, QStringLiteral("Teal"));
        QCOMPARE(b.powerGrades()->gallery()->still(id)->name, QStringLiteral("Teal"));
        QVERIFY(a.gallery()->stills().isEmpty());
        a.gallery()->addStill(picture(Qt::cyan), gradeWith(1.0));
        // Copy a project still into the PowerGrade store and back.
        const quint64 projectStill = a.gallery()->stills().first().id;
        const quint64 copy = a.gallery()->copyStillTo(*a.powerGrades()->gallery(), projectStill, album);
        QVERIFY(copy);
        QCOMPARE(a.powerGrades()->gallery()->still(copy)->album, album);
        {
            Project c;
            c.setPowerGradeFile(file);
            QVERIFY(c.powerGrades()->gallery()->still(copy)); // a third project sees it
            QVERIFY(c.gallery()->stills().isEmpty());
        }
        const quint64 back = a.powerGrades()->gallery()->copyStillTo(
            *a.gallery(), copy, GradeGallery::kDefaultAlbum);
        QVERIFY(back);
        QCOMPARE(a.gallery()->stills().size(), 2);

        // A project change does not rewrite the store file, a store change does not dirty it.
        Project clean;
        clean.setPowerGradeFile(file);
        QVERIFY(!clean.isDirty());
        clean.powerGrades()->gallery()->addAlbum(QStringLiteral("More"));
        QVERIFY(!clean.isDirty());
    }

    void powerGradeDamagedFile()
    {
        QTemporaryDir dir;
        const QString file = fileOf(dir, QStringLiteral("powergrades.json"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{ not json");
        f.close();
        PowerGradeStore store(file);
        QVERIFY(store.gallery()->stills().isEmpty());
        QVERIFY(QFile::exists(file + QStringLiteral(".bak"))); // kept, not lost
        store.gallery()->addStill(picture(Qt::red), gradeWith(1.0));
        PowerGradeStore again(file);
        QCOMPARE(again.gallery()->stills().size(), 1);
    }

    // 151.12: Append Node Graph
    void appendNodeGraph()
    {
        ClipGrade target;
        QVERIFY(go::appendGraph(target, twoNodeGrade())); // onto an ungraded clip: a copy
        QCOMPARE(target, twoNodeGrade());
        QVERIFY(!go::appendGraph(target, ClipGrade {})); // nothing to append

        ClipGrade base = twoNodeGrade();
        const quint64 baseOutput = base.output;
        const int baseCount = int(base.nodes.size());
        ClipGrade source = twoNodeGrade();
        source.node(source.output)->label = QStringLiteral("Tail");
        QVERIFY(go::appendGraph(base, source));
        QCOMPARE(int(base.nodes.size()), baseCount + int(source.nodes.size()));
        // Every id is unique (nodes and windows share one space).
        QSet<quint64> ids;
        for (const GradeNode& n : base.nodes) {
            QVERIFY(!ids.contains(n.id));
            ids.insert(n.id);
            for (const GradeWindow& w : n.windows) {
                QVERIFY(!ids.contains(w.id));
                ids.insert(w.id);
            }
        }
        // The appended graph starts on the old output and its output leaves the clip.
        const GradeNode* tail = base.node(base.output);
        QVERIFY(tail);
        QCOMPARE(tail->label, QStringLiteral("Tail"));
        QVERIFY(base.output != baseOutput);
        bool attached = false;
        for (int i = baseCount; i < base.nodes.size(); ++i)
            attached = attached || base.nodes[i].inputs.contains(baseOutput);
        QVERIFY(attached);
        // All nodes are reached from the output, in a valid order, and sit to the right.
        QCOMPARE(int(go::evaluationOrder(base).size()), baseCount + int(source.nodes.size()));
        double maxOld = 0;
        for (int i = 0; i < baseCount; ++i)
            maxOld = std::max(maxOld, base.nodes[i].pos.x());
        for (int i = baseCount; i < base.nodes.size(); ++i)
            QVERIFY(base.nodes[i].pos.x() > maxOld);
        // Settings came along.
        QCOMPARE(base.nodes[baseCount].params.value(gk::Saturation, 0, 1.0), 1.4);
    }

    // 151.13 - 151.15: Export Still and Import Still
    void exportAndImport()
    {
        QTemporaryDir dir;
        GradeGallery g;
        const quint64 id = g.addStill(picture(QColor(30, 90, 200)), twoNodeGrade());
        GalleryStill still = *g.still(id);
        still.name = QStringLiteral("Teal Orange");

        const QStringList suffixes = gallerystills::exportSuffixes();
        QVERIFY(suffixes.contains(QStringLiteral("png")));
        QVERIFY(suffixes.contains(QStringLiteral("tif")));
        QVERIFY(suffixes.contains(QStringLiteral("jpg")));
        QVERIFY2(suffixes.contains(QStringLiteral("dpx")), "FFmpeg has no DPX encoder");
        for (const QString& suffix : suffixes) {
            const QString path = fileOf(dir, QStringLiteral("still.") + suffix);
            QString error;
            QVERIFY2(gallerystills::exportStill(still, path, true, &error), qPrintable(suffix + error));
            QVERIFY(QFile::exists(path));
            QVERIFY(QFile::exists(fileOf(dir, QStringLiteral("still.kkgrade"))));
            GalleryStill in;
            QVERIFY2(gallerystills::importStill(path, &in, &error), qPrintable(suffix + error));
            QCOMPARE(in.name, QStringLiteral("Teal Orange")); // from the grade file
            QCOMPARE(in.grade, twoNodeGrade());
            QCOMPARE(in.image.size(), still.image.size());
            // The picture is the one exported (left top red, the rest blue; JPEG is lossy).
            const QColor topLeft = in.image.pixelColor(10, 10);
            const QColor rest = in.image.pixelColor(still.image.width() - 10, still.image.height() - 10);
            QVERIFY2(topLeft.red() > 200 && topLeft.blue() < 80, qPrintable(suffix));
            QVERIFY2(rest.blue() > 170 && rest.red() < 70, qPrintable(suffix));
        }

        // The grade file alone carries grade, name and a small picture.
        const QString gradeOnly = fileOf(dir, QStringLiteral("only.kkgrade"));
        QVERIFY(gallerystills::exportStill(still, gradeOnly, true, nullptr));
        GalleryStill fromGrade;
        QVERIFY(gallerystills::importStill(gradeOnly, &fromGrade, nullptr));
        QCOMPARE(fromGrade.grade, twoNodeGrade());
        QCOMPARE(fromGrade.name, QStringLiteral("Teal Orange"));
        QVERIFY(!fromGrade.image.isNull());

        // A picture without a grade file: a still without grade, named after the file.
        QVERIFY(picture(Qt::darkGreen).save(fileOf(dir, QStringLiteral("plain.png"))));
        GalleryStill plain;
        QVERIFY(gallerystills::importStill(fileOf(dir, QStringLiteral("plain.png")), &plain, nullptr));
        QVERIFY(plain.grade.isEmpty());
        QCOMPARE(plain.name, QStringLiteral("plain"));
        QVERIFY(!plain.image.isNull());

        // Without the grade file option only the picture is written.
        QVERIFY(gallerystills::exportStill(still, fileOf(dir, QStringLiteral("bare.png")), false, nullptr));
        QVERIFY(!QFile::exists(fileOf(dir, QStringLiteral("bare.kkgrade"))));

        // Failures say why.
        QString error;
        GalleryStill none;
        QVERIFY(!gallerystills::importStill(fileOf(dir, QStringLiteral("missing.png")), &none, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(QFile(fileOf(dir, QStringLiteral("junk.kkgrade"))).open(QIODevice::WriteOnly));
        error.clear();
        QVERIFY(!gallerystills::importStill(fileOf(dir, QStringLiteral("junk.kkgrade")), &none, &error));
        QVERIFY(!error.isEmpty());
        GalleryStill blank;
        error.clear();
        QVERIFY(!gallerystills::exportStill(blank, fileOf(dir, QStringLiteral("blank.png")), true, &error));
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestGallery)
#include "test_gallery.moc"
