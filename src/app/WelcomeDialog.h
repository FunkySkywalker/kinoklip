// SPDX-License-Identifier: GPL-3.0-or-later
// WelcomeDialog — the window that opens on the first start (and once after an update that adds
// choices, Help > Welcome reopens it): a few short pages that ask about what Kinoklip does
// differently from DaVinci Resolve or offers on top of it (docs/behavior-spec.md section 110).
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;

namespace kk {

class ActionRegistry;

/// Four pages: Look (language, colour theme, interface scale), Menus and keys (menu layout,
/// keyboard profile, Vim mode), Timeline (animations, zoom anchor, mouse wheel, tabs, ruler,
/// stop on playhead move) and System (hardware decoding, render notifications). Every field
/// starts at what is stored now, so Next, Next, Next, Finish changes nothing; Finish stores
/// only the fields that were changed, through the same code the Preferences pages use, and
/// emits applied() so the window re-reads them. Skip (or closing the window) stores nothing.
/// Every page says where the choice lives in Preferences.
///
/// The window records itself as shown ("welcome/shown" = kVersion) when it appears; raise
/// kVersion to show it once more after a change big enough to need new questions.
class WelcomeDialog : public QDialog
{
    Q_OBJECT
public:
    /// QSettings key holding the version of the welcome window that was last shown (0 / unset:
    /// never). KeymapChoiceBanner reads it too.
    static constexpr char kShownKey[] = "welcome/shown";
    static constexpr int kVersion = 1;
    /// Environment variable that suppresses the window at start-up (tests, scripts).
    static constexpr char kSuppressEnv[] = "KK_NO_WELCOME";

    /// @p registry gives the keyboard profiles and the Vim mode action; it must outlive the
    /// dialog.
    explicit WelcomeDialog(ActionRegistry& registry, QWidget* parent = nullptr);

    /// True if the stored "welcome/shown" is older than kVersion.
    [[nodiscard]] static bool shouldShow();
    /// True if the window should open by itself at start-up: shouldShow(), and neither
    /// @p noWelcomeFlag (--no-welcome), the KK_NO_WELCOME variable nor an offscreen
    /// @p platform (tests, scripted runs) says no.
    [[nodiscard]] static bool wantedAtStartup(bool noWelcomeFlag, const QString& platform);

    [[nodiscard]] int pageCount() const;
    [[nodiscard]] int currentPage() const;
    /// Next, or Finish on the last page.
    void next();
    void back();
    /// Closes without storing anything (the Skip button).
    void skip();
    /// Stores the changed choices, announces them and closes (the Finish button).
    void finish();
    /// What needed a restart in the last finish() (language, interface scale).
    [[nodiscard]] QStringList restartNeeded() const { return m_restartNeeded; }

signals:
    /// Choices were stored: the application re-reads the preferences (timeline tabs, animations,
    /// hardware decoding, ...).
    void applied();

protected:
    void showEvent(QShowEvent* event) override;

private:
    QWidget* makePage(const QString& title, QWidget* header = nullptr);
    QWidget* lookPage();
    QWidget* menusPage();
    QWidget* timelinePage();
    QWidget* systemPage();
    void showPage(int index);
    void updateMenuPreview();

    ActionRegistry& m_registry;
    QStackedWidget* m_pages = nullptr;
    QPushButton* m_back = nullptr;
    QPushButton* m_next = nullptr;
    QStringList m_restartNeeded;

    // Look
    QComboBox* m_language = nullptr;
    QComboBox* m_theme = nullptr;
    QComboBox* m_scale = nullptr;
    // Menus and keys
    QComboBox* m_menuLayout = nullptr;
    QLabel* m_menuPreview = nullptr;
    QComboBox* m_keymap = nullptr;
    QCheckBox* m_vim = nullptr;
    // Timeline
    QCheckBox* m_animate = nullptr;
    QComboBox* m_zoom = nullptr;
    QComboBox* m_wheel = nullptr;
    QCheckBox* m_tabs = nullptr;
    QCheckBox* m_ruler = nullptr;
    QCheckBox* m_stopOnMove = nullptr;
    // System
    QComboBox* m_hwAccel = nullptr;
    QCheckBox* m_notify = nullptr;
};

} // namespace kk
