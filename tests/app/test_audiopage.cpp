// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the Audio page (docs/behavior-spec.md section 37): page wiring (Shift+7,
// shared project / timeline / playhead / undo with the Edit page), the timeline's audio page
// view, one undo step per mixer gesture (fader, pan, mute, EQ handle, bus routing), touch
// automation recording, meters feeding the widgets, the Edit page's Mixer panel.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/AudioPage.h"
#include "app/pages/EditPage.h"
#include "core/EqPresets.h"
#include "core/MixerOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/audio/AudioScales.h"
#include "ui/audio/AutomationBar.h"
#include "ui/audio/DynamicsWindow.h"
#include "ui/audio/EqCurveView.h"
#include "ui/audio/EqWindow.h"
#include "ui/audio/FaderWidget.h"
#include "ui/audio/LoudnessPanel.h"
#include "ui/audio/MeterModel.h"
#include "ui/audio/MixerController.h"
#include "ui/audio/MixerPanel.h"
#include "ui/audio/MixerStrip.h"
#include "ui/audio/SendsWindow.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/timeline/AutomationLanes.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QToolTip>
#include <QUndoStack>

#include <functional>
#include <memory>
#include <optional>

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

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    AudioPage* audio() const { return window->audioPage(); }
    const TimelineData& data() const { return project.timeline()->data(); }
    int steps() const { return project.undoStack()->count(); }
    MixerStrip* strip(const MixTarget& t) const { return audio()->mixer()->strip(t); }
    quint64 a1() const { return data().audioTracks[0].id; }

    void showAudio()
    {
        window->resize(1800, 1000);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        action("view.page_audio")->trigger();
        QCoreApplication::processEvents();
    }
};

