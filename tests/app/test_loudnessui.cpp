// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the loudness meter on the Audio page, the Normalize Audio Levels dialog
// and the Deliver page's "Normalize audio to" (docs/behavior-spec.md section 134): the standard
// choice and its target, the over / under read-out, LRA, Start / Pause / Reset, the history
// graph, the stored standard, the render options and tooltips on every control.
#include "../panels/PanelTestUtils.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/AudioPage.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "export/ExportPresets.h"
#include "playback/PlaybackEngine.h"
#include "ui/audio/LoudnessPanel.h"
#include "ui/audio/MeterModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/deliver/RenderSettingsPanel.h"
#include "ui/dialogs/NormalizeMixDialog.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>

#include <memory>

using namespace kk;
using namespace kk::test;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    LoudnessPanel* panel() const { return window->audioPage()->meters(); }

    void showAudio()
    {
        window->resize(1800, 1000);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        actions.action(QStringLiteral("view.page_audio"))->trigger();
        QCoreApplication::processEvents();
    }
};

MeterFrame frameAt(double shortTerm, double integrated, double range)
{
    MeterFrame f;
    f.sample = 1000;
    f.momentaryLufs = shortTerm;
    f.shortTermLufs = shortTerm;
    f.integratedLufs = integrated;
    f.loudnessRange = range;
    f.truePeakMaxDb = -3.0;
    return f;
}

} // namespace

