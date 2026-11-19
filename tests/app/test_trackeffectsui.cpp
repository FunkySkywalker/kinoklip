// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the Audio page's Effects Library, Inspector and the mixer track effect slots
// (docs/behavior-spec.md section 133): the two panels toggle from the interface bar, the "+"
// menu and a drop from the Effects Library add a slot (one undo step), bypass and remove, a
// double-click opens the slot's controls in the Inspector, the Inspector shows the clip or the
// track, and the slots survive saving a project.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/AudioPage.h"
#include "app/pages/EditPage.h"
#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"
#include "core/MixerOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/audio/MixerPanel.h"
#include "ui/audio/MixerStrip.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
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

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    AudioPage* audio() const { return window->audioPage(); }
    const TimelineData& data() const { return project.timeline()->data(); }
    int steps() const { return project.undoStack()->index(); }
    MixTarget a1() const { return MixTarget::track(data().audioTracks[0].id); }
    MixerStrip* strip() const { return audio()->mixer()->strip(a1()); }
    const QVector<EffectInstance>& fxList() const { return data().audioTracks[0].mix.effects; }

    void showAudio()
    {
        window->resize(1900, 1000);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        action("view.page_audio")->trigger();
        QCoreApplication::processEvents();
    }

    /// A clip on A1; returns its id.
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

    /// The "+" menu entry named @p name.
    QAction* menuEntry(MixerStrip* s, const QString& name) const
    {
        QMenu* menu = s->addEffectButton()->menu();
        emit menu->aboutToShow();
        for (QAction* a : menu->actions()) {
            if (a->text() == name)
                return a;
            if (a->menu()) {
                for (QAction* b : a->menu()->actions())
                    if (b->text() == name)
                        return b;
            }
        }
        return nullptr;
    }
};

/// The slot rows shown (rows of an earlier rebuild are hidden and wait for deletion).
QList<QWidget*> liveRows(MixerStrip* s)
{
    QList<QWidget*> rows;
    for (QWidget* w : s->findChildren<QWidget*>(QStringLiteral("FxSlot")))
        if (!w->property("kkRetired").toBool())
            rows << w;
    return rows;
}

QToolButton* slotButton(MixerStrip* s, int index, const char* objectName)
{
    const auto rows = liveRows(s);
    return index < rows.size() ? rows[index]->findChild<QToolButton*>(QLatin1String(objectName))
                               : nullptr;
}

QLabel* slotName(MixerStrip* s, int index)
{
    const auto rows = liveRows(s);
    return index < rows.size() ? rows[index]->findChild<QLabel*>(QStringLiteral("FxSlotName"))
                               : nullptr;
}

} // namespace

