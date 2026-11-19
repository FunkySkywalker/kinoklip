// SPDX-License-Identifier: GPL-3.0-or-later
// Noise Reduction audio effect, Inspector part (docs/behavior-spec.md section 132): the effect
// goes on an audio clip like the other audio effects, every control has a tooltip, Learn keeps a
// noise profile from the marked range (one undo step), Clear Profile removes it, and the settings
// and the profile survive saving and loading.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "core/Timeline.h"
#include "render/NoiseReduction.h"
#include "ui/common/Resources.h"
#include "ui/effects/EffectActions.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <random>

using namespace kk;
using namespace kk::test;

namespace {

const TrackRef kA1 {TrackType::Audio, 0};

/// A 48 kHz mono 16-bit WAV of @p seconds of white noise.
bool writeNoiseWav(const QString& path, int seconds)
{
    const quint32 frames = quint32(48000 * seconds);
    QByteArray bytes;
    auto u32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i)
            bytes.append(char((v >> (8 * i)) & 0xff));
    };
    auto u16 = [&](quint16 v) {
        bytes.append(char(v & 0xff));
        bytes.append(char(v >> 8));
    };
    bytes.append("RIFF");
    u32(36 + frames * 2);
    bytes.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(48000);
    u32(96000);
    u16(2);
    u16(16);
    bytes.append("data");
    u32(frames * 2);
    std::mt19937 rng(5);
    std::normal_distribution<double> gauss(0.0, 0.03);
    for (quint32 n = 0; n < frames; ++n)
        u16(quint16(qint16(std::lround(gauss(rng) * 32767.0))));
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

ParamGroupBox* groupTitled(InspectorPanel& panel, const QString& title)
{
    for (ParamGroupBox* g : panel.groups()) {
        if (g->findChild<QLabel*>(QStringLiteral("InspectorGroupTitle"))->text() == title)
            return g;
    }
    return nullptr;
}

} // namespace

