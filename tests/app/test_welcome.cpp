// SPDX-License-Identifier: GPL-3.0-or-later
// The welcome window (docs/behavior-spec.md section 110): the five pages, every choice reaches the
// setting Preferences uses, Skip and Next-Next-Next-Finish leave the settings alone, it shows
// once, Help > Welcome reopens it, the suppress switches, and the keyboard banner steps aside.
#include "app/CommandLine.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/WelcomeDialog.h"
#include "app/WelcomeHeroes.h"
#include "core/EditBehaviour.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Translations.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/MenuLayouts.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/UiAnimation.h"
#include "ui/common/VimOptions.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/projects/KeymapChoiceBanner.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTest>

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
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }
};

template <class T>
T* field(QDialog* dialog, const char* name)
{
    return dialog->findChild<T*>(QLatin1String(name));
}

void pick(QComboBox* combo, const QVariant& data)
{
    const int index = combo->findData(data);
    QVERIFY2(index >= 0, qPrintable(combo->objectName()));
    combo->setCurrentIndex(index);
}

/// The stored values other than the welcome window's own marker.
QMap<QString, QVariant> stored()
{
    QSettings s;
    QMap<QString, QVariant> all;
    for (const QString& key : s.allKeys()) {
        if (key != QLatin1String(WelcomeDialog::kShownKey))
            all.insert(key, s.value(key));
    }
    return all;
}

} // namespace

