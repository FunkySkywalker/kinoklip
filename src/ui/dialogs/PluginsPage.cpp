// SPDX-License-Identifier: GPL-3.0-or-later
// PluginsPage — see PluginsPage.h.
#include "ui/dialogs/PluginsPage.h"

#include "platform/FileDialogs.h"
#include "platform/OpenUrl.h"
#include "plugins/OfxRegistry.h"
#include "plugins/PluginRegistry.h"
#include "render/PluginValidate.h"

#include <QCheckBox>
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
/// Id prefix of the list entries that are OpenFX bundle folders (the rest of the id is the path).
constexpr char kOfxBundlePrefix[] = "ofxbundle:";
/// Where the free plugin packs and how to install them are explained.
constexpr char kOfxHelpUrl[] = "https://github.com/SeniorMatt/kinoklip/blob/main/docs/plugins/openfx.md";
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

QString PluginsPage::ofxDetailsHtml(const QString& bundlePath)
{
    const plugins::OfxBundleEntry* b = plugins::OfxRegistry::instance().bundle(bundlePath);
    if (!b)
        return {};
    QString html = QStringLiteral("<h3>%1 <small>(OpenFX)</small></h3>")
                       .arg(esc(QFileInfo(b->path).fileName()));
    html += QStringLiteral("<p>%1 <a href=\"file://%2\">%2</a><br>%3 %4</p>")
                .arg(tr("Found in"), esc(QFileInfo(b->path).absolutePath()), tr("Source:"),
                    esc(b->source));
    if (b->bad) {
        html += QStringLiteral("<p><b>%1</b></p><p>%2</p><p>%3</p>")
                    .arg(tr("This is a bad plugin: Kinoklip does not load it."), esc(b->reason),
                        tr("Press Retry to try it again, for example after updating the plugin."));
        return html;
    }
    QString rows;
    for (const ofx::PluginInfo& p : b->info.plugins) {
        const QString id = QString::fromLatin1(plugins::kOfxIdPrefix) + p.identifier;
        QString status = p.error.isEmpty() ? tr("in the Effects library, folder OpenFX")
                                           : tr("not used: ") + p.error;
        if (const QString err = plugins::OfxRegistry::instance().renderError(id); !err.isEmpty())
            status += QStringLiteral(" - ") + tr("last problem: ") + err;
        rows += QStringLiteral(
                    "<tr><td>%1</td><td><small>%2</small></td><td>%3.%4</td><td>%5</td></tr>")
                    .arg(esc(p.label), esc(p.identifier))
                    .arg(p.major)
                    .arg(p.minor)
                    .arg(esc(status));
    }
    html += QStringLiteral("<p><b>%1</b></p><table cellspacing=\"6\">%2</table>")
                .arg(tr("Plugins in this bundle"), rows);
    return html;
}

