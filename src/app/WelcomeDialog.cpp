// SPDX-License-Identifier: GPL-3.0-or-later
// WelcomeDialog implementation.
#include "app/WelcomeDialog.h"

#include "app/AppInfo.h"
#include "app/Theme.h"
#include "app/WelcomeAnimations.h"
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

constexpr QSize kDialogSize {720, 700};

QString plain(const QString& menuTitle)
{
    QString t = menuTitle;
    t.remove(QLatin1Char('&'));
    return t;
}

QString joinTitles(const QStringList& titles)
{
    return titles.join(QStringLiteral(", "));
}

/// Dimmed explanatory text (the palette's placeholder colour reads in light and dark schemes).
QLabel* dim(const QString& text)
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

/// The note at the foot of a page: dimmed, smaller, centred.
QLabel* note(const QString& text)
{
    QLabel* label = dim(text);
    label->setAlignment(Qt::AlignHCenter);
    QFont small = label->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    label->setFont(small);
    return label;
}

/// Adds @p form to @p page as a centred column of fixed width, so the labels and fields sit in
/// the middle of the window instead of along its left edge.
void addCentered(QWidget* page, QFormLayout* form)
{
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* column = new QWidget;
    column->setLayout(form);
    // Hints under a field use the field column's width, not the field's.
    for (int row = 0; row < form->rowCount(); ++row)
        if (auto* item = form->itemAt(row, QFormLayout::FieldRole))
            if (auto* hint = qobject_cast<QLabel*>(item->widget()); hint && hint->wordWrap())
                hint->setMinimumWidth(320);
    column->setFixedWidth(500);
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(column);
    row->addStretch(1);
    static_cast<QVBoxLayout*>(page->layout())->addLayout(row);
}

QComboBox* choice(const char* objectName)
{
    auto* combo = new QComboBox;
    combo->setObjectName(QLatin1String(objectName));
    combo->setMinimumWidth(metrics::kFormComboMinWidth);
    return combo;
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

    // The app icon (the dev icon in a dev build), the greeting and the steps, centred above a
    // card that holds the pages; the buttons sit below a hairline. welcomeanim animates it all.
    // The 32 px pixel art drawn from the 512 px file: no smoothing, crisp edges.
    m_icon = new welcomeanim::IconBadge(QPixmap(appinfo::devBuildCommit().isEmpty()
            ? QStringLiteral(":/icons/app/kinoklip-512.png")
            : QStringLiteral(":/icons/app/kinoklip-dev-512.png")), 88);
    auto* greeting = new QLabel(tr("Welcome to %1").arg(appinfo::displayName()));
    greeting->setObjectName(QStringLiteral("WelcomeGreeting"));
    greeting->setAlignment(Qt::AlignHCenter);
    QFont big = greeting->font();
    big.setWeight(QFont::DemiBold);
    big.setPointSizeF(big.pointSizeF() * 2.0);
    greeting->setFont(big);
    QLabel* intro = dim(tr("It works like DaVinci Resolve. These few questions are about what "
                           "is different here. Nothing changes unless you choose it."));
    intro->setAlignment(Qt::AlignHCenter);
    m_step = new welcomeanim::StepIndicator(4);
    layout->addWidget(m_icon, 0, Qt::AlignHCenter);
    layout->addSpacing(metrics::kSpaceS);
    layout->addWidget(greeting);
    layout->addWidget(intro);
    layout->addSpacing(metrics::kSpaceS);
    layout->addWidget(m_step, 0, Qt::AlignHCenter);

    auto* card = new welcomeanim::Card;
    card->setObjectName(QStringLiteral("WelcomeCard"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(
        metrics::kSpaceXl, metrics::kSpaceL, metrics::kSpaceXl, metrics::kSpaceL);
    m_pages = new QStackedWidget;
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
    auto* buttons = new QHBoxLayout;
    auto* skip = new QPushButton(tr("Skip"));
    skip->setObjectName(QStringLiteral("WelcomeSkip"));
    skip->setFlat(true);
    skip->setToolTip(tr("Close this window and change nothing. Help > Welcome opens it again."));
    m_back = new QPushButton(tr("Back"));
    m_back->setObjectName(QStringLiteral("WelcomeBack"));
    m_next = new QPushButton(tr("Next"));
    m_next->setObjectName(QStringLiteral("WelcomeNext"));
    m_next->setDefault(true);
    m_next->setMinimumWidth(110);
    m_back->setMinimumWidth(110);
    buttons->addWidget(skip);
    buttons->addStretch(1);
    buttons->addWidget(m_back);
    buttons->addWidget(m_next);
    footerLayout->addLayout(buttons);
    layout->addWidget(footer);
    m_revealed = {greeting, intro, m_step, card, footer};
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
        welcomeanim::reveal(m_icon, m_revealed);
    }
    QSettings().setValue(QLatin1String(kShownKey), kVersion);
}

void WelcomeDialog::showPage(int index)
{
    index = std::clamp(index, 0, m_pages->count() - 1);
    welcomeanim::slideTo(m_pages, index, index > m_pages->currentIndex());
    m_back->setEnabled(currentPage() > 0);
    m_next->setText(currentPage() == pageCount() - 1 ? tr("Finish") : tr("Next"));
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

/// A page: a title, an optional header above it (the first page), the fields added by the
/// caller to the returned form's widget, and the "change it later" note added last.
QWidget* WelcomeDialog::makePage(const QString& title, QWidget* header)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(metrics::kSpaceL);
    if (header)
        layout->addWidget(header);
    auto* heading = new QLabel(title);
    QFont font = heading->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.3);
    heading->setFont(font);
    heading->setAlignment(Qt::AlignHCenter);
    layout->addWidget(heading);
    return page;
}

