// SPDX-License-Identifier: GPL-3.0-or-later
// AutomationBar implementation.
#include "ui/audio/AutomationBar.h"

#include "core/MixerOps.h"
#include "ui/audio/MixerController.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

namespace kk {

const QVector<AutomationMode>& AutomationBar::modes()
{
    static const QVector<AutomationMode> all {AutomationMode::Off, AutomationMode::Read,
        AutomationMode::Touch, AutomationMode::Latch, AutomationMode::Write, AutomationMode::Trim};
    return all;
}

AutomationBar::AutomationBar(MixerController* controller, QWidget* parent)
    : QWidget(parent)
    , m_controller(controller)
{
    setObjectName(QStringLiteral("AutomationBar"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 2);
    layout->setSpacing(2);
    auto* title = new QLabel(tr("Automation"));
    title->setToolTip(tr("What the audio tracks do with their automation curves (volume, pan, "
                         "mute, sends and effect parameters)"));
    layout->addWidget(title);

    struct Entry
    {
        AutomationMode mode;
        const char* name;
        const char* tip;
    };
    const Entry entries[] = {
        {AutomationMode::Off, QT_TR_NOOP("Off"),
            QT_TR_NOOP("Off: the curves are ignored and the fader and knobs keep their own values")},
        {AutomationMode::Read, QT_TR_NOOP("Read"),
            QT_TR_NOOP("Read: the curves play; the faders and knobs follow them")},
        {AutomationMode::Touch, QT_TR_NOOP("Touch"),
            QT_TR_NOOP("Touch: while playing, moving a control records it; on release it returns "
                       "to the existing curve")},
        {AutomationMode::Latch, QT_TR_NOOP("Latch"),
            QT_TR_NOOP("Latch: like Touch, but after the release the last value keeps being "
                       "recorded until playback stops")},
        {AutomationMode::Write, QT_TR_NOOP("Write"),
            QT_TR_NOOP("Write: while playing, the values of the controls are recorded all the "
                       "time and replace the curve")},
        {AutomationMode::Trim, QT_TR_NOOP("Trim"),
            QT_TR_NOOP("Trim: while playing, moving a control adds the change as an offset on "
                       "top of the existing curve")},
    };
    for (const Entry& e : entries) {
        auto* b = new QToolButton;
        b->setObjectName(QStringLiteral("AutomationMode_") + automationModeToString(e.mode));
        b->setText(tr(e.name));
        b->setToolTip(tr(e.tip));
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(b);
        m_buttons.append(b);
        connect(b, &QToolButton::clicked, this, [this, mode = e.mode] { choose(mode); });
    }
    layout->addStretch(1);
    connect(m_controller, &MixerController::mixChanged, this, &AutomationBar::refresh);
    refresh();
}

QToolButton* AutomationBar::button(AutomationMode mode) const
{
    return m_buttons.value(int(modes().indexOf(mode)), nullptr);
}

void AutomationBar::refresh()
{
    const TimelineData& d = m_controller->data();
    std::optional<AutomationMode> common;
    bool same = !d.audioTracks.empty();
    for (const TrackData& t : d.audioTracks) {
        if (!common)
            common = t.mix.automation;
        else if (*common != t.mix.automation)
            same = false;
    }
    for (int i = 0; i < m_buttons.size(); ++i)
        m_buttons[i]->setChecked(same && common && *common == modes()[i]);
}

void AutomationBar::choose(AutomationMode mode)
{
    m_controller->edit(tr("Automation Mode"), [mode](TimelineData& d) {
        bool changed = false;
        for (const TrackData& t : d.audioTracks)
            changed |= mixerops::setAutomationMode(d, t.id, mode);
        return changed;
    });
    refresh(); // a click on the lit button must not turn it off
}

} // namespace kk
