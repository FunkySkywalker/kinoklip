// SPDX-License-Identifier: GPL-3.0-or-later
// EqWindow — the small floating editor of a mixer channel's 6-band EQ.
#pragma once

#include "core/MixerOps.h"

#include <QWidget>

#include <array>
#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QToolButton;
class QDoubleSpinBox;

namespace kk {

class EqCurveView;
class MixerController;

/// Tool window with the response curve (EqCurveView, handles draggable) and per band a type,
/// on / off, frequency, gain and Q. Every gesture is one undo step: a handle drag from press
/// to release, each committed field value, each switch. Follows the timeline (undo, the
/// strip's own buttons) and closes itself when its track or bus is deleted.
///
/// Thread-safety: GUI thread only.
class EqWindow : public QWidget
{
    Q_OBJECT
public:
    EqWindow(MixerController* controller, const MixTarget& target, QWidget* parent = nullptr);

    [[nodiscard]] const MixTarget& target() const { return m_target; }
    [[nodiscard]] EqCurveView* curve() const { return m_curve; }
    /// The Presets button (a menu: the factory presets, the user's, Save as Preset, Delete).
    [[nodiscard]] QToolButton* presetButton() const { return m_presets; }

    /// Applies factory or user preset @p name to the strip (one undo step); false if unknown.
    bool applyPreset(const QString& name);
    /// Saves the strip's EQ as the user preset @p name (refused for an empty or factory name).
    bool saveAsPreset(const QString& name);
    /// Deletes the user preset @p name.
    bool deletePreset(const QString& name);

    /// Asks for the name of a new preset (default: an input dialog; tests replace it).
    using NamePrompt = std::function<std::optional<QString>()>;
    void setNamePrompt(NamePrompt prompt) { m_namePrompt = std::move(prompt); }

private:
    void refresh();
    void fillPresetMenu();
    /// One undo step changing band @p band of the target's EQ with @p change.
    void editBand(int band, const QString& label, const std::function<void(EqBand&)>& change);

    MixerController* m_controller;
    MixTarget m_target;
    EqCurveView* m_curve = nullptr;
    QCheckBox* m_enabled = nullptr;
    QToolButton* m_presets = nullptr;
    NamePrompt m_namePrompt;
    struct BandRow
    {
        QCheckBox* on = nullptr;
        QComboBox* type = nullptr;
        QDoubleSpinBox* frequency = nullptr;
        QDoubleSpinBox* gain = nullptr;
        QDoubleSpinBox* q = nullptr;
    };
    std::array<BandRow, kEqBandCount> m_rows;
};

} // namespace kk
