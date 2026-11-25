// SPDX-License-Identifier: GPL-3.0-or-later
// AudioPluginCore — the format-independent part of an instance: a Core runs one plugin on planar
// channels; ChannelAdapter turns Cores into an Instance for interleaved stereo (mono plugins as two
// Cores, mono-to-stereo and stereo-to-mono plugins, the block splitting). Internal to src/audioplug.
#pragma once

#include "audioplug/AudioPluginHost.h"

#include <vector>

namespace kk::audioplug {

/// One plugin instance on planar float channels.
class Core
{
public:
    virtual ~Core() = default;
    /// Runs @p frames (<= Instance::kMaxBlock) frames. @p in / @p out hold min(channels, 2)
    /// pointers (a mono plugin gets one); the plugin's other inputs get silence and its other
    /// outputs go to scratch. @p values: the parameter values by index (plugin units).
    virtual void run(const float* const* in, float* const* out, int frames, const double* values) = 0;
    virtual void reset() = 0;
    [[nodiscard]] virtual int latencyFrames() const = 0;
};

class ChannelAdapter final : public Instance
{
public:
    /// @p cores: one core of a stereo (or more) plugin, or one per channel of a mono plugin.
    /// @p inChannels / @p outChannels: the plugin's main bus channels (0 counts as 1).
    ChannelAdapter(std::vector<std::unique_ptr<Core>> cores, int inChannels, int outChannels, int paramCount,
        const std::vector<double>& defaults);

    void setParam(int index, double value) override
    {
        if (index >= 0 && size_t(index) < m_values.size())
            m_values[size_t(index)] = value;
    }
    void process(float* io, int frames) override;
    void reset() override;
    [[nodiscard]] int latencyFrames() const override;

private:
    std::vector<std::unique_ptr<Core>> m_cores;
    bool m_dual; ///< a mono plugin run once per channel
    int m_in, m_out;
    std::vector<double> m_values;
    std::vector<float> m_in0, m_in1, m_out0, m_out1; ///< kMaxBlock each
};

} // namespace kk::audioplug