/// Press at @p from, move to @p to in steps, release (left button).
void drag(QWidget* w, QPoint from, QPoint to)
{
    auto send = [w](QEvent::Type type, QPoint p, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent e(
            type, QPointF(p), w->mapToGlobal(QPointF(p)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(w, &e);
    };
    send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 5; ++i)
        send(QEvent::MouseMove, from + (to - from) * i / 5, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

/// Plays frames @p from .. @p to (the playhead advances one frame at a time).
void playFrames(Fixture& f, MixerController* mc, bool& playing, int64_t from, int64_t to,
    const std::function<void(int64_t)>& each = {})
{
    f.project.timeline()->setPlayhead(from);
    playing = true;
    mc->transportStateChanged();
    for (int64_t frame = from + 1; frame <= to; ++frame) {
        f.project.timeline()->setPlayhead(frame);
        if (each)
            each(frame);
    }
}

void setUpCurve(Fixture& f, MixerController* mc, AutomationMode mode, double a, double b)
{
    mc->edit(QStringLiteral("Setup"), [&](TimelineData& d) {
        mixerops::addAutomationPoint(d, f.a1(), mixkeys::kVolumeDb, 0, a);
        mixerops::addAutomationPoint(d, f.a1(), mixkeys::kVolumeDb, 100, b);
        return mixerops::setAutomationMode(d, f.a1(), mode);
    });
}

} // namespace

class TestAudioPage : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_audiopage"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // ---- 37.1 page wiring ----
    void pageIsEnabledAndSwitchesWithShift7()
    {
        Fixture f;
        QVERIFY(f.audio());
        QVERIFY(f.action("view.page_audio")->isEnabled());
        QCOMPARE(f.actions.shortcuts(QStringLiteral("view.page_audio")).value(0),
            QKeySequence(QStringLiteral("Shift+7")));
        f.action("view.page_audio")->trigger();
        QCOMPARE(f.window->currentPageId(), QStringLiteral("audio"));
        // The interface bar offers the mixer toggle there.
        QVERIFY(f.action("view.toggle_mixer")->isEnabled());
        f.action("view.toggle_mixer")->trigger();
        QVERIFY(!f.audio()->isPanelVisible(PanelId::Mixer));
        f.action("view.toggle_mixer")->trigger();
        QVERIFY(f.audio()->isPanelVisible(PanelId::Mixer));
    }

    void pageActionsOnlyOnTheAudioPage()
    {
        Fixture f;
        f.showAudio();
        QVERIFY(f.action("audio.float_viewer")->isEnabled());
        f.action("audio.float_viewer")->trigger();
        QVERIFY(f.audio()->isViewerFloating());
        // Polish round 2: the floating viewer and the EQ / dynamics windows carry the
        // window-scoped shortcuts (Space plays while they have focus).
        QWidget* floating = f.window->findChild<QWidget*>(QStringLiteral("AudioViewerWindow"));
        QVERIFY(floating);
        QVERIFY(floating->actions().contains(f.action("transport.play_pause")));
        QVERIFY(f.strip(MixTarget::track(f.a1()))
                    ->openEq()
                    ->actions()
                    .contains(f.action("transport.play_pause")));
        QVERIFY(f.strip(MixTarget::track(f.a1()))
                    ->openDynamics()
                    ->actions()
                    .contains(f.action("edit.undo")));
        f.action("audio.float_viewer")->trigger();
        QVERIFY(!f.audio()->isViewerFloating());
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(!f.action("audio.float_viewer")->isEnabled());
        QVERIFY(f.action("audio.add_bus")->isEnabled()); // the Edit page's mixer uses it too
    }

    // ---- 37.2 shared state ----
    void sharesTimelinePlayheadAndUndoWithEditPage()
    {
        Fixture f;
        f.showAudio();
        // One timeline controller (tool, snapping, targets) for every view.
        QCOMPARE(
            f.audio()->timeline()->controller(), f.window->editPage()->timeline()->controller());
        f.project.timeline()->setPlayhead(42);
        f.action("view.page_edit")->trigger();
        QCOMPARE(f.project.timeline()->playhead(), int64_t(42));
        // A mixer edit on the Audio page is undone from the Edit page (one undo stack) and
        // shows in the Edit page's mixer panel.
        f.action("view.page_audio")->trigger();
        f.audio()->mixerController()->setControl(
            MixTarget::track(f.a1()), MixerController::Control::Fader, -9.0);
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, -9.0);
        auto* editMixer = f.window->editPage()->findChild<MixerPanel*>();
        QVERIFY(editMixer);
        QCOMPARE(editMixer->strip(MixTarget::track(f.a1()))->fader()->value(), -9.0);
        f.action("view.page_edit")->trigger();
        f.action("edit.undo")->trigger();
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, 0.0);
    }

    // ---- 37.3 the timeline view ----
    void timelineShowsTallAudioAndCollapsedVideo()
    {
        Fixture f;
        f.showAudio();
        const TimelineLayout& L = f.audio()->timeline()->layout();
        const int audioRow = L.rowOf(TrackRef {TrackType::Audio, 0});
        const int videoRow = L.rowOf(TrackRef {TrackType::Video, 0});
        QVERIFY(audioRow >= 0 && videoRow >= 0);
        QVERIFY(L.rowRect(audioRow).height() >= 2 * timelinemetrics::kDefaultAudioHeight);
        QCOMPARE(L.rowRect(videoRow).height(), timelinemetrics::kMinTrackHeight);
        // Audio page headers: record arm and a meter; the Edit page has the arm button only
        // (voice-over, spec 75).
        QVERIFY(!L.headerButtonRect(audioRow, HeaderButton::Arm).isEmpty());
        QVERIFY(!L.headerMeterRect(audioRow).isEmpty());
        // Round 2 polish: the buttons go to a second line so the track name has the first
        // line up to the meter (it used to be cut to "A...").
        QVERIFY(L.twoLineHeader(audioRow));
        const QRect patch = L.headerButtonRect(audioRow, HeaderButton::Target);
        for (const HeaderButton b : L.buttonsFor(TrackType::Audio)) {
            const QRect br = L.headerButtonRect(audioRow, b);
            QVERIFY(br.top() > patch.bottom());
            QVERIFY(br.right() < L.headerMeterRect(audioRow).left());
        }
        QVERIFY(L.headerMeterRect(audioRow).left() - patch.right() > 100); // name room
        const TimelineLayout& edit = f.window->editPage()->timeline()->timelineWidget()->layout();
        const int editRow = edit.rowOf(TrackRef {TrackType::Audio, 0});
        QVERIFY(!edit.headerButtonRect(editRow, HeaderButton::Arm).isEmpty());
        QVERIFY(edit.headerMeterRect(editRow).isEmpty());
        // Arming is not an edit (and without a voice-over controller in this fixture it does
        // nothing at all): no undo step.
        const int before = f.steps();
        QWidget* vp = f.audio()->timeline()->viewport();
        const QPoint arm = L.headerButtonRect(audioRow, HeaderButton::Arm).center();
        QTest::mouseClick(vp, Qt::LeftButton, {}, arm);
        QCOMPARE(f.steps(), before);
        // Mute from the timeline header shows in the mixer (shared state).
        QTest::mouseClick(
            vp, Qt::LeftButton, {}, L.headerButtonRect(audioRow, HeaderButton::Mute).center());
        QVERIFY(f.data().audioTracks[0].muted);
        QVERIFY(f.strip(MixTarget::track(f.a1()))->muteButton()->isChecked());
    }

    // ---- 37.x one undo step per gesture ----
    void faderDragIsOneUndoStep()
    {
        Fixture f;
        f.showAudio();
        FaderWidget* fader = f.strip(MixTarget::track(f.a1()))->fader();
        const int before = f.steps();
        const QPoint cap = fader->capRect().center();
        drag(fader, cap, cap + QPoint(0, 40));
        const double db = f.data().audioTracks[0].mix.strip.volumeDb;
        QVERIFY2(db < -1.0, qPrintable(QString::number(db)));
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(fader->value(), db);
        // Double-click: back to 0 dB, one more step.
        QTest::mouseDClick(fader, Qt::LeftButton, {}, cap);
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, 0.0);
        QCOMPARE(f.steps(), before + 2);
        f.project.undo();
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, db);
    }

    void panMuteSoloAndBusFaderSteps()
    {
        Fixture f;
        f.showAudio();
        MixerStrip* s = f.strip(MixTarget::track(f.a1()));
        int steps = f.steps();
        drag(s->panKnob(), QPoint(20, 20), QPoint(60, 20));
        QVERIFY(f.data().audioTracks[0].mix.strip.pan > 0.1);
        QCOMPARE(f.steps(), ++steps);
        s->muteButton()->click();
        QVERIFY(f.data().audioTracks[0].muted);
        QCOMPARE(f.steps(), ++steps);
        s->soloButton()->click();
        QVERIFY(f.data().audioTracks[0].solo);
        QCOMPARE(f.steps(), ++steps);
        MixerStrip* bus = f.strip(MixTarget::bus(kMainBusId));
        QVERIFY(bus);
        const QPoint cap = bus->fader()->capRect().center();
        drag(bus->fader(), cap, cap + QPoint(0, 30));
        QVERIFY(f.data().mixer.buses[0].strip.volumeDb < 0.0);
        QCOMPARE(f.steps(), ++steps);
    }

    void busesAndRouting()
    {
        Fixture f;
        f.showAudio();
        const int steps = f.steps();
        f.action("audio.add_bus")->trigger();
        QCOMPARE(int(f.data().mixer.buses.size()), 2);
        QCOMPARE(f.steps(), steps + 1);
        const int bus2 = f.data().mixer.buses[1].id;
        QVERIFY(f.strip(MixTarget::bus(bus2))); // a strip appeared
        // Route A1 to Bus 2 from its strip's bus menu.
        QMenu* menu = f.strip(MixTarget::track(f.a1()))->busButton()->menu();
        emit menu->aboutToShow();
        QAction* entry = nullptr;
        for (QAction* a : menu->actions()) {
            if (a->text() == QStringLiteral("Bus %1").arg(bus2))
                entry = a;
        }
        QVERIFY(entry);
        entry->trigger();
        QCOMPARE(f.data().audioTracks[0].mix.buses, (QVector<int> {kMainBusId, bus2}));
        QCOMPARE(f.steps(), steps + 2);
        // The bus strip's Output button makes it the output.
        f.strip(MixTarget::bus(bus2))->busButton()->click();
        QCOMPARE(f.data().mixer.outputBus, bus2);
        QCOMPARE(f.steps(), steps + 3);
    }

    void spec23_22_longTrackNamesElideInTheStrip()
    {
        Fixture f;
        f.showAudio();
        const QString name = QStringLiteral("Dialogue Microphone Left Channel");
        f.project.beginEdit(QStringLiteral("Rename"));
        f.project.timeline()->mutableData().audioTracks[0].name = name;
        f.project.endEdit();
        QCoreApplication::processEvents();
        MixerStrip* s = f.strip(MixTarget::track(f.a1()));
        QVERIFY(s);
        int elided = 0;
        for (QLabel* label : s->findChildren<QLabel*>()) {
            for (const QString& line : label->text().split(QLatin1Char('\n'))) {
                QVERIFY2(
                    label->fontMetrics().horizontalAdvance(line) <= label->contentsRect().width(),
                    qPrintable(line));
            }
            if (label->text().contains(QChar(0x2026))) {
                ++elided;
                QVERIFY(label->toolTip().startsWith(name));
            }
        }
        QCOMPARE(elided, 2); // the input and the name plate
    }

    // 135: the Dynamics window's Gate / Expander and Key (side chain).
    void dynamicsWindowHasGateAndKeyWithTooltips()
    {
        Fixture f;
        f.showAudio();
        f.project.beginEdit(QStringLiteral("Add track"));
        TrackData extra;
        extra.id = f.project.timeline()->mutableData().allocateId();
        f.project.timeline()->mutableData().audioTracks.push_back(extra);
        f.project.endEdit();
        QCoreApplication::processEvents();
        QVERIFY(f.data().audioTracks.size() >= 2);
        const quint64 a2 = f.data().audioTracks[1].id;
        DynamicsWindow* w = f.strip(MixTarget::track(f.a1()))->openDynamics();
        QVERIFY(w);
        int controls = 0;
        for (QWidget* c : w->findChildren<QWidget*>()) {
            if (qobject_cast<QDoubleSpinBox*>(c) || qobject_cast<QCheckBox*>(c)
                || qobject_cast<QComboBox*>(c)) {
                ++controls;
                QVERIFY2(!c->toolTip().isEmpty(), qPrintable(c->objectName()));
            }
        }
        QVERIFY(controls >= 22);
        // The gate switch is one undo step.
        auto* gate = w->findChild<QCheckBox*>(QStringLiteral("DynamicsGate"));
        QVERIFY(gate);
        const int steps = f.steps();
        gate->click();
        QCOMPARE(f.steps(), steps + 1);
        QVERIFY(f.data().audioTracks[0].mix.strip.dynamics.gateEnabled);
        f.project.undo();
        QVERIFY(!f.data().audioTracks[0].mix.strip.dynamics.gateEnabled);
        // The mode switch enables the expander ratio.
        auto* mode = w->findChild<QComboBox*>(QStringLiteral("DynamicsGateMode"));
        auto* ratio = w->findChild<QDoubleSpinBox*>(QStringLiteral("DynamicsGateRatio"));
        QVERIFY(mode && ratio);
        QVERIFY(!ratio->isEnabled());
        mode->setCurrentIndex(1);
        emit mode->activated(1);
        QCOMPARE(f.data().audioTracks[0].mix.strip.dynamics.gateMode, GateMode::Expander);
        QVERIFY(ratio->isEnabled());
        // The key list: Self, the other track, the buses. A1 feeds the main bus, so the main
        // bus (a loop) is greyed out; the other track is not.
        auto* key = w->findChild<QComboBox*>(QStringLiteral("DynamicsKey"));
        QVERIFY(key);
        int trackRow = -1, busRow = -1;
        for (int i = 0; i < key->count(); ++i) {
            if (key->itemData(i, Qt::UserRole).toInt() == int(KeyKind::Track)
                && key->itemData(i, Qt::UserRole + 1).toULongLong() == a2)
                trackRow = i;
            if (key->itemData(i, Qt::UserRole).toInt() == int(KeyKind::Bus))
                busRow = i;
        }
        QVERIFY(trackRow >= 0 && busRow >= 0);
        QVERIFY(key->model()->flags(key->model()->index(trackRow, 0)).testFlag(Qt::ItemIsEnabled));
        QVERIFY(!key->model()->flags(key->model()->index(busRow, 0)).testFlag(Qt::ItemIsEnabled));
        const int before = f.steps();
        key->setCurrentIndex(trackRow);
        emit key->activated(trackRow);
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(f.data().audioTracks[0].mix.strip.dynamics.keyKind, KeyKind::Track);
        QCOMPARE(f.data().audioTracks[0].mix.strip.dynamics.keyId, a2);
        f.project.undo();
        QCOMPARE(f.data().audioTracks[0].mix.strip.dynamics.keyKind, KeyKind::Self);
        f.project.redo();
        QCOMPARE(f.data().audioTracks[0].mix.strip.dynamics.keyId, a2);
        // A2 cannot now be keyed by A1.
        DynamicsWindow* w2 = f.strip(MixTarget::track(a2))->openDynamics();
        auto* key2 = w2->findChild<QComboBox*>(QStringLiteral("DynamicsKey"));
        for (int i = 0; i < key2->count(); ++i) {
            if (key2->itemData(i, Qt::UserRole + 1).toULongLong() == f.a1()
                && key2->itemData(i, Qt::UserRole).toInt() == int(KeyKind::Track))
                QVERIFY(!key2->model()->flags(key2->model()->index(i, 0)).testFlag(Qt::ItemIsEnabled));
        }
    }

    void eqHandleDragIsOneUndoStep()
    {
        Fixture f;
        f.showAudio();
        EqWindow* eq = f.strip(MixTarget::track(f.a1()))->openEq();
        eq->resize(700, 500);
        QVERIFY(QTest::qWaitForWindowExposed(eq));
        EqCurveView* curve = eq->curve();
        const int steps = f.steps();
        const QPoint handle = curve->handlePosition(2).toPoint(); // the 500 Hz bell
        drag(curve, handle, handle + QPoint(30, -40));
        const EqBand& band = f.data().audioTracks[0].mix.strip.eq.bands[2];
        QVERIFY(band.gainDb > 3.0);
        QVERIFY(band.frequencyHz > 500.0);
        QVERIFY(f.data().audioTracks[0].mix.strip.eq.enabled); // dragging switches the EQ on
        QCOMPARE(f.steps(), steps + 1);
        // The strip's EQ button shows it is on.
        QVERIFY(f.strip(MixTarget::track(f.a1()))->eqButton()->isChecked());
        f.project.undo();
        QCOMPARE(f.data().audioTracks[0].mix.strip.eq, EqSettings {});
        QVERIFY(eq->isVisible());
    }

    // ---- 37.x automation ----
    void touchAutomationRecordsWhilePlaying()
    {
        Fixture f;
        f.showAudio();
        MixerController* mc = f.audio()->mixerController();
        bool playing = false;
        mc->setPlayingProbe([&playing] { return playing; });
        const MixTarget t = MixTarget::track(f.a1());
        mc->edit(QStringLiteral("Touch"), [&](TimelineData& d) {
            return mixerops::setAutomationMode(d, f.a1(), AutomationMode::Touch);
        });
        const int steps = f.steps();
        // "Playing": the playhead advances; the fader is held from frame 10 to 40.
        playing = true;
        mc->transportStateChanged();
        f.project.timeline()->setPlayhead(10);
        mc->pressControl(t, MixerController::Control::Fader);
        mc->moveControl(t, MixerController::Control::Fader, -12.0);
        for (int64_t frame = 11; frame <= 40; ++frame) {
            f.project.timeline()->setPlayhead(frame);
            if (frame == 25)
                mc->moveControl(t, MixerController::Control::Fader, -6.0);
        }
        mc->releaseControl(t, MixerController::Control::Fader);
        f.project.timeline()->setPlayhead(60); // released: no more keys
        playing = false;
        mc->transportStateChanged();
        QCOMPARE(f.steps(), steps + 1); // one undo step for the pass
        const TrackData& track = f.data().audioTracks[0];
        QVERIFY(mixerops::readsAutomation(track.mix, mixkeys::kVolumeDb));
        QCOMPARE(mixerops::faderDbAt(track, 15.0), -12.0);
        QCOMPARE(mixerops::faderDbAt(track, 35.0), -6.0);
        QVERIFY(!track.mix.automationKeys.values.value(mixkeys::kVolumeDb).hasKeyAt(60));
        // The strip's fader follows the curve at the playhead.
        f.project.timeline()->setPlayhead(15);
        QCOMPARE(f.strip(t)->fader()->value(), -12.0);
        f.project.undo();
        QVERIFY(!mixerops::readsAutomation(f.data().audioTracks[0].mix, mixkeys::kVolumeDb));
    }

    void writeModeRecordsTheWholePass()
    {
        Fixture f;
        f.showAudio();
        MixerController* mc = f.audio()->mixerController();
        bool playing = false;
        mc->setPlayingProbe([&playing] { return playing; });
        mc->edit(QStringLiteral("Write"), [&](TimelineData& d) {
            d.audioTracks[0].mix.strip.volumeDb = -3.0;
            return mixerops::setAutomationMode(d, f.a1(), AutomationMode::Write);
        });
        const int steps = f.steps();
        f.project.timeline()->setPlayhead(0);
        playing = true;
        mc->transportStateChanged();
        for (int64_t frame = 1; frame <= 20; ++frame)
            f.project.timeline()->setPlayhead(frame);
        playing = false;
        mc->transportStateChanged();
        QCOMPARE(f.steps(), steps + 1);
        const TrackData& track = f.data().audioTracks[0];
        const AnimatedValue curve = track.mix.automationKeys.values.value(mixkeys::kVolumeDb);
        QVERIFY(curve.hasKeyAt(0) && curve.hasKeyAt(20));
        QCOMPARE(mixerops::faderDbAt(track, 10.0), -3.0);
    }

    void stoppedFaderMoveSetsTheKeyAtThePlayhead()
    {
        Fixture f;
        f.showAudio();
        MixerController* mc = f.audio()->mixerController();
        mc->edit(QStringLiteral("Keys"), [&](TimelineData& d) {
            mixerops::writeAutomation(d, f.a1(), mixkeys::kVolumeDb, 0, 0.0, 0);
            return mixerops::writeAutomation(d, f.a1(), mixkeys::kVolumeDb, 50, -10.0, 50);
        });
        f.project.timeline()->setPlayhead(25);
        const MixTarget t = MixTarget::track(f.a1());
        mc->setControl(t, MixerController::Control::Fader, -2.0);
        const TrackData& track = f.data().audioTracks[0];
        QCOMPARE(mixerops::faderDbAt(track, 25.0), -2.0); // a key at the playhead
        QCOMPARE(track.mix.strip.volumeDb, 0.0); // the static value is untouched
    }

    // ---- 138 automation modes ----
    void touchReturnsToTheCurveOnRelease()
    {
        Fixture f;
        f.showAudio();
        MixerController* mc = f.audio()->mixerController();
        bool playing = false;
        mc->setPlayingProbe([&playing] { return playing; });
        setUpCurve(f, mc, AutomationMode::Touch, 0.0, 0.0);
        const MixTarget t = MixTarget::track(f.a1());
        using C = MixerController::Control;
        playFrames(f, mc, playing, 20, 20);
        mc->pressControl(t, C::Fader);
        mc->moveControl(t, C::Fader, -12.0);
        for (int64_t frame = 21; frame <= 40; ++frame)
            f.project.timeline()->setPlayhead(frame);
        mc->releaseControl(t, C::Fader);
        for (int64_t frame = 41; frame <= 70; ++frame)
            f.project.timeline()->setPlayhead(frame);
        playing = false;
        mc->transportStateChanged();
        const TrackData& track = f.data().audioTracks[0];
        QCOMPARE(mixerops::faderDbAt(track, 30.0), -12.0);
        const AnimatedValue c = track.mix.automationKeys.values.value(mixkeys::kVolumeDb);
        QVERIFY(!c.hasKeyAt(41) && !c.hasKeyAt(60)); // nothing written after the release
        QVERIFY(c.hasKeyAt(100));
        QCOMPARE(mixerops::faderDbAt(track, 100.0), 0.0); // back on the existing curve
    }

    void latchKeepsTheLastValueUntilStop()
    {
        Fixture f;
        f.showAudio();
        MixerController* mc = f.audio()->mixerController();
        bool playing = false;
        mc->setPlayingProbe([&playing] { return playing; });
        setUpCurve(f, mc, AutomationMode::Latch, 0.0, 0.0);
        const MixTarget t = MixTarget::track(f.a1());
        using C = MixerController::Control;
        const int steps = f.steps();
        playFrames(f, mc, playing, 20, 20);
        mc->pressControl(t, C::Fader);
        mc->moveControl(t, C::Fader, -9.0);
        for (int64_t frame = 21; frame <= 40; ++frame)
            f.project.timeline()->setPlayhead(frame);
        mc->releaseControl(t, C::Fader);
        for (int64_t frame = 41; frame <= 70; ++frame)
            f.project.timeline()->setPlayhead(frame);
        playing = false;
        mc->transportStateChanged();
        QCOMPARE(f.steps(), steps + 1);
        const TrackData& track = f.data().audioTracks[0];
        QCOMPARE(mixerops::faderDbAt(track, 30.0), -9.0);
        QCOMPARE(mixerops::faderDbAt(track, 55.0), -9.0); // held after the release
        QCOMPARE(mixerops::faderDbAt(track, 70.0), -9.0); // until the stop
        QVERIFY(track.mix.automationKeys.values.value(mixkeys::kVolumeDb).hasKeyAt(70));
        QCOMPARE(mixerops::faderDbAt(track, 100.0), 0.0);
    }

    void writeOverwritesTheExistingCurve()
    {
        Fixture f;
        f.showAudio();
        MixerController* mc = f.audio()->mixerController();
        bool playing = false;
        mc->setPlayingProbe([&playing] { return playing; });
        setUpCurve(f, mc, AutomationMode::Write, -20.0, -20.0);
        mc->edit(QStringLiteral("Static"), [&](TimelineData& d) {
            mixerops::addAutomationPoint(d, f.a1(), mixkeys::kVolumeDb, 25, -40.0);
            d.audioTracks[0].mix.strip.volumeDb = -3.0;
            return true;
        });
        playFrames(f, mc, playing, 20, 30);
        playing = false;
        mc->transportStateChanged();
        const TrackData& track = f.data().audioTracks[0];
        // The pass holds the value the fader showed when it began: the -40 point is gone.
        QCOMPARE(mixerops::faderDbAt(track, 25.0), mixerops::faderDbAt(track, 22.0));
        QVERIFY(mixerops::faderDbAt(track, 25.0) != -40.0);
        QVERIFY(track.mix.automationKeys.values.value(mixkeys::kVolumeDb).hasKeyAt(30));
    }

    void trimAddsTheChangeToTheCurve()
    {
        Fixture f;
        f.showAudio();
        MixerController* mc = f.audio()->mixerController();
        bool playing = false;
        mc->setPlayingProbe([&playing] { return playing; });
        setUpCurve(f, mc, AutomationMode::Trim, -10.0, -30.0);
        const MixTarget t = MixTarget::track(f.a1());
        using C = MixerController::Control;
        const int steps = f.steps();
        // The curve falls from -10 (frame 0) to -30 (frame 100): -20 at 50, -22 at 60.
        playFrames(f, mc, playing, 50, 50);
        mc->pressControl(t, C::Fader);
        mc->moveControl(t, C::Fader, -20.0 + 5.0); // +5 dB over the curve's -20
        for (int64_t frame = 51; frame <= 60; ++frame)
            f.project.timeline()->setPlayhead(frame);
        mc->releaseControl(t, C::Fader);
        playing = false;
        mc->transportStateChanged();
        QCOMPARE(f.steps(), steps + 1);
        const TrackData& track = f.data().audioTracks[0];
        QCOMPARE(mixerops::faderDbAt(track, 10.0), -12.0); // before: untouched
        QCOMPARE(mixerops::faderDbAt(track, 49.0), -19.8); // the curve as it was up to the press
        QCOMPARE(mixerops::faderDbAt(track, 50.0), -15.0); // from the press on: the offset
        QVERIFY(std::abs(mixerops::faderDbAt(track, 55.0) - (-21.0 + 5.0)) < 1e-9);
        QVERIFY(std::abs(mixerops::faderDbAt(track, 60.0) - (-22.0 + 5.0)) < 1e-9);
        QCOMPARE(mixerops::faderDbAt(track, 100.0), -30.0); // the end of the curve is as it was
    }

    void automationToolbarSetsEveryTrackAndShowsTheMode()
    {
        Fixture f;
        f.showAudio();
        AutomationBar* bar = f.audio()->automationBar();
        QVERIFY(bar);
        QVERIFY(bar->isVisible());
        QVERIFY(bar->button(AutomationMode::Read)->isChecked()); // the default
        QVERIFY(!bar->button(AutomationMode::Latch)->isChecked());
        for (AutomationMode m : AutomationBar::modes())
            QVERIFY(!bar->button(m)->toolTip().isEmpty());
        const int steps = f.steps();
        bar->button(AutomationMode::Latch)->click();
        for (const TrackData& t : f.data().audioTracks)
            QCOMPARE(t.mix.automation, AutomationMode::Latch);
        QCOMPARE(f.steps(), steps + 1);
        QVERIFY(bar->button(AutomationMode::Latch)->isChecked());
        QVERIFY(!bar->button(AutomationMode::Read)->isChecked());
        bar->button(AutomationMode::Latch)->click(); // stays on
        QVERIFY(bar->button(AutomationMode::Latch)->isChecked());
        // The strip's own mode list has Latch and Trim.
        QComboBox* combo = f.strip(MixTarget::track(f.a1()))->automationCombo();
        QVERIFY(combo->findData(int(AutomationMode::Trim)) >= 0);
        QCOMPARE(combo->currentData().toInt(), int(AutomationMode::Latch));
        f.project.undo();
        QVERIFY(bar->button(AutomationMode::Read)->isChecked());
        // One track on its own: no button is lit.
        f.project.beginEdit(QStringLiteral("Add track"));
        TrackData extra;
        extra.id = f.project.timeline()->mutableData().allocateId();
        f.project.timeline()->mutableData().audioTracks.push_back(extra);
        f.project.endEdit();
        QCoreApplication::processEvents();
        f.audio()->mixerController()->edit(QStringLiteral("One"), [&](TimelineData& d) {
            return mixerops::setAutomationMode(d, f.a1(), AutomationMode::Trim);
        });
        for (AutomationMode m : AutomationBar::modes())
            QVERIFY(!bar->button(m)->isChecked());
    }

    // ---- 138 the lane ----
    void automationLaneOpensFromTheHeaderAndEditsPoints()
    {
        Fixture f;
        f.showAudio();
        TimelineWidget* tw = f.audio()->timeline();
        const TimelineLayout& L = tw->layout();
        const int row = L.rowOf(TrackRef {TrackType::Audio, 0});
        const int heightBefore = L.rowRect(row).height();
        const QRect button = L.headerButtonRect(row, HeaderButton::Automation);
        QVERIFY(!button.isEmpty());
        QVERIFY(L.automationLaneRect(row).isEmpty());
        QTest::mouseClick(tw->viewport(), Qt::LeftButton, {}, button.center());
        QVERIFY(tw->automationLanes()->isOpen(f.a1()));
        QCOMPARE(f.steps(), 0); // opening a lane is view state
        QVERIFY(L.rowRect(row).height() > heightBefore);
        QVERIFY(!L.automationLaneRect(row).isEmpty());
        QCoreApplication::processEvents();
        AutomationLaneWidget* lane = tw->automationLanes()->widget(f.a1());
        QVERIFY(lane);
        QVERIFY(lane->isVisible());
        QVERIFY(!lane->toolTip().isEmpty());
        // The parameter picker: Volume, Pan, Mute first; every item can be chosen.
        QComboBox* picker = lane->picker();
        QVERIFY(!picker->toolTip().isEmpty());
        QVERIFY(picker->count() >= 3);
        QVERIFY(picker->itemData(0).toString() == mixkeys::kVolumeDb);
        QVERIFY(picker->itemData(1).toString() == mixkeys::kPan);
        QVERIFY(picker->itemData(2).toString() == mixkeys::kMute);
        QCOMPARE(lane->parameterKey(), mixkeys::kVolumeDb);

        // Click adds a point.
        const int steps = f.steps();
        const QPoint at = lane->pointPos(20.0, -12.0).toPoint();
        QTest::mouseClick(lane, Qt::LeftButton, {}, at);
        const TrackData& track = f.data().audioTracks[0];
        const AnimatedValue& curve = track.mix.automationKeys.values[mixkeys::kVolumeDb];
        QCOMPARE(curve.keys.size(), qsizetype(1));
        QCOMPARE(curve.keys[0].frame, int64_t(20));
        QVERIFY(std::abs(curve.keys[0].value - (-12.0)) < 1.5);
        QCOMPARE(f.steps(), steps + 1);
        // A second point, then a drag of the first (one undo step; it follows the pointer).
        QTest::mouseClick(lane, Qt::LeftButton, {}, lane->pointPos(60.0, 0.0).toPoint());
        QCOMPARE(f.data().audioTracks[0].mix.automationKeys.values[mixkeys::kVolumeDb].keys.size(),
            qsizetype(2));
        const int beforeDrag = f.steps();
        drag(lane, lane->pointPos(20.0, -12.0).toPoint(), lane->pointPos(30.0, -30.0).toPoint());
        QCOMPARE(f.steps(), beforeDrag + 1);
        {
            const AnimatedValue& c2
                = f.data().audioTracks[0].mix.automationKeys.values[mixkeys::kVolumeDb];
            QVERIFY(c2.hasKeyAt(30) && !c2.hasKeyAt(20));
            QVERIFY(std::abs(c2.keys[c2.indexOf(30)].value - (-30.0)) < 2.0);
        }
        f.project.undo();
        QVERIFY(f.data().audioTracks[0].mix.automationKeys.values[mixkeys::kVolumeDb].hasKeyAt(20));
        // Dragging a segment up moves both its points.
        const double vA
            = f.data().audioTracks[0].mix.automationKeys.values[mixkeys::kVolumeDb].keys[0].value;
        const QPoint mid = lane->pointPos(40.0, mixerops::faderDbAt(f.data().audioTracks[0], 40.0))
                               .toPoint();
        drag(lane, mid, mid - QPoint(0, 15));
        const double vA2
            = f.data().audioTracks[0].mix.automationKeys.values[mixkeys::kVolumeDb].keys[0].value;
        QVERIFY(vA2 > vA + 1.0);
        // Select and Delete removes the point (one undo step each).
        lane->selectPoint(20);
        const int beforeDelete = f.project.undoStack()->index();
        QTest::keyClick(lane, Qt::Key_Delete);
        QCOMPARE(f.project.undoStack()->index(), beforeDelete + 1);
        QVERIFY(!f.data().audioTracks[0].mix.automationKeys.values[mixkeys::kVolumeDb].hasKeyAt(20));
        f.project.undo();
        QVERIFY(f.data().audioTracks[0].mix.automationKeys.values[mixkeys::kVolumeDb].hasKeyAt(20));
        // Another parameter: Pan has its own (empty) curve; the lane keeps the choice.
        picker->setCurrentIndex(1);
        emit picker->activated(1);
        QCOMPARE(lane->parameterKey(), mixkeys::kPan);
        QCOMPARE(tw->automationLanes()->parameter(f.a1()), mixkeys::kPan);
        QTest::mouseClick(lane, Qt::LeftButton, {}, lane->pointPos(10.0, 0.5).toPoint());
        QVERIFY(f.data().audioTracks[0].mix.automationKeys.values.contains(mixkeys::kPan));
        // Closing the lane gives the row its height back.
        QTest::mouseClick(tw->viewport(), Qt::LeftButton, {}, button.center());
        QVERIFY(!tw->automationLanes()->isOpen(f.a1()));
        QCOMPARE(L.rowRect(row).height(), heightBefore);
    }

    void automationHeaderButtonHasATooltipAndTheLaneMenuEntryExists()
    {
        Fixture f;
        f.showAudio();
        TimelineWidget* tw = f.audio()->timeline();
        const TimelineLayout& L = tw->layout();
        const int row = L.rowOf(TrackRef {TrackType::Audio, 0});
        const QPoint p = L.headerButtonRect(row, HeaderButton::Automation).center();
        QTest::mouseMove(tw->viewport(), p);
        QTest::mouseMove(tw->viewport(), p + QPoint(1, 0));
        QTRY_VERIFY(QToolTip::isVisible());
        QVERIFY(QToolTip::text().contains(QStringLiteral("Automation")));
        QToolTip::hideText();
        // The Edit page has no header button but the lane works there too.
        TimelineWidget* edit = f.window->editPage()->timeline()->timelineWidget();
        edit->automationLanes()->setOpen(f.a1(), true);
        QVERIFY(edit->layout().rowRect(edit->layout().rowOf(TrackRef {TrackType::Audio, 0})).height()
            > L.rowRect(row).height() / 2);
        QVERIFY(!edit->layout().automationLaneRect(edit->layout().rowOf(TrackRef {TrackType::Audio, 0})).isEmpty());
    }

    void stoppedFaderMoveStaysOnTheStaticValueWithoutACurve()
    {
        Fixture f;
        f.showAudio();
        MixerController* mc = f.audio()->mixerController();
        const MixTarget t = MixTarget::track(f.a1());
        mc->setControl(t, MixerController::Control::Fader, -5.0);
        QVERIFY(f.data().audioTracks[0].mix.automationKeys.values.isEmpty());
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, -5.0);
    }

    // ---- 37.x meters ----
    void metersFeedTheWidgets()
    {
        Fixture f;
        f.showAudio();
        MeterFrame frame;
        frame.sample = 0;
        ChannelLevel* level = frame.levels.tracks.add();
        level->id = f.a1();
        level->peak[0] = 0.5f;
        level->peak[1] = 0.25f;
        ChannelLevel* bus = frame.levels.buses.add();
        bus->id = kMainBusId;
        bus->peak[0] = bus->peak[1] = 0.5f;
        frame.levels.output[0] = frame.levels.output[1] = 0.5f;
        frame.momentaryLufs = -23.0;
        frame.shortTermLufs = -22.5;
        frame.integratedLufs = -23.4;
        frame.truePeakMaxDb = -5.9;
        MeterModel* model = f.audio()->meterModel();
        model->setFrame(frame);
        const MeterReading r = model->track(f.a1());
        QVERIFY(std::abs(r.levelDb[0] - audioscale::toDb(0.5)) < 0.01);
        QVERIFY(std::abs(r.holdDb[1] - audioscale::toDb(0.25)) < 0.01);
        QVERIFY(std::abs(model->output().levelDb[0] - audioscale::toDb(0.5)) < 0.01);
        const QString readout = f.audio()->meters()->readoutText();
        QVERIFY2(readout.contains(QStringLiteral("-23.0 LUFS"))
                && readout.contains(QStringLiteral("-23.4 LUFS"))
                && readout.contains(QStringLiteral("-5.9 dBTP")),
            qPrintable(readout));
        // A silent frame later: the bar falls back, the hold mark stays for a while.
        QTest::qWait(200);
        model->setFrame(MeterFrame {});
        const MeterReading later = model->track(f.a1());
        QVERIFY(later.levelDb[0] < r.levelDb[0]);
        QCOMPARE(later.holdDb[0], r.holdDb[0]);
    }

    // 137: sends, aux buses, bus pan, Phase Invert and EQ presets on the strip.
    void sendsInvertBusPanAndEqPresets()
    {
        QTemporaryDir config;
        QVERIFY(config.isValid());
        eqpresets::setConfigDirectoryForTests(config.path());
        Fixture f;
        f.showAudio();
        f.project.beginEdit(QStringLiteral("Add buses"));
        const int aux = mixerops::addBus(f.project.timeline()->mutableData(), false, {}, true);
        const int sub = mixerops::addBus(f.project.timeline()->mutableData(), false, {}, false);
        f.project.endEdit();
        QCoreApplication::processEvents();
        MixerStrip* a1 = f.strip(MixTarget::track(f.a1()));
        QVERIFY(a1->sendsButton() && a1->invertButton());
        QVERIFY(!a1->sendsButton()->toolTip().isEmpty());
        QVERIFY(!a1->invertButton()->toolTip().isEmpty());

        // Phase Invert: one undo step, lit, undone.
        int steps = f.project.undoStack()->index();
        a1->invertButton()->click();
        QCOMPARE(f.project.undoStack()->index(), steps + 1);
        QVERIFY(f.data().audioTracks[0].mix.strip.invertLeft
            && f.data().audioTracks[0].mix.strip.invertRight);
        QVERIFY(a1->invertButton()->isChecked());
        f.project.undo();
        QVERIFY(!f.data().audioTracks[0].mix.strip.invertLeft);
        QVERIFY(!a1->invertButton()->isChecked());

        // Sends window: every control has a tooltip; add / edit / remove are undo steps.
        SendsWindow* w = a1->openSends();
        QVERIFY(w);
        QVERIFY(!w->addCombo()->toolTip().isEmpty() && !w->addButton()->toolTip().isEmpty());
        QVERIFY(w->addCombo()->findData(aux) >= 0 && w->addCombo()->findData(sub) >= 0);
        w->addCombo()->setCurrentIndex(w->addCombo()->findData(aux));
        steps = f.project.undoStack()->index();
        w->addButton()->click();
        QCOMPARE(f.project.undoStack()->index(), steps + 1);
        QCOMPARE(w->rowCount(), 1);
        QVERIFY(a1->sendsButton()->isChecked());
        QVERIFY(w->addCombo()->findData(aux) < 0); // one send per bus
        int controls = 0;
        for (QWidget* c : w->findChildren<QWidget*>()) {
            if (qobject_cast<QDoubleSpinBox*>(c) || qobject_cast<QCheckBox*>(c)
                || qobject_cast<QComboBox*>(c) || qobject_cast<QPushButton*>(c)) {
                ++controls;
                QVERIFY2(!c->toolTip().isEmpty(), qPrintable(c->objectName()));
            }
        }
        QVERIFY(controls >= 7);
        auto* level = w->findChild<QDoubleSpinBox*>(QStringLiteral("SendLevel"));
        auto* mode = w->findChild<QComboBox*>(QStringLiteral("SendMode"));
        auto* pan = w->findChild<QDoubleSpinBox*>(QStringLiteral("SendPan"));
        auto* on = w->findChild<QCheckBox*>(QStringLiteral("SendOn"));
        QVERIFY(level && mode && pan && on);
        steps = f.project.undoStack()->index();
        level->setValue(-6.0);
        QCOMPARE(f.project.undoStack()->index(), steps + 1);
        QCOMPARE(f.data().audioTracks[0].mix.strip.sends[0].levelDb, -6.0);
        mode->setCurrentIndex(1);
        emit mode->activated(1);
        QVERIFY(f.data().audioTracks[0].mix.strip.sends[0].preFader);
        pan->setValue(50.0);
        QCOMPARE(f.data().audioTracks[0].mix.strip.sends[0].pan, 0.5);
        on->setChecked(false);
        QVERIFY(!f.data().audioTracks[0].mix.strip.sends[0].enabled);
        QVERIFY(!a1->sendsButton()->isChecked());
        QCOMPARE(f.project.undoStack()->index(), steps + 4);
        f.project.undo();
        QVERIFY(f.data().audioTracks[0].mix.strip.sends[0].enabled);
        w->findChild<QPushButton*>(QStringLiteral("SendRemove"))->click();
        QVERIFY(f.data().audioTracks[0].mix.strip.sends.isEmpty());
        QCOMPARE(w->rowCount(), 0);

        // Loops are not offered: bus "sub" sends to the aux bus, so the aux bus cannot send to sub.
        steps = f.project.undoStack()->index();
        QVERIFY(f.strip(MixTarget::bus(sub))->openSends()->addSendTo(aux));
        QCOMPARE(f.project.undoStack()->index(), steps + 1);
        SendsWindow* auxWindow = f.strip(MixTarget::bus(aux))->openSends();
        QVERIFY(auxWindow->addCombo()->findData(sub) < 0);
        QVERIFY(auxWindow->addCombo()->findData(kMainBusId) >= 0);
        QVERIFY(!auxWindow->addSendTo(sub));
        QCOMPARE(f.project.undoStack()->index(), steps + 1);

        // Bus pan: the knob exists on a bus strip and is one undo step.
        MixerStrip* busStrip = f.strip(MixTarget::bus(kMainBusId));
        QVERIFY(busStrip->panKnob());
        steps = f.project.undoStack()->index();
        emit busStrip->panKnob()->valueRequested(0.5);
        QCOMPARE(f.project.undoStack()->index(), steps + 1);
        QCOMPARE(f.data().mixer.buses[0].strip.pan, 0.5);
        f.project.undo();
        QCOMPARE(f.data().mixer.buses[0].strip.pan, 0.0);

        // EQ presets: one undo step; user presets can be saved and deleted.
        EqWindow* eq = a1->openEq();
        QVERIFY(eq->presetButton() && !eq->presetButton()->toolTip().isEmpty());
        const EqSettings before = f.data().audioTracks[0].mix.strip.eq;
        steps = f.project.undoStack()->index();
        QVERIFY(eq->applyPreset(QStringLiteral("Telephone")));
        QCOMPARE(f.project.undoStack()->index(), steps + 1);
        QCOMPARE(f.data().audioTracks[0].mix.strip.eq,
            eqpresets::find(QStringLiteral("Telephone")).eq);
        f.project.undo();
        QCOMPARE(f.data().audioTracks[0].mix.strip.eq, before);
        QVERIFY(!eq->applyPreset(QStringLiteral("No such preset")));
        QVERIFY(eq->applyPreset(QStringLiteral("Voice")));
        eq->setNamePrompt([] { return std::optional<QString>(QStringLiteral("Mine")); });
        QMenu* menu = eq->presetButton()->menu();
        emit menu->aboutToShow();
        QStringList texts;
        QAction* save = nullptr;
        QAction* voice = nullptr;
        for (QAction* a : menu->actions()) {
            texts << a->text();
            if (a->text().startsWith(QStringLiteral("Save as Preset")))
                save = a;
            if (a->text() == QStringLiteral("Voice"))
                voice = a;
            if (!a->isSeparator())
                QVERIFY2(!a->toolTip().isEmpty() || a->menu(), qPrintable(a->text()));
        }
        QVERIFY(save && voice);
        QVERIFY(texts.contains(QStringLiteral("Podcast Voice")));
        save->trigger();
        QCOMPARE(eqpresets::userPresets().size(), 1);
        QVERIFY(eq->applyPreset(QStringLiteral("Music")));
        QVERIFY(eq->applyPreset(QStringLiteral("Mine")));
        QCOMPARE(f.data().audioTracks[0].mix.strip.eq, eqpresets::find(QStringLiteral("Voice")).eq);
        QVERIFY(eq->deletePreset(QStringLiteral("Mine")));
        QVERIFY(eqpresets::userPresets().isEmpty());
        eqpresets::setConfigDirectoryForTests({});
    }

    void scalesAreMonotonicAndInvertible()
    {
        for (double db : {-60.0, -30.0, -12.0, -6.0, 0.0, 6.0, 12.0}) {
            QVERIFY(std::abs(audioscale::faderDb(audioscale::faderPosition(db)) - db) < 1e-9);
            QVERIFY(
                audioscale::faderPosition(db) < audioscale::faderPosition(db + 1.0) || db >= 12.0);
        }
        QCOMPARE(audioscale::faderPosition(kFaderMinDb), 0.0);
        QCOMPARE(audioscale::faderPosition(12.0), 1.0);
        QVERIFY(std::abs(audioscale::faderPosition(0.0) - 0.76) < 1e-9);
        QVERIFY(
            std::abs(audioscale::positionFrequency(audioscale::frequencyPosition(1000.0)) - 1000.0)
            < 1e-6);
    }
};

QTEST_MAIN(TestAudioPage)
#include "test_audiopage.moc"
