// SPDX-License-Identifier: GPL-3.0-or-later
// Spectrum analyser and phase correlation meter (docs/behavior-spec.md section 141): a sine puts
// its peak in the right bin and display band at its level, silence reads the floor, correlation is
// +1 for mono, -1 for inverted and about 0 for unrelated noise, the readings do not depend on the
// block size, the goniometer puts mono on a vertical line.
#include "render/Spectrum.h"
#include "ui/audio/SpectrumDisplay.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace kk::dsp;
using kk::SpectrumDisplay;
using kk::SpectrumSlope;
using kk::SpectrumSmoothing;

namespace {

constexpr double kRate = 48000.0;
using Signal = std::vector<float>;

Signal sine(double seconds, double hz, double amp, double rightPhase = 0.0, bool invertRight = false)
{
    Signal s(size_t(seconds * kRate) * 2);
    for (size_t i = 0; i < s.size() / 2; ++i) {
        s[2 * i] = float(amp * std::sin(2.0 * std::numbers::pi * hz * double(i) / kRate));
        const float r = float(amp * std::sin(2.0 * std::numbers::pi * hz * double(i) / kRate + rightPhase));
        s[2 * i + 1] = invertRight ? -r : r;
    }
    return s;
}

Signal noise(double seconds, unsigned seedL, unsigned seedR)
{
    std::mt19937 a(seedL), b(seedR);
    std::uniform_real_distribution<float> d(-0.5f, 0.5f);
    Signal s(size_t(seconds * kRate) * 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        s[i] = d(a);
        s[i + 1] = d(b);
    }
    return s;
}

void feed(SpectrumAnalyzer& a, const Signal& s, int block)
{
    const int n = int(s.size() / 2);
    for (int at = 0; at < n; at += block)
        a.process(s.data() + size_t(at) * 2, std::min(block, n - at));
}

} // namespace

class TestSpectrum : public QObject
{
    Q_OBJECT
private slots:
    void aSinePeaksInTheRightBinAtItsLevel()
    {
        for (const double hz : {100.0, 1000.0, 5000.0, 12000.0}) {
            SpectrumAnalyzer a;
            feed(a, sine(0.5, hz, 0.5), 1024);
            const int expected = int(std::lround(hz / (kRate / kSpectrumFft)));
            int best = 0;
            for (int k = 1; k < kSpectrumFft / 2; ++k)
                if (a.binDb(k) > a.binDb(best))
                    best = k;
            QVERIFY2(std::abs(best - expected) <= 1, qPrintable(QString("%1 Hz: bin %2 expected %3").arg(hz).arg(best).arg(expected)));
            // 0.5 amplitude = -6.02 dBFS (the Hann window's scalloping loses up to 1.4 dB between bins).
            QVERIFY2(std::fabs(a.binDb(best) - (-6.02)) < 1.5, qPrintable(QString::number(a.binDb(best))));
            // The same peak in the log bands: the loudest band is the one nearest the frequency.
            std::vector<float> bands(kSpectrumBands);
            a.bandsDb(bands.data());
            const int top = int(std::max_element(bands.begin(), bands.end()) - bands.begin());
            QVERIFY2(std::abs(top - spectrumBandOf(best * kRate / kSpectrumFft)) <= 1, qPrintable(QString("%1 Hz: band %2 expected %3").arg(hz).arg(top).arg(spectrumBandOf(best * kRate / kSpectrumFft))));
            QVERIFY(bands[size_t(top)] > -8.0f);
            // Far from the sine the floor is low.
            const int far = std::clamp(spectrumBandOf(hz) + (spectrumBandOf(hz) < 128 ? 80 : -80), 0, kSpectrumBands - 1);
            QVERIFY2(bands[size_t(far)] < -60.0f, qPrintable(QString::number(bands[size_t(far)])));
        }
    }

    void silenceReadsTheFloor()
    {
        SpectrumAnalyzer a;
        feed(a, Signal(48000 * 2, 0.0f), 512);
        std::vector<float> bands(kSpectrumBands);
        a.bandsDb(bands.data());
        for (const float b : bands)
            QCOMPARE(b, float(kSpectrumFloorDb));
    }

    void theBlockSizeDoesNotChangeTheReading()
    {
        const Signal in = noise(0.5, 1, 2);
        std::vector<float> whole(kSpectrumBands), parts(kSpectrumBands);
        SpectrumAnalyzer a, b;
        feed(a, in, 100000);
        feed(b, in, 777);
        a.bandsDb(whole.data());
        b.bandsDb(parts.data());
        QVERIFY(whole == parts);
    }

    void bandAxisIsLogarithmic()
    {
        QCOMPARE(spectrumBandOf(20.0), 0);
        QCOMPARE(spectrumBandOf(20000.0), kSpectrumBands - 1);
        QVERIFY(std::fabs(spectrumBandHz(0) - 20.0) < 1e-9);
        QVERIFY(std::fabs(spectrumBandHz(kSpectrumBands - 1) - 20000.0) < 1e-6);
        // an octave is the same number of bands everywhere
        const int low = spectrumBandOf(200) - spectrumBandOf(100);
        const int high = spectrumBandOf(4000) - spectrumBandOf(2000);
        QVERIFY(std::abs(low - high) <= 1);
    }

