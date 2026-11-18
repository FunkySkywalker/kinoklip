// SPDX-License-Identifier: GPL-3.0-or-later
// Project Settings presets and the user default config (spec 127): saving, loading, updating,
// deleting and importing / exporting presets; new projects (library, File > New Project, the
// project new command) starting from the user default config; factory reset; broken files;
// the frame rate staying put under clips; the first-clip prompt matching the resolution too.
#include "app/commands/Command.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/ProjectPresets.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "projects/ProjectKinds.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaImport.h"
#include "ui/dialogs/ProjectSettingsDialog.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUndoStack>

using namespace kk;
namespace pp = kk::projectpresets;

namespace {

const QString kVideo = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4"); // 1280x720
const QString kVideo60 = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_60fps.mp4"); // 640x360

pp::Preset qhd60(const QString& name = {})
{
    pp::Preset p = pp::factoryDefault();
    p.name = name;
    p.settings.width = 2560;
    p.settings.height = 1440;
    p.settings.fps = Rational {60, 1};
    p.settings.timelineStart = oneHourTimecode(p.settings.fps);
    return p;
}

void addClip(Project& project)
{
    project.beginEdit(QStringLiteral("Add Clip"));
    ClipData clip;
    clip.kind = ClipKind::Solid;
    clip.solidColor = QColor(40, 90, 200);
    clip.name = QStringLiteral("Clip");
    clip.start = 0;
    clip.duration = 50;
    clip.sourceDurationUs = 20'000'000;
    editops::overwriteClip(project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
    project.endEdit();
}

} // namespace

class TestProjectPresets : public QObject
{
    Q_OBJECT
    QTemporaryDir m_config;
    QTemporaryDir m_work;

private slots:
    void initTestCase()
    {
        QVERIFY(m_config.isValid());
        QVERIFY(m_work.isValid());
    }

    void init()
    {
        // Never the real config directory.
        QDir(m_config.path()).removeRecursively();
        QDir().mkpath(m_config.path());
        pp::setConfigDirectoryForTests(m_config.path());
    }

    void cleanup()
    {
        setFirstClipPromptHandlerForTests({});
        setFrameRatePromptHandlerForTests({});
    }

    void cleanupTestCase() { pp::setConfigDirectoryForTests({}); }

    void nothingSavedMeansFactoryDefaults()
    {
        QVERIFY(pp::list().isEmpty());
        QVERIFY(!pp::userDefault());
        Project p;
        QCOMPARE(p.settings().width, 1920);
        QCOMPARE(p.settings().height, 1080);
        QCOMPARE(p.settings().fps, (Rational {24, 1}));
        QCOMPARE(p.settings().timelineStart, oneHourTimecode(Rational {24, 1}));
    }

    void saveLoadUpdateDelete()
    {
        pp::Preset a = qhd60(QStringLiteral("Gameplay 1440p"));
        a.settings.retimeProcess = RetimeProcess::FrameBlend;
        a.settings.toneMap.agx = true;
        a.settings.toneMap.look = AgxLook::Golden;
        a.settings.stdVideoTransition = QStringLiteral("wipe");
        a.cache.proxyFormat = QStringLiteral("dnxhr_lb");
        a.cache.backgroundCacheDelaySec = 11;
        QVERIFY(pp::save(a));
        pp::Preset archive = pp::factoryDefault();
        archive.name = QStringLiteral("Archive");
        QVERIFY(pp::save(archive));
        const QList<pp::PresetInfo> list = pp::list();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].name, QStringLiteral("Archive")); // sorted by name
        QCOMPARE(list[1].name, QStringLiteral("Gameplay 1440p"));
        QCOMPARE(list[1].width, 2560);
        QCOMPARE(list[1].fps, (Rational {60, 1}));

