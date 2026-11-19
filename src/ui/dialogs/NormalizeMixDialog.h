// SPDX-License-Identifier: GPL-3.0-or-later
// NormalizeMixDialog — "Normalize Audio Levels..." of an audio track, a bus or the main mix:
// mode (loudness, true peak, sample peak), loudness standard and target; measures the channel's
// whole mix over the In / Out range (or the whole timeline) off the GUI thread and moves its
// fader by one gain as one undo step (spec 134).
#pragma once

#include "core/MixerOps.h"
#include "render/AudioNormalize.h"
#include "render/LoudnessStandards.h"

#include <QDialog>
#include <QFuture>

#include <atomic>
#include <memory>
#include <optional>

class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QTimer;

namespace kk {

class Project;

/// Normalize dialog for one mixer channel. @p channel empty = the main mix (the output bus
/// fader). Normalize starts the measurement: a worker thread mixes the timeline's snapshot with
/// the real mixer (audionorm::measureMix()) while a progress bar follows; Cancel stops it and
/// leaves the project untouched. With the result the fader of the channel moves by
/// target - measured (mixerops::offsetFader(); a track's volume curve keeps its shape) in one
/// undo step "Normalize Audio Levels", and the dialog closes with Accepted. A silent mix is
/// reported and left alone.
///
/// Thread-safety: GUI thread; the worker only reads its own copy of the timeline.
class NormalizeMixDialog : public QDialog
{
    Q_OBJECT
public:
    NormalizeMixDialog(Project* project, std::optional<MixTarget> channel, QWidget* parent = nullptr);
    /// Waits for a running measurement (after cancelling it).
    ~NormalizeMixDialog() override;

    void setMode(audionorm::Mode mode);
    [[nodiscard]] audionorm::Mode mode() const;
    /// Loudness standard (Loudness mode); its preset target fills the target box.
    void setStandard(loudness::Standard standard);
    [[nodiscard]] loudness::Standard standard() const;
    /// Target level in the mode's unit (dBFS, dBTP, LUFS); a manual value selects Custom.
    void setTarget(double level);
    [[nodiscard]] double target() const;

    /// The Normalize button: starts measuring (no-op while running).
    void start();
    [[nodiscard]] bool isRunning() const;
    /// The last finished measurement, and the fader change applied (dB; NaN = none).
    [[nodiscard]] const audionorm::Measurement& measurement() const { return m_result; }
    [[nodiscard]] double appliedChange() const { return m_change; }
    /// Status line (tests).
    [[nodiscard]] QString statusText() const;

public slots:
    void reject() override;

private:
    void updateUnit();
    void pollProgress();
    void finishMeasurement();
    void setControlsEnabled(bool enabled);

    Project* m_project;
    std::optional<MixTarget> m_channel;
    QComboBox* m_mode = nullptr;
    QComboBox* m_standard = nullptr;
    QDoubleSpinBox* m_target = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_status = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QTimer* m_poll = nullptr;
    QFuture<audionorm::Measurement> m_future;
    struct Shared
    {
        std::atomic<bool> cancel {false};
        std::atomic<int> permille {0};
    };
    std::shared_ptr<Shared> m_shared;
    audionorm::Measurement m_result;
    double m_change = 0.0;
    bool m_updating = false;
};

/// Shows the dialog for a track (@p channel = MixTarget::track(id)), a bus, or the main mix
/// (no channel); returns whether the fader was changed. @p parent owns the dialog.
bool normalizeMix(Project* project, std::optional<MixTarget> channel, QWidget* parent);

} // namespace kk
