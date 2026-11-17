// SPDX-License-Identifier: GPL-3.0-or-later
// WelcomeDialog implementation.
#include "app/WelcomeDialog.h"

#include "app/AppInfo.h"
#include "app/Theme.h"
#include "app/WelcomeAnimations.h"
#include "app/WelcomeHeroes.h"
#include "core/EditBehaviour.h"
#include "core/Preferences.h"
#include "core/Translations.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/DialogSizing.h"
#include "ui/common/MenuLayouts.h"
#include "ui/common/Metrics.h"
#include "ui/common/StandardActions.h"
#include "ui/common/VimOptions.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

constexpr char WelcomeDialog::kShownKey[];
constexpr char WelcomeDialog::kSuppressEnv[];

namespace {

constexpr QSize kDialogSize {720, 600};
constexpr int kPageCount = 5;

QString plain(const QString& menuTitle)
{
    QString t = menuTitle;
    t.remove(QLatin1Char('&'));
    return t;
}

/// Adds @p form to @p page at its own width, centred however wide the window is.
void addCentered(QWidget* page, QFormLayout* form)
{
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* column = new QWidget;
    column->setLayout(form);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    column->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(column);
    row->addStretch(1);
    static_cast<QVBoxLayout*>(page->layout())->addLayout(row);
}

QFormLayout* newForm()
{
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(metrics::kSpaceL);
    form->setVerticalSpacing(metrics::kSpaceL);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    return form;
}

/// A row whose label and field show the same tooltip; returns the two as one animation group.
welcomeanim::Group addRow(QFormLayout* form, const QString& label, QWidget* field, const QString& tip)
{
    auto* name = new QLabel(label);
    name->setToolTip(tip);
    field->setToolTip(tip);
    form->addRow(name, field);
    return {name, field};
}

QComboBox* choice(const char* objectName)
{
    auto* combo = new QComboBox;
    combo->setObjectName(QLatin1String(objectName));
    combo->setMinimumWidth(metrics::kFormComboMinWidth);
    return combo;
}

/// Tooltip of the combo's item @p index (shown in the open list).
void itemTip(QComboBox* combo, int index, const QString& tip)
{
    combo->setItemData(index, tip, Qt::ToolTipRole);
}

QCheckBox* box(const char* objectName, const QString& text, bool checked)
{
    auto* check = new QCheckBox(text);
    check->setObjectName(QLatin1String(objectName));
    check->setChecked(checked);
    return check;
}

} // namespace

WelcomeDialog::WelcomeDialog(ActionRegistry& registry, QWidget* parent)
    : QDialog(parent)
    , m_registry(registry)
{
    setObjectName(QStringLiteral("WelcomeDialog"));
    setWindowTitle(tr("Welcome to %1").arg(appinfo::displayName()));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceXl, metrics::kSpaceXl, metrics::kSpaceXl, metrics::kSpaceL);
    layout->setSpacing(metrics::kSpaceL);

    // The pages sit on a card; the buttons sit below a hairline. welcomeanim animates it all.
    auto* card = new welcomeanim::Card;
    card->setObjectName(QStringLiteral("WelcomeCard"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(
        metrics::kSpaceXl, metrics::kSpaceL, metrics::kSpaceXl, metrics::kSpaceL);
    m_pages = new QStackedWidget;
    m_pages->addWidget(welcomePage());
    m_pages->addWidget(lookPage());
    m_pages->addWidget(menusPage());
    m_pages->addWidget(timelinePage());
    m_pages->addWidget(systemPage());
    cardLayout->addWidget(m_pages);
    layout->addWidget(card, 1);

    auto* footer = new QWidget;
    auto* footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setSpacing(metrics::kSpaceL);
    auto* rule = new QFrame;
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    rule->setForegroundRole(QPalette::Mid);
    footerLayout->addWidget(rule);
    // Like KDE's Welcome Center: Skip on the left, the steps in the middle, Back and Next on
    // the right. The two sides are equally wide, so the steps sit in the true middle.
    auto* skip = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), tr("Skip"));
    skip->setObjectName(QStringLiteral("WelcomeSkip"));
    skip->setToolTip(tr("Close this window and change nothing. Help > Welcome opens it again."));
    m_back = new QPushButton(QIcon::fromTheme(QStringLiteral("go-previous")), tr("Back"));
    m_back->setObjectName(QStringLiteral("WelcomeBack"));
    m_back->setToolTip(tr("Go back to the previous page."));
    m_next = new QPushButton(QIcon::fromTheme(QStringLiteral("go-next")), tr("Next"));
    m_next->setObjectName(QStringLiteral("WelcomeNext"));
    m_next->setLayoutDirection(Qt::RightToLeft); // the arrow after the text
    m_next->setDefault(true);
    m_next->setMinimumWidth(100);
    m_back->setMinimumWidth(100);
    auto* left = new QWidget;
    auto* leftLayout = new QHBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(skip);
    leftLayout->addStretch(1);
    auto* right = new QWidget;
    auto* rightLayout = new QHBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addStretch(1);
    rightLayout->addWidget(m_back);
    rightLayout->addWidget(m_next);
    const int side = std::max(left->sizeHint().width(), right->sizeHint().width());
    left->setMinimumWidth(side);
    right->setMinimumWidth(side);
    m_step = new welcomeanim::StepIndicator(kPageCount);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(left, 1);
    buttons->addWidget(m_step, 0, Qt::AlignCenter);
    buttons->addWidget(right, 1);
    footerLayout->addLayout(buttons);
    layout->addWidget(footer);
    connect(skip, &QPushButton::clicked, this, &WelcomeDialog::skip);
    connect(m_back, &QPushButton::clicked, this, &WelcomeDialog::back);
    connect(m_next, &QPushButton::clicked, this, &WelcomeDialog::next);

    showPage(0);
    dialogsizing::fit(this, kDialogSize);
}

