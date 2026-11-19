// SPDX-License-Identifier: GPL-3.0-or-later
// AudioMixerGraph — the mixer graph of AudioMixer: tracks through their channel strips into
// the buses, buses through theirs, the output bus to the caller (see AudioMixer.h).
#include "core/MixerOps.h"
#include "render/AudioMixer.h"
#include "render/MixerChannel.h"

#include <algorithm>

namespace kk {

namespace {

/// Below this output peak (-120 dBFS) after a block without input, a channel is quiet:
/// its filter tails have died away and its processing can be skipped.
constexpr float kQuietPeak = 1e-6f;

void addInto(float* dst, const float* src, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        dst[i] += src[i];
}

} // namespace

AudioMixer::Channel& AudioMixer::trackChannel(quint64 id)
{
    auto& slot = m_trackChannels[id];
    if (!slot)
        slot = std::make_unique<Channel>(); // once per track (not in steady state)
    return *slot;
}

AudioMixer::Channel& AudioMixer::busChannel(int id)
{
    auto& slot = m_busChannels[id];
    if (!slot)
        slot = std::make_unique<Channel>();
    return *slot;
}

void AudioMixer::resetChannels()
{
    for (auto& [id, channel] : m_trackChannels) {
        channel->processor.reset();
        channel->effects.reset();
        channel->effectsPrimed = false;
        channel->quiet = true;
    }
    for (auto& [id, channel] : m_busChannels) {
        channel->processor.reset();
        channel->quiet = true;
    }
}

void AudioMixer::mixTimeline(const TimelineData& tl, const NestedTimelines& nested,
    int64_t startSample48k, int frames, float* out)
{
    // A jump (seek, loop, a new render) must not carry filter memory or gain glides from the
    // old position: the same range then always mixes to the same samples.
    if (startSample48k != m_nextSample)
        resetChannels();
    m_nextSample = startSample48k + frames;

    const size_t count = size_t(frames) * kChannels;
    const MixerData& mixer = tl.mixer;
    const int busCount = int(mixer.buses.size());
    if (int(m_busBuffers.size()) < busCount)
        m_busBuffers.resize(size_t(busCount));
    for (int b = 0; b < busCount; ++b) {
        std::vector<float>& buffer = m_busBuffers[size_t(b)];
        if (buffer.size() < count)
            buffer.resize(count);
        std::fill_n(buffer.data(), count, 0.0f);
    }
    if (m_trackBuffer.size() < count)
        m_trackBuffer.resize(count);
    float* trackBuffer = m_trackBuffer.data();
    if (m_tapDest)
        std::fill_n(m_tapDest, count, 0.0f);

    const qint64 now = m_clock.elapsed();
    const bool anySolo = std::any_of(
        tl.audioTracks.begin(), tl.audioTracks.end(), [](const TrackData& t) { return t.solo; });
    const double framesPerSample = double(tl.fps.num) / (double(kSampleRate) * tl.fps.den);

    // ---- Tracks ----
    unsigned fedBuses = 0; // bit per bus index (more than 32 buses: always processed)
    for (const TrackData& track : tl.audioTracks) {
        Channel& channel = trackChannel(track.id);
        channel.lastUsedMs = now;
        dsp::ChannelProcessor& proc = channel.processor;
        ChannelLevel* level = m_levels ? m_levels->tracks.add() : nullptr;
        if (level) {
            level->id = track.id;
            level->mono = track.mix.mono;
        }
        const bool audible = !track.muted && !(anySolo && !track.solo);
        // A muted track is not decoded, except while its gain still glides down to 0.
        const bool wanted = audible || proc.currentGain() > 0.0f;
        // Track effect slots (spec 133): the track's clips are summed, then run through the
        // slots. A slot that delays the sound (Noise Reduction) is compensated like a clip's:
        // the clips are mixed that many samples ahead, and after a restart the first stretch
        // is run through the slots first and dropped (priming), so the sound stays in place.
        const bool fxOn = wanted && dsp::ClipEffectChain::hasActive(track.mix.effects);
        const int latency = fxOn ? channel.effects.latencyFrames(track.mix.effects) : 0;
        const bool needPrime
            = fxOn && (!channel.effectsPrimed || latency != channel.effectsLatency);
        bool primeSounds = false;
        if (needPrime && latency > 0) { // the stretch before the block, in time order
            if (m_trackPrimeBuffer.size() < size_t(latency) * kChannels)
                m_trackPrimeBuffer.resize(size_t(latency) * kChannels);
            std::fill_n(m_trackPrimeBuffer.data(), size_t(latency) * kChannels, 0.0f);
            primeSounds
                = mixTrackClips(tl, nested, track, startSample48k, latency, m_trackPrimeBuffer.data());
        }
        std::fill_n(trackBuffer, count, 0.0f);
        const bool sounding = wanted
            && mixTrackClips(tl, nested, track, startSample48k + latency, frames, trackBuffer);
        if (!sounding && !primeSounds && channel.quiet) {
            channel.effectsPrimed = false;
            continue;
        }
        if (fxOn) {
            if (needPrime) {
                channel.effects.reset();
                if (latency > 0)
                    channel.effects.process(track.mix.effects, m_trackPrimeBuffer.data(), latency);
            }
            channel.effects.process(track.mix.effects, trackBuffer, frames);
        }
        channel.effectsPrimed = fxOn;
        channel.effectsLatency = latency;

        proc.setStrip(track.mix.strip);
        proc.processInserts(trackBuffer, frames, track.mix.mono);
        proc.applyGains(trackBuffer, frames, [&](int offset) {
            const double frame = double(startSample48k + offset) * framesPerSample;
            dsp::ChannelGains g;
            g.gain = audible ? dbToGain(mixerops::faderDbAt(track, frame)) : 0.0f;
            panGains(mixerops::panAt(track, frame), g.left, g.right);
            return g;
        });
        if (m_tapDest && !m_tap.isBus() && m_tap.id == track.id)
            std::copy_n(trackBuffer, count, m_tapDest);
        if (level) {
            level->peak[0] = proc.peak(0);
            level->peak[1] = proc.peak(1);
            level->gainReductionDb = proc.gainReductionDb();
        }
        channel.quiet = !sounding && proc.peak(0) < kQuietPeak && proc.peak(1) < kQuietPeak;
        if (channel.quiet) {
            proc.resetInserts(); // tails are gone: start clean when a clip plays again
            channel.effects.reset();
            channel.effectsPrimed = false;
            continue;
        }
        for (int busId : track.mix.buses) {
            const int b = mixerops::busIndex(mixer, busId);
            if (b < 0)
                continue;
            addInto(m_busBuffers[size_t(b)].data(), trackBuffer, count);
            if (b < 32)
                fedBuses |= 1u << b;
        }
    }

    // ---- Buses ----
    for (int b = 0; b < busCount; ++b) {
        const BusData& bus = mixer.buses[size_t(b)];
        Channel& channel = busChannel(bus.id);
        channel.lastUsedMs = now;
        dsp::ChannelProcessor& proc = channel.processor;
        ChannelLevel* level = m_levels ? m_levels->buses.add() : nullptr;
        if (level) {
            level->id = quint64(bus.id);
            level->mono = bus.mono;
        }
        const bool fed = b >= 32 || (fedBuses & (1u << b)) != 0;
        if (!fed && channel.quiet)
            continue;
        float* buffer = m_busBuffers[size_t(b)].data();
        proc.setStrip(bus.strip);
        proc.processInserts(buffer, frames, bus.mono);
        const float gain = bus.strip.muted ? 0.0f : dbToGain(bus.strip.volumeDb);
        proc.applyGains(
            buffer, frames, [gain](int) { return dsp::ChannelGains {gain, 1.0f, 1.0f}; });
        if (m_tapDest && m_tap.isBus() && m_tap.id == quint64(bus.id))
            std::copy_n(buffer, count, m_tapDest);
        if (level) {
            level->peak[0] = proc.peak(0);
            level->peak[1] = proc.peak(1);
            level->gainReductionDb = proc.gainReductionDb();
        }
        channel.quiet = !fed && proc.peak(0) < kQuietPeak && proc.peak(1) < kQuietPeak;
        if (channel.quiet)
            proc.resetInserts();
    }

    // ---- Output ----
    int output = mixerops::busIndex(mixer, mixer.outputBus);
    if (output < 0)
        output = busCount > 0 ? 0 : -1;
    if (output >= 0)
        addInto(out, m_busBuffers[size_t(output)].data(), count);
}

} // namespace kk
