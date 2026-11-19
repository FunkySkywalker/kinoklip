// SPDX-License-Identifier: GPL-3.0-or-later
// SendsWindow — the small floating editor of a mixer channel's sends (spec 137).
#pragma once

#include "core/MixerOps.h"

#include <QVector>
#include <QWidget>

#include <functional>

class QComboBox;
class QGridLayout;
class QPushButton;
class QLabel;

namespace kk {

class MixerController;

/// Tool window listing the sends of a track or a bus: per send an on / off switch, the
/// destination bus, the level (dB), Pre / Post fader, the send pan and a remove button; below,
/// a menu of the buses the strip may still send to and an Add button. The menu lists only
/// buses mixerops::sendAllowed() accepts, so a send that would make a loop (a bus feeding
/// itself through other buses, a strip sending to a bus it depends on) cannot be chosen.
/// Every change is one undo step; the window follows the timeline and closes itself when its
/// track or bus is deleted.
///
/// Thread-safety: GUI thread only.
class SendsWindow : public QWidget
{
    Q_OBJECT
public:
    SendsWindow(MixerController* controller, const MixTarget& target, QWidget* parent = nullptr);

    [[nodiscard]] const MixTarget& target() const { return m_target; }
    /// Number of send rows shown.
    [[nodiscard]] int rowCount() const { return int(m_rows.size()); }
    /// The menu of buses a send can still be added to, and its Add button.
    [[nodiscard]] QComboBox* addCombo() const { return m_addBus; }
    [[nodiscard]] QPushButton* addButton() const { return m_add; }
    /// Adds a send to bus @p busId (what the Add button does); false if refused.
    bool addSendTo(int busId);

private:
    void refresh();
    void rebuildRows(const ChannelStrip& strip);
    void editSend(int busId, const QString& label, const std::function<void(SendData&)>& change);

    MixerController* m_controller;
    MixTarget m_target;
    QGridLayout* m_grid = nullptr;
    QLabel* m_empty = nullptr;
    QComboBox* m_addBus = nullptr;
    QPushButton* m_add = nullptr;
    struct Row
    {
        int busId = 0;
        QWidget* on = nullptr;
        QWidget* level = nullptr;
        QWidget* mode = nullptr;
        QWidget* pan = nullptr;
        QWidget* remove = nullptr;
    };
    QVector<Row> m_rows;
};

} // namespace kk
