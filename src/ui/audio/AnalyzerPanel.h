// SPDX-License-Identifier: GPL-3.0-or-later
// AnalyzerPanel — the Audio page's analyser and monitor panel (spec 141): the spectrum of the
// main mix or of one strip (log frequency, dB, peak hold, slope and smoothing), the phase
// correlation meter and a goniometer of the main mix, and the monitor controls Dim, Mono and
// Mute that change what the speakers play but never a render.
#pragma once

#include "ui/audio/SpectrumDisplay.h"
#include "ui/common/PanelFrame.h"

#include <QElapsedTimer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QToolButton;

namespace kk {

struct AppContext;
class MeterModel;
class MixerController;

/// The spectrum bars (log frequency 20 Hz - 20 kHz, -90 ... 0 dB) of a SpectrumDisplay.
class SpectrumView : public QWidget
{
    Q_OBJECT
public:
    static constexpr double kFloorDb = -90.0;
    explicit SpectrumView(const SpectrumDisplay* display, QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override { return {360, 140}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {160, 70}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    const SpectrumDisplay* m_display;
};

/// A horizontal bar from -1 (out of phase) through 0 to +1 (mono) with the value marked.
class CorrelationBar : public QWidget
{
    Q_OBJECT
public:
    explicit CorrelationBar(QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override { return {160, 26}; }
    void setValue(double value);
    [[nodiscard]] double value() const { return m_value; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double m_value = 0.0;
};

/// The stereo picture of the last block: mono is a vertical line, out of phase a horizontal one.
class GoniometerView : public QWidget
{
    Q_OBJECT
public:
    explicit GoniometerView(QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override { return {120, 120}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {70, 70}; }
    void setPoints(const float* xy, int count);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<QPointF> m_points; ///< normalised to -1 ... 1
};

class AnalyzerPanel : public PanelFrame
{
    Q_OBJECT
public:
    AnalyzerPanel(AppContext& ctx, MixerController* controller, MeterModel* meters,
        QWidget* parent = nullptr);

    [[nodiscard]] SpectrumDisplay& display() { return m_display; }
    [[nodiscard]] QComboBox* sourceBox() const { return m_source; }
    [[nodiscard]] QCheckBox* peakHoldBox() const { return m_hold; }
    [[nodiscard]] QComboBox* slopeBox() const { return m_slope; }
    [[nodiscard]] QComboBox* smoothingBox() const { return m_smoothing; }
    [[nodiscard]] CorrelationBar* correlationBar() const { return m_correlation; }
    [[nodiscard]] QToolButton* dimButton() const { return m_dim; }
    [[nodiscard]] QDoubleSpinBox* dimSpin() const { return m_dimDb; }
    [[nodiscard]] QToolButton* monoButton() const { return m_mono; }
    [[nodiscard]] QToolButton* muteButton() const { return m_mute; }

private:
    void syncSources();
    void applySource();
    void onMeters();
    void saveDisplaySettings() const;

    AppContext& m_ctx;
    MixerController* m_controller;
    MeterModel* m_meters;
    SpectrumDisplay m_display;
    SpectrumView* m_view = nullptr;
    QComboBox* m_source = nullptr;
    QCheckBox* m_hold = nullptr;
    QComboBox* m_slope = nullptr;
    QComboBox* m_smoothing = nullptr;
    CorrelationBar* m_correlation = nullptr;
    class QLabel* m_correlationLabel = nullptr;
    GoniometerView* m_gonio = nullptr;
    QToolButton* m_dim = nullptr;
    QDoubleSpinBox* m_dimDb = nullptr;
    QToolButton* m_mono = nullptr;
    QToolButton* m_mute = nullptr;
    QElapsedTimer m_clock;
    qint64 m_lastMs = 0;
    bool m_updating = false;
};

} // namespace kk
