// SPDX-License-Identifier: GPL-3.0-or-later
// PluginRegistry — finds, validates, installs and registers the plugins (docs/plugins/design.md):
// folders in the bundled and the user plugin directory, each with a plugin.json. Enabled plugins
// put their effects and transitions into the EffectRegistry (so the Effects Library, the
// Inspector, keyframes, undo, project files and the renderer treat them like built-ins), their
// presets into the preset list and their scripts into Workspace > Scripts.
#pragma once

#include "plugins/PluginManifest.h"

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

namespace kk::plugins {

enum class PluginSource {
    Bundled,
    User
};

/// One plugin as found on disk.
struct LoadedPlugin
{
    PluginManifest manifest; ///< empty when the manifest is invalid
    QString dir; ///< absolute folder
    QString folderName; ///< name of the folder (the id when the manifest is invalid)
    PluginSource source = PluginSource::User;
    bool valid = false; ///< manifest and files are fine
    bool enabled = false; ///< what the person chose (or the default)
    /// Manifest and file problems; shader compile errors found while rendering are added by
    /// PluginRegistry::reportShaderError().
    QVector<Problem> problems;

    [[nodiscard]] QString id() const { return valid ? manifest.id : folderName; }
    /// True when the plugin is registered (valid and enabled).
    [[nodiscard]] bool active() const { return valid && enabled; }
};

/// What the renderer needs of one shader effect or transition; immutable once published.
struct PluginShaderSpec
{
    QString fullId; ///< "plugin:<plugin id>/<item id>"
    QString pluginId;
    bool transition = false;
    QString entry; ///< effects: "main" or "color"
    QVector<PluginParam> params;
    QString source; ///< the author's GLSL
    QString file; ///< file name, for messages
    double paddingPx = 0.0;
};

/// A preset or script of an enabled plugin, for the Effects Library and the Scripts menu.
struct PresetEntry
{
    PluginPresetDef def;
    QString pluginId;
    QString pluginName;
};
struct ScriptEntry
{
    PluginScriptDef def;
    QString pluginId;
    QString pluginName;
    QString dir; ///< the plugin folder (the script runs with it as working folder)
};

/// Thread-safety: GUI thread for everything that changes the registry (load, reload, install,
/// ...); shaderSpec(), generation() and reportShaderError() may be called from any thread
/// (the render thread asks for shaders).
class PluginRegistry : public QObject
{
    Q_OBJECT
public:
    static PluginRegistry& instance();

    /// Directories. The user directory is <AppData>/plugins (~/.local/share/Kinoklip/plugins),
    /// KK_PLUGIN_USER_DIR overrides it; the bundled one is KK_PLUGIN_BUNDLED_DIR or next to
    /// the program (share/kinoklip/plugins). Tests use setDirectories().
    [[nodiscard]] QString userDir() const;
    [[nodiscard]] QString bundledDir() const;
    /// The folder of the example plugins (templates to copy), or empty.
    [[nodiscard]] static QString examplesDir();
    /// Replaces both directories (tests); empty strings restore the defaults.
    void setDirectories(const QString& bundled, const QString& user);

    /// Scans the directories once; later calls do nothing. Call after the application name is
    /// set and before projects load.
    void load();
    [[nodiscard]] bool isLoaded() const { return m_loaded; }
    /// Scans again (new folders, changed manifests and shaders), re-registers everything and
    /// makes the renderer rebuild plugin shaders. Emits changed().
    void reload();

    [[nodiscard]] const QVector<LoadedPlugin>& plugins() const { return m_plugins; }
    [[nodiscard]] const LoadedPlugin* find(const QString& pluginId) const;

    /// Installs the plugin folder or .kkplugin file @p path into the user directory (replacing
    /// an installed one of the same id). Never runs anything of the plugin. Returns false with
    /// @p error (all problems, one per line). @p installedId gets the plugin id.
    bool install(const QString& path, QString* error, QString* installedId = nullptr);
    /// Deletes the user plugin @p pluginId (a bundled one cannot be removed).
    bool uninstall(const QString& pluginId, QString* error);
    /// Enables or disables a plugin (stored in the settings). Returns false for an unknown id.
    bool setEnabled(const QString& pluginId, bool enabled);
    /// Packs the plugin @p pluginId into the .kkplugin file @p zipPath.
    bool exportPackage(const QString& pluginId, const QString& zipPath, QString* error) const;

    /// The shader of effect or transition @p fullId, or null (unknown, disabled, broken).
    [[nodiscard]] std::shared_ptr<const PluginShaderSpec> shaderSpec(const QString& fullId) const;
    /// Counts up whenever shaders may have changed; the renderer drops its compiled plugin
    /// shaders when it sees a new number.
    [[nodiscard]] quint64 generation() const { return m_generation.load(); }
    /// A hash of the shader code and parameters of every enabled plugin item: part of the render
    /// cache key, so editing a shader invalidates cached chunks that used it.
    [[nodiscard]] QByteArray contentKey() const;
    /// Remembers a shader compile problem of @p fullId (shown in the Plugins dialog, returned by
    /// plugin.info). An empty @p message clears it.
    void reportShaderError(const QString& fullId, const QString& message);
    /// The last reported compile problem of @p fullId, or empty.
    [[nodiscard]] QString shaderError(const QString& fullId) const;

    /// Presets and scripts of the enabled plugins.
    [[nodiscard]] QVector<PresetEntry> presets() const;
    [[nodiscard]] QVector<ScriptEntry> scripts() const;
    /// The recipe JSON of preset @p fullId; an empty object with @p error when unreadable.
    [[nodiscard]] QJsonObject presetRecipe(const QString& fullId, QString* error) const;
    /// The preset with @p fullId of an enabled plugin, or null.
    [[nodiscard]] const PluginPresetDef* findPreset(const QString& fullId) const;

    /// Checks that every file the manifest names exists and is readable and that recipe files
    /// are JSON; appends problems.
    static void checkFiles(
        const QString& dir, const PluginManifest& manifest, QVector<Problem>* problems);

signals:
    /// The set of plugins, their enabled state or their content changed.
    void changed();

private:
    PluginRegistry() = default;
    void scanDirectory(const QString& dir, PluginSource source);
    void registerAll();
    [[nodiscard]] bool storedEnabled(const LoadedPlugin& plugin) const;

    QString m_bundledOverride;
    QString m_userOverride;
    bool m_overridden = false;
    bool m_loaded = false;
    QVector<LoadedPlugin> m_plugins;
    QStringList m_registeredPlugins; ///< ids whose descriptors were registered at some time

    mutable QMutex m_mutex; ///< guards m_specs and m_shaderErrors
    QHash<QString, std::shared_ptr<const PluginShaderSpec>> m_specs;
    QHash<QString, QString> m_shaderErrors;
    QByteArray m_contentKey;
    std::atomic<quint64> m_generation {1};
};

} // namespace kk::plugins
