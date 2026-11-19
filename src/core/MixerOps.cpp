// SPDX-License-Identifier: GPL-3.0-or-later
// MixerOps — bus, routing, strip and automation edits.
#include "core/MixerOps.h"

#include "core/EffectRegistry.h"

#include <algorithm>
#include <cmath>

namespace kk::mixerops {

namespace {

constexpr double kPanMin = -1.0;
constexpr double kPanMax = 1.0;

double clampAutomation(const QString& key, double value)
{
    return key == mixkeys::kPan ? std::clamp(value, kPanMin, kPanMax)
                                : std::clamp(value, kFaderMinDb, kFaderMaxDb);
}

} // namespace

TrackData* audioTrack(TimelineData& data, quint64 trackId)
{
    for (TrackData& t : data.audioTracks) {
        if (t.id == trackId)
            return &t;
    }
    return nullptr;
}

const TrackData* audioTrack(const TimelineData& data, quint64 trackId)
{
    return audioTrack(const_cast<TimelineData&>(data), trackId);
}

int busIndex(const MixerData& mixer, int busId)
{
    for (int i = 0; i < int(mixer.buses.size()); ++i) {
        if (mixer.buses[size_t(i)].id == busId)
            return i;
    }
    return -1;
}

BusData* bus(TimelineData& data, int busId)
{
    const int i = busIndex(data.mixer, busId);
    return i < 0 ? nullptr : &data.mixer.buses[size_t(i)];
}

const BusData* bus(const TimelineData& data, int busId)
{
    return bus(const_cast<TimelineData&>(data), busId);
}

ChannelStrip* strip(TimelineData& data, const MixTarget& target)
{
    if (target.isBus()) {
        BusData* b = bus(data, int(target.id));
        return b ? &b->strip : nullptr;
    }
    TrackData* t = audioTrack(data, target.id);
    return t ? &t->mix.strip : nullptr;
}

const ChannelStrip* strip(const TimelineData& data, const MixTarget& target)
{
    return strip(const_cast<TimelineData&>(data), target);
}

QString targetName(const TimelineData& data, const MixTarget& target)
{
    if (target.isBus()) {
        const BusData* b = bus(data, int(target.id));
        return b ? busDisplayName(*b) : QString();
    }
    for (int i = 0; i < int(data.audioTracks.size()); ++i) {
        if (data.audioTracks[size_t(i)].id == target.id)
            return trackDisplayName(data.audioTracks[size_t(i)], i);
    }
    return {};
}

int addBus(TimelineData& data, bool mono, const QString& name, bool aux)
{
    BusData b;
    b.id = std::max(data.mixer.nextBusId, kMainBusId + 1);
    // Never reuse an id that is still present (damaged files with a stale allocator).
    for (const BusData& existing : data.mixer.buses)
        b.id = std::max(b.id, existing.id + 1);
    b.name = name;
    b.mono = mono;
    b.aux = aux;
    data.mixer.buses.push_back(b);
    data.mixer.nextBusId = b.id + 1;
    return b.id;
}

bool renameBus(TimelineData& data, int busId, const QString& name)
{
    BusData* b = bus(data, busId);
    if (!b || b->name == name)
        return false;
    b->name = name;
    return true;
}

bool setBusMono(TimelineData& data, int busId, bool mono)
{
    BusData* b = bus(data, busId);
    if (!b || b->mono == mono)
        return false;
    b->mono = mono;
    return true;
}

bool removeBus(TimelineData& data, int busId)
{
    const int i = busIndex(data.mixer, busId);
    if (i < 0 || busId == kMainBusId)
        return false;
    data.mixer.buses.erase(data.mixer.buses.begin() + i);
    for (TrackData& t : data.audioTracks) {
        t.mix.buses.removeAll(busId);
        removeSend(data, MixTarget::track(t.id), busId);
    }
    for (BusData& other : data.mixer.buses)
        removeSend(data, MixTarget::bus(other.id), busId);
    if (data.mixer.outputBus == busId)
        data.mixer.outputBus = kMainBusId;
    return true;
}

bool setOutputBus(TimelineData& data, int busId)
{
    if (!bus(data, busId) || data.mixer.outputBus == busId)
        return false;
    data.mixer.outputBus = busId;
    return true;
}

bool setTrackRoute(TimelineData& data, quint64 trackId, int busId, bool on)
{
    TrackData* t = audioTrack(data, trackId);
    const BusData* target = bus(data, busId);
    if (!t || !target || t->mix.buses.contains(busId) == on || (on && target->aux))
        return false;
    if (on) {
        t->mix.buses.append(busId);
        std::sort(t->mix.buses.begin(), t->mix.buses.end());
    } else {
        t->mix.buses.removeAll(busId);
    }
    return true;
}

bool setTrackMono(TimelineData& data, quint64 trackId, bool mono)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t || t->mix.mono == mono)
        return false;
    t->mix.mono = mono;
    return true;
}

