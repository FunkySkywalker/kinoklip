// SPDX-License-Identifier: GPL-3.0-or-later
// ChannelProcessor — inserts and smoothing constants.
#include "render/ChannelProcessor.h"

namespace kk::dsp {

namespace {
constexpr double kSampleRate = 48000.0;
/// Glide time constant of the fader / pan / mute gains.
constexpr double kGainSmoothSeconds = 0.005;
} // namespace

void addSend(float* dst, const float* src, int frames, float wantLeft, float wantRight,
    SendGainState& state)
{
    constexpr int kBlock = ChannelProcessor::kGainBlock;
    static const float alpha
        = float(1.0 - std::exp(-double(kBlock) / (kGainSmoothSeconds * kSampleRate)));
    constexpr float kSnap = 1e-6f;
    auto glide = [](float from, float to) {
        const float d = to - from;
        return std::abs(d) < kSnap ? to : from + d * alpha;
    };
    if (state.fresh) {
        state.left = wantLeft;
        state.right = wantRight;
        state.fresh = false;
    }
    for (int b0 = 0; b0 < frames; b0 += kBlock) {
        const int b1 = std::min(frames, b0 + kBlock);
        const float nextL = glide(state.left, wantLeft);
        const float nextR = glide(state.right, wantRight);
        if (state.left != 0.0f || state.right != 0.0f || nextL != 0.0f || nextR != 0.0f) {
            const float inv = 1.0f / float(b1 - b0);
            const float dl = (nextL - state.left) * inv;
            const float dr = (nextR - state.right) * inv;
            float gl = state.left;
            float gr = state.right;
            for (int i = b0; i < b1; ++i, gl += dl, gr += dr) {
                dst[size_t(i) * 2] += src[size_t(i) * 2] * gl;
                dst[size_t(i) * 2 + 1] += src[size_t(i) * 2 + 1] * gr;
            }
        }
        state.left = nextL;
        state.right = nextR;
    }
}

void invertPhase(float* io, int frames, bool left, bool right)
{
    if (left) {
        for (int i = 0; i < frames; ++i)
            io[size_t(i) * 2] = -io[size_t(i) * 2];
    }
    if (right) {
        for (int i = 0; i < frames; ++i)
            io[size_t(i) * 2 + 1] = -io[size_t(i) * 2 + 1];
    }
}

ChannelProcessor::ChannelProcessor()
    : m_eq(kSampleRate)
    , m_dynamics(kSampleRate)
    , m_alpha(float(1.0 - std::exp(-double(kGainBlock) / (kGainSmoothSeconds * kSampleRate))))
{ }

void ChannelProcessor::setStrip(const ChannelStrip& strip)
{
    m_eq.setTarget(strip.eq);
    m_dynamics.setTarget(strip.dynamics);
}

void ChannelProcessor::reset()
{
    m_eq.reset();
    m_dynamics.reset();
    m_fresh = true;
}

void ChannelProcessor::resetInserts()
{
    m_eq.reset();
    m_dynamics.reset();
}

void ChannelProcessor::processInserts(float* io, int frames, bool mono, const float* key)
{
    if (mono) {
        for (int i = 0; i < frames; ++i) {
            const float m = 0.5f * (io[size_t(i) * 2] + io[size_t(i) * 2 + 1]);
            io[size_t(i) * 2] = m;
            io[size_t(i) * 2 + 1] = m;
        }
    }
    m_eq.process(io, frames);
    m_dynamics.process(io, frames, key);
}

} // namespace kk::dsp