QWidget* WelcomeDialog::lookPage()
{
    QWidget* page = makePage(tr("Look"));
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(metrics::kSpaceL);
    form->setVerticalSpacing(metrics::kSpaceM);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_language = choice("WelcomeLanguage");
    m_language->addItem(tr("System default"), QStringLiteral("system"));
    for (const i18n::Language& language : i18n::languages())
        m_language->addItem(language.name, language.code); // each in its own language
    m_language->setCurrentIndex(std::max(0,
        m_language->findData(QSettings()
                                 .value(QLatin1String(i18n::kLanguageKey), QStringLiteral("system"))
                                 .toString())));
    form->addRow(tr("Language"), m_language);

    m_theme = choice("WelcomeTheme");
    for (const theme::ColorSchemeInfo& scheme : theme::availableColorSchemes())
        m_theme->addItem(scheme.id.isEmpty() ? tr("System") : scheme.name, scheme.id);
    m_theme->setCurrentIndex(std::max(0, m_theme->findData(theme::currentColorScheme())));
    form->addRow(tr("Colour theme"), m_theme);

    m_scale = choice("WelcomeScale");
    m_scale->addItem(tr("Automatic (system)"), QStringLiteral("auto"));
    for (const QString& value : uiScaleFactors())
        m_scale->addItem(QStringLiteral("%1%").arg(qRound(value.toDouble() * 100)), value);
    m_scale->setCurrentIndex(std::max(0,
        m_scale->findData(
            QSettings().value(QLatin1String(prefs::kUiScale), QStringLiteral("auto")).toString())));
    form->addRow(tr("Interface scale"), m_scale);

    addCentered(page, form);
    static_cast<QVBoxLayout*>(page->layout())->addStretch(1);
    page->layout()->addWidget(note(tr("The language and the scale apply after a restart. Change all "
                                     "of this later in Preferences > User > UI Settings (the "
                                     "theme is under Appearance there, or in the Workspace "
                                     "menu).")));
    return page;
}

QWidget* WelcomeDialog::menusPage()
{
    QWidget* page = makePage(tr("Menus and keys"));
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(metrics::kSpaceL);
    form->setVerticalSpacing(metrics::kSpaceM);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_menuLayout = choice("WelcomeMenuLayout");
    m_menuLayout->addItem(
        tr("Resolve"), menulayout::layoutId(menulayout::Layout::Resolve));
    m_menuLayout->addItem(
        tr("Streamlined"), menulayout::layoutId(menulayout::Layout::Streamlined));
    m_menuLayout->setCurrentIndex(
        std::max(0, m_menuLayout->findData(menulayout::layoutId(menulayout::layout()))));
    form->addRow(tr("Menu layout"), m_menuLayout);
    m_menuPreview = dim({});
    m_menuPreview->setObjectName(QStringLiteral("WelcomeMenuPreview"));
    form->addRow(QString(), m_menuPreview);
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
    form->addRow(tr("Keyboard"), m_keymap);
    form->addRow(QString(), dim(tr("Coming from another editor? Use its keys.")));

    QAction* vimAction = m_registry.action(QLatin1String(vimopts::kActionId));
    m_vim = box("WelcomeVim", tr("Vim mode"), vimAction ? vimAction->isChecked() : vimopts::enabled());
    form->addRow(tr("Vim"), m_vim);
    form->addRow(QString(),
        dim(tr("Vim keys for editing (h, j, k, l, i, Esc). Off unless you know Vim.")));

    addCentered(page, form);
    static_cast<QVBoxLayout*>(page->layout())->addStretch(1);
    page->layout()->addWidget(note(tr("Change these later in Preferences > User > UI Settings "
                                     "(menu layout) and Preferences > User > Keyboard (Vim mode); "
                                     "the keyboard profile is under Keyboard Customization in the first menu.")));
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
        m_menuPreview->setText(tr("Shorter menus grouped by task, with plain names, and one menu "
                                  "per page. The commands and their keys are the same.")
            + QLatin1Char('\n') + joinTitles(titles));
    } else {
        for (const MenuCategory& menu : standardMenus())
            titles << plain(menu.title);
        m_menuPreview->setText(tr("The menus as in DaVinci Resolve.") + QLatin1Char('\n')
            + joinTitles(titles));
    }
}