bool WelcomeDialog::shouldShow()
{
    return QSettings().value(QLatin1String(kShownKey), 0).toInt() < kVersion;
}

bool WelcomeDialog::wantedAtStartup(bool noWelcomeFlag, const QString& platform)
{
    if (noWelcomeFlag || qEnvironmentVariableIsSet(kSuppressEnv)
        || platform == QLatin1String("offscreen"))
        return false;
    return shouldShow();
}

int WelcomeDialog::pageCount() const
{
    return m_pages->count();
}

int WelcomeDialog::currentPage() const
{
    return m_pages->currentIndex();
}

void WelcomeDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (!event->spontaneous() && !m_didReveal) {
        m_didReveal = true;
        welcomeanim::enter(m_pages, m_groups.value(currentPage()), 120);
    }
    QSettings().setValue(QLatin1String(kShownKey), kVersion);
}

void WelcomeDialog::showPage(int index)
{
    index = std::clamp(index, 0, m_pages->count() - 1);
    welcomeanim::changePage(m_pages, index, index > m_pages->currentIndex(), m_groups.value(index));
    m_back->setEnabled(currentPage() > 0);
    const bool last = currentPage() == pageCount() - 1;
    m_next->setText(last ? tr("Finish") : tr("Next"));
    m_next->setIcon(QIcon::fromTheme(last ? QStringLiteral("dialog-ok-apply") : QStringLiteral("go-next")));
    m_next->setToolTip(last ? tr("Save your choices and close this window. Help > Welcome opens it again.")
                            : tr("Go to the next page."));
    m_step->setCurrent(currentPage());
}

void WelcomeDialog::next()
{
    if (currentPage() >= pageCount() - 1)
        finish();
    else
        showPage(currentPage() + 1);
}

void WelcomeDialog::back()
{
    showPage(currentPage() - 1);
}

void WelcomeDialog::skip()
{
    reject();
}

/// A page from the second on: a bold centred title and the hero under it. The fields are added
/// by the caller; every element is also an entrance group.
QWidget* WelcomeDialog::makePage(const QString& title, QWidget* hero)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(metrics::kSpaceL);
    layout->addSpacing(metrics::kSpaceM);
    auto* heading = new QLabel(title);
    QFont font = heading->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.4);
    heading->setFont(font);
    heading->setAlignment(Qt::AlignHCenter);
    layout->addWidget(heading);
    layout->addWidget(hero, 0, Qt::AlignHCenter);
    layout->addSpacing(metrics::kSpaceM);
    m_groups.append({{heading}, {hero}});
    return page;
}

