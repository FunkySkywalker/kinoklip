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

/// "send.N" -> N, or -1.
int sendBusOfKey(const QString& key)
{
    if (!key.startsWith(mixkeys::kSendPrefix))
        return -1;
    bool ok = false;
    const int id = key.mid(mixkeys::kSendPrefix.size()).toInt(&ok);
    return ok && id > 0 ? id : -1;
}

/// "fx.<id>.<param>" -> true with the pieces.
bool splitEffectKey(const QString& key, quint64& effectId, QString& param)
{
    if (!key.startsWith(mixkeys::kEffectPrefix))
        return false;
    const qsizetype dot = key.indexOf(QLatin1Char('.'), mixkeys::kEffectPrefix.size());
    if (dot < 0)
        return false;
    bool ok = false;
    effectId = key.mid(mixkeys::kEffectPrefix.size(), dot - mixkeys::kEffectPrefix.size())
                   .toULongLong(&ok);
    param = key.mid(dot + 1);
    return ok && effectId != 0 && !param.isEmpty();
}

/// Range of automation @p key of @p track; false for a key that is none of the known kinds.
bool rangeOfKey(const TrackData& track, const QString& key, double& lo, double& hi)
{
    if (key == mixkeys::kVolumeDb) {
        lo = kFaderMinDb;
        hi = kFaderMaxDb;
        return true;
    }
    if (key == mixkeys::kPan) {
        lo = kPanMin;
        hi = kPanMax;
        return true;
    }
    if (key == mixkeys::kMute) {
        lo = 0.0;
        hi = 1.0;
        return true;
    }
    if (sendBusOfKey(key) > 0) {
        lo = kSendMinDb;
        hi = kSendMaxDb;
        return true;
    }
    quint64 id = 0;
    QString param;
    if (splitEffectKey(key, id, param)) {
        for (const EffectInstance& fx : track.mix.effects) {
            if (fx.id != id)
                continue;
            const EffectDescriptor* d = EffectRegistry::instance().find(fx.type);
            const ParamDescriptor* pd = d ? d->param(param) : nullptr;
            if (!pd || !pd->isScalar())
                return false;
            lo = pd->min;
            hi = pd->max;
            return true;
        }
    }
    return false;
}

