// SPDX-License-Identifier: GPL-3.0-or-later
// AudioPluginRegistry — the LV2 and CLAP side of the plugin system (docs/plugins/audio-plugins.md,
// spec 159), built like OfxRegistry: finds *.lv2 folders and *.clap files, asks the probe program
// (kinoklip-audio-probe, one process per bundle, up to three at once) what each contains, keeps the
// answers in a cache, and registers every audio effect plugin as an EffectRegistry descriptor
// (category AudioEffect, id "audio_lv2:<URI>" / "audio_clap:<id>"). A bundle whose probe crashes,
// hangs or fails goes on the bad-plugin list and is never loaded into the editor; Retry tries
// again. Scanning runs on background threads (loadAsync): the effects library updates when it is
// done. The processing side is dsp::PluginClipEffect in render/AudioClipEffects.cpp.
#pragma once

#include "audioplug/AudioPluginInfo.h"
#include "core/EffectRegistry.h"

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

inline constexpr char kLv2IdPrefix[] = "audio_lv2:";
inline constexpr char kClapIdPrefix[] = "audio_clap:";
/// EffectDescriptor::plugin of the effects of each format (the "plugin" that goes missing as a whole).
inline constexpr char kLv2PluginTag[] = "lv2";
inline constexpr char kClapPluginTag[] = "clap";

/// One audio plugin as the host lists it.
struct AudioPluginEntry
{
    QString fullId; ///< "audio_lv2:<uri>" / "audio_clap:<id>"
    audioplug::PluginInfo info;
    QString bundlePath;
    QString source; ///< "bundled", "user", "environment", "system"
};

/// One bundle (an .lv2 folder or a .clap file) as the scan found it.
struct AudioBundleEntry
{
    QString path;
    QString format;
    QString source;
    qint64 stampSize = 0; ///< size and newest date of the files: what a changed plugin looks like
    qint64 stampMtime = 0;
    bool bad = false; ///< on the bad-plugin list: not loaded
    QString reason; ///< why it is bad
    bool probing = false; ///< the probe has not answered yet
    audioplug::BundleInfo info;
    QDateTime probed;
};

class AudioPluginRegistry : public QObject
{
    Q_OBJECT
public:
    static AudioPluginRegistry& instance();

    /// Folders scanned for bundles of @p format ("lv2" / "clap"), in priority order (the first
    /// plugin with an identifier wins).
    [[nodiscard]] QStringList searchDirs(const QString& format) const;
    /// Tests: replaces the folders of a format, the probe program, its time limit and the cache file.
    void setSearchDirs(const QString& format, const QStringList& dirs);
    void setProbeProgram(const QString& path) { m_probeProgram = path; }
    void setProbeTimeoutMs(int ms) { m_probeTimeoutMs = ms; }
    void setCacheFile(const QString& path) { m_cacheFile = path; }
    /// Probes that run at once (default 3; 1 to 4).
    void setProbeThreads(int n);
    /// The user folder for bundles of @p format (~/.local/share/Kinoklip/lv2 or .../clap;
    /// KK_LV2_USER_DIR / KK_CLAP_USER_DIR override).
    [[nodiscard]] static QString userDir(const QString& format);

    /// This format on / off as a whole (Preferences > Plugins; default on).
    [[nodiscard]] bool enabled(const QString& format) const;
    void setEnabled(const QString& format, bool on);

    /// Scans and waits: reads the cache, probes new or changed bundles (in parallel). Later
    /// calls do nothing. For the command line and the tests.
    void load();
    /// Starts the same scan in the background and returns at once: the plugins of the cache
    /// are registered immediately, the others when their probe is done (changed() fires after
    /// each stage; scanFinished() at the end).
    void loadAsync();
    [[nodiscard]] bool isLoaded() const { return m_loaded; }
    /// True while probes are running.
    [[nodiscard]] bool isScanning() const { return m_scanning; }
    /// Blocks (with the event loop running) until no scan is running. For tests and the CLI.
    void waitForScan();
    /// Tests: forgets the scan so the next load() reads the cache file again.
    void resetForTesting();
    /// Scans again (blocking / in the background); with @p retryBad the bad bundles are probed again.
    void reload(bool retryBad = false);
    void reloadAsync(bool retryBad = false);
    /// Probes the bundle @p path again (Retry): clears its bad mark first.
    void retry(const QString& path);
    void retryAsync(const QString& path);

