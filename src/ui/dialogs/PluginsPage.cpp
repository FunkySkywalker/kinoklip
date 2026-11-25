// SPDX-License-Identifier: GPL-3.0-or-later
// PluginsPage — see PluginsPage.h.
#include "ui/dialogs/PluginsPage.h"

#include "platform/FileDialogs.h"
#include "platform/OpenUrl.h"
#include "plugins/PluginRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace kk {

namespace {

constexpr int kIdRole = Qt::UserRole;
constexpr int kListMinHeight = 120;

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::PluginsPage", text);
}

QString esc(const QString& s)
{
    return s.toHtmlEscaped();
}

/// One line of a list in the details: the item name and its id.
QString itemLine(const QString& name, const QString& id)
{
    return QStringLiteral("<li>%1 <small>(%2)</small></li>").arg(esc(name), esc(id));
}

} // namespace

QString PluginsPage::detailsHtml(const QString& pluginId)
{
    const plugins::PluginRegistry& registry = plugins::PluginRegistry::instance();
    const plugins::LoadedPlugin* p = registry.find(pluginId);
    if (!p)
        return {};
    QString html;
    if (!p->valid && p->manifest.name.isEmpty()) {
        html += QStringLiteral("<h3>%1</h3>").arg(esc(p->folderName));
    } else {
        html += QStringLiteral("<h3>%1 <small>%2</small></h3>")
                    .arg(esc(p->manifest.name), esc(p->manifest.version));
        html += QStringLiteral("<p>%1</p>").arg(esc(p->manifest.description));
        html += QStringLiteral("<p>%1 %2<br>%3 %4<br>%5 %6</p>")
                    .arg(tr("Author:"), esc(p->manifest.author), tr("Licence:"),
                        esc(p->manifest.license), tr("Id:"), esc(p->manifest.id));
    }
    html += QStringLiteral("<p>%1 <a href=\"file://%2\">%2</a></p>")
                .arg(p->source == plugins::PluginSource::Bundled ? tr("Bundled with Kinoklip in")
                                                                 : tr("Installed in"),
                    esc(p->dir));
    if (!p->valid) {
        html += QStringLiteral("<p><b>%1</b></p><ul>")
                    .arg(tr("This plugin has problems and is not used:"));
        for (const plugins::Problem& problem : p->problems)
            html += QStringLiteral("<li>%1</li>").arg(esc(problem.text()));
        html += QStringLiteral("</ul>");
        return html;
    }
    const plugins::PluginManifest& m = p->manifest;
    QString contents;
    for (const auto& e : m.effects)
        contents += itemLine(tr("Effect: ") + e.name, e.fullId);
    for (const auto& t : m.transitions)
        contents += itemLine(tr("Transition: ") + t.name, t.fullId);
    for (const auto& pr : m.presets)
        contents += itemLine(tr("Preset: ") + pr.name, pr.fullId);
    for (const auto& s : m.scripts)
        contents += itemLine(tr("Script: ") + s.name, s.fullId);
    html += QStringLiteral("<p><b>%1</b></p><ul>%2</ul>").arg(tr("Contains"), contents);
    // Compile errors found while rendering (the problem the author needs to see).
    QString errors;
    for (const auto& e : m.effects) {
        if (const QString err = registry.shaderError(e.fullId); !err.isEmpty())
            errors += QStringLiteral("<li>%1: <pre>%2</pre></li>").arg(esc(e.name), esc(err));
    }
    for (const auto& t : m.transitions) {
        if (const QString err = registry.shaderError(t.fullId); !err.isEmpty())
            errors += QStringLiteral("<li>%1: <pre>%2</pre></li>").arg(esc(t.name), esc(err));
    }
    if (!errors.isEmpty())
        html += QStringLiteral("<p><b>%1</b></p><ul>%2</ul>")
                    .arg(tr("The shader does not compile:"), errors);
    if (!m.permissions.isEmpty()) {
        QString perms;
        for (const QString& perm : m.permissions)
            perms += QStringLiteral("<li>%1</li>").arg(esc(plugins::permissionText(perm)));
        html += QStringLiteral("<p><b>%1</b></p><ul>%2</ul>").arg(tr("It asks to"), perms);
    }
    if (m.hasScripts())
        html += QStringLiteral("<p>%1</p>")
                    .arg(tr("Scripts are programs (Python 3) that run on this computer when you "
                            "choose them in Workspace > Scripts, with your rights. Enable only "
                            "plugins you trust."));
    return html;
}