/// Page 1: the big header (icon, name, one line) and the language.
QWidget* WelcomeDialog::welcomePage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(metrics::kSpaceL);

    // The app icon (the dev icon in a dev build): 32 px pixel art drawn from the 512 px file,
    // no smoothing, crisp edges.
    m_icon = new welcomeanim::IconBadge(QPixmap(appinfo::devBuildCommit().isEmpty()
            ? QStringLiteral(":/icons/app/kinoklip-512.png")
            : QStringLiteral(":/icons/app/kinoklip-dev-512.png")), 112);
    auto* greeting = new QLabel(tr("Welcome to %1").arg(appinfo::displayName()));
    greeting->setObjectName(QStringLiteral("WelcomeGreeting"));
    greeting->setAlignment(Qt::AlignHCenter);
    QFont big = greeting->font();
    big.setWeight(QFont::DemiBold);
    big.setPointSizeF(big.pointSizeF() * 2.2);
    greeting->setFont(big);
    auto* intro = new QLabel(tr("Works like DaVinci Resolve. A few choices and you're ready."));
    intro->setObjectName(QStringLiteral("WelcomeIntro"));
    intro->setAlignment(Qt::AlignHCenter);
    intro->setWordWrap(true);
    intro->setForegroundRole(QPalette::PlaceholderText);
    // Wraps to the window's width instead of widening it.
    intro->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    layout->addStretch(2);
    layout->addWidget(m_icon, 0, Qt::AlignHCenter);
    layout->addSpacing(metrics::kSpaceM);
    layout->addWidget(greeting);
    layout->addWidget(intro);
    layout->addStretch(2);

    auto* form = newForm();
    m_language = choice("WelcomeLanguage");
    m_language->addItem(tr("System default"), QStringLiteral("system"));
    for (const i18n::Language& language : i18n::languages())
        m_language->addItem(language.name, language.code); // each in its own language
    m_language->setCurrentIndex(std::max(0,
        m_language->findData(QSettings()
                                 .value(QLatin1String(i18n::kLanguageKey), QStringLiteral("system"))
                                 .toString())));
    const auto languageRow = addRow(form, tr("Language"), m_language,
        tr("The language of the menus and dialogs. It applies after a restart. Change it later "
           "in Preferences > User > UI Settings."));
    addCentered(page, form);
    layout->addStretch(1);
    m_groups.append({{m_icon}, {greeting}, {intro}, languageRow});
    return page;
}

QWidget* WelcomeDialog::lookPage()
{
    QWidget* page = makePage(tr("Look"), new welcomeanim::ThemeHero);
    auto* form = newForm();
    QList<welcomeanim::Group> rows;

    m_theme = choice("WelcomeTheme");
    for (const theme::ColorSchemeInfo& scheme : theme::availableColorSchemes())
        m_theme->addItem(scheme.id.isEmpty() ? tr("System") : scheme.name, scheme.id);
    m_theme->setCurrentIndex(std::max(0, m_theme->findData(theme::currentColorScheme())));
    rows << addRow(form, tr("Colour theme"), m_theme,
        tr("The colours of the whole window, dark or light. Change it later in the Workspace "
           "menu or in Preferences > User > UI Settings."));

    m_scale = choice("WelcomeScale");
    m_scale->addItem(tr("Automatic (system)"), QStringLiteral("auto"));
    for (const QString& value : uiScaleFactors())
        m_scale->addItem(QStringLiteral("%1%").arg(qRound(value.toDouble() * 100)), value);
    m_scale->setCurrentIndex(std::max(0,
        m_scale->findData(
            QSettings().value(QLatin1String(prefs::kUiScale), QStringLiteral("auto")).toString())));
    itemTip(m_scale, 0, tr("Follows the scale of your desktop."));
    rows << addRow(form, tr("Interface scale"), m_scale,
        tr("Makes everything bigger or smaller. It applies after a restart. Change it later in "
           "Preferences > User > UI Settings."));

    m_animate = box("WelcomeAnimate", tr("Animate timeline edits"), prefs::animationsEnabled());
    rows << addRow(form, tr("Animations"), m_animate,
        tr("Clips glide and settle after an edit instead of jumping. Turn it off for instant "
           "changes. Change it later in Preferences > User > UI Settings."));

    addCentered(page, form);
    static_cast<QVBoxLayout*>(page->layout())->addStretch(1);
    m_groups.last() += rows;
    return page;
}