class TestNoiseReductionUi : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
        QVERIFY(m_dir.isValid());
    }

    // Spec 132.1: Add Effect puts Noise Reduction on an audio clip (one undo step) with the
    // defaults: Auto Speech, RNNoise.
    void theEffectGoesOnAnAudioClip()
    {
        PanelFixture f;
        const quint64 a = f.addClip(kA1, 0, 50);
        const int commands = f.project.undoStack()->count();
        QCOMPARE(effectactions::applyEffect(f.project, {a}, QLatin1String(afxid::NoiseReduction)), 1);
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        const EffectInstance& e = f.clip(a)->effects[0];
        QCOMPARE(e.params.staticValue(QLatin1String(afxkeys::NrMode)).toString(), QStringLiteral("auto"));
        QCOMPARE(e.params.staticValue(QLatin1String(afxkeys::NrEngine)).toString(), QStringLiteral("rnnoise"));
        QCOMPARE(e.params.value(QLatin1String(afxkeys::NrReductionDb), 0, -1.0), 20.0);
        QCOMPARE(e.params.value(QLatin1String(afxkeys::Mix), 0, -1.0), 100.0);
        // Not on a video clip.
        const quint64 v = f.addClip({TrackType::Video, 0}, 0, 50, ClipKind::Solid);
        QCOMPARE(effectactions::applyEffect(f.project, {v}, QLatin1String(afxid::NoiseReduction)), 0);
    }

    // Spec 132.2: the Inspector shows every control with a tooltip, the AI label and the Learn
    // buttons.
    void inspectorShowsTheControlsWithTooltips()
    {
        PanelFixture f;
        const quint64 a = f.addClip(kA1, 0, 50);
        effectactions::applyEffect(f.project, {a}, QLatin1String(afxid::NoiseReduction));
        InspectorPanel panel(f.ctx);
        panel.resize(420, 900);
        panel.show();
        f.selection.selectClip(a);
        QCoreApplication::processEvents();
        ParamGroupBox* group = groupTitled(panel, QStringLiteral("Noise Reduction"));
        QVERIFY(group);
        const EffectDescriptor* d = EffectRegistry::instance().find(QLatin1String(afxid::NoiseReduction));
        QVERIFY(d);
        for (const ParamDescriptor& p : d->params) {
            if (p.hidden)
                continue;
            QLabel* label = nullptr;
            for (QLabel* l : group->findChildren<QLabel*>()) {
                if (l->text() == p.label)
                    label = l;
            }
            QVERIFY2(label, qPrintable(p.label));
            QVERIFY2(!label->toolTip().isEmpty(), qPrintable(p.label));
        }
        QVERIFY(group->findChild<QLabel*>(QStringLiteral("AiLabel")));
        QVERIFY(group->findChild<QLabel*>(QStringLiteral("AiLabel"))->text().contains(QStringLiteral("Optional AI")));
        QPushButton* learn = group->findChild<QPushButton*>(QStringLiteral("NrLearn"));
        QPushButton* clear = group->findChild<QPushButton*>(QStringLiteral("NrClear"));
        QVERIFY(learn && clear);
        QVERIFY(!learn->toolTip().isEmpty());
        QVERIFY(!clear->toolTip().isEmpty());
        QVERIFY(!clear->isEnabled()); // nothing learned yet
    }

    // Spec 132.3: Learn takes the noise of the marked range, switches to Manual Learn, is one
    // undo step; Clear Profile removes it.
    void learnKeepsAProfileFromTheMarkedRange()
    {
        PanelFixture f;
        const QString wav = m_dir.filePath(QStringLiteral("noise.wav"));
        QVERIFY(writeNoiseWav(wav, 3));
        MediaItem item;
        item.id = QStringLiteral("m1");
        item.path = wav;
        item.name = QStringLiteral("noise");
        f.project.mediaPool()->addItem(item);
        const quint64 a = f.addClip(kA1, 0, 75);
        effectactions::applyEffect(f.project, {a}, QLatin1String(afxid::NoiseReduction));
        f.project.beginEdit(QStringLiteral("marks"));
        f.project.timeline()->mutableData().markIn = 10;
        f.project.timeline()->mutableData().markOut = 35; // one second
        f.project.endEdit();
        InspectorPanel panel(f.ctx);
        panel.resize(420, 900);
        panel.show();
        f.selection.selectClip(a);
        QCoreApplication::processEvents();
        ParamGroupBox* group = groupTitled(panel, QStringLiteral("Noise Reduction"));
        QVERIFY(group);
        QPushButton* learn = group->findChild<QPushButton*>(QStringLiteral("NrLearn"));
        QPushButton* clear = group->findChild<QPushButton*>(QStringLiteral("NrClear"));
        QVERIFY(learn && clear);
        const int commands = f.project.undoStack()->count();
        learn->click();
        QCoreApplication::processEvents();
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        const EffectInstance& e = f.clip(a)->effects[0];
        const auto profile = dsp::noise::decodeProfile(
            e.params.staticValue(QLatin1String(afxkeys::NrProfile)).toString());
        QVERIFY(profile.valid());
        QCOMPARE(e.params.staticValue(QLatin1String(afxkeys::NrMode)).toString(), QStringLiteral("learn"));
        // White noise of sigma 0.03: about 10 log10(0.03^2 * 512) = -3.4 dB in every bin.
        double mean = 0;
        for (int k = 20; k < dsp::noise::kBins - 20; ++k)
            mean += profile.powerDb[size_t(k)];
        mean /= dsp::noise::kBins - 40;
        QVERIFY2(std::abs(mean - (-3.4)) < 1.0, qPrintable(QString::number(mean)));
        QVERIFY(group->findChild<QLabel*>(QStringLiteral("NrStatus"))->text().contains(QStringLiteral("learned")));
        QVERIFY(clear->isEnabled());
        f.project.undo();
        QVERIFY(f.clip(a)->effects[0].params.staticValue(QLatin1String(afxkeys::NrProfile)).toString().isEmpty());
        f.project.redo();
        clear->click();
        QCoreApplication::processEvents();
        QVERIFY(f.clip(a)->effects[0].params.staticValue(QLatin1String(afxkeys::NrProfile)).toString().isEmpty());
    }

    // Spec 132.4: the settings and the profile survive saving and loading a project.
    void settingsAndProfileSurviveSaveAndLoad()
    {
        PanelFixture f;
        const quint64 a = f.addClip(kA1, 0, 50);
        effectactions::applyEffect(f.project, {a}, QLatin1String(afxid::NoiseReduction));
        std::vector<float> noise(2 * 48000);
        std::mt19937 rng(3);
        std::normal_distribution<float> g(0.0f, 0.05f);
        for (float& x : noise)
            x = g(rng);
        const QString text = dsp::noise::encodeProfile(dsp::noise::learnProfile(noise.data(), 48000));
        QVERIFY(!text.isEmpty());
        f.project.beginEdit(QStringLiteral("edit"));
        EffectInstance& e = editops::clipById(f.project.timeline()->mutableData(), a)->effects[0];
        e.params.setStatic(QLatin1String(afxkeys::NrMode), QStringLiteral("learn"));
        e.params.setStatic(QLatin1String(afxkeys::NrEngine), QStringLiteral("gate"));
        e.params.setStatic(QLatin1String(afxkeys::NrProfile), text);
        e.params.setBase(afxkeys::NrReductionDb, 33.5);
        e.params.setBase(afxkeys::Mix, 64.0);
        f.project.endEdit();
        const QVector<EffectInstance> before = f.clip(a)->effects;
        const QString path = m_dir.filePath(QStringLiteral("nr.kkproj"));
        QVERIFY(f.project.save(path));
        Project loaded;
        QString error;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        const ClipData* back = editops::clipById(loaded.timeline()->data(), a);
        QVERIFY(back);
        QCOMPARE(back->effects.size(), 1);
        QVERIFY(back->effects == before);
        QCOMPARE(back->effects[0].params.staticValue(QLatin1String(afxkeys::NrProfile)).toString(), text);
        QCOMPARE(back->effects[0].params.value(QLatin1String(afxkeys::NrReductionDb), 0, 0.0), 33.5);
    }
};

QTEST_MAIN(TestNoiseReductionUi)
#include "test_noisereductionui.moc"