QString PluginsPage::checkHtml(const QString& dir)
{
    const render::PluginValidation v = render::validatePluginFolder(dir, true);
    QString html = QStringLiteral("<hr><p><b>%1</b></p>").arg(tr("Check"));
    QStringList lines;
    for (const plugins::Problem& p : v.problems)
        lines << QStringLiteral("<li>%1</li>").arg(esc(p.text()));
    for (const render::ValidatedItem& item : v.items)
        lines << QStringLiteral("<li>%1: OpenGL 3.3 %2, OpenGL ES 3.0 %3</li>")
                     .arg(esc(item.fullId), esc(item.desktop), esc(item.gles));
    if (v.problems.isEmpty())
        html += QStringLiteral("<p>%1</p>").arg(tr("No problems found."));
    html += QStringLiteral("<ul>%1</ul>").arg(lines.join(QString()));
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
    m_ofxEnabled = new QCheckBox(tr("Use OpenFX plugins (effects of other programs, such as the "
                                    "free openfx-misc pack)"));
    m_ofxEnabled->setObjectName(QStringLiteral("OfxEnabled"));
    m_ofxEnabled->setChecked(plugins::OfxRegistry::instance().enabled());
    layout->addWidget(m_ofxEnabled);
    connect(m_ofxEnabled, &QCheckBox::toggled, this,
        [](bool on) { plugins::OfxRegistry::instance().setEnabled(on); });

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
    auto* check = new QPushButton(tr("Check"));
    check->setObjectName(QStringLiteral("PluginCheck"));
    check->setToolTip(tr("Compile the shaders of the selected plugin for OpenGL 3.3 and OpenGL ES "
                         "3.0 and list every problem with its line"));
    auto* open = new QPushButton(tr("Open Plugin Folder"));
    open->setObjectName(QStringLiteral("PluginOpenFolder"));
    m_retry = new QPushButton(tr("Retry"));
    m_retry->setObjectName(QStringLiteral("OfxRetry"));
    m_retry->setToolTip(tr("Try the selected OpenFX bad plugin again"));
    auto* openOfx = new QPushButton(tr("Open OpenFX Folder"));
    openOfx->setObjectName(QStringLiteral("OfxOpenFolder"));
    openOfx->setToolTip(tr("The folder for OpenFX bundles (*.ofx.bundle)"));
    auto* packs = new QPushButton(tr("Get Free Plugin Packs"));
    packs->setObjectName(QStringLiteral("OfxGetPacks"));
    packs->setToolTip(tr("Which free OpenFX packs work with Kinoklip and how to install them"));
    for (QPushButton* b :
        {install, installFolder, m_uninstall, reload, check, open, m_retry, openOfx, packs})
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
    connect(reload, &QPushButton::clicked, this, [] {
        plugins::PluginRegistry::instance().reload();
        plugins::OfxRegistry::instance().reload(false);
    });
    connect(m_retry, &QPushButton::clicked, this, [this] {
        const QString id = currentId();
        if (id.startsWith(QLatin1String(kOfxBundlePrefix)))
            plugins::OfxRegistry::instance().retry(id.mid(int(sizeof(kOfxBundlePrefix)) - 1));
    });
    connect(openOfx, &QPushButton::clicked, this, [] {
        const QString dir = plugins::OfxRegistry::userDir();
        QDir().mkpath(dir);
        platform::openUrl(QUrl::fromLocalFile(dir));
    });
    connect(packs, &QPushButton::clicked, this,
        [] { platform::openUrl(QUrl(QString::fromLatin1(kOfxHelpUrl))); });
    connect(check, &QPushButton::clicked, this, [this] {
        const plugins::LoadedPlugin* p = plugins::PluginRegistry::instance().find(currentId());
        if (!p)
            return;
        m_checkedId = p->id();
        m_checkHtml = checkHtml(p->dir);
        showDetails();
    });
    connect(open, &QPushButton::clicked, this, [] {
        const QString dir = plugins::PluginRegistry::instance().userDir();
        QDir().mkpath(dir);
        platform::openUrl(QUrl::fromLocalFile(dir));
    });
    connect(m_list, &QListWidget::currentItemChanged, this, [this] { showDetails(); });
    connect(m_list, &QListWidget::itemChanged, this, &PluginsPage::onItemChanged);
    connect(&plugins::PluginRegistry::instance(), &plugins::PluginRegistry::changed, this,
        &PluginsPage::refresh);
    connect(&plugins::OfxRegistry::instance(), &plugins::OfxRegistry::changed, this,
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
    for (const plugins::OfxBundleEntry& b : plugins::OfxRegistry::instance().bundles()) {
        qsizetype usable = 0;
        for (const ofx::PluginInfo& p : b.info.plugins)
            usable += p.error.isEmpty() ? 1 : 0;
        QString text = QFileInfo(b.path).fileName() + tr("  (OpenFX)");
        text += b.bad ? tr("  (bad plugin)") : tr("  %1 effects").arg(usable);
        auto* item = new QListWidgetItem(text, m_list);
        const QString id = QString::fromLatin1(kOfxBundlePrefix) + b.path;
        item->setData(kIdRole, id);
        item->setToolTip(b.path);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        if (id == keep)
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
    const bool isOfx = id.startsWith(QLatin1String(kOfxBundlePrefix));
    const QString ofxPath = isOfx ? id.mid(int(sizeof(kOfxBundlePrefix)) - 1) : QString();
    QString html = id.isEmpty() ? QStringLiteral("<p>%1</p>").arg(tr("No plugins installed."))
        : isOfx                 ? ofxDetailsHtml(ofxPath)
                                : detailsHtml(id);
    if (!id.isEmpty() && id == m_checkedId)
        html += m_checkHtml;
    m_details->setHtml(html);
    const plugins::LoadedPlugin* p = plugins::PluginRegistry::instance().find(id);
    m_uninstall->setEnabled(p && p->source == plugins::PluginSource::User);
    const plugins::OfxBundleEntry* bundle
        = isOfx ? plugins::OfxRegistry::instance().bundle(ofxPath) : nullptr;
    m_retry->setEnabled(bundle && bundle->bad);
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