QWidget* WelcomeDialog::menusPage()
{
    m_menusHero = new welcomeanim::MenusHero;
    QWidget* page = makePage(tr("Menus and keys"), m_menusHero);
    auto* form = newForm();
    QList<welcomeanim::Group> rows;

    m_menuLayout = choice("WelcomeMenuLayout");
    m_menuLayout->addItem(tr("Resolve"), menulayout::layoutId(menulayout::Layout::Resolve));
    m_menuLayout->addItem(
        tr("Streamlined"), menulayout::layoutId(menulayout::Layout::Streamlined));
    itemTip(m_menuLayout, 0, tr("The menus as in DaVinci Resolve."));
    itemTip(m_menuLayout, 1,
        tr("Shorter menus grouped by task, with plain names. The commands and their keys are "
           "the same."));
    m_menuLayout->setCurrentIndex(
        std::max(0, m_menuLayout->findData(menulayout::layoutId(menulayout::layout()))));
    rows << addRow(form, tr("Menu layout"), m_menuLayout,
        tr("How the menus are arranged. The picture above shows the menu titles. Change it "
           "later in Preferences > User > UI Settings."));
    connect(m_menuLayout, &QComboBox::currentIndexChanged, this, &WelcomeDialog::updateMenuPreview);
    updateMenuPreview();

    m_keymap = choice("WelcomeKeymap");
    const QString active = m_registry.activeProfile();
    for (const ActionRegistry::KeymapProfile& p : m_registry.profiles()) {
        if (!p.builtIn && p.id != active)
            continue; // the other own profiles stay in Keyboard Customization
        m_keymap->addItem(p.id == QLatin1String(ActionRegistry::kDefaultProfile)
                ? tr("%1 (default)").arg(p.name)
                : p.name,
            p.id);
    }
    m_keymap->setCurrentIndex(std::max(0, m_keymap->findData(active)));
    rows << addRow(form, tr("Keyboard"), m_keymap,
        tr("Coming from another editor? Use its keys. Change it later in Keyboard "
           "Customization, in the first menu."));

    QAction* vimAction = m_registry.action(QLatin1String(vimopts::kActionId));
    m_vim = box("WelcomeVim", tr("Vim mode"), vimAction ? vimAction->isChecked() : vimopts::enabled());
    rows << addRow(form, tr("Vim"), m_vim,
        tr("Edit with the Vim keys h, j, k, l, i and Esc. Leave it off unless you know Vim. "
           "Change it later in Preferences > User > Keyboard."));
    m_menusHero->setVim(m_vim->isChecked());
    connect(m_vim, &QCheckBox::toggled, m_menusHero, &welcomeanim::MenusHero::setVim);

    addCentered(page, form);
    static_cast<QVBoxLayout*>(page->layout())->addStretch(1);
    m_groups.last() += rows;
    return page;
}

void WelcomeDialog::updateMenuPreview()
{
    const bool streamlined = m_menuLayout->currentData().toString()
        == menulayout::layoutId(menulayout::Layout::Streamlined);
    QStringList titles;
    if (streamlined) {
        for (const menulayout::Menu& menu : menulayout::streamlinedMenus())
            titles << plain(menu.title);
    } else {
        for (const MenuCategory& menu : standardMenus())
            titles << plain(menu.title);
    }
    m_menusHero->setTitles(titles);
}

