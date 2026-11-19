// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the Audio page's Analyzer panel and of the Voice Isolation controls
// (docs/behavior-spec.md section 141): the panel has the spectrum, correlation and goniometer
// widgets and a tooltip on every control, a reading moves them, the monitor buttons change the
// shared monitor controls (and are saved per user), the strip's Voice Isolation button switches
// the effect of its track (one undo step) and Voice Isolation of a clip is edited in the
// Inspector's Audio tab.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/AudioPage.h"
#include "core/AudioEffectKeys.h"
#include "core/MixerOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "core/VoiceIsolation.h"
#include "playback/MonitorControls.h"
#include "ui/audio/AnalyzerPanel.h"
#include "ui/audio/MeterModel.h"
#include "ui/audio/MixerPanel.h"
#include "ui/audio/MixerStrip.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
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

    AudioPage* audio() const { return window->audioPage(); }
    const TimelineData& data() const { return project.timeline()->data(); }
    int steps() const { return project.undoStack()->index(); }
    MixTarget a1() const { return MixTarget::track(data().audioTracks[0].id); }

    void showAudio()
    {
        window->resize(1900, 1000);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        actions.action(QStringLiteral("view.page_audio"))->trigger();
        QCoreApplication::processEvents();
    }

    quint64 addClip()
    {
        project.beginEdit(QStringLiteral("clip"));
        TimelineData& d = project.timeline()->mutableData();
        ClipData c;
        c.id = d.allocateId();
        c.kind = ClipKind::Media;
        c.mediaId = QStringLiteral("m");
        c.start = 0;
        c.duration = 50;
        c.sourceDurationUs = 2'000'000;
        d.audioTracks[0].clips.push_back(c);
        project.endEdit();
        return c.id;
    }
};

} // namespace

