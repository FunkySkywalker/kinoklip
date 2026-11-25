// SPDX-License-Identifier: GPL-3.0-or-later
// OfxRegistry — the OpenFX side of the plugin system (docs/plugins/openfx.md, spec 158): finds
// *.ofx.bundle folders (OFX_PLUGIN_PATH, /usr/OFX/Plugins, ~/.OFX/Plugins, the user and bundled
// folders of Kinoklip), asks the probe program (kinoklip-ofx-probe, one process per bundle) what
// each contains, keeps the answers in a cache, and registers every image effect plugin as an
// EffectRegistry descriptor with the id "ofx:<plugin identifier>". A bundle whose probe crashes,
// hangs or fails goes on the bad-plugin list and is never loaded into the editor; Retry tries
// again. The renderer side is render/GlOfxEffect.*, which runs ofx::Instance on the CPU.
#pragma once

#include "core/EffectRegistry.h"
#include "ofx/OfxHost.h"

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <memory>

namespace kk::plugins {

inline constexpr char kOfxIdPrefix[] = "ofx:";
/// EffectDescriptor::plugin of every OpenFX effect (the "plugin" that goes missing as a whole).
inline constexpr char kOfxPluginTag[] = "ofx";

/// How a Kinoklip parameter maps to OFX parameters of one effect.
struct OfxParamMap
{
    ofx::ParamInfo info;
    QStringList keys; ///< ParamSet keys: one, or one per dimension of a multi-value number
    bool color = false; ///< RGB / RGBA: one Color key
    bool spatial = false; ///< XY / X / Y: stored as a fraction of the clip size
};

/// One OpenFX plugin as the host lists it.
struct OfxPluginEntry
{
    QString fullId; ///< "ofx:<identifier>"
    ofx::PluginInfo info;
    QString bundlePath;
    QString binary;
    QString source; ///< "bundled", "user", "OFX_PLUGIN_PATH", "system"
    QVector<OfxParamMap> params; ///< value parameters in display order
    [[nodiscard]] ofx::BundleInfo bundleInfo() const; ///< what ofx::Instance::create() needs
};

/// One bundle folder as the scan found it.
struct OfxBundleEntry
{
    QString path;
    QString source;
    QString binary;
    qint64 binarySize = 0;
    qint64 binaryMtime = 0;
    bool bad = false; ///< on the bad-plugin list: not loaded
    QString reason; ///< why it is bad
    ofx::BundleInfo info;
    QDateTime probed;
    bool probing = false; ///< the probe has not answered yet (a background scan is running)
};

class OfxRegistry : public QObject
{
    Q_OBJECT
public:
    static OfxRegistry& instance();

    /// Folders scanned for bundles, in priority order (the first plugin with an identifier wins).
    [[nodiscard]] QStringList searchDirs() const;
    /// Tests: replaces the folders, the probe program, its time limit and the cache file.
    void setSearchDirs(const QStringList& dirs);
    void setProbeProgram(const QString& path) { m_probeProgram = path; }
    void setProbeTimeoutMs(int ms) { m_probeTimeoutMs = ms; }
    void setCacheFile(const QString& path) { m_cacheFile = path; }
    /// The user folder for bundles (~/.local/share/Kinoklip/ofx, KK_OFX_USER_DIR overrides).
    [[nodiscard]] static QString userDir();

    /// OpenFX plugins on / off as a whole (Preferences > Plugins; default on).
    [[nodiscard]] bool enabled() const;
    void setEnabled(bool on);