QWidget* WelcomeDialog::timelinePage()
{
    m_timelineHero = new welcomeanim::TimelineHero;
    QWidget* page = makePage(tr("Timeline"), m_timelineHero);
    auto* form = newForm();
    QList<welcomeanim::Group> rows;
    const EditBehaviour edit = EditBehaviour::fromSettings();
    const PlaybackBehaviour playback = PlaybackBehaviour::fromSettings();

    m_zoom = choice("WelcomeZoom");
    m_zoom->addItem(tr("Playhead"), QStringLiteral("playhead"));
    m_zoom->addItem(tr("Mouse pointer"), QStringLiteral("mouse"));
    itemTip(m_zoom, 0, tr("Zooming keeps the playhead where it is, like Resolve."));
    itemTip(m_zoom, 1, tr("Zooming keeps the point under the mouse pointer where it is."));
    m_zoom->setCurrentIndex(prefs::zoomAroundMouse() ? 1 : 0);
    rows << addRow(form, tr("Timeline zooms around"), m_zoom,
        tr("Which point stays in place when you zoom the timeline. Change it later in "
           "Preferences > User > Editing."));
    connect(m_zoom, &QComboBox::currentIndexChanged, this,
        [this] { m_timelineHero->setZoomAroundMouse(m_zoom->currentData().toString() == QLatin1String("mouse")); });
    m_timelineHero->setZoomAroundMouse(prefs::zoomAroundMouse());

    m_wheel = choice("WelcomeWheel");
    m_wheel->addItem(tr("Resolve (Alt zooms, Ctrl scrolls time, Shift track height)"),
        int(WheelScheme::Resolve));
    m_wheel->addItem(tr("Classic (Ctrl zooms, Shift scrolls time)"), int(WheelScheme::Classic));
    itemTip(m_wheel, 0, tr("The wheel as in DaVinci Resolve."));
    itemTip(m_wheel, 1, tr("The wheel as in most other editors."));
    m_wheel->setCurrentIndex(std::max(0, m_wheel->findData(int(edit.wheelScheme))));
    rows << addRow(form, tr("Mouse wheel"), m_wheel,
        tr("What the mouse wheel does on the timeline, alone and with Alt, Ctrl or Shift. "
           "Change it later in Preferences > User > Editing."));

    m_tabs = box("WelcomeTabs", tr("Show open timelines as tabs"), edit.timelineTabs);
    rows << addRow(form, tr("Timelines"), m_tabs,
        tr("Open timelines appear as tabs above the timeline. Change it later in Preferences > "
           "User > Timeline and Media Pool."));
    m_ruler = box("WelcomeRuler", tr("Pressing the ruler moves the playhead there at once"),
        edit.rulerPressJumpsPlayhead);
    rows << addRow(form, tr("Timeline ruler"), m_ruler,
        tr("The playhead jumps to the point you press on the ruler, with no need to drag. Change "
           "it later in Preferences > User > Timeline and Media Pool."));
    m_stopOnMove
        = box("WelcomeStopOnMove", tr("Stop playback when the playhead is moved"),
            playback.stopOnPlayheadMove);
    rows << addRow(form, tr("Moving the playhead"), m_stopOnMove,
        tr("Playback stops when you move the playhead. Off keeps playing, as in Resolve. Change "
           "it later in Preferences > User > Playback Settings."));

    addCentered(page, form);
    static_cast<QVBoxLayout*>(page->layout())->addStretch(1);
    m_groups.last() += rows;
    return page;
}

QWidget* WelcomeDialog::systemPage()
{
    m_systemHero = new welcomeanim::SystemHero;
    QWidget* page = makePage(tr("System"), m_systemHero);
    auto* form = newForm();
    QList<welcomeanim::Group> rows;

    m_hwAccel = choice("WelcomeHwAccel");
    for (const QString& id : prefs::hwAccelIds())
        m_hwAccel->addItem(hwAccelDisplayName(prefs::hwAccelFromString(id)), id);
    m_hwAccel->setCurrentIndex(
        std::max(0, m_hwAccel->findData(prefs::hwAccelToString(prefs::hwAccel()))));
    rows << addRow(form, tr("Hardware decoding"), m_hwAccel,
        tr("Uses the graphics card to play video. Automatic is right for most computers. Change "
           "it later in Preferences > System > Decode Options."));

    m_notify = box("WelcomeNotify", tr("Show a system notification when renders finish"),
        prefs::renderNotificationsEnabled());
    rows << addRow(form, tr("Notifications"), m_notify,
        tr("Your desktop shows a message when a render is done. Change it later in Preferences "
           "> User > UI Settings."));
    m_systemHero->setNotifications(m_notify->isChecked());
    connect(m_notify, &QCheckBox::toggled, m_systemHero, &welcomeanim::SystemHero::setNotifications);

    addCentered(page, form);
    static_cast<QVBoxLayout*>(page->layout())->addStretch(1);
    m_groups.last() += rows;
    return page;
}