bool setAutomationMode(TimelineData& data, quint64 trackId, AutomationMode mode)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t || t->mix.automation == mode)
        return false;
    t->mix.automation = mode;
    return true;
}

double staticFaderDb(const TimelineData& data, const MixTarget& target)
{
    const ChannelStrip* s = strip(data, target);
    return s ? s->volumeDb : 0.0;
}

bool offsetFader(TimelineData& data, const MixTarget& target, double deltaDb)
{
    ChannelStrip* s = strip(data, target);
    if (!s || !std::isfinite(deltaDb) || deltaDb == 0.0)
        return false;
    auto shifted = [&](double v) { return std::clamp(v + deltaDb, kFaderMinDb, kFaderMaxDb); };
    bool changed = false;
    const double before = s->volumeDb;
    s->volumeDb = shifted(s->volumeDb);
    changed |= s->volumeDb != before;
    if (!target.isBus()) {
        TrackData* t = audioTrack(data, target.id);
        const auto it = t->mix.automationKeys.values.find(mixkeys::kVolumeDb);
        if (it != t->mix.automationKeys.values.end()) {
            const double base = it->base;
            it->base = shifted(it->base);
            changed |= it->base != base;
            for (Keyframe& k : it->keys) {
                const double old = k.value;
                k.value = shifted(k.value);
                changed |= k.value != old;
            }
        }
    }
    return changed;
}

namespace {

/// Keys of one node of the key graph.
struct KeyNode
{
    bool bus = false;
    quint64 id = 0;
};

bool hasSendTo(const ChannelStrip& s, int busId)
{
    return std::any_of(
        s.sends.begin(), s.sends.end(), [busId](const SendData& d) { return d.busId == busId; });
}

/// The key source of node @p n (kind Self when it has none or it is unknown).
std::pair<KeyKind, quint64> keyOf(const TimelineData& d, KeyNode n)
{
    const ChannelStrip* s = strip(d, n.bus ? MixTarget::bus(int(n.id)) : MixTarget::track(n.id));
    if (!s || s->dynamics.keyKind == KeyKind::Self)
        return {KeyKind::Self, 0};
    return {s->dynamics.keyKind, s->dynamics.keyId};
}

/// True if node @p from depends on node @p target (through keys, bus routes and sends).
bool dependsOn(const TimelineData& d, KeyNode from, KeyNode target, int depth = 0)
{
    if (depth > 64)
        return true; // a loop that is already there: treat as dependent
    if (from.bus == target.bus && from.id == target.id)
        return true;
    if (from.bus) {
        for (const TrackData& t : d.audioTracks) {
            if ((t.mix.buses.contains(int(from.id)) || hasSendTo(t.mix.strip, int(from.id)))
                && dependsOn(d, {false, t.id}, target, depth + 1))
                return true;
        }
        // Buses that send to this bus feed it (spec 137); every send counts, on or off.
        for (const BusData& b : d.mixer.buses) {
            if (hasSendTo(b.strip, int(from.id)) && dependsOn(d, {true, quint64(b.id)}, target, depth + 1))
                return true;
        }
    }
    const auto [kind, id] = keyOf(d, from);
    if (kind != KeyKind::Self) {
        const KeyNode src {kind == KeyKind::Bus, id};
        if (dependsOn(d, src, target, depth + 1))
            return true;
    }
    return false;
}

} // namespace

