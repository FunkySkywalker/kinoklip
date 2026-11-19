// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorVoiceIsolation — the Voice Isolation section of the Audio tab (spec 141): a switch
// and an Amount (0 - 100). They edit the clip's "Voice Isolation" audio effect (core/
// VoiceIsolation.h), so the same effect also shows in the Effects tab. Part of InspectorPanel.
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "core/VoiceIsolation.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>

namespace kk {

void InspectorPanel::buildVoiceIsolationSection(quint64 audioClipId)
{
    ParamGroupBox* group
        = addGroup(AudioTab, QStringLiteral("voice_isolation"), tr("Voice Isolation"), audioClipId);

    auto* toggle = new QCheckBox(tr("Voice Isolation (Optional AI)"));
    toggle->setObjectName(QStringLiteral("VoiceIsolationToggle"));
    toggle->setToolTip(tr("Keep the voice of this clip and take out noise, music and room sound. "
                          "Optional AI: the RNNoise model is built into Kinoklip and runs on "
                          "this computer; nothing is downloaded or uploaded."));
    group->addWidgetRow(QString(), toggle);

    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setObjectName(QStringLiteral("VoiceIsolationSlider"));
    slider->setRange(0, 100);
    auto* spin = new QSpinBox;
    spin->setObjectName(QStringLiteral("VoiceIsolationAmount"));
    spin->setRange(0, 100);
    spin->setSuffix(QStringLiteral(" %"));
    spin->setKeyboardTracking(false);
    const QString amountTip = tr("Amount: how strongly everything that is not a voice is taken "
                                 "away. 0 leaves the sound as it is, 100 removes as much as "
                                 "possible.");
    slider->setToolTip(amountTip);
    spin->setToolTip(amountTip);
    layout->addWidget(slider, 1);
    layout->addWidget(spin);
    group->addWidgetRow(tr("Amount"), row);

    auto commitOn = [this, audioClipId](bool on) {
        editClip(on ? tr("Voice Isolation On") : tr("Voice Isolation Off"), audioClipId,
            [this, on](ClipData& c) {
                const bool exists = voiceisolation::find(c.effects) != nullptr;
                voiceisolation::setOn(c.effects, on,
                    exists || !on ? 0 : m_ctx.project->timeline()->mutableData().allocateId());
            });
    };
    auto commitAmount = [this, audioClipId](int value) {
        editClip(tr("Voice Isolation Amount"), audioClipId, [this, value](ClipData& c) {
            const bool exists = voiceisolation::find(c.effects) != nullptr;
            voiceisolation::setAmount(c.effects, value,
                exists ? 0 : m_ctx.project->timeline()->mutableData().allocateId());
        });
    };
    connect(toggle, &QCheckBox::clicked, this, commitOn);
    connect(spin, &QSpinBox::valueChanged, this, commitAmount);
    // The slider follows the drag in the number box and commits once, on release.
    connect(slider, &QSlider::valueChanged, this, [slider, spin, commitAmount](int v) {
        if (slider->isSliderDown()) {
            const QSignalBlocker block(spin);
            spin->setValue(v);
        } else if (spin->value() != v) {
            commitAmount(v); // keyboard or wheel
        }
    });
    connect(slider, &QSlider::sliderReleased, this, [slider, commitAmount] { commitAmount(slider->value()); });

    group->addHeaderRefresh([toggle, slider, spin](const ClipData& c) {
        const bool on = voiceisolation::isOn(c.effects);
        const int amount = int(voiceisolation::amount(c.effects));
        if (toggle->isChecked() != on) {
            const QSignalBlocker b(toggle);
            toggle->setChecked(on);
        }
        if (!slider->isSliderDown() && (slider->value() != amount || spin->value() != amount)) {
            const QSignalBlocker b1(slider);
            const QSignalBlocker b2(spin);
            slider->setValue(amount);
            spin->setValue(amount);
        }
    });
}

} // namespace kk
