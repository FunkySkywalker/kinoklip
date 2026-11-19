// SPDX-License-Identifier: GPL-3.0-or-later
// Sends to buses (Pre / Post fader, send pan), aux buses, bus-to-bus feeds, bus pan, Phase
// Invert and EQ presets (docs/behavior-spec.md section 137).
#include "core/EqPresets.h"
#include "core/MixerJson.h"
#include "core/MixerOps.h"
#include "render/AudioMixer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <functional>
#include <numbers>
#include <vector>

using namespace kk;
using namespace kk::serialization;

namespace {

constexpr double kRate = 48000.0;
using Signal = std::vector<float>; ///< interleaved stereo

double tone(int i, double hz, double amp)
{
    return amp * std::sin(2.0 * std::numbers::pi * hz * i / kRate);
}

double fromDb(double db)
{
    return std::pow(10.0, db / 20.0);
}

/// Level (dB) of the 1 kHz component of channel @p side (0 = left) in [from, to) seconds.
double sideDb(const Signal& s, int side, double fromSec, double toSec, double hz = 1000.0)
{
    const int a = int(fromSec * kRate), b = int(toSec * kRate);
    double re = 0.0, im = 0.0;
    for (int i = a; i < b; ++i) {
        const double w = 2.0 * std::numbers::pi * hz * i / kRate;
        re += double(s[size_t(i) * 2 + size_t(side)]) * std::cos(w);
        im += double(s[size_t(i) * 2 + size_t(side)]) * std::sin(w);
    }
    const double amp = 2.0 * std::sqrt(re * re + im * im) / (b - a);
    return 20.0 * std::log10(std::max(amp, 1e-12));
}

float peakOf(const Signal& s)
{
    float p = 0.0f;
    for (float v : s)
        p = std::max(p, std::abs(v));
    return p;
}

} // namespace