bool keySourceAllowed(
    const TimelineData& data, const MixTarget& strip, KeyKind kind, quint64 sourceId)
{
    if (kind == KeyKind::Self)
        return true;
    if (!mixerops::strip(data, strip))
        return false;
    const bool sourceBus = kind == KeyKind::Bus;
    if (sourceBus ? !bus(data, int(sourceId)) : !audioTrack(data, sourceId))
        return false;
    const KeyNode self {strip.isBus(), strip.id};
    const KeyNode source {sourceBus, sourceId};
    if (self.bus == source.bus && self.id == source.id)
        return false;
    return !dependsOn(data, source, self);
}

bool sendAllowed(const TimelineData& data, const MixTarget& from, int busId)
{
    if (!strip(data, from) || !bus(data, busId))
        return false;
    if (from.isBus() && from.id == quint64(busId))
        return false;
    // The new edge makes the bus depend on the sender: a loop if the sender already depends
    // on the bus (through routes, other sends or keys).
    return !dependsOn(data, KeyNode {from.isBus(), from.id}, KeyNode {true, quint64(busId)});
}

bool addSend(TimelineData& data, const MixTarget& from, int busId, double levelDb, bool preFader)
{
    ChannelStrip* s = strip(data, from);
    if (!s || hasSendTo(*s, busId) || s->sends.size() >= kMaxSends
        || !sendAllowed(data, from, busId))
        return false;
    SendData d;
    d.busId = busId;
    d.levelDb = std::clamp(levelDb, kSendMinDb, kSendMaxDb);
    d.preFader = preFader;
    s->sends.append(d);
    return true;
}

bool removeSend(TimelineData& data, const MixTarget& from, int busId)
{
    ChannelStrip* s = strip(data, from);
    if (!s)
        return false;
    for (int i = 0; i < s->sends.size(); ++i) {
        if (s->sends[i].busId == busId) {
            s->sends.removeAt(i);
            return true;
        }
    }
    return false;
}

bool updateSend(TimelineData& data, const MixTarget& from, const SendData& value)
{
    ChannelStrip* s = strip(data, from);
    if (!s)
        return false;
    for (SendData& d : s->sends) {
        if (d.busId != value.busId)
            continue;
        SendData next = value;
        next.levelDb = std::clamp(next.levelDb, kSendMinDb, kSendMaxDb);
        next.pan = std::clamp(next.pan, -1.0, 1.0);
        if (next == d)
            return false;
        d = next;
        return true;
    }
    return false;
}

bool setBusAux(TimelineData& data, int busId, bool aux)
{
    BusData* b = bus(data, busId);
    if (!b || b->aux == aux || busId == kMainBusId || (aux && data.mixer.outputBus == busId))
        return false;
    b->aux = aux;
    if (aux) {
        for (TrackData& t : data.audioTracks)
            t.mix.buses.removeAll(busId);
    }
    return true;
}

bool setStripInvert(TimelineData& data, const MixTarget& target, bool left, bool right)
{
    ChannelStrip* s = strip(data, target);
    if (!s || (s->invertLeft == left && s->invertRight == right))
        return false;
    s->invertLeft = left;
    s->invertRight = right;
    return true;
}

bool applyEqPreset(TimelineData& data, const MixTarget& target, const EqSettings& eq)
{
    ChannelStrip* s = strip(data, target);
    if (!s)
        return false;
    ChannelStrip next = *s;
    next.eq = eq;
    clampStrip(next);
    if (next.eq == s->eq)
        return false;
    s->eq = next.eq;
    return true;
}