class TestAnalyzerUi : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_analyzerui"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void init()
    {
        QSettings().clear();
        MonitorControls& m = MonitorControls::shared();
        m.setDim(false);
        m.setMono(false);
        m.setMute(false);
        m.setDimDb(MonitorControls::kDefaultDimDb);
    }

    // 141.7: every control of the panel has a tooltip.
    void analyzerPanelHasTipsAndTheMonitorButtonsWork()
    {
        Fixture f;
        f.showAudio();
        AnalyzerPanel* p = f.audio()->analyzer();
        QVERIFY(p);
        for (QWidget* w : std::initializer_list<QWidget*> {p->sourceBox(), p->peakHoldBox(),
                 p->slopeBox(), p->smoothingBox(), p->correlationBar(), p->dimButton(), p->dimSpin(),
                 p->monoButton(), p->muteButton(), p->findChild<QWidget*>(QStringLiteral("SpectrumView")),
                 p->findChild<QWidget*>(QStringLiteral("GoniometerView"))}) {
            QVERIFY(w);
            QVERIFY2(!w->toolTip().isEmpty(), qPrintable(w->objectName()));
        }
        QCOMPARE(p->sourceBox()->itemText(0), QStringLiteral("Main mix"));
        QVERIFY(p->sourceBox()->count() >= 2); // the main mix and A1
        // The monitor buttons drive the shared controls.
        MonitorControls& m = MonitorControls::shared();
        QCOMPARE(p->dimSpin()->value(), -20.0);
        p->dimButton()->click();
        QVERIFY(m.dim());
        p->monoButton()->click();
        QVERIFY(m.mono());
        p->muteButton()->click();
        QVERIFY(m.mute());
        p->dimSpin()->setValue(-12.0);
        QCOMPARE(m.dimDb(), -12.0);
        // Dim and Mono are saved for the user; Mute is not.
        QVERIFY(QSettings().value(QStringLiteral("monitor/dim")).toBool());
        QVERIFY(QSettings().value(QStringLiteral("monitor/mono")).toBool());
        QCOMPARE(QSettings().value(QStringLiteral("monitor/dimDb")).toDouble(), -12.0);
        QVERIFY(!QSettings().contains(QStringLiteral("monitor/mute")));
        // None of it is in the project.
        QCOMPARE(f.steps(), 0);
        p->muteButton()->click();
        QVERIFY(!m.mute());
    }

    void aReadingMovesTheDisplays()
    {
        Fixture f;
        f.showAudio();
        AnalyzerPanel* p = f.audio()->analyzer();
        MeterFrame frame;
        frame.sample = 1000;
        frame.hasAnalysis = true;
        frame.spectrumDb.fill(-70.0f);
        frame.spectrumDb[dsp::spectrumBandOf(1000)] = -12.0f;
        frame.correlation = -0.6f;
        p->smoothingBox()->setCurrentIndex(p->smoothingBox()->findData(int(SpectrumSmoothing::Off)));
        f.audio()->findChild<MeterModel*>()->setFrame(frame);
        QTest::qWait(60);
        f.audio()->findChild<MeterModel*>()->setFrame(frame);
        QCOMPARE(p->display().barDb(dsp::spectrumBandOf(1000)), -12.0f);
        QVERIFY(p->display().barDb(5) <= -69.0f);
        QVERIFY(std::fabs(p->correlationBar()->value() + 0.6) < 1e-6);
        // The display choices are kept.
        p->slopeBox()->setCurrentIndex(2);
        p->peakHoldBox()->setChecked(false);
        QCOMPARE(QSettings().value(QStringLiteral("audio/spectrum/slope")).toInt(), 2);
        QVERIFY(!QSettings().value(QStringLiteral("audio/spectrum/peakHold")).toBool());
    }

    // 141.3: the strip's button.
    void theStripButtonSwitchesVoiceIsolationOnTheTrackWithOneUndoStep()
    {
        Fixture f;
        f.showAudio();
        MixerStrip* strip = f.audio()->mixer()->strip(f.a1());
        QVERIFY(strip);
        QToolButton* b = strip->voiceIsolationButton();
        QVERIFY(b);
        QVERIFY(!b->toolTip().isEmpty());
        QVERIFY(b->toolTip().contains(QStringLiteral("Optional AI")));
        QVERIFY(!b->isChecked());
        const int before = f.steps();
        b->click();
        QCOMPARE(f.steps(), before + 1);
        const auto& fx = f.data().audioTracks[0].mix.effects;
        QVERIFY(voiceisolation::isOn(fx));
        QCOMPARE(fx.first().type, QString::fromLatin1(afxid::VoiceIsolation)); // the first slot
        QVERIFY(strip->voiceIsolationButton()->isChecked());
        b->click();
        QVERIFY(!voiceisolation::isOn(f.data().audioTracks[0].mix.effects));
        QCOMPARE(f.steps(), before + 2);
        f.project.undoStack()->undo();
        QVERIFY(voiceisolation::isOn(f.data().audioTracks[0].mix.effects));
        // Buses have no button.
        for (const BusData& bus : f.data().mixer.buses) {
            MixerStrip* s = f.audio()->mixer()->strip(MixTarget::bus(bus.id));
            if (s)
                QVERIFY(!s->voiceIsolationButton());
        }
    }

    // 141.3: the Inspector's Audio tab.
    void theInspectorAudioTabEditsVoiceIsolationOfAClip()
    {
        Fixture f;
        f.showAudio();
        const quint64 clip = f.addClip();
        InspectorPanel panel(f.ctx);
        panel.resize(420, 900);
        panel.show();
        f.selection.selectClip(clip);
        QCoreApplication::processEvents();
        auto* toggle = panel.findChild<QCheckBox*>(QStringLiteral("VoiceIsolationToggle"));
        auto* spin = panel.findChild<QSpinBox*>(QStringLiteral("VoiceIsolationAmount"));
        auto* slider = panel.findChild<QSlider*>(QStringLiteral("VoiceIsolationSlider"));
        QVERIFY(toggle && spin && slider);
        QVERIFY(!toggle->toolTip().isEmpty() && !spin->toolTip().isEmpty() && !slider->toolTip().isEmpty());
        QVERIFY(toggle->text().contains(QStringLiteral("Optional AI")));
        QCOMPARE(spin->value(), 100);
        QVERIFY(!toggle->isChecked());
        const int before = f.steps();
        toggle->click();
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(voiceisolation::isOn(f.data().audioTracks[0].clips[0].effects));
        // The Amount is one undo step as well.
        spin = panel.findChild<QSpinBox*>(QStringLiteral("VoiceIsolationAmount"));
        spin->setValue(40);
        QCOMPARE(f.steps(), before + 2);
        QCOMPARE(voiceisolation::amount(f.data().audioTracks[0].clips[0].effects), 40.0);
        QCOMPARE(f.data().audioTracks[0].clips[0].effects.size(), 1);
        f.project.undoStack()->undo();
        QCOMPARE(voiceisolation::amount(f.data().audioTracks[0].clips[0].effects), 100.0);
        toggle = panel.findChild<QCheckBox*>(QStringLiteral("VoiceIsolationToggle"));
        QVERIFY(toggle->isChecked());
        toggle->click();
        QVERIFY(!voiceisolation::isOn(f.data().audioTracks[0].clips[0].effects));
    }
};

QTEST_MAIN(TestAnalyzerUi)
#include "test_analyzerui.moc"
