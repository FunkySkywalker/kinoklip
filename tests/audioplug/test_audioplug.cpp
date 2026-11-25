// SPDX-License-Identifier: GPL-3.0-or-later
// LV2 and CLAP audio effect plugins (docs/behavior-spec.md section 159): small test plugins
// (tests/audioplug/kk_test_lv2.cpp, kk_test_clap.cpp: a gain with an Invert switch, one that aborts while
// loading, a 32 frame delay that reports its latency) are found through the probe program, their
// parameters become Kinoklip parameters, they filter sound like the reference calculation also with
// a changing parameter, report their latency, are skipped (bypass) when missing, and a crashing
// plugin goes on the bad-plugin list. With the LSP plugins installed a real plugin runs too.
#include "audioplug/AudioPluginHost.h"
#include "core/EffectRegistry.h"
#include "core/Serialization.h"
#include "core/TimelineData.h"
#include "plugins/AudioPluginRegistry.h"
#include "render/AudioClipEffects.h"
#include "ui/dialogs/PluginsPage.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

using namespace kk;
using namespace kk::plugins;
using kk::dsp::ClipEffectChain;

namespace {

std::atomic<bool> gCounting {false};
std::atomic<size_t> gAllocations {0};

const QString kLv2 = QStringLiteral("lv2");
const QString kClap = QStringLiteral("clap");
const QString kLv2Gain = QStringLiteral("audio_lv2:urn:kinoklip:test:gain");
const QString kLv2Delay = QStringLiteral("audio_lv2:urn:kinoklip:test:delay");
const QString kClapGain = QStringLiteral("audio_clap:org.kinoklip.test.gain");
const QString kClapDelay = QStringLiteral("audio_clap:org.kinoklip.test.delay");

using Signal = std::vector<float>; ///< interleaved stereo

Signal noise(int frames, unsigned seed = 3)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-0.5f, 0.5f);
    Signal s(size_t(frames) * 2);
    for (float& v : s)
        v = d(rng);
    return s;
}

EffectInstance make(const QString& type, double gainDb, bool invert = false, const QString& gainKey = QStringLiteral("gain"),
    const QString& invertKey = QStringLiteral("invert"))
{
    EffectInstance e = EffectRegistry::instance().createEffect(type, 7);
    e.params.setBase(gainKey, gainDb);
    e.params.setStatic(invertKey, invert);
    return e;
}

/// The gain and invert keys of a format (LV2: port symbols; CLAP: "p<id>").
struct Keys
{
    QString type, delay, gain, invert;
};

Keys keysOf(const QString& format)
{
    return format == kLv2 ? Keys {kLv2Gain, kLv2Delay, QStringLiteral("gain"), QStringLiteral("invert")}
                          : Keys {kClapGain, kClapDelay, QStringLiteral("p1"), QStringLiteral("p2")};
}

/// Runs @p in through the chain in blocks of @p block frames.
Signal run(ClipEffectChain& chain, const QVector<EffectInstance>& effects, Signal in, int block)
{
    const int frames = int(in.size() / 2);
    for (int off = 0; off < frames; off += block)
        chain.process(effects, in.data() + size_t(off) * 2, std::min(block, frames - off));
    return in;
}

double maxDiff(const Signal& a, const Signal& b)
{
    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        d = std::max(d, double(std::fabs(a[i] - b[i])));
    return d;
}

} // namespace

