// SPDX-License-Identifier: GPL-3.0-or-later
// AudioPluginHost — see AudioPluginHost.h; ChannelAdapter of AudioPluginCore.h.
#include "audioplug/AudioPluginCore.h"

#include "audioplug/ClapHost.h"
#include "audioplug/Lv2Host.h"

#include <algorithm>
#include <cmath>

namespace kk::audioplug {

ChannelAdapter::ChannelAdapter(std::vector<std::unique_ptr<Core>> cores, int inChannels,
    int outChannels, int paramCount, const std::vector<double>& defaults)
    : m_cores(std::move(cores))
    , m_dual(m_cores.size() == 2)
    , m_in(std::max(inChannels, 1))
    , m_out(std::max(outChannels, 1))
    , m_values(size_t(paramCount), 0.0)
    , m_in0(Instance::kMaxBlock)
    , m_in1(Instance::kMaxBlock)
    , m_out0(Instance::kMaxBlock)
    , m_out1(Instance::kMaxBlock)
{
    for (size_t i = 0; i < m_values.size() && i < defaults.size(); ++i)
        m_values[i] = defaults[i];
}

void ChannelAdapter::process(float* io, int frames)
{
    for (int off = 0; off < frames;) {
        const int n = std::min(frames - off, kMaxBlock);
        float* p = io + size_t(off) * 2;
        for (int i = 0; i < n; ++i) {
            m_in0[size_t(i)] = p[size_t(i) * 2];
            m_in1[size_t(i)] = p[size_t(i) * 2 + 1];
        }
        if (m_dual) {
            const float* in0[1] = {m_in0.data()};
            float* out0[1] = {m_out0.data()};
            m_cores[0]->run(in0, out0, n, m_values.data());
            const float* in1[1] = {m_in1.data()};
            float* out1[1] = {m_out1.data()};
            m_cores[1]->run(in1, out1, n, m_values.data());
        } else {
            if (m_in == 1) { // a mono input gets the mix of both channels
                for (int i = 0; i < n; ++i)
                    m_in0[size_t(i)] = 0.5f * (m_in0[size_t(i)] + m_in1[size_t(i)]);
            }
            const float* in[2] = {m_in0.data(), m_in1.data()};
            float* out[2] = {m_out0.data(), m_out1.data()};
            m_cores[0]->run(in, out, n, m_values.data());
            if (m_out == 1) // a mono output goes to both channels
                std::copy(m_out0.begin(), m_out0.begin() + n, m_out1.begin());
        }
        // A plugin that produces NaN or infinity must not poison the mix.
        for (int i = 0; i < n; ++i) {
            const float l = m_out0[size_t(i)], r = m_out1[size_t(i)];
            p[size_t(i) * 2] = std::isfinite(l) ? l : 0.0f;
            p[size_t(i) * 2 + 1] = std::isfinite(r) ? r : 0.0f;
        }
        off += n;
    }
}

void ChannelAdapter::reset()
{
    for (auto& c : m_cores)
        c->reset();
}

int ChannelAdapter::latencyFrames() const
{
    return m_cores.empty() ? 0 : m_cores[0]->latencyFrames();
}

bool lv2Supported()
{
    return lv2::supported();
}

BundleInfo describeBundle(const QString& format, const QString& path)
{
    if (format == QLatin1String(kFormatLv2))
        return lv2::describe(path);
    if (format == QLatin1String(kFormatClap))
        return clap::describe(path);
    BundleInfo b;
    b.path = path;
    b.format = format;
    b.error = QStringLiteral("unknown plugin format");
    return b;
}

std::unique_ptr<Instance> Instance::create(
    const PluginInfo& plugin, const QString& bundlePath, double sampleRate, QString* error)
{
    QString local;
    QString* why = error ? error : &local;
    if (!plugin.error.isEmpty()) {
        *why = plugin.error;
        return nullptr;
    }
    if (plugin.format == QLatin1String(kFormatLv2))
        return lv2::create(plugin, bundlePath, sampleRate, why);
    if (plugin.format == QLatin1String(kFormatClap))
        return clap::create(plugin, bundlePath, sampleRate, why);
    *why = QStringLiteral("unknown plugin format");
    return nullptr;
}

} // namespace kk::audioplug