double clampAutomation(const TrackData& track, const QString& key, double value)
{
    double lo = 0.0, hi = 0.0;
    if (!rangeOfKey(track, key, lo, hi))
        return value;
    return std::clamp(value, lo, hi);
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

bool mutedAt(const TrackData& track, double frame)
{
    if (readsAutomation(track.mix, mixkeys::kMute))
        return track.mix.automationKeys.values.value(mixkeys::kMute).valueAtTime(frame) >= 0.5;
    return track.muted;
}

QString sendKey(int busId)
{
    return mixkeys::kSendPrefix + QString::number(busId);
}

QString effectKey(quint64 effectId, const QString& paramKey)
{
    return mixkeys::kEffectPrefix + QString::number(effectId) + QLatin1Char('.') + paramKey;
}

bool parseSendKey(const QString& key, int& busId)
{
    busId = sendBusOfKey(key);
    return busId > 0;
}

bool parseEffectKey(const QString& key, quint64& effectId, QString& paramKey)
{
    return splitEffectKey(key, effectId, paramKey);
}

double staticAutomationValue(const TrackData& track, const QString& key)
{
    if (key == mixkeys::kVolumeDb)
        return track.mix.strip.volumeDb;
    if (key == mixkeys::kPan)
        return track.mix.strip.pan;
    if (key == mixkeys::kMute)
        return track.muted ? 1.0 : 0.0;
    if (const int bus = sendBusOfKey(key); bus > 0) {
        for (const SendData& s : track.mix.strip.sends) {
            if (s.busId == bus)
                return s.levelDb;
        }
        return 0.0;
    }
    quint64 id = 0;
    QString param;
    if (splitEffectKey(key, id, param)) {
        for (const EffectInstance& fx : track.mix.effects) {
            if (fx.id != id)
                continue;
            const EffectDescriptor* d = EffectRegistry::instance().find(fx.type);
            const ParamDescriptor* pd = d ? d->param(param) : nullptr;
            return fx.params.value(param, 0, pd ? pd->def : 0.0);
        }
    }
    return 0.0;
}

double automationValueAt(const TrackData& track, const QString& key, double frame)
{
    if (readsAutomation(track.mix, key))
        return track.mix.automationKeys.values.value(key).valueAtTime(frame);
    return staticAutomationValue(track, key);
}

QVector<AutomationParam> automationParams(const TimelineData& data, const TrackData& track)
{
    QVector<AutomationParam> out;
    out.append({mixkeys::kVolumeDb, QObject::tr("Volume"), kFaderMinDb, kFaderMaxDb, 0.0,
        QObject::tr(" dB"), false});
    out.append({mixkeys::kPan, QObject::tr("Pan"), kPanMin, kPanMax, 0.0, QString(), false});
    out.append({mixkeys::kMute, QObject::tr("Mute"), 0.0, 1.0, 0.0, QString(), true});
    for (const SendData& s : track.mix.strip.sends) {
        const BusData* b = bus(data, s.busId);
        out.append({sendKey(s.busId),
            QObject::tr("Send to %1").arg(b ? busDisplayName(*b) : QString::number(s.busId)),
            kSendMinDb, kSendMaxDb, 0.0, QObject::tr(" dB"), false});
    }
    for (const EffectInstance& fx : track.mix.effects) {
        const EffectDescriptor* d = EffectRegistry::instance().find(fx.type);
        if (!d)
            continue;
        for (const ParamDescriptor& pd : d->params) {
            if (!pd.isScalar() || pd.hidden)
                continue;
            out.append({effectKey(fx.id, pd.key), d->name + QStringLiteral(": ") + pd.label,
                pd.min, pd.max, pd.def, pd.unit, false});
        }
    }
    return out;
}

std::optional<AutomationParam> automationParam(
    const TimelineData& data, const TrackData& track, const QString& key)
{
    for (const AutomationParam& p : automationParams(data, track)) {
        if (p.key == key)
            return p;
    }
    return std::nullopt;
}

namespace {

/// The curve of @p key on @p t, created from the static value when missing.
AnimatedValue& curveOf(TrackData& t, const QString& key)
{
    return t.mix.automationKeys.animated(key, staticAutomationValue(t, key));
}

/// Drops the curve entry of @p key when its last key is gone (the static value plays again).
void dropIfEmpty(TrackData& t, const QString& key)
{
    const auto it = t.mix.automationKeys.values.find(key);
    if (it != t.mix.automationKeys.values.end() && !it->isAnimated())
        t.mix.automationKeys.values.erase(it);
}

void markStepped(Keyframe& k, const QString& key)
{
    if (key == mixkeys::kMute)
        applyInterp(k, Interp::Hold);
}

} // namespace

bool writeAutomation(TimelineData& data, quint64 trackId, const QString& key, int64_t frame,
    double value, int64_t eraseFrom)
{
    TrackData* t = audioTrack(data, trackId);
    double lo = 0.0, hi = 0.0;
    if (!t || frame < 0 || !rangeOfKey(*t, key, lo, hi))
        return false;
    AnimatedValue& curve = curveOf(*t, key);
    const AnimatedValue before = curve;
    // Overwrite what the control passed over since the last write (either direction).
    const int64_t from = std::min(eraseFrom, frame);
    const int64_t to = std::max(eraseFrom, frame);
    curve.keys.erase(std::remove_if(curve.keys.begin(), curve.keys.end(),
                         [from, to](const Keyframe& k) { return k.frame > from && k.frame < to; }),
        curve.keys.end());
    const double v = std::clamp(value, lo, hi);
    // A stepped parameter (Mute) needs a key only where its value changes.
    const bool stepped = key == mixkeys::kMute;
    const qsizetype at = curve.indexOf(frame);
    qsizetype prev = -1;
    for (qsizetype i = 0; i < curve.keys.size() && curve.keys[i].frame < frame; ++i)
        prev = i;
    if (stepped && at < 0 && prev >= 0 && curve.keys[prev].value == v) {
        // same step as before: nothing to add
    } else {
        curve.setKey(frame, v);
        if (stepped)
            markStepped(curve.keys[curve.indexOf(frame)], key);
    }
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

bool addAutomationPoint(
    TimelineData& data, quint64 trackId, const QString& key, int64_t frame, double value)
{
    TrackData* t = audioTrack(data, trackId);
    double lo = 0.0, hi = 0.0;
    if (!t || frame < 0 || !rangeOfKey(*t, key, lo, hi))
        return false;
    AnimatedValue& curve = curveOf(*t, key);
    const AnimatedValue before = curve;
    curve.setKey(frame, std::clamp(value, lo, hi));
    markStepped(curve.keys[curve.indexOf(frame)], key);
    return !(curve == before);
}

bool moveAutomationPoint(TimelineData& data, quint64 trackId, const QString& key,
    int64_t fromFrame, int64_t toFrame, double value)
{
    TrackData* t = audioTrack(data, trackId);
    double lo = 0.0, hi = 0.0;
    if (!t || toFrame < 0 || !rangeOfKey(*t, key, lo, hi))
        return false;
    const auto it = t->mix.automationKeys.values.find(key);
    if (it == t->mix.automationKeys.values.end() || !it->hasKeyAt(fromFrame))
        return false;
    const AnimatedValue before = *it;
    Keyframe k = it->keys[it->indexOf(fromFrame)];
    it->removeKey(fromFrame); // the last key moves its value into base: putKey brings it back
    k.frame = toFrame;
    k.value = std::clamp(value, lo, hi);
    it->putKey(k);
    return !(*it == before);
}

bool removeAutomationPoint(TimelineData& data, quint64 trackId, const QString& key, int64_t frame)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t)
        return false;
    const auto it = t->mix.automationKeys.values.find(key);
    if (it == t->mix.automationKeys.values.end() || !it->hasKeyAt(frame))
        return false;
    it->removeKey(frame);
    dropIfEmpty(*t, key);
    return true;
}