// counts the allocations of C++ code while a test asks for it (the real-time check)
void* operator new(std::size_t n)
{
    if (gCounting)
        ++gAllocations;
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

class TestAudioPlug : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;

    static QString testDir(const QString& format) { return QStringLiteral(KK_AUDIOPLUG_TEST_DIR) + QLatin1Char('/') + format; }

    void useTestPlugins()
    {
        AudioPluginRegistry& reg = AudioPluginRegistry::instance();
        reg.waitForScan();
        reg.setProbeProgram(QStringLiteral(KK_AUDIO_PROBE_PATH));
        reg.setSearchDirs(kLv2, {testDir(kLv2)});
        reg.setSearchDirs(kClap, {testDir(kClap)});
        const QString cache = m_dir.filePath(QStringLiteral("cache.json"));
        QFile::remove(cache);
        reg.setCacheFile(cache);
        reg.setEnabled(kLv2, true);
        reg.setEnabled(kClap, true);
        reg.reload();
    }

    static bool haveLv2() { return KK_AUDIOPLUG_HAVE_LV2 != 0; }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_audioplug"));
        QSettings().clear();
        QVERIFY(m_dir.isValid());
        AudioPluginRegistry::instance().setProbeTimeoutMs(30000);
        useTestPlugins();
    }

    void init() { AudioPluginRegistry::instance().setProbeProgram(QStringLiteral(KK_AUDIO_PROBE_PATH)); }

    // ---- discovery and parameters ----

    void discoveryListsThePluginsAsAudioEffects_data()
    {
        QTest::addColumn<QString>("format");
        if (haveLv2())
            QTest::newRow("lv2") << kLv2;
        QTest::newRow("clap") << kClap;
    }
    void discoveryListsThePluginsAsAudioEffects()
    {
        QFETCH(QString, format);
        const Keys k = keysOf(format);
        AudioPluginRegistry& reg = AudioPluginRegistry::instance();
        const auto entry = reg.plugin(k.type);
        QVERIFY2(entry, "plugin not found");
        QVERIFY(entry->info.error.isEmpty());
        QCOMPARE(entry->info.name.contains(QStringLiteral("Test")), true);
        QVERIFY(AudioPluginRegistry::isPluginType(k.type));
        const EffectDescriptor* d = EffectRegistry::instance().find(k.type);
        QVERIFY(d);
        QVERIFY(!d->unavailable);
        QCOMPARE(d->category, EffectCategory::AudioEffect);
        QCOMPARE(d->group, format);
        QCOMPARE(d->params.size(), 2);
        const ParamDescriptor* gain = d->param(k.gain);
        const ParamDescriptor* invert = d->param(k.invert);
        QVERIFY(gain && invert);
        QCOMPARE(gain->type, ParamType::Double);
        QCOMPARE(gain->min, -60.0);
        QCOMPARE(gain->max, 24.0);
        QCOMPARE(gain->def, 0.0);
        if (format == kLv2)
            QCOMPARE(gain->unit, QStringLiteral("dB"));
        QCOMPARE(invert->type, ParamType::Bool);
        QVERIFY(EffectRegistry::instance().byCategory(EffectCategory::AudioEffect).contains(d));
        // the default effect carries the defaults
        const EffectInstance fx = EffectRegistry::instance().createEffect(k.type, 1);
        QCOMPARE(fx.params.value(k.gain, 0, -1.0), 0.0);
    }

    void aCrashingPluginIsOnTheBadList_data() { discoveryListsThePluginsAsAudioEffects_data(); }
    void aCrashingPluginIsOnTheBadList()
    {
        QFETCH(QString, format);
        AudioPluginRegistry& reg = AudioPluginRegistry::instance();
        const QString path = format == kLv2 ? testDir(kLv2) + QStringLiteral("/kk-test-crash.lv2")
                                            : testDir(kClap) + QStringLiteral("/kk-test-crash.clap");
        const AudioBundleEntry* b = reg.bundle(path);
        QVERIFY(b);
        QVERIFY(b->bad);
        QVERIFY2(b->reason.contains(QStringLiteral("crashed")), qPrintable(b->reason));
        QVERIFY(!reg.plugin(format == kLv2 ? QStringLiteral("audio_lv2:urn:kinoklip:test:crash")
                                           : QStringLiteral("audio_clap:org.kinoklip.test.crash")));
        // the good plugin next to it is unaffected
        QVERIFY(reg.plugin(keysOf(format).type));
        // the page names the reason and offers Retry
        PluginsPage page;
        auto* list = page.findChild<QListWidget*>(QStringLiteral("PluginsList"));
        auto* details = page.findChild<QTextBrowser*>(QStringLiteral("PluginDetails"));
        auto* retry = page.findChild<QPushButton*>(QStringLiteral("OfxRetry"));
        QVERIFY(list && details && retry);
        QListWidgetItem* bad = nullptr;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->text().contains(QStringLiteral("kk-test-crash")))
                bad = list->item(i);
        }
        QVERIFY(bad);
        QVERIFY(bad->text().contains(QStringLiteral("bad plugin")));
        list->setCurrentItem(bad);
        QVERIFY(retry->isEnabled());
        QVERIFY2(details->toPlainText().contains(QStringLiteral("crashed while loading")), qPrintable(details->toPlainText()));
        retry->click(); // probes in the background
        reg.waitForScan();
        QVERIFY(reg.bundle(path)->bad);
    }

    // ---- sound ----

    void theSoundMatchesTheReferenceInAnyBlockSize_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<int>("block");
        QTest::addColumn<double>("db");
        QTest::addColumn<bool>("invert");
        for (const QString& f : {kLv2, kClap}) {
            if (f == kLv2 && !haveLv2())
                continue;
            QTest::newRow(qPrintable(f + QStringLiteral(" -6 dB 480"))) << f << 480 << -6.0 << false;
            QTest::newRow(qPrintable(f + QStringLiteral(" +12 dB 333"))) << f << 333 << 12.0 << false;
            QTest::newRow(qPrintable(f + QStringLiteral(" invert 4096"))) << f << 4096 << -20.0 << true;
            QTest::newRow(qPrintable(f + QStringLiteral(" 0 dB 1"))) << f << 1 << 0.0 << false;
        }
    }
    void theSoundMatchesTheReferenceInAnyBlockSize()
    {
        QFETCH(QString, format);
        QFETCH(int, block);
        QFETCH(double, db);
        QFETCH(bool, invert);
        const Keys k = keysOf(format);
        const EffectInstance fx = make(k.type, db, invert, k.gain, k.invert);
        const Signal in = noise(block == 1 ? 3000 : 48000);
        ClipEffectChain chain;
        const Signal out = run(chain, {fx}, in, block);
        const double factor = std::pow(10.0, db / 20.0) * (invert ? -1.0 : 1.0);
        Signal reference = in;
        for (float& v : reference)
            v = float(v * factor);
        QVERIFY2(maxDiff(out, reference) < 1e-5, qPrintable(QString::number(maxDiff(out, reference))));
        // the same again after a reset gives the same samples (an export always starts clean)
        chain.reset();
        QCOMPARE(run(chain, {fx}, in, block), out);
    }

    void aChangingParameterFollowsBlockByBlock_data() { discoveryListsThePluginsAsAudioEffects_data(); }
    void aChangingParameterFollowsBlockByBlock()
    {
        // what track automation does: the effect list is handed over with new values for every chunk
        QFETCH(QString, format);
        const Keys k = keysOf(format);
        ClipEffectChain chain;
        const int chunk = 256;
        const Signal in = noise(chunk * 40, 11);
        Signal out = in, reference = in;
        for (int c = 0; c < 40; ++c) {
            const double db = -24.0 + 0.6 * c; // a ramp
            const EffectInstance fx = make(k.type, db, c % 7 == 3, k.gain, k.invert);
            chain.process({fx}, out.data() + size_t(c) * chunk * 2, chunk);
            const double factor = std::pow(10.0, db / 20.0) * (c % 7 == 3 ? -1.0 : 1.0);
            for (size_t i = size_t(c) * chunk * 2; i < size_t(c + 1) * chunk * 2; ++i)
                reference[i] = float(reference[i] * factor);
        }
        QVERIFY2(maxDiff(out, reference) < 1e-5, qPrintable(QString::number(maxDiff(out, reference))));
    }

    void aBypassedEffectLeavesTheSoundAlone_data() { discoveryListsThePluginsAsAudioEffects_data(); }
    void aBypassedEffectLeavesTheSoundAlone()
    {
        QFETCH(QString, format);
        const Keys k = keysOf(format);
        EffectInstance fx = make(k.type, -30.0, false, k.gain, k.invert);
        fx.enabled = false;
        ClipEffectChain chain;
        const Signal in = noise(1000);
        QCOMPARE(run(chain, {fx}, in, 100), in);
        QVERIFY(!ClipEffectChain::hasActive({fx}));
        fx.enabled = true;
        QVERIFY(ClipEffectChain::hasActive({fx}));
    }

    void theLatencyIsReported_data() { discoveryListsThePluginsAsAudioEffects_data(); }
    void theLatencyIsReported()
    {
        QFETCH(QString, format);
        const Keys k = keysOf(format);
        const EffectInstance plain = make(k.type, 0.0, false, k.gain, k.invert);
        const EffectInstance delay = make(k.delay, 0.0, false, k.gain, k.invert);
        ClipEffectChain chain;
        QCOMPARE(chain.latencyFrames({plain}), 0);
        QCOMPARE(chain.latencyFrames({delay}), 32);
        QCOMPARE(chain.latencyFrames({plain, delay, delay}), 64);
        // the delay really delays by what it says: an impulse comes out 32 frames later
        Signal in(2000, 0.0f);
        in[0] = in[1] = 1.0f;
        const Signal out = run(chain, {delay}, in, 128);
        QVERIFY(std::fabs(out[size_t(32) * 2] - 1.0f) < 1e-6);
        QVERIFY(std::fabs(out[size_t(31) * 2]) < 1e-6);
    }

    // ---- missing plugins ----

    void aMissingPluginKeepsItsDataAndPassesTheSoundThrough()
    {
        useTestPlugins();
        AudioPluginRegistry& reg = AudioPluginRegistry::instance();
        const Keys k = keysOf(haveLv2() ? kLv2 : kClap);
        EffectInstance fx = make(k.type, -9.5, true, k.gain, k.invert);
        ClipData clip;
        clip.id = 1;
        clip.kind = ClipKind::Solid;
        clip.duration = 50;
        clip.effects.append(fx);
        const QJsonObject json = serialization::clipToJson(clip);
        // the plugin folders are gone (another computer)
        reg.setSearchDirs(kLv2, {});
        reg.setSearchDirs(kClap, {});
        reg.reload();
        const EffectDescriptor* d = EffectRegistry::instance().find(k.type);
        QVERIFY(!d || d->unavailable);
        QVERIFY(!reg.plugin(k.type));
        const ClipData back = serialization::clipFromJson(json);
        QCOMPARE(back.effects.size(), 1);
        QCOMPARE(back.effects[0].type, k.type);
        QCOMPARE(back.effects[0].params.value(k.gain, 0, 0.0), -9.5);
        QCOMPARE(back.effects[0].params.statics.value(k.invert).toBool(), true);
        QCOMPARE(serialization::clipToJson(back), json);
        // the sound goes through unchanged
        ClipEffectChain chain;
        const Signal in = noise(2000);
        QCOMPARE(run(chain, back.effects, in, 300), in);
        // the plugin comes back: the same effect object starts to work (the chain looks again)
        useTestPlugins();
        QVERIFY(!EffectRegistry::instance().find(k.type)->unavailable);
        Signal out = run(chain, back.effects, in, 300);
        QVERIFY(maxDiff(out, in) > 0.01);
        // switched off as a whole: unavailable again, nothing lost
        reg.setEnabled(haveLv2() ? kLv2 : kClap, false);
        QVERIFY(EffectRegistry::instance().find(k.type)->unavailable);
        QVERIFY(!reg.plugin(k.type));
        QCOMPARE(run(chain, back.effects, in, 300), in);
        reg.setEnabled(haveLv2() ? kLv2 : kClap, true);
        QVERIFY(!EffectRegistry::instance().find(k.type)->unavailable);
    }

    // ---- scanning ----

    void theScanIsCachedAndRetryProbesAgain()
    {
        useTestPlugins();
        AudioPluginRegistry& reg = AudioPluginRegistry::instance();
        const QString crash = testDir(kClap) + QStringLiteral("/kk-test-crash.clap");
        // Without a probe program a reload still knows everything: it comes from the cache.
        reg.setProbeProgram(QStringLiteral("/nonexistent/probe"));
        reg.reload();
        QVERIFY(reg.bundle(crash)->bad);
        QVERIFY(reg.bundle(crash)->reason.contains(QStringLiteral("crashed")));
        QVERIFY(reg.plugin(kClapGain));
        reg.retry(crash);
        QVERIFY(reg.bundle(crash)->bad);
        QVERIFY2(reg.bundle(crash)->reason.contains(QStringLiteral("probe")), qPrintable(reg.bundle(crash)->reason));
        reg.setProbeProgram(QStringLiteral(KK_AUDIO_PROBE_PATH));
        reg.retry(crash);
        QVERIFY(reg.bundle(crash)->reason.contains(QStringLiteral("crashed")));
    }

    void aCrashWhileTheEditorHadThePluginLoadedPutsItOnTheBadList()
    {
        useTestPlugins();
        AudioPluginRegistry& reg = AudioPluginRegistry::instance();
        const QString path = testDir(kClap) + QStringLiteral("/kk-test-gain.clap");
        QVERIFY(!reg.bundle(path)->bad);
        reg.markLoading(path); // the editor started to load it ... and died
        reg.resetForTesting(); // the next start
        reg.load();
        QVERIFY(reg.bundle(path)->bad);
        QVERIFY(reg.bundle(path)->reason.contains(QStringLiteral("crashed")));
        QVERIFY(!reg.plugin(kClapGain));
        reg.retry(path);
        QVERIFY(!reg.bundle(path)->bad);
        QVERIFY(reg.plugin(kClapGain));
        // a plugin that came through its first block leaves no mark
        reg.markLoading(path);
        reg.markLoaded(path);
        reg.resetForTesting();
        reg.load();
        QVERIFY(!reg.bundle(path)->bad);
    }

    void theEditorMarksAPluginUntilItsFirstBlock()
    {
        useTestPlugins();
        const Keys k = keysOf(kClap);
        const QDir markers(QFileInfo(m_dir.filePath(QStringLiteral("cache.json"))).absolutePath()
            + QStringLiteral("/audio-plugin-loading"));
        ClipEffectChain chain;
        Signal in = noise(256);
        chain.process({make(k.type, 0.0, false, k.gain, k.invert)}, in.data(), 256);
        QVERIFY(markers.entryList(QDir::Files).isEmpty()); // marked while loading, cleared after the block
    }

    void aBackgroundScanDoesNotBlockAndFinishesWithTheEffects()
    {
        AudioPluginRegistry& reg = AudioPluginRegistry::instance();
        reg.waitForScan();
        reg.setProbeProgram(QStringLiteral(KK_AUDIO_PROBE_PATH));
        const QString cache = m_dir.filePath(QStringLiteral("cache-async.json"));
        QFile::remove(cache);
        reg.setCacheFile(cache);
        reg.setSearchDirs(kLv2, haveLv2() ? QStringList {testDir(kLv2)} : QStringList {});
        reg.setSearchDirs(kClap, {testDir(kClap)});
        reg.resetForTesting();
        QSignalSpy changed(&reg, &AudioPluginRegistry::changed);
        QSignalSpy finished(&reg, &AudioPluginRegistry::scanFinished);
        reg.loadAsync();
        QVERIFY(reg.isScanning()); // the probes have not answered yet
        QVERIFY(!reg.plugin(kClapGain));
        QVERIFY(reg.bundle(testDir(kClap) + QStringLiteral("/kk-test-gain.clap"))->probing);
        reg.waitForScan();
        QVERIFY(!reg.isScanning());
        QCOMPARE(finished.size(), 1);
        QVERIFY(changed.size() >= 2); // once with the cache, once with the answers
        QVERIFY(reg.plugin(kClapGain));
        // the cache is written: the next scan probes nothing, so a missing probe program does not matter
        reg.setProbeProgram(QStringLiteral("/nonexistent/probe"));
        reg.resetForTesting();
        reg.load();
        QVERIFY(reg.plugin(kClapGain));
        reg.setProbeProgram(QStringLiteral(KK_AUDIO_PROBE_PATH));
    }

    void probesRunInParallel()
    {
        // six "plugins" whose probe takes a second: three at a time need about two seconds, one by one six
        AudioPluginRegistry& reg = AudioPluginRegistry::instance();
        reg.waitForScan();
        const QString dir = m_dir.filePath(QStringLiteral("slow"));
        QDir().mkpath(dir);
        for (int i = 0; i < 6; ++i) {
            QFile f(dir + QStringLiteral("/slow%1.clap").arg(i));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
        const QString script = m_dir.filePath(QStringLiteral("slow-probe.sh"));
        {
            QFile f(script);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\nsleep 1\necho '{\"plugins\":[]}' > \"$3\"\n");
            f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
        }
        reg.setProbeProgram(script);
        reg.setSearchDirs(kLv2, {});
        reg.setSearchDirs(kClap, {dir});
        reg.setCacheFile(m_dir.filePath(QStringLiteral("cache-slow.json")));
        QFile::remove(m_dir.filePath(QStringLiteral("cache-slow.json")));
        reg.resetForTesting();
        reg.setProbeThreads(3);
        QElapsedTimer t;
        t.start();
        reg.load();
        const qint64 ms = t.elapsed();
        QCOMPARE(reg.bundles().size(), 6);
        QVERIFY2(ms < 4500, qPrintable(QString::number(ms)));
        QVERIFY2(ms >= 1900, qPrintable(QString::number(ms)));
        reg.setProbeThreads(3);
        reg.setProbeProgram(QStringLiteral(KK_AUDIO_PROBE_PATH));
    }

    // ---- real-time behaviour ----

    void processingAllocatesNothing_data() { discoveryListsThePluginsAsAudioEffects_data(); }
    void processingAllocatesNothing()
    {
        useTestPlugins();
        QFETCH(QString, format);
        const Keys k = keysOf(format);
        const auto entry = AudioPluginRegistry::instance().plugin(k.type);
        QVERIFY(entry);
        QString error;
        auto instance = audioplug::Instance::create(entry->info, entry->bundlePath, 48000.0, &error);
        QVERIFY2(instance, qPrintable(error));
        Signal in = noise(1024);
        instance->process(in.data(), 1024); // the first block may set things up
        gAllocations = 0;
        gCounting = true;
        for (int i = 0; i < 200; ++i) {
            instance->setParam(0, -3.0 + 0.01 * i);
            instance->setParam(1, i % 2);
            instance->process(in.data(), 1024);
        }
        gCounting = false;
        QCOMPARE(size_t(gAllocations), size_t(0));
        // and through the effect chain of the mixer (parameters read from the project's ParamSet)
        ClipEffectChain chain;
        const EffectInstance fx = make(k.type, -3.0, true, k.gain, k.invert);
        const QVector<EffectInstance> effects {fx};
        chain.process(effects, in.data(), 1024); // creates the plugin
        gAllocations = 0;
        gCounting = true;
        for (int i = 0; i < 200; ++i)
            chain.process(effects, in.data(), 1024);
        gCounting = false;
        QCOMPARE(size_t(gAllocations), size_t(0));
    }

    // ---- real packs (rows only for the bundles that are installed: the LSP plugins, ZAM, x42) ----

    void theFreePacksWork_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("only"); ///< names that are run (| separated); empty: the first few usable
        const QString root = qEnvironmentVariable("KK_AUDIO_PACKS_ROOT", QStringLiteral("/usr/lib"));
        const QString lsp = QStringLiteral("Compressor Stereo|Parametric Equalizer x8 Stereo|Limiter Stereo");
        struct Row
        {
            const char* tag;
            QString format;
            const char* file;
            QString only;
        };
        const Row rows[] = {{"lsp lv2", kLv2, "lv2/lsp-plugins.lv2", lsp}, {"lsp clap", kClap, "clap/lsp-plugins.clap", lsp},
            {"zam comp lv2", kLv2, "lv2/ZamComp.lv2", {}}, {"zam tube lv2", kLv2, "lv2/ZamTube.lv2", {}}, {"zam comp clap", kClap, "clap/ZamComp.clap", {}},
            {"zam tube clap", kClap, "clap/ZamTube.clap", {}}, {"x42 fil4 lv2", kLv2, "lv2/fil4.lv2", {}},
            {"x42 dpl lv2", kLv2, "lv2/dpl.lv2", {}}};
        bool any = false;
        for (const Row& r : rows) {
            const QString path = root + QLatin1Char('/') + QLatin1String(r.file);
            if ((r.format == kLv2 && !haveLv2()) || !QFileInfo::exists(path))
                continue;
            any = true;
            QTest::newRow(r.tag) << r.format << path << r.only;
        }
        if (!any)
            QTest::newRow("not installed") << QString() << QString() << QString(); // skipped
    }
    void theFreePacksWork()
    {
        QFETCH(QString, format);
        QFETCH(QString, path);
        QFETCH(QString, only);
        if (path.isEmpty())
            QSKIP("no free plugin pack is installed (LSP, ZAM, x42 in /usr/lib/lv2 and /usr/lib/clap, or KK_AUDIO_PACKS_ROOT)");
        AudioPluginRegistry& reg = AudioPluginRegistry::instance();
        reg.waitForScan();
        reg.setProbeTimeoutMs(120000);
        const QString root = m_dir.filePath(QStringLiteral("pack-") + QFileInfo(path).fileName());
        QDir().mkpath(root);
        QVERIFY(QFile::link(path, root + QLatin1Char('/') + QFileInfo(path).fileName()));
        reg.setSearchDirs(kLv2, format == kLv2 ? QStringList {root} : QStringList {});
        reg.setSearchDirs(kClap, format == kClap ? QStringList {root} : QStringList {});
        const QString cache = m_dir.filePath(QStringLiteral("cache-pack-") + QFileInfo(path).fileName() + QStringLiteral(".json"));
        QFile::remove(cache);
        reg.setCacheFile(cache);
        reg.reload();
        reg.setProbeTimeoutMs(30000);
        const QVector<AudioBundleEntry> bundles = reg.bundles();
        QVERIFY(bundles.size() == 1);
        QVERIFY2(!bundles[0].bad, qPrintable(bundles[0].reason));
        const QStringList names = only.isEmpty() ? QStringList() : only.split(QLatin1Char('|'));
        int usable = 0, ran = 0;
        for (const AudioPluginEntry& p : reg.allPlugins()) {
            if (!p.info.error.isEmpty())
                continue;
            ++usable;
            if (!names.isEmpty()
                && std::none_of(names.begin(), names.end(), [&](const QString& n) { return p.info.name.contains(n); }))
                continue;
            if (ran >= 3 && names.isEmpty())
                continue;
            ++ran;
            // process noise with the defaults, in blocks of 512, then again with every number at its maximum
            QString error;
            auto inst = audioplug::Instance::create(p.info, p.bundlePath, 48000.0, &error);
            QVERIFY2(inst, qPrintable(p.info.name + QStringLiteral(": ") + error));
            double bestEnergy = 0.0;
            for (int pass = 0; pass < 2; ++pass) {
                if (pass == 1) {
                    for (int k = 0; k < p.info.params.size(); ++k)
                        inst->setParam(k, p.info.params[k].max);
                }
                Signal in = noise(48000, 5);
                for (int off = 0; off < 48000; off += 512)
                    inst->process(in.data() + size_t(off) * 2, std::min(512, 48000 - off));
                double energy = 0.0;
                for (float v : in) {
                    QVERIFY2(std::isfinite(v), qPrintable(p.info.name));
                    energy += double(v) * v;
                }
                bestEnergy = std::max(bestEnergy, energy); // a plugin may be silent at its own defaults
            }
            QVERIFY2(bestEnergy > 1.0, qPrintable(p.info.name + QStringLiteral(" made silence")));
        }
        qInfo() << path << "usable plugins" << usable << "run" << ran;
        QVERIFY(ran >= 1);
        if (!names.isEmpty())
            QVERIFY(usable > 10);
    }
};

QTEST_MAIN(TestAudioPlug)
#include "test_audioplug.moc"
