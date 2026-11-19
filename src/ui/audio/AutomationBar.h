// SPDX-License-Identifier: GPL-3.0-or-later
// AutomationBar — the Audio page's Automation toolbar: one button per automation mode (Off,
// Read, Touch, Latch, Write, Trim) that sets the mode of every audio track at once (spec 139).
#pragma once

#include "core/MixerData.h"

#include <QWidget>

class QToolButton;

namespace kk {

class MixerController;

/// A strip of exclusive mode buttons. The button of the mode all audio tracks share is down;
/// when the tracks differ none is. A click sets the mode of every audio track in one undo step
/// (a single track's mode is chosen on its mixer strip).
/// Thread-safety: GUI thread only.
class AutomationBar : public QWidget
{
    Q_OBJECT
public:
    explicit AutomationBar(MixerController* controller, QWidget* parent = nullptr);

    /// The button of @p mode (tests).
    [[nodiscard]] QToolButton* button(AutomationMode mode) const;
    /// The modes in button order.
    [[nodiscard]] static const QVector<AutomationMode>& modes();

private:
    void refresh();
    void choose(AutomationMode mode);

    MixerController* m_controller;
    QVector<QToolButton*> m_buttons;
};

} // namespace kk
