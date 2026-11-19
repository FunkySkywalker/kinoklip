// SPDX-License-Identifier: GPL-3.0-or-later
// MixerOps — pure edits of the Audio page's mixer (buses, routing, channel strips, fader / pan
// automation). Callable without the mouse (docs/archive/plan.md 5.9b); the mixer widgets only fill in the
// arguments and wrap each gesture in one Project edit.
#pragma once

#include "core/TimelineData.h"

#include <cstdint>
#include <optional>

namespace kk {

/// A mixer channel: an audio track (by TrackData::id) or a bus (by BusData::id).
struct MixTarget
{
    enum class Kind {
        Track,
        Bus
    };
    Kind kind = Kind::Track;
    quint64 id = 0;

    /// The track with TrackData::id @p trackId.
    [[nodiscard]] static MixTarget track(quint64 trackId) { return {Kind::Track, trackId}; }
    /// The bus with BusData::id @p busId.
    [[nodiscard]] static MixTarget bus(int busId) { return {Kind::Bus, quint64(busId)}; }
    [[nodiscard]] bool isBus() const { return kind == Kind::Bus; }

    friend bool operator==(const MixTarget&, const MixTarget&) = default;
};

namespace mixerops {

/// The audio track with id @p trackId, or nullptr.
[[nodiscard]] TrackData* audioTrack(TimelineData& data, quint64 trackId);
/// The audio track with id @p trackId, or nullptr (const).
[[nodiscard]] const TrackData* audioTrack(const TimelineData& data, quint64 trackId);
/// The bus with id @p busId, or nullptr.
[[nodiscard]] BusData* bus(TimelineData& data, int busId);
/// The bus with id @p busId, or nullptr (const).
[[nodiscard]] const BusData* bus(const TimelineData& data, int busId);
/// Index of bus @p busId in MixerData::buses, or -1.
[[nodiscard]] int busIndex(const MixerData& mixer, int busId);

/// The channel strip of @p target, or nullptr if it does not exist.
[[nodiscard]] ChannelStrip* strip(TimelineData& data, const MixTarget& target);
/// The channel strip of @p target, or nullptr (const).
[[nodiscard]] const ChannelStrip* strip(const TimelineData& data, const MixTarget& target);

/// Display name of @p target: the track's name (automatic: "A1", "A2", ...) or the bus
/// name; empty for an unknown target.
[[nodiscard]] QString targetName(const TimelineData& data, const MixTarget& target);

/// Adds a bus (mono or stereo) named @p name (empty = automatic) and returns its id.
int addBus(TimelineData& data, bool mono, const QString& name = {});
/// Renames bus @p busId (empty = automatic name). Returns whether anything changed.
bool renameBus(TimelineData& data, int busId, const QString& name);
/// Sets the channel format of bus @p busId. Returns whether anything changed.
bool setBusMono(TimelineData& data, int busId, bool mono);
/// Deletes bus @p busId (never the main bus): tracks stop feeding it and, if it was the
/// output, the main bus becomes the output. Returns whether anything changed.
bool removeBus(TimelineData& data, int busId);
/// Makes bus @p busId the one heard in playback and rendered by export. Returns whether
/// anything changed (false for an unknown bus).
bool setOutputBus(TimelineData& data, int busId);

/// Routes track @p trackId to bus @p busId (@p on) or removes that route. Unknown buses are
/// refused. Returns whether anything changed.
bool setTrackRoute(TimelineData& data, quint64 trackId, int busId, bool on);
/// Sets the channel format of track @p trackId. Returns whether anything changed.
bool setTrackMono(TimelineData& data, quint64 trackId, bool mono);
/// Sets the automation mode of track @p trackId. Returns whether anything changed.
bool setAutomationMode(TimelineData& data, quint64 trackId, AutomationMode mode);

// ---- Key (side chain) of the Dynamics section (spec 136) ----

/// True if the strip of @p strip may take its key (the detector of its Gate and Compressor)
/// from the track or bus @p source (KeyKind::Self is always allowed). Refused: an unknown track
/// or bus, the strip itself, and every source that would make a loop: a source that depends on
/// the strip. A channel depends on its own key source, and a bus on every track routed to it,
/// so a track cannot key itself through a bus it feeds, and two tracks cannot key each other.
[[nodiscard]] bool keySourceAllowed(
    const TimelineData& data, const MixTarget& strip, KeyKind kind, quint64 sourceId);
/// Sets the key of @p strip. Refused (false) when keySourceAllowed() says no; also false when
/// nothing changed.
bool setKeySource(TimelineData& data, const MixTarget& strip, KeyKind kind, quint64 sourceId);
/// The key source of @p strip that really applies: KeyKind::Self when none is set or the
/// source no longer exists or would loop (a hand-edited or damaged project).
[[nodiscard]] DynamicsSettings effectiveKey(const TimelineData& data, const MixTarget& strip);

// ---- Track effect slots (spec 133) ----

/// Adds an effect of audio effect descriptor @p type (EffectRegistry, category AudioEffect;
/// the same ids and processors as a clip's audio effects) to track @p trackId at @p index (-1 =
/// the end of the list). The new slot has the descriptor's defaults and a fresh id. Returns the
/// id, or 0 for an unknown track or a type that is not an audio effect.
quint64 addTrackEffect(TimelineData& data, quint64 trackId, const QString& type, int index = -1);
/// Removes slot @p effectId of track @p trackId. Returns whether anything changed.
bool removeTrackEffect(TimelineData& data, quint64 trackId, quint64 effectId);
/// Switches slot @p effectId of track @p trackId on (@p enabled) or bypasses it. Returns
/// whether anything changed.
bool setTrackEffectEnabled(TimelineData& data, quint64 trackId, quint64 effectId, bool enabled);
/// Moves slot @p effectId of track @p trackId to list position @p index (clamped). Returns
/// whether anything changed.
bool moveTrackEffect(TimelineData& data, quint64 trackId, quint64 effectId, int index);
/// The slot @p effectId of track @p trackId, or nullptr.
[[nodiscard]] EffectInstance* trackEffect(TimelineData& data, quint64 trackId, quint64 effectId);

// ---- Normalize (spec 134) ----

/// Raises (or lowers) the level of @p target by @p deltaDb with its fader, as Normalize Audio
/// Levels does for a track, a bus or the main mix (the output bus): the static fader value and,
/// for a track with a volume curve, every key of the curve move by the same amount, so a
/// written automation keeps its shape. Values stay within kFaderMinDb ... kFaderMaxDb. Returns
/// whether anything changed (false for an unknown target, a zero or non-finite delta, or a
/// fader already at its limit).
bool offsetFader(TimelineData& data, const MixTarget& target, double deltaDb);
/// The fader value (static) of @p target in dB, or 0 for an unknown target.
[[nodiscard]] double staticFaderDb(const TimelineData& data, const MixTarget& target);

/// Clamps every value of @p strip to the documented control ranges (loader, ops).
void clampStrip(ChannelStrip& strip);

/// True if @p mix plays its automation curve for @p key ("volumeDb" / "pan"): not Off and
/// the curve has keys.
[[nodiscard]] bool readsAutomation(const TrackMix& mix, const QString& key);
/// Fader value (dB) of @p track at fractional timeline frame @p frame: the automation curve
/// while it is read, else the strip's static value.
[[nodiscard]] double faderDbAt(const TrackData& track, double frame);
/// Pan of @p track at fractional timeline frame @p frame (automation or the static value).
[[nodiscard]] double panAt(const TrackData& track, double frame);

/// Records automation of track @p trackId: a key with @p value at timeline frame @p frame
/// for @p key, after removing the keys in (@p eraseFrom, @p frame) (the stretch the moving
/// fader passed over since the previous write; pass @p frame for none). Values are clamped
/// to the fader / pan range. Returns whether anything changed.
bool writeAutomation(TimelineData& data, quint64 trackId, const QString& key, int64_t frame,
    double value, int64_t eraseFrom);
/// Removes every automation key of @p key on track @p trackId. Returns whether anything
/// changed.
bool clearAutomation(TimelineData& data, quint64 trackId, const QString& key);

} // namespace mixerops
} // namespace kk
