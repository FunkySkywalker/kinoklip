// SPDX-License-Identifier: GPL-3.0-or-later
// LoudnessPanel — the Audio page's monitoring panel: output and bus meters, BS.1770 / EBU
// R128 loudness (momentary, short-term, integrated) and true peak of the output bus.
#pragma once

#include "render/LoudnessStandards.h"
#include "ui/common/PanelFrame.h"

#include <QVector>

class QComboBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QToolButton;

namespace kk {

struct AppContext;
class LevelMeter;
class MeterModel;
class MixerController;

/// Vertical loudness bars (momentary, short-term) on a -50 ... 0 LUFS scale with the target
/// line; within +-1 LU of the target the bars use the scheme's positive colour, above it the
/// negative one.
class LoudnessGauge : public QWidget
{
    Q_OBJECT
public:
    explicit LoudnessGauge(MeterModel* meters, QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override { return {90, 120}; }
    /// The target line and the on-target colours follow this value (LUFS).
    void setTargetLufs(double lufs);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    MeterModel* m_meters;
    double m_target = -23.0;
};

/// Scrolling history of the short-term loudness over time (one point every kSampleMs while the
/// measurement runs, the last kMaxPoints kept), against the dashed target line; the colours
/// follow the gauge (within +-1 LU of the target: positive colour).
class LoudnessHistoryGraph : public QWidget
{
    Q_OBJECT
public:
    static constexpr int kMaxPoints = 600; ///< 5 minutes at one point per 500 ms
    static constexpr int kSampleMs = 500;

    explicit LoudnessHistoryGraph(QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override { return {240, 100}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {120, 60}; }

    /// Appends a short-term loudness (LUFS; <= -199 = silence) and drops the oldest point
    /// beyond kMaxPoints.
    void addSample(double lufs);
    /// Forgets the history.
    void clear();
    void setTargetLufs(double lufs);
    [[nodiscard]] const QVector<float>& samples() const { return m_samples; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<float> m_samples;
    double m_target = -23.0;
};

/// The monitoring panel: the output bus meter with a dB scale, a small meter per bus, the
/// loudness gauge and readouts (M / S / I in LUFS, loudness range in LU, true peak max in dBTP),
/// the chosen standard's target with an over / under read-out, a history graph of the short-term
/// loudness against the target line, Start / Pause (the measurement stops and continues; the
/// readings stay) and Reset (the registry action "audio.reset_loudness"). The standard (EBU R128
/// -23 LUFS, ATSC A/85 -24, YouTube / streaming -14, Podcast -16, Custom) is stored in the
/// settings. Loudness is measured on the output bus by the playback engine
/// (render/LoudnessMeter.h).
///
/// Thread-safety: GUI thread only.
class LoudnessPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// EBU R128 programme target (LUFS), the default standard.
    static constexpr double kTargetLufs = -23.0;

    LoudnessPanel(AppContext& ctx, MixerController* controller, MeterModel* meters,
        QWidget* parent = nullptr);

    /// The readout labels (tests): momentary, short-term, integrated, range, true peak.
    [[nodiscard]] QString readoutText() const;
    /// The standard / target the meter compares with (stored in the settings).
    [[nodiscard]] loudness::Target target() const { return m_targetSetting; }
    void setTarget(const loudness::Target& target);
    /// Start / Pause of the measurement: false while paused.
    [[nodiscard]] bool measuring() const { return m_measuring; }
    void setMeasuring(bool on);
    [[nodiscard]] LoudnessHistoryGraph* history() const { return m_history; }
    [[nodiscard]] QComboBox* standardBox() const { return m_standard; }
    [[nodiscard]] QDoubleSpinBox* targetBox() const { return m_targetSpin; }
    [[nodiscard]] QToolButton* startPauseButton() const { return m_startPause; }
    /// The target / deviation label text (tests): "-23.0 LUFS: Over by 2.0 LU".
    [[nodiscard]] QString targetText() const;

private:
    void syncBuses();
    void updateReadouts();
    void updateTargetUi();
    void storeTarget() const;
    AppContext& m_ctx;

    MixerController* m_controller;
    MeterModel* m_meters;
    LevelMeter* m_output = nullptr;
    QHBoxLayout* m_busRow = nullptr;
    QVector<LevelMeter*> m_busMeters;
    QVector<QLabel*> m_busLabels;
    QVector<int> m_busIds;
    QLabel* m_momentary = nullptr;
    QLabel* m_shortTerm = nullptr;
    QLabel* m_integrated = nullptr;
    QLabel* m_truePeak = nullptr;
    QLabel* m_range = nullptr;
    QLabel* m_targetLabel = nullptr;
    QComboBox* m_standard = nullptr;
    QDoubleSpinBox* m_targetSpin = nullptr;
    QToolButton* m_startPause = nullptr;
    LoudnessGauge* m_gauge = nullptr;
    LoudnessHistoryGraph* m_history = nullptr;
    loudness::Target m_targetSetting;
    bool m_measuring = true;
    bool m_updatingUi = false;
    qint64 m_lastSampleMs = 0;
};

} // namespace kk