class TestTrackEffectsUi : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_trackeffectsui"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
        QVERIFY(m_dir.isValid());
    }

    // 133.1: the Effects Library and the Inspector toggle on the Audio page.
    void panelTogglesShowTheLibraryAndTheInspector()
    {
        Fixture f;
        f.showAudio();
        QVERIFY(f.audio()->hasPanel(PanelId::Effects));
        QVERIFY(f.audio()->hasPanel(PanelId::Inspector));
        QVERIFY(f.action("view.toggle_effects")->isEnabled());
        QVERIFY(f.action("view.toggle_inspector")->isEnabled());
        QVERIFY(!f.audio()->isPanelVisible(PanelId::Effects));
        QVERIFY(!f.audio()->isPanelVisible(PanelId::Inspector));
        f.action("view.toggle_effects")->trigger();
        QVERIFY(f.audio()->isPanelVisible(PanelId::Effects));
        QVERIFY(f.audio()->effectsLibrary()->isVisible());
        f.action("view.toggle_inspector")->trigger();
        QVERIFY(f.audio()->isPanelVisible(PanelId::Inspector));
        QVERIFY(f.audio()->inspector()->isVisible());
        f.action("view.toggle_effects")->trigger();
        f.action("view.toggle_inspector")->trigger();
        QVERIFY(!f.audio()->effectsLibrary()->isVisible());
        QVERIFY(!f.audio()->inspector()->isVisible());
        // The Audio page's library lists the audio effects.
        f.action("view.toggle_effects")->trigger();
        EffectsLibraryPanel* lib = f.audio()->effectsLibrary();
        QCOMPARE(lib->category(), int(EffectCategory::AudioEffect));
        const QStringList ids = lib->visibleDescriptorIds();
        QVERIFY(!ids.isEmpty());
        for (const QString& id : ids)
            QVERIFY2(id.startsWith(QStringLiteral("audio_")), qPrintable(id));
        // Layout and visibility are stored and restored.
        QSettings s(m_dir.filePath(QStringLiteral("layout.ini")), QSettings::IniFormat);
        f.audio()->saveLayout(s);
        f.action("view.toggle_effects")->trigger();
        QVERIFY(!f.audio()->isPanelVisible(PanelId::Effects));
        f.audio()->restoreLayout(s);
        QVERIFY(f.audio()->isPanelVisible(PanelId::Effects));
    }

    // 133.2: the "+" menu lists the audio effects and adds one (one undo step).
    void thePlusMenuAddsASlot()
    {
        Fixture f;
        f.showAudio();
        MixerStrip* strip = f.strip();
        QVERIFY(strip && strip->addEffectButton());
        QVERIFY(!strip->addEffectButton()->toolTip().isEmpty());
        QCOMPARE(strip->effectSlotCount(), 0);
        QAction* gain = f.menuEntry(strip, QStringLiteral("Gain"));
        QVERIFY(gain);
        const int before = f.steps();
        gain->trigger();
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(f.fxList().size(), 1);
        QCOMPARE(f.fxList()[0].type, QString(afxid::Gain));
        QVERIFY(f.fxList()[0].enabled);
        QCOMPARE(strip->effectSlotCount(), 1);
        QVERIFY(slotName(strip, 0) && !slotName(strip, 0)->toolTip().isEmpty());
        // The menu has every audio effect of the registry.
        for (const EffectDescriptor* d : EffectRegistry::instance().byCategory(EffectCategory::AudioEffect))
            QVERIFY2(f.menuEntry(strip, d->name), qPrintable(d->name));
        // Undo takes it back, redo returns it.
        f.project.undo();
        QCOMPARE(f.fxList().size(), 0);
        QCOMPARE(strip->effectSlotCount(), 0);
        f.project.redo();
        QCOMPARE(f.fxList().size(), 1);
        QCOMPARE(strip->effectSlotCount(), 1);
        // A bus strip has no effects section.
        MixerStrip* bus = f.audio()->mixer()->strip(MixTarget::bus(kMainBusId));
        QVERIFY(bus && !bus->addEffectButton());
    }

    // 133.2: an effect dragged from the Effects Library onto a strip becomes a slot.
    void dragFromTheLibraryOntoAStrip()
    {
        Fixture f;
        f.showAudio();
        f.action("view.toggle_effects")->trigger();
        EffectsLibraryPanel* lib = f.audio()->effectsLibrary();
        QTreeWidgetItem* entry = nullptr;
        QTreeWidgetItemIterator it(lib->tree());
        for (; *it; ++it) {
            if ((*it)->text(0) == QStringLiteral("Equalizer") || (*it)->text(0) == QStringLiteral("EQ"))
                entry = *it;
        }
        if (!entry) {
            QTreeWidgetItemIterator again(lib->tree());
            for (; *again; ++again) {
                if ((*again)->data(0, Qt::UserRole).toString() == QLatin1String(afxid::Eq))
                    entry = *again;
            }
        }
        QVERIFY(entry);
        QMimeData* mime = lib->tree()->model()->mimeData({lib->tree()->indexFromItem(entry)});
        QVERIFY(mime && mime->hasFormat(QLatin1String(mime::Effect)));
        MixerStrip* strip = f.strip();
        const QPointF at = QPointF(strip->rect().center());
        const int before = f.steps();
        QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(strip, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(strip, &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(f.fxList().size(), 1);
        QCOMPARE(f.fxList()[0].type, QString(afxid::Eq));
        // A video effect is refused.
        QMimeData video;
        video.setData(QLatin1String(mime::Effect), "gaussian_blur");
        QDragEnterEvent refuse(at.toPoint(), Qt::CopyAction, &video, Qt::LeftButton, Qt::NoModifier);
        refuse.ignore();
        QApplication::sendEvent(strip, &refuse);
        QVERIFY(!refuse.isAccepted());
        delete mime;
    }

    // 133.2 / 133.4: bypass and remove on a slot.
    void bypassAndRemoveOnASlot()
    {
        Fixture f;
        f.showAudio();
        MixerStrip* strip = f.strip();
        f.menuEntry(strip, QStringLiteral("Gain"))->trigger();
        f.menuEntry(strip, QStringLiteral("Equalizer"))->trigger();
        QCOMPARE(f.fxList().size(), 2);
        QToolButton* bypass = slotButton(strip, 0, "FxSlotBypass");
        QToolButton* remove = slotButton(strip, 1, "FxSlotRemove");
        QVERIFY(bypass && remove);
        QVERIFY(!bypass->toolTip().isEmpty() && !remove->toolTip().isEmpty());
        QVERIFY(bypass->isChecked());
        int before = f.steps();
        bypass->click();
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(!f.fxList()[0].enabled);
        QVERIFY(f.fxList()[1].enabled);
        QVERIFY(!slotButton(strip, 0, "FxSlotBypass")->isChecked());
        f.project.undo();
        QVERIFY(f.fxList()[0].enabled);
        before = f.steps();
        slotButton(strip, 1, "FxSlotRemove")->click();
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(f.fxList().size(), 1);
        QCOMPARE(strip->effectSlotCount(), 1);
        f.project.undo();
        QCOMPARE(f.fxList().size(), 2);
    }

    // 133.9: a double-click on a slot opens its controls in the Inspector.
    void doubleClickOpensTheControlsInTheInspector()
    {
        Fixture f;
        f.showAudio();
        MixerStrip* strip = f.strip();
        f.menuEntry(strip, QStringLiteral("Gain"))->trigger();
        f.menuEntry(strip, QStringLiteral("Delay"))->trigger();
        QVERIFY(!f.audio()->isPanelVisible(PanelId::Inspector));
        QLabel* name = slotName(strip, 1);
        QVERIFY(name);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(3, 3), name->mapToGlobal(QPointF(3, 3)),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(name, &dbl);
        QCoreApplication::processEvents();
        QVERIFY(f.audio()->isPanelVisible(PanelId::Inspector));
        InspectorPanel* inspector = f.audio()->inspector();
        QCOMPARE(inspector->trackId(), f.a1().id);
        QCOMPARE(inspector->clipId(), quint64(0));
        QCOMPARE(inspector->tabs()->currentIndex(), int(InspectorPanel::EffectsTab));
        QStringList titles;
        for (ParamGroupBox* g : inspector->groups())
            titles << g->findChild<QLabel*>(QStringLiteral("InspectorGroupTitle"))->text();
        QCOMPARE(titles, QStringList({QStringLiteral("Gain"), QStringLiteral("Delay")}));
    }

    // 133.9: the Inspector shows the clip, then the track of a clicked strip, then a clip again.
    void theInspectorShowsTheRightObject()
    {
        Fixture f;
        f.showAudio();
        f.action("view.toggle_inspector")->trigger();
        const quint64 clip = f.addClip();
        InspectorPanel* inspector = f.audio()->inspector();
        f.selection.selectClip(clip);
        QCoreApplication::processEvents();
        QCOMPARE(inspector->clipId(), clip);
        QCOMPARE(inspector->trackId(), quint64(0));
        // A click on the strip shows the track's effects.
        MixerStrip* strip = f.strip();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), strip->mapToGlobal(QPointF(5, 5)),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(strip, &press);
        QCOMPARE(inspector->trackId(), f.a1().id);
        QCOMPARE(inspector->clipId(), quint64(0));
        QVERIFY(strip->isSelected());
        QCOMPARE(f.audio()->mixer()->selectedTarget().value(), f.a1());
        // Effects added there show up in the Inspector; Add Effect in it goes to the track.
        f.menuEntry(strip, QStringLiteral("Gain"))->trigger();
        QCOMPARE(inspector->groups().size(), 1);
        QVERIFY(inspector->groups()[0]->rows().size() >= 1);
        // The Inspector's switch edits the slot (one undo step), a drop adds to the track.
        const int before = f.steps();
        auto* enable = inspector->groups()[0]->findChild<QCheckBox*>();
        QVERIFY(enable);
        enable->click();
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(!f.fxList()[0].enabled);
        f.project.undo();
        QVERIFY(f.fxList()[0].enabled);
        // A clip selected later takes the Inspector back.
        f.selection.clear();
        f.selection.selectClip(clip);
        QCoreApplication::processEvents();
        QCOMPARE(inspector->trackId(), quint64(0));
        QCOMPARE(inspector->clipId(), clip);
        QCOMPARE(f.steps(), before); // the switch was undone
    }

    // 133.3 / 133.8: the slots are saved with the project and undone with it.
    void slotsSurviveSavingTheProject()
    {
        Fixture f;
        f.showAudio();
        MixerStrip* strip = f.strip();
        f.menuEntry(strip, QStringLiteral("Gain"))->trigger();
        f.menuEntry(strip, QStringLiteral("Equalizer"))->trigger();
        slotButton(strip, 1, "FxSlotBypass")->click();
        const QVector<EffectInstance> before = f.fxList();
        QCOMPARE(before.size(), 2);
        QVERIFY(!before[1].enabled);
        const QString path = m_dir.filePath(QStringLiteral("slots.kkproj"));
        QVERIFY(f.project.save(path));
        Project loaded;
        QString error;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        QVERIFY(loaded.timeline()->data().audioTracks[0].mix.effects == before);
    }

    // 133.9: the Edit page's mixer also hands a clicked strip to its Inspector.
    void theEditPageInspectorFollowsAStrip()
    {
        Fixture f;
        f.window->resize(1900, 1000);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        f.action("view.page_edit")->trigger();
        auto* editMixer = f.window->editPage()->findChild<MixerPanel*>();
        QVERIFY(editMixer);
        MixerStrip* strip = editMixer->strip(f.a1());
        QVERIFY(strip);
        strip->addEffect(QString(afxid::Gain));
        QCOMPARE(f.window->editPage()->inspector()->trackId(), f.a1().id);
        QCOMPARE(f.fxList().size(), 1);
    }
};

QTEST_MAIN(TestTrackEffectsUi)
#include "test_trackeffectsui.moc"