QWidget* WelcomeDialog::timelinePage()
{
    QWidget* page = makePage(tr("Timeline"));
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(metrics::kSpaceL);
    form->setVerticalSpacing(metrics::kSpaceM);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    const EditBehaviour edit = EditBehaviour::fromSettings();
    const PlaybackBehaviour playback = PlaybackBehaviour::fromSettings();

    m_animate = box("WelcomeAnimate", tr("Animate timeline edits"), prefs::animationsEnabled());
    form->addRow(tr("Animations"), m_animate);

    m_zoom = choice("WelcomeZoom");
    m_zoom->addItem(tr("Playhead"), QStringLiteral("playhead"));
    m_zoom->addItem(tr("Mouse pointer"), QStringLiteral("mouse"));
    m_zoom->setCurrentIndex(prefs::zoomAroundMouse() ? 1 : 0);
    form->addRow(tr("Timeline zooms around"), m_zoom);

    m_wheel = choice("WelcomeWheel");
    m_wheel->addItem(tr("Resolve (Alt zooms, Ctrl scrolls time, Shift track height)"),
        int(WheelScheme::Resolve));
    m_wheel->addItem(tr("Classic (Ctrl zooms, Shift scrolls time)"), int(WheelScheme::Classic));
    m_wheel->setCurrentIndex(std::max(0, m_wheel->findData(int(edit.wheelScheme))));
    form->addRow(tr("Mouse wheel"), m_wheel);

    m_tabs = box("WelcomeTabs", tr("Show open timelines as tabs"), edit.timelineTabs);
    form->addRow(tr("Timelines"), m_tabs);
    m_ruler = box("WelcomeRuler", tr("Pressing the ruler moves the playhead there at once"),
        edit.rulerPressJumpsPlayhead);
    form->addRow(tr("Timeline ruler"), m_ruler);
    m_stopOnMove
        = box("WelcomeStopOnMove", tr("Stop playback when the playhead is moved"),
            playback.stopOnPlayheadMove);
    form->addRow(tr("Moving the playhead"), m_stopOnMove);

    addCentered(page, form);
    static_cast<QVBoxLayout*>(page->layout())->addStretch(1);
    page->layout()->addWidget(note(tr("Resolve keeps the playhead where it is when zooming and "
                                     "carries on playing when you move it. Change these later in "
                                     "Preferences > User: Animations in UI Settings; zoom and "
                                     "wheel in Editing; tabs and ruler in Timeline and Media "
                                     "Pool; the playhead in Playback Settings.")));
    return page;
}

QWidget* WelcomeDialog::systemPage()
{
    QWidget* page = makePage(tr("System"));
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(metrics::kSpaceL);
    form->setVerticalSpacing(metrics::kSpaceM);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_hwAccel = choice("WelcomeHwAccel");
    for (const QString& id : prefs::hwAccelIds())
        m_hwAccel->addItem(hwAccelDisplayName(prefs::hwAccelFromString(id)), id);
    m_hwAccel->setCurrentIndex(
        std::max(0, m_hwAccel->findData(prefs::hwAccelToString(prefs::hwAccel()))));
    form->addRow(tr("Hardware decoding"), m_hwAccel);
    form->addRow(QString(), dim(tr("Uses the graphics card to play video. Automatic is right "
                                   "for most computers.")));

    m_notify = box("WelcomeNotify", tr("Show a system notification when renders finish"),
        prefs::renderNotificationsEnabled());
    form->addRow(tr("Notifications"), m_notify);

    addCentered(page, form);
    static_cast<QVBoxLayout*>(page->layout())->addStretch(1);
    page->layout()->addWidget(note(tr("Change these later in Preferences > System > Decode "
                                     "Options and Preferences > User > UI Settings. Help > "
                                     "Welcome opens this window again.")));
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
