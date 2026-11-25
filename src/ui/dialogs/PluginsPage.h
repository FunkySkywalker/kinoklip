// SPDX-License-Identifier: GPL-3.0-or-later
// PluginsPage — Preferences > Plugins (docs/plugins/installing-plugins.md): the installed plugins
// with their state, what each contains and may do, install from a .kkplugin file or a folder,
// uninstall, enable and disable, reload for plugin developers, and the folder to put plugins in.
// Changes take effect at once (the page has nothing to save).
#pragma once

#include "ui/common/SettingsPages.h"

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTextBrowser;

namespace kk {

class PluginsPage final : public SettingsPage
{
    Q_OBJECT
public:
    explicit PluginsPage(QWidget* parent = nullptr);

    void apply() override { }

    /// Installs @p path (a .kkplugin file or folder) the way the Install button does, with the
    /// same messages; true on success. Also used for files dropped on the window.
    static bool installWithMessages(const QString& path, QWidget* parent);

    /// Details text of the plugin @p pluginId as HTML (tests, the details pane).
    [[nodiscard]] static QString detailsHtml(const QString& pluginId);

private:
    void refresh();
    void showDetails();
    void onItemChanged(QListWidgetItem* item);
    [[nodiscard]] QString currentId() const;

    QListWidget* m_list = nullptr;
    QTextBrowser* m_details = nullptr;
    QPushButton* m_uninstall = nullptr;
    bool m_refreshing = false;
};

/// Adds the "Plugins" page to Preferences > User.
void registerPluginsPage(SettingsPageRegistry& pages);

} // namespace kk