    void correlationIsPlusOneForMonoMinusOneForInvertedAndZeroForNoise()
    {
        auto reading = [](const Signal& s, int block) {
            CorrelationMeter m;
            const int n = int(s.size() / 2);
            for (int at = 0; at < n; at += block)
                m.process(s.data() + size_t(at) * 2, std::min(block, n - at));
            return m.value();
        };
        QVERIFY(std::fabs(reading(sine(1.0, 440, 0.5), 1024) - 1.0) < 1e-3);
        QVERIFY(std::fabs(reading(sine(1.0, 440, 0.5, 0.0, true), 1024) + 1.0) < 1e-3);
        const double wide = reading(noise(2.0, 3, 4), 1024);
        QVERIFY2(std::fabs(wide) < 0.1, qPrintable(QString::number(wide)));
        // 90 degrees apart: uncorrelated as well
        QVERIFY(std::fabs(reading(sine(1.0, 440, 0.5, std::numbers::pi / 2), 1024)) < 0.05);
        // Silence reads 0, and the block size changes nothing.
        QCOMPARE(reading(Signal(48000 * 2, 0.0f), 512), 0.0);
        QCOMPARE(reading(noise(1.0, 5, 6), 1), reading(noise(1.0, 5, 6), 4096));
    }

    void displayAppliesSlopeSmoothingAndPeakHold()
    {
        std::vector<float> flat(kSpectrumBands, -40.0f);
        // Slope: +4.5 dB per octave around 1 kHz.
        SpectrumDisplay d;
        d.setSmoothing(SpectrumSmoothing::Off);
        d.setSlope(SpectrumSlope::Mix45);
        d.update(flat.data(), 20);
        const int at1k = spectrumBandOf(1000), at8k = spectrumBandOf(8000);
        QVERIFY(std::fabs(d.barDb(at1k) + 40.0f) < 0.3f);
        QVERIFY2(std::fabs((d.barDb(at8k) - d.barDb(at1k)) - 13.5f) < 0.4f,
            qPrintable(QString::number(d.barDb(at8k) - d.barDb(at1k))));
        // Smoothing: a jump up is followed slowly; Off is instant.
        SpectrumDisplay slow;
        slow.setSmoothing(SpectrumSmoothing::Slow);
        slow.setPeakHold(false);
        std::vector<float> low(kSpectrumBands, -80.0f), high(kSpectrumBands, -20.0f);
        for (int i = 0; i < 20; ++i)
            slow.update(low.data(), 33);
        slow.update(high.data(), 33);
        QVERIFY(slow.barDb(100) < -60.0f && slow.barDb(100) > -80.0f);
        for (int i = 0; i < 100; ++i)
            slow.update(high.data(), 33);
        QVERIFY(slow.barDb(100) > -21.0f);
        // Peak hold: the mark stays while the bar falls, then comes down.
        SpectrumDisplay hold;
        hold.setSmoothing(SpectrumSmoothing::Off);
        hold.setPeakHold(true);
        hold.update(high.data(), 33);
        hold.update(low.data(), 33);
        QCOMPARE(hold.barDb(100), -80.0f);
        QCOMPARE(hold.peakDb(100), -20.0f);
        for (int i = 0; i < 30; ++i) // about one second: still held
            hold.update(low.data(), 33);
        QCOMPARE(hold.peakDb(100), -20.0f);
        for (int i = 0; i < 150; ++i) // five seconds later it fell
            hold.update(low.data(), 33);
        QVERIFY(hold.peakDb(100) < -30.0f);
        // Without peak hold the mark follows the bar.
        hold.setPeakHold(false);
        QCOMPARE(hold.peakDb(100), hold.barDb(100));
        // Silence (no reading) lets the bars fall to the floor.
        SpectrumDisplay fall;
        fall.setSmoothing(SpectrumSmoothing::Fast);
        fall.update(high.data(), 33);
        for (int i = 0; i < 200; ++i)
            fall.decay(33);
        QVERIFY(fall.barDb(10) < -100.0f);
    }

    void theGoniometerPutsMonoOnAVerticalLine()
    {
        float mono[2 * kGoniometerPoints], inverted[2 * kGoniometerPoints];
        const Signal m = sine(0.1, 300, 0.5);
        goniometerPoints(m.data(), int(m.size() / 2), mono);
        const Signal v = sine(0.1, 300, 0.5, 0.0, true);
        goniometerPoints(v.data(), int(v.size() / 2), inverted);
        double widthMono = 0, heightMono = 0, widthInv = 0, heightInv = 0;
        for (int p = 0; p < kGoniometerPoints; ++p) {
            widthMono = std::max(widthMono, double(std::fabs(mono[2 * p])));
            heightMono = std::max(heightMono, double(std::fabs(mono[2 * p + 1])));
            widthInv = std::max(widthInv, double(std::fabs(inverted[2 * p])));
            heightInv = std::max(heightInv, double(std::fabs(inverted[2 * p + 1])));
        }
        QVERIFY(widthMono < 1e-6 && heightMono > 0.3);
        QVERIFY(heightInv < 1e-6 && widthInv > 0.3);
    }
};

QTEST_GUILESS_MAIN(TestSpectrum)
#include "test_spectrum.moc"
