// SPDX-License-Identifier: GPL-3.0-or-later
// MixerStrip — one channel strip of the mixer: input, EQ, dynamics, bus routing, pan,
// automation mode, record arm / mute / solo, fader with meter, name and channel format.
#pragma once

#include "core/MixerOps.h"

#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>
#include <optional>

class QComboBox;
class QLabel;
class QToolButton;
class QVBoxLayout;

namespace kk {

class DynamicsWindow;
class EqWindow;
class FaderWidget;
class LevelMeter;
class MeterModel;
class MixerController;
class PanKnob;

/// A vertical channel strip for a track or a bus (top to bottom, Resolve's order): input,
/// EQ and Dynamics buttons (lit when on; a click opens the small editor window), bus
/// assignment (tracks: a menu of checkable buses; buses: "Output" makes the bus the one heard
/// and rendered), pan knob and automation mode (tracks), record arm (voice-over, spec 75;
/// right-click: the input; a level bar shows while armed) / mute / solo, the fader with its meter,
/// the fader value, and the name with the channel format (right-click: Mono / Stereo; buses also
/// Rename / Delete).
///
/// Tracks also have an Effects section under Dyn (spec 133): a "+" button that lists the audio
/// effects, and one slot per effect of TrackMix::effects with a bypass button, the effect's name
/// (double-click opens its controls in the Inspector) and a remove button; an effect dragged from
/// the Effects Library onto the strip is added at the end.
///
/// Every gesture is one undo step through MixerController. The strip re-reads the timeline on
/// MixerController::mixChanged() and follows the playhead (automated faders move).
///
/// Thread-safety: GUI thread only.
class MixerStrip : public QWidget
{
    Q_OBJECT
public:
    MixerStrip(MixerController* controller, MeterModel* meters, const MixTarget& target,
        QWidget* parent = nullptr);

    [[nodiscard]] const MixTarget& target() const { return m_target; }
    [[nodiscard]] FaderWidget* fader() const { return m_fader; }
    [[nodiscard]] PanKnob* panKnob() const { return m_pan; }
    [[nodiscard]] QToolButton* eqButton() const { return m_eq; }
    [[nodiscard]] QToolButton* dynamicsButton() const { return m_dynamics; }
    [[nodiscard]] QToolButton* muteButton() const { return m_mute; }
    [[nodiscard]] QToolButton* soloButton() const { return m_solo; }
    [[nodiscard]] QToolButton* armButton() const { return m_arm; }
    [[nodiscard]] QToolButton* busButton() const { return m_bus; }
    [[nodiscard]] QComboBox* automationCombo() const { return m_automation; }
    /// The "+" button of the Effects section (tracks only; nullptr on buses).
    [[nodiscard]] QToolButton* addEffectButton() const { return m_addEffect; }
    /// Number of effect slots shown.
    [[nodiscard]] int effectSlotCount() const { return int(m_slotIds.size()); }
    /// Adds the audio effect @p type to the end of the track's slots (one undo step); the id of
    /// the new slot, 0 if refused. What the "+" menu and a drop from the Effects Library do.
    quint64 addEffect(const QString& type);
    /// The selected strip is drawn with a frame; selection itself is kept by the MixerPanel.
    void setSelected(bool selected);
    [[nodiscard]] bool isSelected() const { return m_selected; }

    /// Opens (or raises) the EQ / dynamics window of this strip.
    EqWindow* openEq();
    DynamicsWindow* openDynamics();

    /// Re-reads the strip's values at timeline frame @p frame.
    void refresh(int64_t frame);

    /// Strip width (px).
    static constexpr int kWidth = 92;

    /// Asks for a new name (a double-click on the strip's name, or the menu's Rename), given
    /// the current one; nothing = cancelled. The default is an input dialog; tests replace it.
    using NamePrompt = std::function<std::optional<QString>(const QString& current)>;
    void setNamePrompt(NamePrompt prompt) { m_namePrompt = std::move(prompt); }
    /// Renames the track or bus of this strip (one undo step; empty = the automatic name).
    void renameTarget(const QString& name);
    /// Asks for the name and renames.
    void askRename();

signals:
    /// The strip was clicked (or one of its effect controls was used): the Inspector shows its
    /// track's effects.
    void selectRequested(const MixTarget& target);
    /// A slot was double-clicked: its controls should open (Inspector).
    void effectOpenRequested(const MixTarget& target, quint64 effectId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void fillEffectMenu();
    void rebuildSlots(const TimelineData& d);
    void fillBusMenu();
    void showNameMenu(const QPoint& globalPos);

    MixerController* m_controller;
    MeterModel* m_meters;
    MixTarget m_target;
    QLabel* m_input = nullptr;
    QToolButton* m_eq = nullptr;
    QToolButton* m_dynamics = nullptr;
    QToolButton* m_bus = nullptr;
    PanKnob* m_pan = nullptr;
    QComboBox* m_automation = nullptr;
    QToolButton* m_addEffect = nullptr;
    QVBoxLayout* m_slotLayout = nullptr;
    QVector<quint64> m_slotIds; ///< effect ids of the slots shown
    QVector<bool> m_slotOn; ///< their enabled state at the last refresh
    bool m_selected = false;
    QToolButton* m_arm = nullptr;
    QWidget* m_inputLevel = nullptr; ///< level of the microphone, shown while armed
    QToolButton* m_mute = nullptr;
    QToolButton* m_solo = nullptr;
    FaderWidget* m_fader = nullptr;
    LevelMeter* m_meter = nullptr;
    QLabel* m_value = nullptr;
    QLabel* m_name = nullptr;
    NamePrompt m_namePrompt;
    int64_t m_frame = 0; ///< frame of the last refresh()
    QPointer<EqWindow> m_eqWindow;
    QPointer<DynamicsWindow> m_dynamicsWindow;
};

} // namespace kk