PluginsPage::PluginsPage(QWidget* parent)
    : SettingsPage(parent)
{
    setObjectName(QStringLiteral("PluginsPage"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(
        tr("Plugins add effects, transitions, presets and scripts. They show up in the Effects "
           "library and in Workspace > Scripts like the built-in ones. Install a .kkplugin file or "
           "a plugin folder; you can also drop a .kkplugin file onto the window."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("PluginsList"));
    m_list->setMinimumHeight(kListMinHeight);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setTextElideMode(Qt::ElideRight);
    layout->addWidget(m_list, 1);

    m_details = new QTextBrowser;
    m_details->setObjectName(QStringLiteral("PluginDetails"));
    m_details->setOpenLinks(false);
    m_details->setMinimumHeight(kListMinHeight);
    connect(m_details, &QTextBrowser::anchorClicked, this,
        [](const QUrl& url) { platform::openUrl(url); });
    layout->addWidget(m_details, 2);

    auto* buttons = new QHBoxLayout;
    auto* install = new QPushButton(tr("Install…"));
    install->setObjectName(QStringLiteral("PluginInstall"));
    install->setToolTip(tr("Install a .kkplugin file"));
    auto* installFolder = new QPushButton(tr("Install Folder…"));
    installFolder->setObjectName(QStringLiteral("PluginInstallFolder"));
    installFolder->setToolTip(tr("Install a plugin folder (one with a plugin.json)"));
    m_uninstall = new QPushButton(tr("Uninstall"));
    m_uninstall->setObjectName(QStringLiteral("PluginUninstall"));
    auto* reload = new QPushButton(tr("Reload"));
    reload->setObjectName(QStringLiteral("PluginReload"));
    reload->setToolTip(
        tr("Read all plugin folders again: edited shaders and manifests take effect at once"));
    auto* open = new QPushButton(tr("Open Plugin Folder"));
    open->setObjectName(QStringLiteral("PluginOpenFolder"));
    for (QPushButton* b : {install, installFolder, m_uninstall, reload, open})
        buttons->addWidget(b);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(install, &QPushButton::clicked, this, [this] {
        const QString file = platform::getOpenFileName(this, tr("Install Plugin"),
            tr("Kinoklip plugins (*.kkplugin);;All files (*)"),
            QStringLiteral("paths/lastPluginDir"));
        if (!file.isEmpty())
            installWithMessages(file, this);
    });
    connect(installFolder, &QPushButton::clicked, this, [this] {
        const QString dir = platform::getExistingDirectory(
            this, tr("Install Plugin Folder"), QStringLiteral("paths/lastPluginDir"));
        if (!dir.isEmpty())
            installWithMessages(dir, this);
    });
    connect(m_uninstall, &QPushButton::clicked, this, [this] {
        const QString id = currentId();
        if (id.isEmpty())
            return;
        if (QMessageBox::question(this, tr("Uninstall Plugin"),
                tr("Delete the plugin \"%1\"? Projects that use it keep the effect's settings and "
                   "show it as a missing plugin.")
                    .arg(id))
            != QMessageBox::Yes)
            return;
        QString error;
        if (!plugins::PluginRegistry::instance().uninstall(id, &error))
            QMessageBox::warning(this, tr("Uninstall Plugin"), error);
    });
    connect(
        reload, &QPushButton::clicked, this, [] { plugins::PluginRegistry::instance().reload(); });
    connect(open, &QPushButton::clicked, this, [] {
        const QString dir = plugins::PluginRegistry::instance().userDir();
        QDir().mkpath(dir);
        platform::openUrl(QUrl::fromLocalFile(dir));
    });
    connect(m_list, &QListWidget::currentItemChanged, this, [this] { showDetails(); });
    connect(m_list, &QListWidget::itemChanged, this, &PluginsPage::onItemChanged);
    connect(&plugins::PluginRegistry::instance(), &plugins::PluginRegistry::changed, this,
        &PluginsPage::refresh);
    refresh();
}

QString PluginsPage::currentId() const
{
    const QListWidgetItem* item = m_list->currentItem();
    return item ? item->data(kIdRole).toString() : QString();
}

void PluginsPage::refresh()
{
    const QString keep = currentId();
    m_refreshing = true;
    m_list->clear();
    for (const plugins::LoadedPlugin& p : plugins::PluginRegistry::instance().plugins()) {
        QString text = p.valid ? p.manifest.name : p.folderName;
        if (p.valid)
            text += QStringLiteral("  ") + p.manifest.version;
        text += p.source == plugins::PluginSource::Bundled ? tr("  (bundled)") : QString();
        if (!p.valid)
            text += tr("  (problem)");
        auto* item = new QListWidgetItem(text, m_list);
        item->setData(kIdRole, p.id());
        item->setToolTip(text);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable
            | (p.valid ? Qt::ItemIsUserCheckable : Qt::ItemFlags()));
        if (p.valid)
            item->setCheckState(p.enabled ? Qt::Checked : Qt::Unchecked);
        if (p.id() == keep)
            m_list->setCurrentItem(item);
    }
    if (!m_list->currentItem() && m_list->count() > 0)
        m_list->setCurrentRow(0);
    m_refreshing = false;
    showDetails();
}

void PluginsPage::showDetails()
{
    const QString id = currentId();
    m_details->setHtml(id.isEmpty() ? QStringLiteral("<p>%1</p>").arg(tr("No plugins installed."))
                                    : detailsHtml(id));
    const plugins::LoadedPlugin* p = plugins::PluginRegistry::instance().find(id);
    m_uninstall->setEnabled(p && p->source == plugins::PluginSource::User);
}

void PluginsPage::onItemChanged(QListWidgetItem* item)
{
    if (m_refreshing)
        return;
    plugins::PluginRegistry& registry = plugins::PluginRegistry::instance();
    const QString id = item->data(kIdRole).toString();
    const plugins::LoadedPlugin* p = registry.find(id);
    if (!p)
        return;
    const bool on = item->checkState() == Qt::Checked;
    if (on && p->valid && p->manifest.hasScripts()) {
        QStringList perms;
        for (const QString& perm : p->manifest.permissions)
            perms << QStringLiteral("- ") + plugins::permissionText(perm);
        const QString message
            = tr("\"%1\" contains scripts: programs that run on this computer "
                 "with your rights when you choose them in Workspace > "
                 "Scripts.\n\nIt asks to:\n%2\n\nOnly enable plugins from "
                 "people you trust.")
                  .arg(p->manifest.name,
                      perms.isEmpty() ? tr("- (nothing declared)") : perms.join(QLatin1Char('\n')));
        if (QMessageBox::question(this, tr("Enable Scripts"), message) != QMessageBox::Yes) {
            m_refreshing = true;
            item->setCheckState(Qt::Unchecked);
            m_refreshing = false;
            return;
        }
    }
    registry.setEnabled(id, on);
}

bool PluginsPage::installWithMessages(const QString& path, QWidget* parent)
{
    QString error;
    QString id;
    if (!plugins::PluginRegistry::instance().install(path, &error, &id)) {
        QMessageBox::warning(parent, tr("Install Plugin"),
            tr("The plugin could not be installed.\n\n%1").arg(error));
        return false;
    }
    const plugins::LoadedPlugin* p = plugins::PluginRegistry::instance().find(id);
    QString note = tr("Installed \"%1\".").arg(p ? p->manifest.name : id);
    if (p && p->manifest.hasScripts() && !p->enabled)
        note += QLatin1Char('\n')
            + tr("It contains scripts, so it is switched off until you enable it in Preferences > "
                 "Plugins.");
    QMessageBox::information(parent, tr("Install Plugin"), note);
    return true;
}

void registerPluginsPage(SettingsPageRegistry& pages)
{
    SettingsPageInfo page {QStringLiteral("plugins"), tr("Plugins"),
        QStringLiteral("preferences-plugin"), 70,
        [](QWidget* parent) { return new PluginsPage(parent); }, QStringLiteral("user"), QString()};
    pages.add(std::move(page));
}

} // namespace kk
