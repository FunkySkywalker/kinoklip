// SPDX-License-Identifier: GPL-3.0-or-later
// MixerChannel — internal: the per-track / per-bus processing state of an AudioMixer
// (included by AudioMixer.cpp and AudioMixerGraph.cpp only).
#pragma once

#include "render/AudioClipEffects.h"
#include "render/AudioMixer.h"
#include "render/ChannelProcessor.h"

namespace kk {

/// DSP state of one mixer channel (a track or a bus) between mix calls.
struct AudioMixer::Channel
{
    dsp::ChannelProcessor processor;
    /// The track's effect slots (TrackMix::effects), run on the track's summed clips before
    /// the strip (spec 133), with the delay (frames) they were last run with and whether their
    /// memory is primed for the next contiguous block.
    dsp::ClipEffectChain effects;
    int effectsLatency = 0;
    bool effectsPrimed = false;
    /// Scratch copy of the effect slots with automated parameters filled in (spec 139).
    QVector<EffectInstance> effectsAuto;
    /// No sound and no filter / envelope tail: the channel's processing is skipped until a
    /// clip plays on it again.
    bool quiet = true;
    /// The channel's signal after its fader in this block, kept for the strips that take it as
    /// their key (spec 136); zeros when it did not sound. Only filled for key sources.
    std::vector<float> keyOut;
    /// Glide state of the strip's sends, one per destination bus (spec 137).
    std::vector<dsp::SendGainState> sends;
    qint64 lastUsedMs = 0; ///< for collectIdle() (deleted tracks / buses)
};

} // namespace kk