class TestSends : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::shared_ptr<MediaPathMap> m_paths = std::make_shared<MediaPathMap>();

    bool writeTone(const QString& name, double seconds, double hz, double amp)
    {
        const int n = int(seconds * kRate);
        Signal s(size_t(n) * 2);
        for (int i = 0; i < n; ++i)
            s[size_t(i) * 2] = s[size_t(i) * 2 + 1] = float(tone(i, hz, amp));
        QByteArray h;
        auto u32 = [&h](quint32 v) { h.append(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&h](quint16 v) { h.append(reinterpret_cast<const char*>(&v), 2); };
        const quint32 bytes = quint32(s.size() * 4);
        h.append("RIFF");
        u32(36 + bytes);
        h.append("WAVEfmt ");
        u32(16);
        u16(3);
        u16(2);
        u32(48000);
        u32(48000 * 8);
        u16(8);
        u16(32);
        h.append("data");
        u32(bytes);
        QFile f(m_dir.filePath(name));
        return f.open(QIODevice::WriteOnly) && f.write(h) == h.size()
            && f.write(reinterpret_cast<const char*>(s.data()), bytes) == qint64(bytes);
    }

    /// Two tracks, each playing the 1 kHz tone (0.25 = -12 dBFS) for 4 s.
    TimelineData timeline()
    {
        TimelineData tl
            = makeDefaultTimeline(QStringLiteral("sends"), Rational {25, 1}, 1920, 1080, 2, 2);
        for (int t = 0; t < 2; ++t) {
            ClipData c;
            c.id = tl.allocateId();
            c.kind = ClipKind::Media;
            c.mediaId = QStringLiteral("tone");
            c.start = 0;
            c.duration = 100;
            c.sourceDurationUs = 4'000'000;
            tl.audioTracks[size_t(t)].clips.push_back(c);
        }
        // Only the first track plays unless a test says otherwise.
        tl.audioTracks[1].muted = true;
        return tl;
    }

    Signal mixChunks(const TimelineData& tl, int frames, int block)
    {
        AudioMixer mixer;
        mixer.setMediaPaths(m_paths);
        Signal out(size_t(frames) * 2);
        for (int at = 0; at < frames; at += block) {
            const int f = std::min(block, frames - at);
            mixer.mix(tl, at, f, out.data() + size_t(at) * 2);
        }
        return out;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(writeTone(QStringLiteral("tone.wav"), 4.0, 1000.0, 0.25));
        (*m_paths)[QStringLiteral("tone")] = m_dir.filePath(QStringLiteral("tone.wav"));
    }

    // ---- 137.1 sends: level, Pre / Post ----
    void sendPostFaderFollowsTheFader()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int aux = mixerops::addBus(tl, false, {}, true);
        tl.audioTracks[0].mix.strip.volumeDb = -12.0;
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), aux, -6.0, false));
        QVERIFY(mixerops::setOutputBus(tl, aux)); // hear the aux bus alone
        const Signal out = mixChunks(tl, 96000, 512);
        // -12 dBFS tone, -12 dB fader, -6 dB send: -30 dBFS.
        QVERIFY2(std::abs(sideDb(out, 0, 0.5, 1.5) - (-30.0)) < 0.1,
            qPrintable(QString::number(sideDb(out, 0, 0.5, 1.5))));
        QVERIFY(std::abs(sideDb(out, 1, 0.5, 1.5) - (-30.0)) < 0.1);
    }

    void sendPreFaderIgnoresTheFader()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int aux = mixerops::addBus(tl, false, {}, true);
        tl.audioTracks[0].mix.strip.volumeDb = -12.0;
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), aux, -6.0, true));
        QVERIFY(mixerops::setOutputBus(tl, aux));
        Signal out = mixChunks(tl, 96000, 512);
        QVERIFY2(std::abs(sideDb(out, 0, 0.5, 1.5) - (-18.0)) < 0.1,
            qPrintable(QString::number(sideDb(out, 0, 0.5, 1.5))));
        // The fader all the way down: the Pre send stays, the Post send is gone.
        tl.audioTracks[0].mix.strip.volumeDb = kFaderMinDb;
        out = mixChunks(tl, 96000, 512);
        QVERIFY(std::abs(sideDb(out, 0, 0.5, 1.5) - (-18.0)) < 0.1);
        SendData post = tl.audioTracks[0].mix.strip.sends[0];
        post.preFader = false;
        QVERIFY(mixerops::updateSend(tl, MixTarget::track(a1), post));
        out = mixChunks(tl, 96000, 512);
        QVERIFY(peakOf(out) < 1e-6f);
    }

    void sendOffAndLevelAreHonoured()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int aux = mixerops::addBus(tl, false, {}, true);
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), aux, 0.0, false));
        QVERIFY(mixerops::setOutputBus(tl, aux));
        QVERIFY(std::abs(sideDb(mixChunks(tl, 96000, 512), 0, 0.5, 1.5) - (-12.0)) < 0.1);
        SendData d = tl.audioTracks[0].mix.strip.sends[0];
        d.enabled = false;
        QVERIFY(mixerops::updateSend(tl, MixTarget::track(a1), d));
        QVERIFY(peakOf(mixChunks(tl, 96000, 512)) < 1e-6f);
        QVERIFY(!mixerops::updateSend(tl, MixTarget::track(a1), d)); // nothing changed
        d.enabled = true;
        d.levelDb = 100.0; // clamped
        QVERIFY(mixerops::updateSend(tl, MixTarget::track(a1), d));
        QCOMPARE(tl.audioTracks[0].mix.strip.sends[0].levelDb, kSendMaxDb);
    }

    // ---- 137.2 send pan, bus pan ----
    void sendPanBalancesTheSend()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int aux = mixerops::addBus(tl, false, {}, true);
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), aux, 0.0, false));
        SendData d = tl.audioTracks[0].mix.strip.sends[0];
        d.pan = 1.0;
        QVERIFY(mixerops::updateSend(tl, MixTarget::track(a1), d));
        QVERIFY(mixerops::setOutputBus(tl, aux));
        const Signal out = mixChunks(tl, 96000, 512);
        // Constant-power law at the edge: left silent, right +3 dB.
        QVERIFY(sideDb(out, 0, 0.5, 1.5) < -100.0);
        QVERIFY2(std::abs(sideDb(out, 1, 0.5, 1.5) - (-12.0 + 3.01)) < 0.1,
            qPrintable(QString::number(sideDb(out, 1, 0.5, 1.5))));
    }

    void busPanPansTheBus()
    {
        TimelineData tl = timeline();
        tl.mixer.buses[0].strip.pan = -1.0;
        const Signal out = mixChunks(tl, 96000, 512);
        QVERIFY2(std::abs(sideDb(out, 0, 0.5, 1.5) - (-12.0 + 3.01)) < 0.1,
            qPrintable(QString::number(sideDb(out, 0, 0.5, 1.5))));
        QVERIFY(sideDb(out, 1, 0.5, 1.5) < -100.0);
        // Centre: unchanged.
        tl.mixer.buses[0].strip.pan = 0.0;
        QVERIFY(std::abs(sideDb(mixChunks(tl, 96000, 512), 1, 0.5, 1.5) - (-12.0)) < 0.1);
    }

    // ---- 137.3 bus-to-bus feeds and loops ----
    void busToBusFeed()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int sub = mixerops::addBus(tl, false, {}, false);
        const int aux = mixerops::addBus(tl, false, {}, true);
        // Tracks cannot be routed to an aux bus, only sent to it.
        QVERIFY(!mixerops::setTrackRoute(tl, a1, aux, true));
        QVERIFY(mixerops::setTrackRoute(tl, a1, sub, true));
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), aux, 0.0));
        QVERIFY(mixerops::addSend(tl, MixTarget::bus(aux), sub, -6.0));
        QVERIFY(mixerops::addSend(tl, MixTarget::bus(sub), kMainBusId, 0.0));
        // main = sub (route, 0 dB) + aux -> sub (-6 dB): both reach the output, in dependency
        // order (the aux bus is processed before sub, sub before the main bus).
        const Signal out = mixChunks(tl, 96000, 512);
        // Sub = 0.25 + 0.25 * 0.5 = 0.375 (-8.52 dBFS), then to main = 0.375 (+ the main route
        // of A1 itself, which is also on: 0.25) = 0.625 in total.
        QVERIFY2(std::abs(sideDb(out, 0, 0.5, 1.5) - 20.0 * std::log10(0.625)) < 0.15,
            qPrintable(QString::number(sideDb(out, 0, 0.5, 1.5))));
    }

    void loopsAreRefused()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int b2 = mixerops::addBus(tl, false);
        const int b3 = mixerops::addBus(tl, false);
        // A bus cannot feed itself.
        QVERIFY(!mixerops::sendAllowed(tl, MixTarget::bus(b2), b2));
        QVERIFY(!mixerops::addSend(tl, MixTarget::bus(b2), b2));
        QVERIFY(mixerops::sendAllowed(tl, MixTarget::bus(b2), b3));
        QVERIFY(mixerops::addSend(tl, MixTarget::bus(b2), b3));
        // b3 -> b2 would close the loop.
        QVERIFY(!mixerops::sendAllowed(tl, MixTarget::bus(b3), b2));
        QVERIFY(!mixerops::addSend(tl, MixTarget::bus(b3), b2));
        QVERIFY(tl.mixer.buses[2].strip.sends.isEmpty());
        // A longer chain: b3 -> b4 -> (b2 refused).
        const int b4 = mixerops::addBus(tl, false);
        QVERIFY(mixerops::addSend(tl, MixTarget::bus(b3), b4));
        QVERIFY(!mixerops::addSend(tl, MixTarget::bus(b4), b2));
        QVERIFY(!mixerops::addSend(tl, MixTarget::bus(b4), b3));
        QVERIFY(mixerops::addSend(tl, MixTarget::bus(b4), kMainBusId));
        // A track routed to b2 can send to b2's targets but a bus cannot send to a track source.
        QVERIFY(mixerops::setTrackRoute(tl, a1, b2, true));
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), b4));
        // Unknown bus, duplicate send, unknown strip.
        QVERIFY(!mixerops::addSend(tl, MixTarget::track(a1), 99));
        QVERIFY(!mixerops::addSend(tl, MixTarget::track(a1), b4));
        QVERIFY(!mixerops::addSend(tl, MixTarget::track(999999), b4));
        // A hand-edited loop does not hang or crash the mixer.
        tl.mixer.buses[size_t(mixerops::busIndex(tl.mixer, b4))].strip.sends.append(
            SendData {b2, true, false, 0.0, 0.0});
        tl.mixer.buses[1].strip.sends.append(SendData {b4, true, false, 0.0, 0.0});
        const Signal out = mixChunks(tl, 9600, 480);
        QCOMPARE(out.size(), size_t(19200));
    }

    void keysAndSendsMakeNoLoop()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int b2 = mixerops::addBus(tl, false);
        // A1 keys from bus 2: A1 may not send into bus 2 (bus 2 would depend on A1).
        QVERIFY(mixerops::setKeySource(tl, MixTarget::track(a1), KeyKind::Bus, quint64(b2)));
        QVERIFY(!mixerops::sendAllowed(tl, MixTarget::track(a1), b2));
        QVERIFY(mixerops::setKeySource(tl, MixTarget::track(a1), KeyKind::Self, 0));
        // The other way round: A1 sends to bus 2, so bus 2 cannot be a key for A1.
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), b2));
        QVERIFY(!mixerops::keySourceAllowed(tl, MixTarget::track(a1), KeyKind::Bus, quint64(b2)));
        // ... and a bus that feeds a bus cannot take the key from it.
        const int b3 = mixerops::addBus(tl, false);
        QVERIFY(mixerops::addSend(tl, MixTarget::bus(b2), b3));
        QVERIFY(!mixerops::keySourceAllowed(tl, MixTarget::bus(b2), KeyKind::Bus, quint64(b3)));
        // A1 is a source of b3 through b2 now: b3 cannot key from... A1 keys from b3 is a loop.
        QVERIFY(!mixerops::keySourceAllowed(tl, MixTarget::track(a1), KeyKind::Bus, quint64(b3)));
    }

    void deletingABusRemovesItsSends()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int b2 = mixerops::addBus(tl, false, {}, true);
        const int b3 = mixerops::addBus(tl, false);
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), b2));
        QVERIFY(mixerops::addSend(tl, MixTarget::bus(b3), b2));
        QVERIFY(mixerops::removeBus(tl, b2));
        QVERIFY(tl.audioTracks[0].mix.strip.sends.isEmpty());
        QVERIFY(mixerops::bus(tl, b3)->strip.sends.isEmpty());
        // Making a bus aux drops the routes; the main and the output bus cannot be aux.
        QVERIFY(mixerops::setTrackRoute(tl, a1, b3, true));
        QVERIFY(mixerops::setBusAux(tl, b3, true));
        QVERIFY(!tl.audioTracks[0].mix.buses.contains(b3));
        QVERIFY(!mixerops::setBusAux(tl, kMainBusId, true));
    }

    // ---- 137.4 same samples at any block size ----
    void blockSizeDoesNotChangeTheMix()
    {
        TimelineData tl = timeline();
        tl.audioTracks[1].muted = false;
        const quint64 a1 = tl.audioTracks[0].id;
        const quint64 a2 = tl.audioTracks[1].id;
        const int sub = mixerops::addBus(tl, false);
        const int aux = mixerops::addBus(tl, false, {}, true);
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), aux, -6.0, true));
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a2), aux, -9.0, false));
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a2), sub, -3.0, false));
        QVERIFY(mixerops::addSend(tl, MixTarget::bus(aux), sub, -2.0, false));
        QVERIFY(mixerops::addSend(tl, MixTarget::bus(sub), kMainBusId, 0.0, true));
        SendData d = tl.audioTracks[0].mix.strip.sends[0];
        d.pan = 0.4;
        QVERIFY(mixerops::updateSend(tl, MixTarget::track(a1), d));
        tl.mixer.buses[1].strip.pan = -0.3;
        tl.audioTracks[1].mix.strip.invertLeft = true;
        tl.audioTracks[0].mix.strip.eq.enabled = true;
        tl.audioTracks[0].mix.strip.eq.bands[3].gainDb = 4.0;
        const Signal a = mixChunks(tl, 48000, 256);
        const Signal b = mixChunks(tl, 48000, 4096);
        const Signal c = mixChunks(tl, 48000, 960);
        QVERIFY(peakOf(a) > 0.1f);
        QCOMPARE(a, b);
        QCOMPARE(a, c);
        // Rendering from the middle (fresh filters and gains) settles to the same samples.
        AudioMixer m;
        m.setMediaPaths(m_paths);
        Signal tail(size_t(9600) * 2);
        m.mix(tl, 19200, 9600, tail.data());
        float worst = 0.0f;
        for (size_t i = 4800 * 2; i < tail.size(); ++i)
            worst = std::max(worst, std::abs(tail[i] - a[19200 * 2 + i]));
        QVERIFY2(worst < 2e-3f, qPrintable(QString::number(worst)));
    }

    // ---- 137.5 Phase Invert ----
    void phaseInvertCancelsAgainstTheOriginal()
    {
        TimelineData tl = timeline();
        tl.audioTracks[1].muted = false;
        QVERIFY(peakOf(mixChunks(tl, 48000, 512)) > 0.4f); // two tones add up
        QVERIFY(mixerops::setStripInvert(
            tl, MixTarget::track(tl.audioTracks[1].id), true, true));
        QVERIFY(peakOf(mixChunks(tl, 48000, 512)) < 1e-6f);
        // Only the left channel: the right still adds.
        QVERIFY(mixerops::setStripInvert(tl, MixTarget::track(tl.audioTracks[1].id), true, false));
        const Signal out = mixChunks(tl, 48000, 512);
        QVERIFY(sideDb(out, 0, 0.5, 0.9) < -100.0);
        QVERIFY(std::abs(sideDb(out, 1, 0.5, 0.9) - (-6.0)) < 0.1);
        QVERIFY(!mixerops::setStripInvert(
            tl, MixTarget::track(tl.audioTracks[1].id), true, false)); // unchanged
        // A bus inverts too: bus 2 gets the inverted tone and the track cancels it.
        TimelineData t2 = timeline();
        const int b2 = mixerops::addBus(t2, false);
        QVERIFY(mixerops::setTrackRoute(t2, t2.audioTracks[0].id, b2, true));
        QVERIFY(mixerops::addSend(t2, MixTarget::bus(b2), kMainBusId));
        QVERIFY(mixerops::setStripInvert(t2, MixTarget::bus(b2), true, true));
        QVERIFY(peakOf(mixChunks(t2, 48000, 512)) < 1e-6f);
    }

    // ---- 137.6 save and load ----
    void sendsInvertAndAuxSurviveSaving()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int aux = mixerops::addBus(tl, false, QStringLiteral("Reverb"), true);
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), aux, -7.5, true));
        SendData d = tl.audioTracks[0].mix.strip.sends[0];
        d.pan = -0.25;
        d.enabled = false;
        QVERIFY(mixerops::updateSend(tl, MixTarget::track(a1), d));
        QVERIFY(mixerops::addSend(tl, MixTarget::bus(aux), kMainBusId, 3.0));
        tl.audioTracks[0].mix.strip.invertLeft = true;
        tl.mixer.buses[0].strip.invertRight = true;
        tl.mixer.buses[0].strip.pan = 0.5;

        const MixerData mixer = mixerFromJson(mixerToJson(tl.mixer));
        QCOMPARE(mixer, tl.mixer);
        QVERIFY(mixer.buses[1].aux);
        const TrackMix mix = trackMixFromJson(trackMixToJson(tl.audioTracks[0].mix));
        QCOMPARE(mix, tl.audioTracks[0].mix);
        QCOMPARE(mix.strip.sends.size(), 1);
        QCOMPARE(mix.strip.sends[0].levelDb, -7.5);
        QVERIFY(mix.strip.sends[0].preFader);
        QVERIFY(!mix.strip.sends[0].enabled);
        QCOMPARE(mix.strip.sends[0].pan, -0.25);
        // A default strip writes nothing new.
        QVERIFY(stripToJson(ChannelStrip {}).isEmpty());
        // Damaged files: a send to an unknown bus, a self send and a duplicate are dropped.
        QJsonObject json = mixerToJson(tl.mixer);
        QJsonArray buses = json.value(QLatin1String("buses")).toArray();
        QJsonObject auxJson = buses.at(1).toObject();
        QJsonObject strip = auxJson.value(QLatin1String("strip")).toObject();
        QJsonArray sends = strip.value(QLatin1String("sends")).toArray();
        sends.append(QJsonObject {{QLatin1String("bus"), aux}});
        sends.append(QJsonObject {{QLatin1String("bus"), 77}});
        sends.append(QJsonObject {{QLatin1String("bus"), kMainBusId}});
        strip[QLatin1String("sends")] = sends;
        auxJson[QLatin1String("strip")] = strip;
        buses[1] = auxJson;
        json[QLatin1String("buses")] = buses;
        const MixerData loaded = mixerFromJson(json);
        QCOMPARE(loaded.buses[1].strip.sends.size(), 1);
        QCOMPARE(loaded.buses[1].strip.sends[0].busId, kMainBusId);
    }

    // ---- 137.7 EQ presets ----
    void factoryEqPresetsExistAndAreSound()
    {
        const auto presets = eqpresets::factoryPresets();
        QStringList names;
        for (const auto& p : presets)
            names << p.name;
        QCOMPARE(names,
            (QStringList {QStringLiteral("Flat"), QStringLiteral("Voice"), QStringLiteral("Music"),
                QStringLiteral("Telephone"), QStringLiteral("Rumble Cut"),
                QStringLiteral("De-Mud"), QStringLiteral("Podcast Voice")}));
        for (const auto& p : presets) {
            QVERIFY(p.factory);
            for (const EqBand& b : p.eq.bands) {
                QVERIFY(b.frequencyHz >= kEqMinFrequencyHz && b.frequencyHz <= kEqMaxFrequencyHz);
                QVERIFY(std::abs(b.gainDb) <= kEqMaxGainDb);
                QVERIFY(b.q >= kEqMinQ && b.q <= kEqMaxQ);
            }
        }
        QVERIFY(!eqpresets::find(QStringLiteral("Flat")).eq.enabled);
        QVERIFY(eqpresets::find(QStringLiteral("telephone")).eq.enabled);
        QVERIFY(eqpresets::find(QStringLiteral("Nope")).name.isEmpty());
        // Telephone is a band-pass: 400 Hz high pass and 3.4 kHz low pass.
        const EqSettings tel = eqpresets::find(QStringLiteral("Telephone")).eq;
        QVERIFY(tel.bands[0].enabled && tel.bands[0].frequencyHz == 400.0);
        QVERIFY(tel.bands[5].enabled && tel.bands[5].frequencyHz == 3400.0);
    }

    void applySaveAndDeleteUserEqPresets()
    {
        QTemporaryDir config;
        QVERIFY(config.isValid());
        eqpresets::setConfigDirectoryForTests(config.path());
        TimelineData tl = timeline();
        const MixTarget a1 = MixTarget::track(tl.audioTracks[0].id);
        QVERIFY(mixerops::applyEqPreset(tl, a1, eqpresets::find(QStringLiteral("Voice")).eq));
        QCOMPARE(tl.audioTracks[0].mix.strip.eq, eqpresets::find(QStringLiteral("Voice")).eq);
        QVERIFY(!mixerops::applyEqPreset(tl, a1, eqpresets::find(QStringLiteral("Voice")).eq));
        // Save the strip's EQ under my own name, change the strip, apply the preset again.
        const EqSettings mine = tl.audioTracks[0].mix.strip.eq;
        QVERIFY(eqpresets::saveUserPreset(QStringLiteral("  My   Voice "), mine));
        QVERIFY(!eqpresets::saveUserPreset(QStringLiteral("voice"), mine)); // factory name
        QVERIFY(!eqpresets::saveUserPreset(QStringLiteral("   "), mine));
        QCOMPARE(eqpresets::userPresets().size(), 1);
        QCOMPARE(eqpresets::userPresets()[0].name, QStringLiteral("My Voice"));
        QVERIFY(mixerops::applyEqPreset(tl, a1, eqpresets::find(QStringLiteral("Telephone")).eq));
        QVERIFY(mixerops::applyEqPreset(tl, a1, eqpresets::find(QStringLiteral("my voice")).eq));
        QCOMPARE(tl.audioTracks[0].mix.strip.eq, mine);
        // Saving under the same name overwrites it (still one file); a second name adds one.
        EqSettings other = mine;
        other.bands[3].gainDb = -3.0;
        QVERIFY(eqpresets::saveUserPreset(QStringLiteral("my voice"), other));
        QVERIFY(eqpresets::saveUserPreset(QStringLiteral("Zed"), other));
        QCOMPARE(eqpresets::userPresets().size(), 2);
        QCOMPARE(eqpresets::find(QStringLiteral("My Voice")).eq.bands[3].gainDb, -3.0);
        // Delete: gone from the list, and factory presets cannot be deleted.
        QVERIFY(eqpresets::removeUserPreset(QStringLiteral("zed")));
        QVERIFY(!eqpresets::removeUserPreset(QStringLiteral("zed")));
        QVERIFY(!eqpresets::removeUserPreset(QStringLiteral("Voice")));
        QCOMPARE(eqpresets::userPresets().size(), 1);
        QVERIFY(eqpresets::find(QStringLiteral("zed")).name.isEmpty());
        QVERIFY(eqpresets::removeUserPreset(QStringLiteral("My Voice")));
        QVERIFY(eqpresets::userPresets().isEmpty());
        eqpresets::setConfigDirectoryForTests({});
    }

    void eqPresetChangesTheSound()
    {
        // Telephone removes a 100 Hz hum and keeps 1 kHz (band-pass).
        TimelineData tl = timeline();
        QVERIFY(mixerops::applyEqPreset(
            tl, MixTarget::track(tl.audioTracks[0].id), eqpresets::find(QStringLiteral("Telephone")).eq));
        const Signal out = mixChunks(tl, 96000, 512);
        QVERIFY(sideDb(out, 0, 0.5, 1.5) > -12.0); // 1 kHz is lifted by the 1 kHz bell
    }
};

QTEST_MAIN(TestSends)
#include "test_sends.moc"