void WelcomeDialog::finish()
{
    m_restartNeeded.clear();
    QSettings settings;

    // ---- Look ----
    const QString language = m_language->currentData().toString();
    if (language
        != settings.value(QLatin1String(i18n::kLanguageKey), QStringLiteral("system")).toString()) {
        settings.setValue(QLatin1String(i18n::kLanguageKey), language);
        m_restartNeeded << tr("Language");
    }
    const QString scheme = m_theme->currentData().toString();
    if (scheme != theme::currentColorScheme())
        theme::setColorScheme(scheme);
    const QString scale = m_scale->currentData().toString();
    if (scale
        != settings.value(QLatin1String(prefs::kUiScale), QStringLiteral("auto")).toString()) {
        settings.setValue(QLatin1String(prefs::kUiScale), scale);
        m_restartNeeded << tr("Interface scale");
    }

    // ---- Menus and keys ----
    const auto layout = menulayout::layoutFromId(m_menuLayout->currentData().toString());
    if (layout != menulayout::layout())
        menulayout::setLayout(layout);
    const QString profile = m_keymap->currentData().toString();
    if (profile != m_registry.activeProfile()) {
        QString error;
        if (!m_registry.setActiveProfile(profile, &error))
            QMessageBox::warning(this, tr("Keyboard Profile"), error);
    }
    QAction* vimAction = m_registry.action(QLatin1String(vimopts::kActionId));
    const bool vimNow = vimAction ? vimAction->isChecked() : vimopts::enabled();
    if (m_vim->isChecked() != vimNow) {
        if (vimAction)
            vimAction->setChecked(m_vim->isChecked()); // stores vim/enabled
        else
            vimopts::setEnabled(m_vim->isChecked());
    }

    // ---- Timeline ----
    const EditBehaviour edit = EditBehaviour::fromSettings();
    if (m_animate->isChecked() != prefs::animationsEnabled())
        prefs::setAnimationsEnabled(m_animate->isChecked());
    const bool mouseZoom = m_zoom->currentData().toString() == QLatin1String("mouse");
    if (mouseZoom != prefs::zoomAroundMouse())
        EditBehaviour::setZoomAnchor(mouseZoom ? ZoomAnchor::Cursor : ZoomAnchor::Playhead);
    const auto wheel = WheelScheme(m_wheel->currentData().toInt());
    if (wheel != edit.wheelScheme)
        EditBehaviour::setWheelScheme(wheel);
    if (m_tabs->isChecked() != edit.timelineTabs)
        EditBehaviour::setTimelineTabs(m_tabs->isChecked());
    if (m_ruler->isChecked() != edit.rulerPressJumpsPlayhead)
        EditBehaviour::setRulerPressJumpsPlayhead(m_ruler->isChecked());
    if (m_stopOnMove->isChecked() != PlaybackBehaviour::fromSettings().stopOnPlayheadMove)
        PlaybackBehaviour::setStopOnPlayheadMove(m_stopOnMove->isChecked());

    // ---- System ----
    const prefs::HwAccel hw = prefs::hwAccelFromString(m_hwAccel->currentData().toString());
    if (hw != prefs::hwAccel())
        prefs::setHwAccel(hw);
    if (m_notify->isChecked() != prefs::renderNotificationsEnabled())
        prefs::setRenderNotificationsEnabled(m_notify->isChecked());

    emit applied();
    if (!m_restartNeeded.isEmpty() && parentWidget()) {
        // Not blocking, and owned by the main window: this window closes right away.
        auto* note = new QMessageBox(QMessageBox::Information, tr("Restart Required"),
            tr("Restart %1 to apply: %2.")
                .arg(appinfo::displayName(), m_restartNeeded.join(QStringLiteral(", "))),
            QMessageBox::Ok, parentWidget());
        note->setObjectName(QStringLiteral("RestartRequired"));
        note->setAttribute(Qt::WA_DeleteOnClose);
        note->open();
    }
    accept();
}

} // namespace kk