bool setKeySource(TimelineData& data, const MixTarget& target, KeyKind kind, quint64 sourceId)
{
    ChannelStrip* s = strip(data, target);
    if (!s || !keySourceAllowed(data, target, kind, sourceId))
        return false;
    if (kind == KeyKind::Self)
        sourceId = 0;
    if (s->dynamics.keyKind == kind && s->dynamics.keyId == sourceId)
        return false;
    s->dynamics.keyKind = kind;
    s->dynamics.keyId = sourceId;
    return true;
}

DynamicsSettings effectiveKey(const TimelineData& data, const MixTarget& target)
{
    DynamicsSettings out;
    const ChannelStrip* s = strip(data, target);
    if (!s)
        return out;
    const KeyNode self {target.isBus(), target.id};
    const auto [kind, id] = keyOf(data, self);
    if (kind == KeyKind::Self)
        return out;
    // Unknown source or a loop: Self. (The check runs with the strip's own key removed.)
    const bool sourceBus = kind == KeyKind::Bus;
    if (sourceBus ? !bus(data, int(id)) : !audioTrack(data, id))
        return out;
    const KeyNode source {sourceBus, id};
    if (source.bus == self.bus && source.id == self.id)
        return out;
    if (dependsOn(data, source, self))
        return out;
    out.keyKind = kind;
    out.keyId = id;
    return out;
}

void clampStrip(ChannelStrip& s)
{
    for (EqBand& b : s.eq.bands) {
        b.frequencyHz = std::clamp(b.frequencyHz, kEqMinFrequencyHz, kEqMaxFrequencyHz);
        b.gainDb = std::clamp(b.gainDb, -kEqMaxGainDb, kEqMaxGainDb);
        b.q = std::clamp(b.q, kEqMinQ, kEqMaxQ);
    }
    DynamicsSettings& d = s.dynamics;
    d.gateThresholdDb = std::clamp(d.gateThresholdDb, kGateMinThresholdDb, 0.0);
    d.gateRangeDb = std::clamp(d.gateRangeDb, 0.0, kGateMaxRangeDb);
    d.gateRatio = std::clamp(d.gateRatio, 1.0, kGateMaxRatio);
    d.gateAttackMs = std::clamp(d.gateAttackMs, kDynMinTimeMs, kDynMaxAttackMs);
    d.gateHoldMs = std::clamp(d.gateHoldMs, 0.0, kGateMaxHoldMs);
    d.gateReleaseMs = std::clamp(d.gateReleaseMs, kDynMinTimeMs, kDynMaxReleaseMs);
    d.gateHysteresisDb = std::clamp(d.gateHysteresisDb, 0.0, kGateMaxHysteresisDb);
    d.keyLowCutHz = std::clamp(d.keyLowCutHz, kKeyMinHz, kKeyMaxHz);
    d.keyHighCutHz = std::clamp(d.keyHighCutHz, kKeyMinHz, kKeyMaxHz);
    if (d.keyKind == KeyKind::Self)
        d.keyId = 0;
    d.thresholdDb = std::clamp(d.thresholdDb, kDynMinThresholdDb, 0.0);
    d.ratio = std::clamp(d.ratio, 1.0, kDynMaxRatio);
    d.attackMs = std::clamp(d.attackMs, kDynMinTimeMs, kDynMaxAttackMs);
    d.releaseMs = std::clamp(d.releaseMs, kDynMinTimeMs, kDynMaxReleaseMs);
    d.makeupDb = std::clamp(d.makeupDb, 0.0, kDynMaxMakeupDb);
    d.kneeDb = std::clamp(d.kneeDb, 0.0, kDynMaxKneeDb);
    d.limiterCeilingDb = std::clamp(d.limiterCeilingDb, kLimiterMinCeilingDb, 0.0);
    d.limiterReleaseMs = std::clamp(d.limiterReleaseMs, kDynMinTimeMs, kDynMaxReleaseMs);
    for (SendData& d : s.sends) {
        d.levelDb = std::clamp(d.levelDb, kSendMinDb, kSendMaxDb);
        d.pan = std::clamp(d.pan, -1.0, 1.0);
    }
    s.volumeDb = std::clamp(s.volumeDb, kFaderMinDb, kFaderMaxDb);
    s.pan = std::clamp(s.pan, kPanMin, kPanMax);
}

