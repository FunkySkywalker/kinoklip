// SPDX-License-Identifier: GPL-3.0-or-later
// Monitor controls (docs/behavior-spec.md section 141): Dim, Mono and Mute change what the sound
// device plays and nothing else (the ring, the meters and every render stay as they are), they do
// not click, and they are saved per user, not in the project.
#include "playback/AudioRing.h"
#include "playback/AudioRouting.h"
#include "playback/MonitorControls.h"

#include <QSettings>
#include <QStandardPaths>
#include <QTest>

#include <cmath>
#include <numbers>
#include <vector>

using namespace kk;

namespace {

std::vector<float> stereoSine(int frames, double amp = 0.5)
{
    std::vector<float> s(size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        s[size_t(i) * 2] = float(amp * std::sin(2.0 * std::numbers::pi * 440.0 * i / 48000.0));
        s[size_t(i) * 2 + 1] = float(0.5 * amp * std::sin(2.0 * std::numbers::pi * 660.0 * i / 48000.0));
    }
    return s;
}

double rms(const std::vector<float>& v, int from, int to, int channel)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i)
        sum += double(v[size_t(i) * 2 + size_t(channel)]) * v[size_t(i) * 2 + size_t(channel)];
    return std::sqrt(sum / (to - from));
}

} // namespace

class TestMonitorControls : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void everythingOffLeavesTheBlockAlone()
    {
        MonitorControls c;
        MonitorProcessor p;
        const auto in = stereoSine(4800);
        auto out = in;
        p.process(c, out.data(), 4800);
        QVERIFY(out == in);
    }

    void dimLowersTheLevelByTheSetAmount()
    {
        MonitorControls c;
        c.setDim(true);
        MonitorProcessor p;
        auto in = stereoSine(9600);
        auto out = in;
        p.process(c, out.data(), 9600);
        QCOMPARE(c.dimDb(), -20.0);
        const double ratio = 20.0 * std::log10(rms(out, 2000, 9600, 0) / rms(in, 2000, 9600, 0));
        QVERIFY2(std::fabs(ratio + 20.0) < 0.1, qPrintable(QString::number(ratio)));
        c.setDimDb(-12.0);
        MonitorProcessor q;
        out = in;
        q.process(c, out.data(), 9600);
        QVERIFY(std::fabs(20.0 * std::log10(rms(out, 2000, 9600, 1) / rms(in, 2000, 9600, 1)) + 12.0) < 0.1);
        c.setDimDb(-500.0); // clamped
        QCOMPARE(c.dimDb(), MonitorControls::kMinDimDb);
    }

    void monoSumsLeftAndRight()
    {
        MonitorControls c;
        c.setMono(true);
        MonitorProcessor p;
        const auto in = stereoSine(9600);
        auto out = in;
        p.process(c, out.data(), 9600);
        for (int i = 2000; i < 9600; ++i) {
            QVERIFY(std::fabs(out[size_t(i) * 2] - out[size_t(i) * 2 + 1]) < 1e-6f);
            QVERIFY(std::fabs(out[size_t(i) * 2] - 0.5f * (in[size_t(i) * 2] + in[size_t(i) * 2 + 1])) < 1e-6f);
        }
    }

    void muteSilencesWithoutAClick()
    {
        MonitorControls c;
        MonitorProcessor p;
        auto block = stereoSine(4800);
        p.process(c, block.data(), 4800); // running, nothing on
        c.setMute(true);
        block = stereoSine(4800);
        const auto in = block;
        p.process(c, block.data(), 4800);
        // a ramp of a few milliseconds, then silence; no sample jumps by more than a small step
        for (int i = 1000; i < 4800; ++i) {
            QCOMPARE(block[size_t(i) * 2], 0.0f);
            QCOMPARE(block[size_t(i) * 2 + 1], 0.0f);
        }
        float worst = 0.0f;
        for (int i = 1; i < 1000; ++i)
            worst = std::max(worst, std::fabs(block[size_t(i) * 2] - block[size_t(i - 1) * 2]));
        QVERIFY2(worst < 0.1f, qPrintable(QString::number(worst)));
        QVERIFY(std::fabs(block[0] - in[0]) < 1e-6f); // starts where the sound was
    }

    void theDeviceHearsItButTheRingAndTheMeterTapDoNot()
    {
        // The ring holds what the mixer made; a puller with the controls plays it changed, a puller
        // without them (the tap of a render or a meter) plays it as it is.
        const auto in = stereoSine(2048);
        AudioRing ringA(8192), ringB(8192);
        ringA.write(in.data(), 2048);
        ringB.write(in.data(), 2048);
        MonitorControls c;
        c.setMute(true);
        RingPuller heard(&ringA, false);
        heard.setMonitor(&c);
        RingPuller plain(&ringB, false);
        std::vector<float> a(2048 * 2), b(2048 * 2);
        heard.pull(reinterpret_cast<char*>(a.data()), 2048);
        plain.pull(reinterpret_cast<char*>(b.data()), 2048);
        QVERIFY(b == in);
        QCOMPARE(rms(a, 1000, 2048, 0), 0.0);
        // switched off again, the next block is the ring's sound
        c.setMute(false);
        ringA.write(in.data(), 2048);
        heard.pull(reinterpret_cast<char*>(a.data()), 2048);
        QVERIFY(rms(a, 1500, 2048, 0) > 0.1);
    }

    void theSettingsAreSavedPerUserAndMuteStartsOff()
    {
        {
            QSettings s;
            s.remove(QStringLiteral("monitor"));
        }
        MonitorControls c;
        c.setDim(true);
        c.setDimDb(-15.0);
        c.setMono(true);
        c.setMute(true);
        c.save();
        MonitorControls d;
        d.load();
        QVERIFY(d.dim() && d.mono());
        QCOMPARE(d.dimDb(), -15.0);
        QVERIFY(!d.mute()); // never opens silent
        QSettings().remove(QStringLiteral("monitor"));
    }
};

QTEST_GUILESS_MAIN(TestMonitorControls)
#include "test_monitorcontrols.moc"
