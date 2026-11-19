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

bool sendsTo(const ChannelStrip& strip, int busId)
{
    for (const SendData& d : strip.sends) {
        if (d.busId == busId)
            return true;
    }
    return false;
}

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
        channel->sends.clear();
        channel->quiet = true;
    }
    for (auto& [id, channel] : m_busChannels) {
        channel->processor.reset();
        channel->sends.clear();
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
    if (m_tapDest)
        std::fill_n(m_tapDest, count, 0.0f);

    const qint64 now = m_clock.elapsed();
    const bool anySolo = std::any_of(
        tl.audioTracks.begin(), tl.audioTracks.end(), [](const TrackData& t) { return t.solo; });
    const double framesPerSample = double(tl.fps.num) / (double(kSampleRate) * tl.fps.den);

    // ---- Key (side chain) sources, spec 136 ----
    // Strips may take the detector of their gate / compressor from another track or a bus.
    // Channels are processed on demand: before a strip runs, the channel it keys from is
    // processed first (a bus first processes the tracks routed to it), so the key is exactly
    // this block's signal of that channel after its fader. Without keys this is the plain
    // order: all tracks, then all buses.
    const int trackCount = int(tl.audioTracks.size());
    constexpr unsigned char kDone = 1, kBusy = 2, kIsKey = 4;
    m_trackState.assign(size_t(trackCount), 0);
    m_busState.assign(size_t(busCount), 0);
    m_trackKeys.resize(size_t(trackCount));
    m_busKeys.resize(size_t(busCount));
    auto trackIndex = [&](quint64 id) {
        for (int i = 0; i < trackCount; ++i) {
            if (tl.audioTracks[size_t(i)].id == id)
                return i;
        }
        return -1;
    };
    auto markKey = [&](const MixTarget& strip, KeyKind& kind, int& index) {
        const DynamicsSettings k = mixerops::effectiveKey(tl, strip);
        kind = k.keyKind;
        index = k.keyKind == KeyKind::Track ? trackIndex(k.keyId)
            : k.keyKind == KeyKind::Bus     ? mixerops::busIndex(mixer, int(k.keyId))
                                            : -1;
        if (index < 0) {
            kind = KeyKind::Self;
            return;
        }
        (kind == KeyKind::Track ? m_trackState : m_busState)[size_t(index)] |= kIsKey;
    };
    for (int i = 0; i < trackCount; ++i) {
        m_trackKeys[size_t(i)] = {KeyKind::Self, -1};
        if (tl.audioTracks[size_t(i)].mix.strip.dynamics.keyKind != KeyKind::Self)
            markKey(MixTarget::track(tl.audioTracks[size_t(i)].id), m_trackKeys[size_t(i)].first,
                m_trackKeys[size_t(i)].second);
    }
    for (int b = 0; b < busCount; ++b) {
        m_busKeys[size_t(b)] = {KeyKind::Self, -1};
        if (mixer.buses[size_t(b)].strip.dynamics.keyKind != KeyKind::Self)
            markKey(MixTarget::bus(mixer.buses[size_t(b)].id), m_busKeys[size_t(b)].first,
                m_busKeys[size_t(b)].second);
    }
    unsigned fedBuses = 0; // bit per bus index (more than 32 buses: always processed)

    // Keeps (or zeroes) the signal of a key source for the strips that listen to it.
    auto storeKey = [&](Channel& channel, const float* signal) {
        if (channel.keyOut.size() < count)
            channel.keyOut.resize(count);
        if (signal)
            std::copy_n(signal, count, channel.keyOut.data());
        else
            std::fill_n(channel.keyOut.data(), count, 0.0f);
    };

    // Sends of a strip (spec 137): copies of its signal into the buses it sends to. @p pre is the
    // signal before the pan and fader (null without Pre sends), @p post the one after them.
    auto runSends = [&](Channel& channel, const ChannelStrip& strip, const float* pre,
                        const float* post) {
        for (const SendData& send : strip.sends) {
            const int b = mixerops::busIndex(mixer, send.busId);
            if (b < 0 || (m_busState[size_t(b)] & kDone))
                continue; // unknown, or already mixed down (a loop in a damaged file)
            const float* src = send.preFader ? pre : post;
            if (!src)
                continue;
            auto it = std::find_if(channel.sends.begin(), channel.sends.end(),
                [&](const dsp::SendGainState& g) { return g.busId == send.busId; });
            if (it == channel.sends.end()) {
                channel.sends.push_back({});
                channel.sends.back().busId = send.busId;
                it = channel.sends.end() - 1;
            }
            float left = 0.0f, right = 0.0f;
            if (send.enabled) {
                panGains(send.pan, left, right);
                const float gain = dbToGain(send.levelDb);
                left *= gain;
                right *= gain;
            }
            dsp::addSend(m_busBuffers[size_t(b)].data(), src, frames, left, right, *it);
            if (b < 32)
                fedBuses |= 1u << b;
        }
    };
    auto hasPreSend = [](const ChannelStrip& strip) {
        return std::any_of(strip.sends.begin(), strip.sends.end(),
            [](const SendData& d) { return d.preFader; });
    };
    // Copies the strip's signal to the send scratch buffer when it has Pre sends.
    auto keepPre = [&](const ChannelStrip& strip, const float* signal) -> const float* {
        if (!hasPreSend(strip))
            return nullptr;
        if (m_sendPre.size() < count)
            m_sendPre.resize(count);
        std::copy_n(signal, count, m_sendPre.data());
        return m_sendPre.data();
    };
    auto peakOf = [&](const float* signal) {
        float peak = 0.0f;
        for (size_t i = 0; i < count; ++i)
            peak = std::max(peak, std::abs(signal[i]));
        return peak;
    };

    // Processes track (@p isBus false) or bus @p index once; returns the channel for keys.
    auto process = [&](auto& self, bool isBus, int index) -> void {
        unsigned char& state = isBus ? m_busState[size_t(index)] : m_trackState[size_t(index)];
        if (state & (kDone | kBusy))
            return;
        state |= kBusy;
        const auto [keyKind, keyIndex] = isBus ? m_busKeys[size_t(index)] : m_trackKeys[size_t(index)];
        const float* key = nullptr;
        if (keyKind != KeyKind::Self) {
            self(self, keyKind == KeyKind::Bus, keyIndex);
            const unsigned char ks = keyKind == KeyKind::Bus ? m_busState[size_t(keyIndex)]
                                                             : m_trackState[size_t(keyIndex)];
            if (ks & kDone) { // not Busy: a loop leaves the key unset (own signal)
                Channel& src = keyKind == KeyKind::Bus
                    ? busChannel(mixer.buses[size_t(keyIndex)].id)
                    : trackChannel(tl.audioTracks[size_t(keyIndex)].id);
                if (src.keyOut.size() >= count)
                    key = src.keyOut.data();
            }
        }
        const bool isKey = (state & kIsKey) != 0;
        if (!isBus) {
            const TrackData& track = tl.audioTracks[size_t(index)];
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
                primeSounds = mixTrackClips(
                    tl, nested, track, startSample48k, latency, m_trackPrimeBuffer.data());
                dsp::invertPhase(m_trackPrimeBuffer.data(), latency, track.mix.strip.invertLeft,
                    track.mix.strip.invertRight);
            }
            // The key sources are processed before this point, so one scratch buffer is enough.
            if (m_trackBuffer.size() < count)
                m_trackBuffer.resize(count);
            float* trackBuffer = m_trackBuffer.data();
            std::fill_n(trackBuffer, count, 0.0f);
            const bool sounding = wanted
                && mixTrackClips(tl, nested, track, startSample48k + latency, frames, trackBuffer);
            // Phase Invert comes first, before the effect slots (spec 137).
            dsp::invertPhase(
                trackBuffer, frames, track.mix.strip.invertLeft, track.mix.strip.invertRight);
            if (!sounding && !primeSounds && channel.quiet) {
                channel.effectsPrimed = false;
                if (isKey)
                    storeKey(channel, nullptr);
                state = kDone | (state & kIsKey);
                return;
            }
            if (fxOn) {
                if (needPrime) {
                    channel.effects.reset();
                    if (latency > 0)
                        channel.effects.process(
                            track.mix.effects, m_trackPrimeBuffer.data(), latency);
                }
                channel.effects.process(track.mix.effects, trackBuffer, frames);
            }
            channel.effectsPrimed = fxOn;
            channel.effectsLatency = latency;

            proc.setStrip(track.mix.strip);
            proc.processInserts(trackBuffer, frames, track.mix.mono, key);
            const float* pre = keepPre(track.mix.strip, trackBuffer);
            const float preLevel = pre ? peakOf(pre) : 0.0f;
            proc.applyGains(trackBuffer, frames, [&](int offset) {
                const double frame = double(startSample48k + offset) * framesPerSample;
                dsp::ChannelGains g;
                g.gain = audible ? dbToGain(mixerops::faderDbAt(track, frame)) : 0.0f;
                panGains(mixerops::panAt(track, frame), g.left, g.right);
                return g;
            });
            if (isKey)
                storeKey(channel, trackBuffer);
            if (m_tapDest && !m_tap.isBus() && m_tap.id == track.id)
                std::copy_n(trackBuffer, count, m_tapDest);
            if (level) {
                level->peak[0] = proc.peak(0);
                level->peak[1] = proc.peak(1);
                level->gainReductionDb = proc.gainReductionDb();
            }
            runSends(channel, track.mix.strip, pre, trackBuffer);
            channel.quiet = !sounding && proc.peak(0) < kQuietPeak && proc.peak(1) < kQuietPeak
                && preLevel < kQuietPeak;
            if (channel.quiet) {
                proc.resetInserts(); // tails are gone: start clean when a clip plays again
                channel.effects.reset();
                channel.effectsPrimed = false;
            } else {
                for (int busId : track.mix.buses) {
                    const int b = mixerops::busIndex(mixer, busId);
                    if (b < 0)
                        continue;
                    addInto(m_busBuffers[size_t(b)].data(), trackBuffer, count);
                    if (b < 32)
                        fedBuses |= 1u << b;
                }
            }
        } else {
            const BusData& bus = mixer.buses[size_t(index)];
            // Every track routed to the bus first.
            for (int i = 0; i < trackCount; ++i) {
                const TrackData& source = tl.audioTracks[size_t(i)];
                if (source.mix.buses.contains(bus.id) || sendsTo(source.mix.strip, bus.id))
                    self(self, false, i);
            }
            // ... and every bus that sends to it (a bus feed, spec 137).
            for (int j = 0; j < busCount; ++j) {
                if (j != index && sendsTo(mixer.buses[size_t(j)].strip, bus.id))
                    self(self, true, j);
            }
            Channel& channel = busChannel(bus.id);
            channel.lastUsedMs = now;
            dsp::ChannelProcessor& proc = channel.processor;
            ChannelLevel* level = m_levels ? m_levels->buses.add() : nullptr;
            if (level) {
                level->id = quint64(bus.id);
                level->mono = bus.mono;
            }
            const bool fed = index >= 32 || (fedBuses & (1u << index)) != 0;
            if (!fed && channel.quiet) {
                if (isKey)
                    storeKey(channel, nullptr);
                state = kDone | (state & kIsKey);
                return;
            }
            float* buffer = m_busBuffers[size_t(index)].data();
            dsp::invertPhase(buffer, frames, bus.strip.invertLeft, bus.strip.invertRight);
            proc.setStrip(bus.strip);
            proc.processInserts(buffer, frames, bus.mono, key);
            const float* pre = keepPre(bus.strip, buffer);
            const float preLevel = pre ? peakOf(pre) : 0.0f;
            const float gain = bus.strip.muted ? 0.0f : dbToGain(bus.strip.volumeDb);
            float panLeft = 1.0f, panRight = 1.0f;
            panGains(bus.strip.pan, panLeft, panRight);
            proc.applyGains(buffer, frames,
                [&](int) { return dsp::ChannelGains {gain, panLeft, panRight}; });
            if (isKey)
                storeKey(channel, buffer);
            if (m_tapDest && m_tap.isBus() && m_tap.id == quint64(bus.id))
                std::copy_n(buffer, count, m_tapDest);
            if (level) {
                level->peak[0] = proc.peak(0);
                level->peak[1] = proc.peak(1);
                level->gainReductionDb = proc.gainReductionDb();
            }
            runSends(channel, bus.strip, pre, buffer);
            channel.quiet = !fed && proc.peak(0) < kQuietPeak && proc.peak(1) < kQuietPeak
                && preLevel < kQuietPeak;
            if (channel.quiet)
                proc.resetInserts();
        }
        state = kDone | (state & kIsKey);
    };

    // ---- Tracks, then buses ----
    for (int i = 0; i < trackCount; ++i)
        process(process, false, i);
    for (int b = 0; b < busCount; ++b)
        process(process, true, b);

    // ---- Output ----
    int output = mixerops::busIndex(mixer, mixer.outputBus);
    if (output < 0)
        output = busCount > 0 ? 0 : -1;
    if (output >= 0)
        addInto(out, m_busBuffers[size_t(output)].data(), count);
}

} // namespace kk