class TestLoudnessUi : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_loudnessui"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void init() { QSettings().clear(); }

    // 134.9: the meter's standards and read-outs.
    void standardChoiceSetsTheTarget()
    {
        Fixture f;
        f.showAudio();
        LoudnessPanel* p = f.panel();
        QCOMPARE(p->target().standard, loudness::Standard::EbuR128); // the default
        QCOMPARE(p->standardBox()->count(), 5);
        QCOMPARE(p->targetBox()->value(), -23.0);
        QVERIFY(!p->targetBox()->isEnabled()); // a preset's number is read only
        struct Case
        {
            loudness::Standard standard;
            double lufs;
        };
        for (const Case c : {Case {loudness::Standard::AtscA85, -24.0},
                 Case {loudness::Standard::Streaming, -14.0}, Case {loudness::Standard::Podcast, -16.0},
                 Case {loudness::Standard::EbuR128, -23.0}}) {
            p->standardBox()->setCurrentIndex(p->standardBox()->findData(int(c.standard)));
            QCOMPARE(p->target().lufs(), c.lufs);
            QCOMPARE(p->targetBox()->value(), c.lufs);
        }
        // Custom: the number is editable and kept.
        p->standardBox()->setCurrentIndex(p->standardBox()->findData(int(loudness::Standard::Custom)));
        QVERIFY(p->targetBox()->isEnabled());
        p->targetBox()->setValue(-18.5);
        QCOMPARE(p->target().lufs(), -18.5);
        QVERIFY(p->targetText().contains(QStringLiteral("-18.5")));
    }

    void theStandardIsStored()
    {
        {
            Fixture f;
            f.panel()->standardBox()->setCurrentIndex(
                f.panel()->standardBox()->findData(int(loudness::Standard::Streaming)));
        }
        Fixture again;
        QCOMPARE(again.panel()->target().standard, loudness::Standard::Streaming);
        QCOMPARE(again.panel()->standardBox()->currentData().toInt(), int(loudness::Standard::Streaming));
        {
            again.panel()->standardBox()->setCurrentIndex(
                again.panel()->standardBox()->findData(int(loudness::Standard::Custom)));
            again.panel()->targetBox()->setValue(-17.0);
        }
        Fixture third;
        QCOMPARE(third.panel()->target().standard, loudness::Standard::Custom);
        QCOMPARE(third.panel()->target().lufs(), -17.0);
    }

    void overAndUnderAgainstTheTarget()
    {
        Fixture f;
        f.showAudio();
        MeterModel* model = f.window->audioPage()->meterModel();
        model->setFrame(frameAt(-20.0, -20.0, 7.5));
        const QString text = f.panel()->readoutText();
        QVERIFY2(text.contains(QStringLiteral("-20.0 LUFS")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("7.5 LU")), qPrintable(text)); // the range
        QVERIFY2(f.panel()->targetText().contains(QStringLiteral("-23.0 LUFS")), qPrintable(f.panel()->targetText()));
        QVERIFY2(f.panel()->targetText().contains(QStringLiteral("Over by 3.0 LU")), qPrintable(f.panel()->targetText()));
        model->setFrame(frameAt(-26.0, -26.0, 1.0));
        QVERIFY2(f.panel()->targetText().contains(QStringLiteral("Under by 3.0 LU")), qPrintable(f.panel()->targetText()));
        model->setFrame(frameAt(-23.2, -23.2, 1.0));
        QVERIFY2(f.panel()->targetText().contains(QStringLiteral("On target")), qPrintable(f.panel()->targetText()));
        // Another standard changes the verdict at once.
        f.panel()->standardBox()->setCurrentIndex(
            f.panel()->standardBox()->findData(int(loudness::Standard::Streaming)));
        QVERIFY2(f.panel()->targetText().contains(QStringLiteral("Under by 9.2 LU")), qPrintable(f.panel()->targetText()));
    }

    void historyGraphCollectsAndResets()
    {
        Fixture f;
        f.showAudio();
        LoudnessHistoryGraph* g = f.panel()->history();
        QVERIFY(g->samples().isEmpty());
        for (int i = 0; i < LoudnessHistoryGraph::kMaxPoints + 25; ++i)
            g->addSample(-20.0 - (i % 5));
        QCOMPARE(g->samples().size(), LoudnessHistoryGraph::kMaxPoints); // the oldest scroll off
        g->addSample(-300.0); // silence is drawn at the floor
        QCOMPARE(double(g->samples().last()), -50.0);
        g->grab(); // paints without trouble
        // The Reset action clears the graph.
        f.actions.action(QStringLiteral("audio.reset_loudness"))->trigger();
        QVERIFY(g->samples().isEmpty());
        // Frames of the playing engine add points, at most one per sample interval.
        MeterModel* model = f.window->audioPage()->meterModel();
        model->setFrame(frameAt(-21.0, -21.0, 2.0));
        model->setFrame(frameAt(-21.0, -21.0, 2.0));
        QVERIFY(g->samples().size() <= 1);
    }

    void startAndPause()
    {
        Fixture f;
        f.showAudio();
        LoudnessPanel* p = f.panel();
        QVERIFY(p->measuring());
        QCOMPARE(p->startPauseButton()->text(), QStringLiteral("Pause"));
        p->startPauseButton()->click();
        QVERIFY(!p->measuring());
        QCOMPARE(p->startPauseButton()->text(), QStringLiteral("Start"));
        // While paused no history points are added.
        f.window->audioPage()->meterModel()->setFrame(frameAt(-21.0, -21.0, 2.0));
        QVERIFY(p->history()->samples().isEmpty());
        p->startPauseButton()->click();
        QVERIFY(p->measuring());
    }

    void everyControlHasATooltip()
    {
        Fixture f;
        f.showAudio();
        LoudnessPanel* p = f.panel();
        for (QWidget* w : {static_cast<QWidget*>(p->standardBox()), static_cast<QWidget*>(p->targetBox()),
                 static_cast<QWidget*>(p->startPauseButton()), static_cast<QWidget*>(p->history())})
            QVERIFY2(!w->toolTip().isEmpty(), qPrintable(w->objectName()));
        for (QLabel* l : p->findChildren<QLabel*>(QStringLiteral("LoudnessTargetReadout")))
            QVERIFY(!l->toolTip().isEmpty());
        // The Normalize dialog.
        NormalizeMixDialog dialog(&f.project, std::nullopt);
        for (const char* name : {"NormalizeMixMode", "NormalizeMixStandard", "NormalizeMixTarget"}) {
            QWidget* w = dialog.findChild<QWidget*>(QLatin1String(name));
            QVERIFY(w);
            QVERIFY2(!w->toolTip().isEmpty(), name);
        }
        QVERIFY(!dialog.findChild<QWidget*>(QStringLiteral("NormalizeMixStatus"))->toolTip().isEmpty());
    }

    // 134.10: Deliver > Audio > Normalize audio to.
    void renderOptionsReachTheSettings()
    {
        PanelFixture fx;
        QSettings().remove(QStringLiteral("deliver"));
        ExportPresets presets;
        auto* panel = new RenderSettingsPanel(fx.ctx, &presets);
        std::unique_ptr<QWidget> owner(panel);
        panel->applyPreset(QStringLiteral("youtube_1080p"));
        auto* normalize = panel->findChild<QCheckBox*>(QStringLiteral("RenderNormalize"));
        auto* standard = panel->findChild<QComboBox*>(QStringLiteral("RenderNormalizeStandard"));
        auto* lufs = panel->findChild<QDoubleSpinBox*>(QStringLiteral("RenderNormalizeTarget"));
        auto* peak = panel->findChild<QDoubleSpinBox*>(QStringLiteral("RenderNormalizePeak"));
        QVERIFY(normalize && standard && lufs && peak);
        QVERIFY(!panel->settings().audio.normalize); // off by default
        for (QWidget* w : {static_cast<QWidget*>(normalize), static_cast<QWidget*>(standard),
                 static_cast<QWidget*>(lufs), static_cast<QWidget*>(peak)})
            QVERIFY2(!w->toolTip().isEmpty(), qPrintable(w->objectName()));
        normalize->setChecked(true);
        QVERIFY(panel->settings().audio.normalize);
        standard->setCurrentIndex(standard->findData(int(loudness::Standard::Streaming)));
        QCOMPARE(panel->settings().audio.normalizeTarget.lufs(), -14.0);
        QCOMPARE(lufs->value(), -14.0);
        QVERIFY(!lufs->isEnabled()); // a preset
        standard->setCurrentIndex(standard->findData(int(loudness::Standard::Custom)));
        QVERIFY(lufs->isEnabled());
        lufs->setValue(-18.0);
        peak->setValue(-3.0);
        const ExportSettings s = panel->settings();
        QCOMPARE(s.audio.normalizeTarget.lufs(), -18.0);
        QCOMPARE(s.audio.normalizeTarget.truePeakDb(), -3.0);
        // The settings come back into the form.
        panel->setSettings(s);
        QCOMPARE(panel->settings().audio.normalizeTarget, s.audio.normalizeTarget);
        QVERIFY(normalize->isChecked());
    }
};

QTEST_MAIN(TestLoudnessUi)
#include "test_loudnessui.moc"