bool setAutomationPointInterp(
    TimelineData& data, quint64 trackId, const QString& key, int64_t frame, Interp interp)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t)
        return false;
    const auto it = t->mix.automationKeys.values.find(key);
    if (it == t->mix.automationKeys.values.end() || !it->hasKeyAt(frame))
        return false;
    Keyframe& k = it->keys[it->indexOf(frame)];
    const Keyframe before = k;
    applyInterp(k, interp);
    return !(k == before);
}

bool moveAutomationSegment(TimelineData& data, quint64 trackId, const QString& key,
    int64_t frameA, int64_t frameB, double deltaValue)
{
    TrackData* t = audioTrack(data, trackId);
    double lo = 0.0, hi = 0.0;
    if (!t || !rangeOfKey(*t, key, lo, hi) || !std::isfinite(deltaValue))
        return false;
    const auto it = t->mix.automationKeys.values.find(key);
    if (it == t->mix.automationKeys.values.end() || !it->hasKeyAt(frameA)
        || !it->hasKeyAt(frameB))
        return false;
    bool changed = false;
    for (int64_t f : {frameA, frameB}) {
        Keyframe& k = it->keys[it->indexOf(f)];
        const double v = std::clamp(k.value + deltaValue, lo, hi);
        changed |= v != k.value;
        k.value = v;
    }
    return changed;
}

bool trimAutomation(TimelineData& data, quint64 trackId, const QString& key,
    const AnimatedValue& original, int64_t fromFrame, int64_t toFrame, double offset)
{
    TrackData* t = audioTrack(data, trackId);
    double lo = 0.0, hi = 0.0;
    if (!t || toFrame < 0 || !rangeOfKey(*t, key, lo, hi) || !std::isfinite(offset))
        return false;
    AnimatedValue& curve = curveOf(*t, key);
    const AnimatedValue before = curve;
    const int64_t a = std::min(fromFrame, toFrame);
    const int64_t b = std::max(fromFrame, toFrame);
    // The stretch (a, b] is rebuilt from the original curve: its keys inside, plus a key at b.
    // The key at a was written by the previous step (it keeps the offset it had then).
    curve.keys.erase(std::remove_if(curve.keys.begin(), curve.keys.end(),
                         [a, b](const Keyframe& k) {
                             return (k.frame > a && k.frame <= b) || (a == b && k.frame == a);
                         }),
        curve.keys.end());
    for (const Keyframe& k : original.keys) {
        if (k.frame > a && k.frame < b) {
            Keyframe copy = k;
            copy.value = std::clamp(k.value + offset, lo, hi);
            curve.putKey(copy);
        }
    }
    auto putOffset = [&](int64_t f) {
        Keyframe k;
        double v = original.valueAtTime(double(f));
        if (original.hasKeyAt(f)) {
            k = original.keys[original.indexOf(f)];
            v = k.value;
        }
        k.frame = f;
        k.value = std::clamp(v + offset, lo, hi);
        curve.putKey(k);
    };
    putOffset(b);
    if (a < b && !curve.hasKeyAt(a))
        putOffset(a);
    return !(curve == before);
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
