// SPDX-License-Identifier: GPL-3.0-or-later
// EffectInstance — one effect on a clip or on a mixer track (its own header so MixerData.h can
// hold the effect slots of a track without including TimelineData.h).
#pragma once

#include "core/Animation.h"

#include <QString>

namespace kk {

/// An effect applied to a clip. `type` is an EffectRegistry descriptor id (e.g. "gaussian_blur").
struct EffectInstance
{
    quint64 id = 0; ///< unique within the timeline (allocated with TimelineData::allocateId())
    QString type;
    bool enabled = true;
    ParamSet params; ///< keys from the descriptor's ParamDescriptor list

    friend bool operator==(const EffectInstance&, const EffectInstance&) = default;
};

} // namespace kk