class TestWelcome : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_welcome"));
        qunsetenv(WelcomeDialog::kSuppressEnv);
        initResources();
        theme::apply(*qApp);
    }

    void init() { QSettings().clear(); }

    void fivePagesWithBackNextFinish()
    {
        Fixture f;
        WelcomeDialog* d = f.window->showWelcome();
        QVERIFY(d->isVisible());
        QCOMPARE(d->pageCount(), 5);
        QCOMPARE(d->currentPage(), 0);
        auto* back = field<QPushButton>(d, "WelcomeBack");
        auto* next = field<QPushButton>(d, "WelcomeNext");
        QVERIFY(field<QPushButton>(d, "WelcomeSkip"));
        QVERIFY(!back->isEnabled());
        QCOMPARE(next->text(), QStringLiteral("Next"));
        // The first page: icon and "Welcome to <name>".
        QVERIFY(d->findChild<QWidget*>(QStringLiteral("WelcomeIcon")));
        QVERIFY(field<QLabel>(d, "WelcomeGreeting")->text().startsWith(QStringLiteral("Welcome to ")));
        next->click();
        QCOMPARE(d->currentPage(), 1);
        QVERIFY(back->isEnabled());
        next->click();
        next->click();
        next->click();
        QCOMPARE(d->currentPage(), 4);
        QCOMPARE(next->text(), QStringLiteral("Finish"));
        back->click();
        QCOMPARE(d->currentPage(), 3);
        QCOMPARE(next->text(), QStringLiteral("Next"));
        d->reject();
    }

    void nextNextNextFinishChangesNothing()
    {
        Fixture f;
        const auto before = stored();
        WelcomeDialog* d = f.window->showWelcome();
        for (int i = 0; i < 5; ++i)
            field<QPushButton>(d, "WelcomeNext")->click();
        QVERIFY(!d->isVisible());
        QCOMPARE(stored(), before);
        QVERIFY(d->restartNeeded().isEmpty());
    }

    void everyChoiceReachesItsSetting()
    {
        Fixture f;
        // Differs from every default.
        WelcomeDialog* d = f.window->showWelcome();
        pick(field<QComboBox>(d, "WelcomeLanguage"), QStringLiteral("de"));
        QComboBox* themeBox = field<QComboBox>(d, "WelcomeTheme");
        const QString otherScheme = theme::currentColorScheme() == QLatin1String("BreezeLight")
            ? QStringLiteral("BreezeDark")
            : QStringLiteral("BreezeLight");
        pick(themeBox, otherScheme);
        pick(field<QComboBox>(d, "WelcomeScale"), QStringLiteral("1.5"));
        pick(field<QComboBox>(d, "WelcomeMenuLayout"), menulayout::layoutId(menulayout::Layout::Streamlined));
        auto* keymap = field<QComboBox>(d, "WelcomeKeymap");
        QVERIFY(keymap->count() > 1);
        QString otherProfile;
        for (int i = 0; i < keymap->count(); ++i)
            if (keymap->itemData(i).toString() != f.actions.activeProfile())
                otherProfile = keymap->itemData(i).toString();
        pick(keymap, otherProfile);
        field<QCheckBox>(d, "WelcomeVim")->setChecked(true);
        field<QCheckBox>(d, "WelcomeAnimate")->setChecked(!prefs::animationsEnabled());
        pick(field<QComboBox>(d, "WelcomeZoom"), QStringLiteral("mouse"));
        pick(field<QComboBox>(d, "WelcomeWheel"), int(WheelScheme::Classic));
        const bool tabs = EditBehaviour::fromSettings().timelineTabs;
        const bool ruler = EditBehaviour::fromSettings().rulerPressJumpsPlayhead;
        field<QCheckBox>(d, "WelcomeTabs")->setChecked(!tabs);
        field<QCheckBox>(d, "WelcomeRuler")->setChecked(!ruler);
        field<QCheckBox>(d, "WelcomeStopOnMove")->setChecked(true);
        pick(field<QComboBox>(d, "WelcomeHwAccel"), prefs::hwAccelToString(prefs::HwAccel::Off));
        field<QCheckBox>(d, "WelcomeNotify")->setChecked(!prefs::renderNotificationsEnabled());
        const bool animationsBefore = prefs::animationsEnabled();
        const bool notifyBefore = prefs::renderNotificationsEnabled();
        bool applied = false;
        connect(d, &WelcomeDialog::applied, this, [&] { applied = true; });

        d->finish();
        QVERIFY(applied);
        QSettings s;
        QCOMPARE(s.value(QLatin1String(i18n::kLanguageKey)).toString(), QStringLiteral("de"));
        QCOMPARE(s.value(QLatin1String(prefs::kUiScale)).toString(), QStringLiteral("1.5"));
        QCOMPARE(d->restartNeeded().size(), 2);
        QCOMPARE(theme::currentColorScheme(), otherScheme);
        QCOMPARE(menulayout::layout(), menulayout::Layout::Streamlined);
        QCOMPARE(f.actions.activeProfile(), otherProfile);
        QVERIFY(vimopts::enabled());
        QCOMPARE(prefs::animationsEnabled(), !animationsBefore);
        QVERIFY(prefs::zoomAroundMouse());
        QCOMPARE(EditBehaviour::fromSettings().wheelScheme, WheelScheme::Classic);
        QCOMPARE(EditBehaviour::fromSettings().timelineTabs, !tabs);
        QCOMPARE(EditBehaviour::fromSettings().rulerPressJumpsPlayhead, !ruler);
        QVERIFY(PlaybackBehaviour::fromSettings().stopOnPlayheadMove);
        QCOMPARE(prefs::hwAccel(), prefs::HwAccel::Off);
        QCOMPARE(prefs::renderNotificationsEnabled(), !notifyBefore);
        // Live: the open window follows.
        QCOMPARE(f.window->menuLayout(), menulayout::Layout::Streamlined);
        // Leave the language alone for the other tests.
        QSettings().remove(QLatin1String(i18n::kLanguageKey));
        theme::setColorScheme(QStringLiteral("BreezeDark"));
    }

    void skipStoresNothing()
    {
        Fixture f;
        const auto before = stored();
        WelcomeDialog* d = f.window->showWelcome();
        pick(field<QComboBox>(d, "WelcomeMenuLayout"), menulayout::layoutId(menulayout::Layout::Streamlined));
        field<QCheckBox>(d, "WelcomeVim")->setChecked(true);
        field<QCheckBox>(d, "WelcomeStopOnMove")->setChecked(true);
        pick(field<QComboBox>(d, "WelcomeScale"), QStringLiteral("2"));
        field<QPushButton>(d, "WelcomeSkip")->click();
        QVERIFY(!d->isVisible());
        QCOMPARE(stored(), before);
        QCOMPARE(menulayout::layout(), menulayout::Layout::Resolve);
        QVERIFY(!vimopts::enabled());
        // Skipping counts as seen: it does not come back by itself.
        QVERIFY(!WelcomeDialog::shouldShow());
    }

    void lookChoicesPreviewLiveAndSkipRestores()
    {
        Fixture f;
        const auto before = stored();
        const QString startStyle = theme::currentStyle();
        const QString startScheme = theme::currentColorScheme();
        WelcomeDialog* d = f.window->showWelcome();
        auto* styleBox = field<QComboBox>(d, "WelcomeStyle");
        auto* themeBox = field<QComboBox>(d, "WelcomeTheme");
        QVERIFY(styleBox && styleBox->count() >= 2);
        QCOMPARE(styleBox->currentData().toString(), startStyle);
        // Colour scheme: applied at once, the style instance stays.
        const QString otherScheme = startScheme == QLatin1String("BreezeLight")
            ? QStringLiteral("BreezeDark")
            : QStringLiteral("BreezeLight");
        const QStyle* styleBefore = QApplication::style();
        pick(themeBox, otherScheme);
        QCOMPARE(theme::currentColorScheme(), otherScheme);
        QCOMPARE(QApplication::style(), styleBefore);
        // Widget style: every listed style applies at once and keeps the chosen scheme.
        for (int i = 0; i < styleBox->count(); ++i) {
            styleBox->setCurrentIndex(i);
            QCOMPARE(theme::currentStyle(), styleBox->currentData().toString());
            QCOMPARE(theme::currentColorScheme(), otherScheme);
            QApplication::processEvents();
            d->grab(); // paints the dialog in the new style without crashing
        }
        // Skip puts everything back, also what is stored.
        field<QPushButton>(d, "WelcomeSkip")->click();
        QCOMPARE(theme::currentStyle(), startStyle);
        QCOMPARE(theme::currentColorScheme(), startScheme);
        QCOMPARE(stored(), before);
    }

    void finishKeepsThePreviewedLook()
    {
        Fixture f;
        const QString startStyle = theme::currentStyle();
        WelcomeDialog* d = f.window->showWelcome();
        auto* styleBox = field<QComboBox>(d, "WelcomeStyle");
        QString other;
        for (int i = 0; i < styleBox->count(); ++i)
            if (styleBox->itemData(i).toString() != startStyle)
                other = styleBox->itemData(i).toString();
        pick(styleBox, other);
        d->finish();
        QCOMPARE(theme::currentStyle(), other);
        QCOMPARE(QSettings().value(QLatin1String(prefs::kWidgetStyle)).toString(), other);
        theme::setStyle(startStyle);
    }

    void fieldsStartAtTheStoredChoices()
    {
        Fixture f;
        menulayout::setLayout(menulayout::Layout::Streamlined);
        EditBehaviour::setTimelineTabs(false);
        WelcomeDialog* d = f.window->showWelcome();
        QCOMPARE(field<QComboBox>(d, "WelcomeMenuLayout")->currentData().toString(),
            menulayout::layoutId(menulayout::Layout::Streamlined));
        QVERIFY(!field<QCheckBox>(d, "WelcomeTabs")->isChecked());
        // The picture names the menus of the chosen layout.
        auto* hero = dynamic_cast<welcomeanim::MenusHero*>(
            d->findChild<QWidget*>(QStringLiteral("WelcomeMenusHero")));
        QVERIFY(hero);
        const QStringList streamlined = hero->titles();
        pick(field<QComboBox>(d, "WelcomeMenuLayout"), menulayout::layoutId(menulayout::Layout::Resolve));
        QVERIFY(hero->titles() != streamlined);
        QVERIFY(hero->titles().contains(QStringLiteral("Trim")));
        // Vim mode swaps the keycaps; the zoom choice reaches the timeline picture.
        field<QCheckBox>(d, "WelcomeVim")->setChecked(true);
        QVERIFY(hero->vim());
        pick(field<QComboBox>(d, "WelcomeZoom"), QStringLiteral("mouse"));
        auto* timeline = dynamic_cast<welcomeanim::TimelineHero*>(
            d->findChild<QWidget*>(QStringLiteral("WelcomeTimelineHero")));
        QVERIFY(timeline && timeline->zoomAroundMouse());
        d->reject();
        menulayout::setLayout(menulayout::Layout::Resolve);
    }

    void onlyTheFirstPageHasTheHeaderAndEveryFieldHasATooltip()
    {
        Fixture f;
        WelcomeDialog* d = f.window->showWelcome();
        auto* pages = d->findChild<QStackedWidget*>();
        QVERIFY(pages);
        for (int i = 0; i < pages->count(); ++i) {
            QWidget* page = pages->widget(i);
            const bool header = page->findChild<QWidget*>(QStringLiteral("WelcomeIcon"))
                || page->findChild<QLabel*>(QStringLiteral("WelcomeGreeting"));
            QCOMPARE(header, i == 0);
        }
        for (auto* combo : d->findChildren<QComboBox*>())
            QVERIFY2(!combo->toolTip().isEmpty(), qPrintable(combo->objectName()));
        for (auto* check : d->findChildren<QCheckBox*>())
            QVERIFY2(!check->toolTip().isEmpty(), qPrintable(check->objectName()));
        for (auto* button : d->findChildren<QPushButton*>())
            QVERIFY2(!button->toolTip().isEmpty(), qPrintable(button->objectName()));
        // A form row's label shows the same tooltip as its field.
        auto* zoom = field<QComboBox>(d, "WelcomeZoom");
        auto* form = qobject_cast<QFormLayout*>(zoom->parentWidget()->layout());
        QVERIFY(form);
        QCOMPARE(form->labelForField(zoom)->toolTip(), zoom->toolTip());
        d->reject();
    }

    void entrancesRunAndLeaveNothingBehind()
    {
        Fixture f;
        uianim::setEnabled(true);
        WelcomeDialog* d = f.window->showWelcome();
        auto* next = field<QPushButton>(d, "WelcomeNext");
        next->click();
        next->click(); // a change while the cascade of the last one still runs
        QCOMPARE(d->currentPage(), 2);
        QTest::qWait(1500);
        for (QWidget* w : d->findChildren<QWidget*>())
            QVERIFY2(!w->graphicsEffect(), qPrintable(w->objectName()));
        field<QPushButton>(d, "WelcomeBack")->click();
        QTest::qWait(1200);
        QCOMPARE(d->currentPage(), 1);
        for (QWidget* w : d->findChildren<QWidget*>())
            QVERIFY2(!w->graphicsEffect(), qPrintable(w->objectName()));
        d->reject();
        uianim::setEnabled(false);
    }

    void showsOnce()
    {
        QVERIFY(WelcomeDialog::shouldShow());
        QVERIFY(WelcomeDialog::wantedAtStartup(false, QStringLiteral("xcb")));
        Fixture f;
        WelcomeDialog* d = f.window->showWelcome();
        QCOMPARE(QSettings().value(QLatin1String(WelcomeDialog::kShownKey)).toInt(),
            WelcomeDialog::kVersion);
        d->reject();
        QVERIFY(!WelcomeDialog::shouldShow());
        QVERIFY(!WelcomeDialog::wantedAtStartup(false, QStringLiteral("xcb")));
        // A later version asks again.
        QSettings().setValue(QLatin1String(WelcomeDialog::kShownKey), WelcomeDialog::kVersion - 1);
        QVERIFY(WelcomeDialog::shouldShow());
    }

    void suppressSwitches()
    {
        QVERIFY(WelcomeDialog::wantedAtStartup(false, QStringLiteral("wayland")));
        QVERIFY(!WelcomeDialog::wantedAtStartup(true, QStringLiteral("wayland"))); // --no-welcome
        QVERIFY(!WelcomeDialog::wantedAtStartup(false, QStringLiteral("offscreen")));
        qputenv(WelcomeDialog::kSuppressEnv, "1");
        QVERIFY(!WelcomeDialog::wantedAtStartup(false, QStringLiteral("wayland")));
        qunsetenv(WelcomeDialog::kSuppressEnv);
        QVERIFY(WelcomeDialog::wantedAtStartup(false, QStringLiteral("wayland")));

        CommandLineOptions options;
        QVERIFY(!options.noWelcome);
        QString error;
        QVERIFY(parseCommandLine({QStringLiteral("kinoklip"), QStringLiteral("--no-welcome")},
            options, &error));
        QVERIFY(options.noWelcome);
    }

    void helpWelcomeReopensIt()
    {
        Fixture f;
        QAction* action = f.actions.action(QStringLiteral("help.welcome"));
        QVERIFY(action);
        QVERIFY(f.actions.defaultShortcuts(QStringLiteral("help.welcome")).isEmpty());
        QVERIFY(!action->statusTip().isEmpty());
        // Resolve layout: in the Help menu.
        auto* helpMenu = f.window->menuBar()->findChild<QMenu*>(QStringLiteral("menu:help"));
        QVERIFY(helpMenu);
        QVERIFY(helpMenu->actions().contains(action));

        action->trigger();
        auto dialogs = f.window->findChildren<WelcomeDialog*>();
        QCOMPARE(dialogs.size(), 1);
        QVERIFY(dialogs.first()->isVisible());
        action->trigger(); // already open: the same window
        QCOMPARE(f.window->findChildren<WelcomeDialog*>().size(), 1);
        dialogs.first()->reject();
        QVERIFY(!dialogs.first()->isVisible());
        // Closed and opened again (already "shown" once): a new window.
        WelcomeDialog* again = f.window->showWelcome();
        QVERIFY(again != dialogs.first());
        QVERIFY(again->isVisible());
        again->reject();

        // Streamlined layout: the entry is in Help, too.
        f.window->setMenuLayout(menulayout::Layout::Streamlined);
        auto* streamlinedHelp = f.window->menuBar()->findChild<QMenu*>(QStringLiteral("menu:help"));
        QVERIFY(streamlinedHelp);
        bool found = false;
        for (const QAction* entry : streamlinedHelp->actions())
            found = found || menulayout::mirroredId(entry) == QLatin1String("help.welcome");
        QVERIFY(found);
    }

    void keyboardBannerStepsAsideOnceWelcomeWasShown()
    {
        QVERIFY(KeymapChoiceBanner::shouldOffer());
        QSettings().setValue(QLatin1String(WelcomeDialog::kShownKey), WelcomeDialog::kVersion);
        QVERIFY(!KeymapChoiceBanner::shouldOffer());
        QSettings().remove(QLatin1String(WelcomeDialog::kShownKey));
        QVERIFY(KeymapChoiceBanner::shouldOffer()); // neither seen: the banner keeps its job
    }
};

QTEST_MAIN(TestWelcome)
#include "test_welcome.moc"