bool readsAutomation(const TrackMix& mix, const QString& key)
{
    if (mix.automation == AutomationMode::Off)
        return false;
    const auto it = mix.automationKeys.values.constFind(key);
    return it != mix.automationKeys.values.constEnd() && it->isAnimated();
}

double faderDbAt(const TrackData& track, double frame)
{
    if (readsAutomation(track.mix, mixkeys::kVolumeDb))
        return track.mix.automationKeys.values.value(mixkeys::kVolumeDb).valueAtTime(frame);
    return track.mix.strip.volumeDb;
}

double panAt(const TrackData& track, double frame)
{
    if (readsAutomation(track.mix, mixkeys::kPan))
        return track.mix.automationKeys.values.value(mixkeys::kPan).valueAtTime(frame);
    return track.mix.strip.pan;
}

bool writeAutomation(TimelineData& data, quint64 trackId, const QString& key, int64_t frame,
    double value, int64_t eraseFrom)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t || frame < 0)
        return false;
    const double base = key == mixkeys::kPan ? t->mix.strip.pan : t->mix.strip.volumeDb;
    AnimatedValue& curve = t->mix.automationKeys.animated(key, base);
    const AnimatedValue before = curve;
    // Overwrite what the fader passed over since the last write (either direction).
    const int64_t lo = std::min(eraseFrom, frame);
    const int64_t hi = std::max(eraseFrom, frame);
    curve.keys.erase(std::remove_if(curve.keys.begin(), curve.keys.end(),
                         [lo, hi](const Keyframe& k) { return k.frame > lo && k.frame < hi; }),
        curve.keys.end());
    curve.setKey(frame, clampAutomation(key, value));
    return !(curve == before);
}

bool clearAutomation(TimelineData& data, quint64 trackId, const QString& key)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t || !t->mix.automationKeys.values.contains(key))
        return false;
    t->mix.automationKeys.values.remove(key);
    return true;
}

EffectInstance* trackEffect(TimelineData& data, quint64 trackId, quint64 effectId)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t)
        return nullptr;
    for (EffectInstance& fx : t->mix.effects) {
        if (fx.id == effectId)
            return &fx;
    }
    return nullptr;
}

quint64 addTrackEffect(TimelineData& data, quint64 trackId, const QString& type, int index)
{
    TrackData* t = audioTrack(data, trackId);
    const EffectDescriptor* d = EffectRegistry::instance().find(type);
    if (!t || !d || d->category != EffectCategory::AudioEffect)
        return 0;
    EffectInstance fx = EffectRegistry::instance().createEffect(type, data.allocateId());
    const quint64 id = fx.id;
    const int at = index < 0 ? int(t->mix.effects.size())
                             : std::min(index, int(t->mix.effects.size()));
    t->mix.effects.insert(at, fx);
    return id;
}

bool removeTrackEffect(TimelineData& data, quint64 trackId, quint64 effectId)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t)
        return false;
    return t->mix.effects.removeIf([effectId](const EffectInstance& fx) { return fx.id == effectId; })
        > 0;
}

bool setTrackEffectEnabled(TimelineData& data, quint64 trackId, quint64 effectId, bool enabled)
{
    EffectInstance* fx = trackEffect(data, trackId, effectId);
    if (!fx || fx->enabled == enabled)
        return false;
    fx->enabled = enabled;
    return true;
}

bool moveTrackEffect(TimelineData& data, quint64 trackId, quint64 effectId, int index)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t)
        return false;
    QVector<EffectInstance>& list = t->mix.effects;
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].id != effectId)
            continue;
        const int to = std::clamp(index, 0, int(list.size()) - 1);
        if (to == i)
            return false;
        list.move(i, to);
        return true;
    }
    return false;
}

} // namespace kk::mixerops