    [[nodiscard]] QVector<AudioBundleEntry> bundles() const;
    [[nodiscard]] const AudioBundleEntry* bundle(const QString& path) const;
    /// Every plugin (including the ones Kinoklip cannot run, with their reasons).
    [[nodiscard]] QVector<AudioPluginEntry> allPlugins() const;
    /// The plugin of descriptor id @p fullId, or null. Safe from any thread.
    [[nodiscard]] std::shared_ptr<const AudioPluginEntry> plugin(const QString& fullId) const;
    /// Counts up when the set of plugins changed; the audio effects look for their plugin again.
    [[nodiscard]] quint64 generation() const { return m_generation.load(); }
    /// Hash of what the plugins are (paths, sizes, versions).
    [[nodiscard]] QByteArray contentKey() const;
    /// True if @p type (an effect id) is an LV2 / CLAP effect id.
    [[nodiscard]] static bool isPluginType(const QString& type);

    /// A plugin of @p bundlePath is about to be loaded into this process for the first time (or
    /// came through its first block): a crash in between puts the bundle on the bad list at the
    /// next start. Called by the audio effect.
    void markLoading(const QString& bundlePath);
    void markLoaded(const QString& bundlePath);
    /// The audio effect's own failures (a plugin that would not start) for the Plugins page.
    void reportError(const QString& fullId, const QString& message);
    [[nodiscard]] QString error(const QString& fullId) const;

    /// Descriptor parameters of @p info (used by the registry and the tests).
    static QVector<ParamDescriptor> descriptorParams(const audioplug::PluginInfo& info);
    /// The value a parameter of @p info has in @p set, in the plugin's units (the default when
    /// the set does not hold it); never outside the plugin's range.
    [[nodiscard]] static double valueFor(const audioplug::ParamInfo& p, const ParamSet& set);
    /// Choice labels of an enumeration parameter, unique.
    [[nodiscard]] static QStringList choiceLabels(const audioplug::ParamInfo& p);

signals:
    /// Plugins, bundles or the bad list changed.
    void changed();
    /// A background scan has finished.
    void scanFinished();

private:
    AudioPluginRegistry();
    struct Job;
    using Jobs = std::shared_ptr<QVector<Job>>;
    /// Finds the bundles and decides which need a probe; fills m_bundles. Returns the jobs.
    Jobs discover(bool retryBad, const QString& onlyBundle);
    void finishScan(const Jobs& jobs);
    void startScan(bool retryBad, const QString& onlyBundle, bool async);
    void registerAll();
    void loadCache();
    void saveCache() const;
    void applyCrashMarkers();
    [[nodiscard]] QString markerDir() const;
    [[nodiscard]] QString cacheFilePath() const;
    audioplug::BundleInfo probe(const QString& format, const QString& path, QString* failure) const;

    QStringList m_dirsLv2, m_dirsClap;
    bool m_overridden[2] = {false, false};
    QString m_probeProgram;
    int m_probeTimeoutMs = 60000;
    QString m_cacheFile;
    bool m_loaded = false;
    std::atomic<bool> m_scanning {false};
    bool m_rescanPending = false;
    bool m_rescanRetry = false;
    QString m_rescanOnly;
    QVector<AudioBundleEntry> m_bundles;
    QThreadPool m_pool;

    mutable QMutex m_mutex; ///< guards m_plugins, m_all, m_errors, m_contentKey
    QHash<QString, std::shared_ptr<const AudioPluginEntry>> m_plugins;
    QVector<AudioPluginEntry> m_all;
    QHash<QString, QString> m_errors;
    QByteArray m_contentKey;
    std::atomic<quint64> m_generation {1};
};

} // namespace kk::plugins
