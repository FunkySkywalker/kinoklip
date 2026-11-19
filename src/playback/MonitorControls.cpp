// SPDX-License-Identifier: GPL-3.0-or-later
// MonitorControls implementation.
#include "playback/MonitorControls.h"

#include <QSettings>

#include <algorithm>
#include <cmath>

namespace kk {

MonitorControls& MonitorControls::shared()
{
    static MonitorControls* instance = [] {
        auto* m = new MonitorControls;
        m->load();
        return m;
    }();
    return *instance;
}

void MonitorControls::setDimDb(double db)
{
    m_dimDb.store(std::clamp(db, kMinDimDb, kMaxDimDb), std::memory_order_relaxed);
}

void MonitorControls::save() const
{
    QSettings s;
    s.setValue(QStringLiteral("monitor/dim"), dim());
    s.setValue(QStringLiteral("monitor/dimDb"), dimDb());
    s.setValue(QStringLiteral("monitor/mono"), mono());
}

void MonitorControls::load()
{
    QSettings s;
    setDim(s.value(QStringLiteral("monitor/dim"), false).toBool());
    setDimDb(s.value(QStringLiteral("monitor/dimDb"), kDefaultDimDb).toDouble());
    setMono(s.value(QStringLiteral("monitor/mono"), false).toBool());
    setMute(false);
}

void MonitorProcessor::process(const MonitorControls& controls, float* io, int frames)
{
    const float targetGain = controls.mute()
        ? 0.0f
        : (controls.dim() ? float(std::pow(10.0, controls.dimDb() / 20.0)) : 1.0f);
    const float targetMono = controls.mono() ? 1.0f : 0.0f;
    if (m_fresh) {
        m_gain = targetGain;
        m_mono = targetMono;
        m_fresh = false;
    }
    if (m_gain == 1.0f && targetGain == 1.0f && m_mono == 0.0f && targetMono == 0.0f)
        return; // everything off: the block stays as it is
    constexpr float kRamp = 1.0f / 240.0f; // 5 ms at 48 kHz
    for (int i = 0; i < frames; ++i) {
        m_gain += std::clamp(targetGain - m_gain, -kRamp, kRamp);
        m_mono += std::clamp(targetMono - m_mono, -kRamp * 4.0f, kRamp * 4.0f);
        if (std::fabs(targetGain - m_gain) < 1e-6f)
            m_gain = targetGain;
        if (std::fabs(targetMono - m_mono) < 1e-6f)
            m_mono = targetMono;
        float& l = io[size_t(i) * 2];
        float& r = io[size_t(i) * 2 + 1];
        const float mid = 0.5f * (l + r);
        l = (l + (mid - l) * m_mono) * m_gain;
        r = (r + (mid - r) * m_mono) * m_gain;
    }
}

} // namespace kk