        const auto loaded = pp::load(QStringLiteral("gameplay 1440P")); // case-insensitive
        QVERIFY(loaded);
        QCOMPARE(loaded->settings.height, 1440);
        QCOMPARE(loaded->settings.retimeProcess, RetimeProcess::FrameBlend);
        QVERIFY(loaded->settings.toneMap.agx);
        QCOMPARE(loaded->settings.toneMap.look, AgxLook::Golden);
        QCOMPARE(loaded->settings.stdVideoTransition, QStringLiteral("wipe"));
        QCOMPARE(loaded->cache.proxyFormat, QStringLiteral("dnxhr_lb"));
        QCOMPARE(loaded->cache.backgroundCacheDelaySec, 11);

        // Saving under the same name replaces it (no second entry).
        a.settings.width = 3840;
        a.settings.height = 2160;
        QVERIFY(pp::save(a));
        QCOMPARE(pp::list().size(), 2);
        QCOMPARE(pp::load(QStringLiteral("Gameplay 1440p"))->settings.width, 3840);

        QVERIFY(!pp::save(pp::Preset {})); // no name
        QVERIFY(pp::remove(QStringLiteral("Archive")));
        QVERIFY(!pp::remove(QStringLiteral("Archive")));
        QCOMPARE(pp::list().size(), 1);
        QVERIFY(QDir(pp::presetsDirectory()).absolutePath().startsWith(m_config.path()));
    }

    void exportImportRoundTrip()
    {
        const pp::Preset a = qhd60(QStringLiteral("Round Trip"));
        const QString file = m_work.filePath(QStringLiteral("trip.json"));
        QVERIFY(pp::writeFile(file, a));
        QString error;
        const auto back = pp::readFile(file, &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(back->name, a.name);
        QCOMPARE(back->settings, a.settings);
        QCOMPARE(back->cache, a.cache);
    }

    void brokenAndPartialFiles()
    {
        QDir().mkpath(pp::presetsDirectory());
        const auto write = [](const QString& path, const QByteArray& bytes) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(bytes);
        };
        const QString dir = pp::presetsDirectory();
        write(dir + "/garbage.json", "this is {not json");
        write(dir + "/array.json", "[1,2,3]");
        write(dir + "/other.json", R"({"format":"something-else","name":"Other"})");
        write(dir + "/empty.json", "");
        // Partial: only a width, an unknown key, a bad frame rate and a too large height.
        write(dir + "/partial.json",
            R"({"format":"kinoklip-project-preset","version":99,"name":"Partial",
                "future":{"x":1},"settings":{"width":1280,"height":99999,"fpsNum":0,
                "fpsDen":-4,"surprise":true}})");
        QCOMPARE(pp::list().size(), 1); // only the partial one is a preset
        const auto partial = pp::load(QStringLiteral("Partial"));
        QVERIFY(partial);
        QCOMPARE(partial->settings.width, 1280);
        QCOMPARE(partial->settings.height, 1080); // out of range: factory value
        QCOMPARE(partial->settings.fps, (Rational {24, 1})); // invalid: factory value
        QCOMPARE(partial->cache, MediaCacheSettings {});

        QString error;
        QVERIFY(!pp::readFile(dir + "/garbage.json", &error));
        QVERIFY(!error.isEmpty());
        error.clear();
        QVERIFY(!pp::readFile(m_work.filePath(QStringLiteral("missing.json")), &error));
        QVERIFY(!error.isEmpty());

        // A broken user default config is ignored: new projects use the built-in settings.
        write(pp::userDefaultPath(), "{{{");
        QVERIFY(!pp::userDefault());
        Project p;
        QCOMPARE(p.settings().width, 1920);

        // The dialog says so instead of failing silently.
        Project project;
        ProjectSettingsDialog dialog(&project);
        QVERIFY(!dialog.importPreset(dir + "/garbage.json"));
        QVERIFY(!dialog.presetStatus().isEmpty());
        QVERIFY(!dialog.loadPreset(QStringLiteral("No such preset")));
        QVERIFY(!dialog.presetStatus().isEmpty());
        QVERIFY(!dialog.deletePreset(QStringLiteral("No such preset")));
    }

    void userDefaultStartsNewProjects()
    {
        QVERIFY(pp::setUserDefault(qhd60()));
        Project p; // the constructor and newProject() both start a project
        QCOMPARE(p.settings().width, 2560);
        QCOMPARE(p.settings().height, 1440);
        QCOMPARE(p.settings().fps, (Rational {60, 1}));
        QCOMPARE(p.timeline()->data().width, 2560);
        QCOMPARE(p.timeline()->data().fps, (Rational {60, 1}));
        QCOMPARE(p.timeline()->data().startTimecode, oneHourTimecode(Rational {60, 1}));
        QCOMPARE(p.settings().name, QStringLiteral("Untitled Project"));

        // File > New Project
        p.setSettings(pp::factoryDefault().settings);
        p.newProject();
        QCOMPARE(p.settings().width, 2560);
        QCOMPARE(p.settings().fps, (Rational {60, 1}));

        // Project Manager: a new file of either kind
        const QString kkproj = m_work.filePath(QStringLiteral("lib.kkproj"));
        QString why;
        QVERIFY2(projectkinds::createKkproj(kkproj, QStringLiteral("lib"), &why), qPrintable(why));
        const ProjectMetadata meta = projectkinds::readKkprojMetadata(kkproj);
        QCOMPARE(meta.width, 2560);
        QCOMPARE(meta.height, 1440);
        QCOMPARE(meta.fps, (Rational {60, 1}));

        // The command line: `project new` without sizes
        const QString cli = m_work.filePath(QStringLiteral("cli.kkproj"));
        auto r = commands::CommandRegistry::builtin().execute(
            QStringLiteral("project.new"), {{"project", cli}, {"force", true}});
        QVERIFY2(r.ok, qPrintable(r.error));
        Project q;
        QVERIFY(q.load(cli));
        QCOMPARE(q.settings().width, 2560);
        QCOMPARE(q.settings().height, 1440);
        QCOMPARE(q.settings().fps, (Rational {60, 1}));
        // Explicit arguments still win.
        r = commands::CommandRegistry::builtin().execute(QStringLiteral("project.new"),
            {{"project", cli}, {"force", true}, {"width", 1280}, {"height", 720}, {"fps", 25}});
        QVERIFY(r.ok);
        QVERIFY(q.load(cli));
        QCOMPARE(q.settings().width, 1280);
        QCOMPARE(q.settings().fps, (Rational {25, 1}));
    }

    void dialogDefaultAndFactoryReset()
    {
        Project project;
        ProjectSettingsDialog dialog(&project);
        auto settings = project.settings();
        settings.width = 2560;
        settings.height = 1440;
        settings.fps = Rational {60, 1};
        dialog.setSettings(settings);
        QVERIFY(dialog.saveAsUserDefault());
        QVERIFY(pp::userDefault());
        Project fresh;
        QCOMPARE(fresh.settings().width, 2560);
        QCOMPARE(fresh.settings().fps, (Rational {60, 1}));
        QCOMPARE(project.settings().width, 1920); // the open project is untouched

        QVERIFY(dialog.resetFactoryDefault());
        QVERIFY(!pp::userDefault());
        QVERIFY(!QFile::exists(pp::userDefaultPath()));
        Project again;
        QCOMPARE(again.settings().width, 1920);
        QCOMPARE(again.settings().height, 1080);
        QCOMPARE(again.settings().fps, (Rational {24, 1}));
        QVERIFY(dialog.resetFactoryDefault()); // nothing to remove is fine
    }

    void dialogPresetPage()
    {
        Project project;
        ProjectSettingsDialog dialog(&project);
        QVERIFY(dialog.showSection(QStringLiteral("presets")));
        auto* list = dialog.findChild<QTreeWidget*>(QStringLiteral("PresetList"));
        QVERIFY(list);
        QCOMPARE(list->topLevelItemCount(), 1); // Current Project
        QCOMPARE(list->topLevelItem(0)->text(1), QStringLiteral("1920 x 1080"));
        // Every control has a tooltip.
        for (const char* name : {"PresetSaveAs", "PresetLoad", "PresetUpdate", "PresetDelete",
                 "PresetImport", "PresetExport", "PresetSaveUserDefault", "PresetResetFactory"}) {
            auto* b = dialog.findChild<QPushButton*>(QLatin1String(name));
            QVERIFY2(b, name);
            QVERIFY2(!b->toolTip().isEmpty(), name);
        }
        QVERIFY(!list->toolTip().isEmpty());

        auto settings = project.settings();
        settings.width = 2560;
        settings.height = 1440;
        settings.fps = Rational {60, 1};
        dialog.setSettings(settings);
        QVERIFY(dialog.saveAsPreset(QStringLiteral("Mine")));
        QCOMPARE(list->topLevelItemCount(), 2);
        QCOMPARE(list->topLevelItem(1)->text(0), QStringLiteral("Mine"));
        QCOMPARE(list->topLevelItem(1)->text(1), QStringLiteral("2560 x 1440"));
        QCOMPARE(list->topLevelItem(1)->text(2), QStringLiteral("60 fps"));

        // Load into the (empty) project: resolution and frame rate change, undoable in one step.
        QVERIFY(dialog.loadPreset(QStringLiteral("Mine")));
        QCOMPARE(project.settings().width, 2560);
        QCOMPARE(project.settings().fps, (Rational {60, 1}));
        QCOMPARE(project.timeline()->data().height, 1440);
        QCOMPARE(project.name(), QStringLiteral("Untitled Project"));
        project.undoStack()->undo();
        QCOMPARE(project.settings().width, 1920);

        // Update overwrites with what is entered now.
        settings.width = 3840;
        settings.height = 2160;
        dialog.setSettings(settings);
        QVERIFY(dialog.updatePreset(QStringLiteral("Mine")));
        QCOMPARE(pp::list().size(), 1);
        QCOMPARE(pp::load(QStringLiteral("Mine"))->settings.width, 3840);
        QVERIFY(!dialog.updatePreset(QStringLiteral("Missing")));

        // Export / import through files.
        const QString file = m_work.filePath(QStringLiteral("mine.json"));
        QVERIFY(dialog.exportPreset(QStringLiteral("Mine"), file));
        QVERIFY(dialog.deletePreset(QStringLiteral("Mine")));
        QCOMPARE(list->topLevelItemCount(), 1);
        QVERIFY(dialog.importPreset(file));
        QCOMPARE(list->topLevelItemCount(), 2);
        QCOMPARE(pp::load(QStringLiteral("Mine"))->settings.width, 3840);
        // The current project can be exported without a saved preset.
        const QString current = m_work.filePath(QStringLiteral("current.json"));
        QVERIFY(dialog.exportPreset({}, current));
        QVERIFY(pp::readFile(current));
    }

    void loadRefusesTheFrameRateOnceClipsExist()
    {
        QVERIFY(pp::save(qhd60(QStringLiteral("Mine"))));
        Project project;
        addClip(project);
        ProjectSettingsDialog dialog(&project);
        QVERIFY(dialog.loadPreset(QStringLiteral("Mine")));
        QCOMPARE(project.settings().width, 2560); // the rest is applied
        QCOMPARE(project.settings().height, 1440);
        QCOMPARE(project.settings().fps, (Rational {24, 1})); // the rate is kept
        QCOMPARE(project.timeline()->data().fps, (Rational {24, 1}));
        QVERIFY2(dialog.presetStatus().contains(QStringLiteral("frame rate")),
            qPrintable(dialog.presetStatus()));
        QVERIFY(dialog.presetStatus().contains(QStringLiteral("24")));
        auto* status = dialog.findChild<QLabel*>(QStringLiteral("PresetStatus"));
        QVERIFY(status);
        QCOMPARE(status->text(), dialog.presetStatus());
    }

    void firstClipPromptMatchesResolutionAndFrameRate()
    {
        if (!QFileInfo::exists(kVideo))
            QSKIP("test media missing (tests/data/generated)");
        Project project;
        SelectionModel selection;
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        QSize askedClip;
        QSize askedProject;
        int asked = 0;
        setFirstClipPromptHandlerForTests([&](Rational, Rational, QSize clip, QSize proj) {
            ++asked;
            askedClip = clip;
            askedProject = proj;
            return true;
        });
        QVERIFY(hasNoMediaYet(ctx));
        // The import itself asks (first import of the project): the answer is Change.
        const QStringList ids = importMediaWithFeedback(ctx, {kVideo}, nullptr);
        QCOMPARE(ids.size(), 1);
        QCOMPARE(asked, 1);
        QVERIFY(!hasNoMediaYet(ctx));
        QCOMPARE(askedClip, QSize(1280, 720));
        QCOMPARE(askedProject, QSize(1920, 1080));
        QCOMPARE(project.settings().fps, (Rational {25, 1}));
        QCOMPARE(project.settings().width, 1280);
        QCOMPARE(project.settings().height, 720);
        QCOMPARE(project.timeline()->data().width, 1280);
        project.undoStack()->undo(); // one undo step for both
        QCOMPARE(project.settings().width, 1920);
        QCOMPARE(project.settings().fps, (Rational {24, 1}));

        // Don't Change leaves both alone.
        setFirstClipPromptHandlerForTests([&](Rational, Rational, QSize, QSize) { return false; });
        QVERIFY(!offerProjectFrameRate(ctx, true, ids, nullptr));
        QCOMPARE(project.settings().width, 1920);
        QCOMPARE(project.settings().fps, (Rational {24, 1}));

        // The same size: the question is about the frame rate only (no size passed).
        auto s = project.settings();
        s.width = 1280;
        s.height = 720;
        project.setSettings(s);
        setFirstClipPromptHandlerForTests([&](Rational, Rational, QSize clip, QSize) {
            askedClip = clip;
            return true;
        });
        QVERIFY(offerProjectFrameRate(ctx, true, ids, nullptr));
        QVERIFY(!askedClip.isValid());
        QCOMPARE(project.settings().fps, (Rational {25, 1}));
        QCOMPARE(project.settings().width, 1280);

        // The older rate-only hook still works (and follows the answer for the size too).
        setFirstClipPromptHandlerForTests({});
        s = project.settings();
        s.width = 1920;
        s.height = 1080;
        s.fps = Rational {24, 1};
        project.setSettings(s);
        int rateOnly = 0;
        setFrameRatePromptHandlerForTests([&](Rational clip, Rational) {
            ++rateOnly;
            return clip == Rational {25, 1};
        });
        QVERIFY(offerProjectFrameRate(ctx, true, ids, nullptr));
        QCOMPARE(rateOnly, 1);
        QCOMPARE(project.settings().fps, (Rational {25, 1}));
        setFrameRatePromptHandlerForTests({});

        // Clips on the timeline: no question, nothing changes.
        s = project.settings();
        s.fps = Rational {24, 1};
        project.setSettings(s);
        addClip(project);
        asked = 0;
        setFirstClipPromptHandlerForTests([&](Rational, Rational, QSize, QSize) {
            ++asked;
            return true;
        });
        QVERIFY(!offerProjectFrameRate(ctx, true, ids, nullptr));
        QCOMPARE(asked, 0);
    }

    void userDefaultAndFirstClipWork()
    {
        // A user default of 1440p60 and a 60 fps clip: the rate matches, so nothing is asked.
        if (!QFileInfo::exists(kVideo60))
            QSKIP("test media missing (tests/data/generated)");
        QVERIFY(pp::setUserDefault(qhd60()));
        Project project;
        SelectionModel selection;
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        int asked = 0;
        setFirstClipPromptHandlerForTests([&](Rational, Rational, QSize, QSize) {
            ++asked;
            return true;
        });
        const QStringList ids = importMediaWithFeedback(ctx, {kVideo60}, nullptr);
        QCOMPARE(asked, 0);
        QCOMPARE(project.settings().width, 2560);
    }
};

QTEST_MAIN(TestProjectPresets)
#include "test_projectpresets.moc"