    /// Scans once and waits (reads the cache, probes new or changed bundles, up to three probes at
    /// once). Later calls do nothing. For the command line and the tests.
    void load();
    /// The same scan in the background: returns at once, the plugins of the cache are registered
    /// immediately, the others when their probe is done (changed() after each stage, scanFinished()
    /// at the end). The editor starts with this one, so no probe delays the first window.
    void loadAsync();
    [[nodiscard]] bool isLoaded() const { return m_loaded; }
    /// True while probes are running in the background.
    [[nodiscard]] bool isScanning() const { return m_scanning; }
    /// Probes that run at once (1 to 4, default 3).
    void setProbeThreads(int n);
    /// Waits (with the event loop running) until no background scan is running.
    void waitForScan();
    /// Tests: forgets the scan so the next load() reads the cache file and the loading marks again.
    void resetForTesting()
    {
        waitForScan();
        m_loaded = false;
        m_bundles.clear();
    }
    /// Scans again; with @p retryBad the bundles on the bad-plugin list are probed again.
    void reload(bool retryBad = false);
    void reloadAsync(bool retryBad = false);
    /// Probes the bundle @p path again (Retry): clears its bad mark first.
    void retry(const QString& path);
    void retryAsync(const QString& path);

    [[nodiscard]] QVector<OfxBundleEntry> bundles() const;
    [[nodiscard]] const OfxBundleEntry* bundle(const QString& path) const;
    /// Every plugin (including the ones Kinoklip cannot run, with their reasons).
    [[nodiscard]] QVector<OfxPluginEntry> allPlugins() const;
    /// The plugin of descriptor id @p fullId ("ofx:..."), or null. Safe from any thread.
    [[nodiscard]] std::shared_ptr<const OfxPluginEntry> plugin(const QString& fullId) const;
    /// Counts up when the set of plugins changed; the renderer drops its instances.
    [[nodiscard]] quint64 generation() const { return m_generation.load(); }
    /// Hash of what the plugins are (paths, sizes, versions): part of the render cache keys.
    [[nodiscard]] QByteArray contentKey() const;

    /// A plugin of @p bundlePath is about to be loaded into this process for the first time (or
    /// came through its first render): a crash in between puts the bundle on the bad list at the
    /// next start. Called by the renderer.
    void markLoading(const QString& bundlePath);
    void markLoaded(const QString& bundlePath);
    /// The renderer's own failures (a render that failed) for the Plugins page.
    void reportRenderError(const QString& fullId, const QString& message);
    [[nodiscard]] QString renderError(const QString& fullId) const;

    /// Descriptor parameters and the mapping for the parameters of @p context (used by the
    /// registry and the tests).
    static void mapParams(const ofx::ContextInfo& context, QVector<ParamDescriptor>* descriptors,
        QVector<OfxParamMap>* maps);
    /// What the plugin sees for @p map at @p frame: the stored values clamped, spatial values
    /// scaled by the clip size.
    [[nodiscard]] static ofx::ParamValue valueFor(
        const OfxParamMap& map, const ParamSet& set, double frame, double widthPx, double heightPx);

signals:
    /// Plugins, bundles or the bad list changed.
    void changed();
    /// A background scan has finished.
    void scanFinished();

private:
    OfxRegistry();
    struct Job;
    using Jobs = std::shared_ptr<QVector<Job>>;
    void scan(bool retryBad, const QString& onlyBundle, bool async);
    Jobs discover(bool retryBad, const QString& onlyBundle);
    void finishScan(const Jobs& jobs);
    void registerAll();
    void loadCache();
    void saveCache() const;
    void applyCrashMarkers();
    [[nodiscard]] QString markerDir() const;
    [[nodiscard]] QString cacheFilePath() const;
    ofx::BundleInfo probe(const QString& bundlePath, QString* failure) const;

    QStringList m_dirsOverride;
    bool m_overridden = false;
    QString m_probeProgram;
    int m_probeTimeoutMs = 30000;
    QString m_cacheFile;
    bool m_loaded = false;
    std::atomic<bool> m_scanning {false};
    bool m_rescanPending = false;
    bool m_rescanRetry = false;
    QString m_rescanOnly;
    QThreadPool m_pool;
    QVector<OfxBundleEntry> m_bundles;
    QStringList m_registeredIds;

    mutable QMutex m_mutex; ///< guards m_plugins, m_errors, m_contentKey
    QHash<QString, std::shared_ptr<const OfxPluginEntry>> m_plugins;
    QVector<OfxPluginEntry> m_all;
    QHash<QString, QString> m_errors;
    QByteArray m_contentKey;
    std::atomic<quint64> m_generation {1};
};

} // namespace kk::plugins
