// SPDX-License-Identifier: GPL-3.0-or-later
// VoiceIsolation — the Voice Isolation toggle and Amount of a clip or a track (spec 141). They
// are one audio effect ("audio_voice_isolation", the Isolator of render/NoiseReduction.h) kept
// first in the effect list of the clip (ClipData::effects) or the track (TrackMix::effects), so
// the same stack, latency alignment and save format serve the Inspector, the strip and the
// Effects tab.
#pragma once

#include "core/TimelineData.h"

#include <QVector>

namespace kk::voiceisolation {

inline constexpr double kDefaultAmount = 100.0;

/// The Voice Isolation effect of @p effects, or nullptr.
[[nodiscard]] const EffectInstance* find(const QVector<EffectInstance>& effects);
/// True if it is there and switched on.
[[nodiscard]] bool isOn(const QVector<EffectInstance>& effects);
/// Its Amount (percent); kDefaultAmount when there is none.
[[nodiscard]] double amount(const QVector<EffectInstance>& effects);
/// Switches it on or off. Switching on adds the effect at the front of the list when there is
/// none (with the new id @p newId, from TimelineData::allocateId()); switching off only bypasses
/// it, so its Amount is kept. Returns whether anything changed.
bool setOn(QVector<EffectInstance>& effects, bool on, quint64 newId);
/// Sets Amount (clamped to 0 .. 100). Adds the effect, switched off, when there is none.
/// Returns whether anything changed.
bool setAmount(QVector<EffectInstance>& effects, double percent, quint64 newId);

} // namespace kk::voiceisolation
