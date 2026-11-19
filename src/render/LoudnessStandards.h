// SPDX-License-Identifier: GPL-3.0-or-later
// LoudnessStandards — the loudness targets of the meter's standards (EBU R128, ATSC A/85,
// streaming, podcast, custom) and the over / under reading against one (spec 134).
#pragma once

#include <QString>

#include <array>

namespace kk::loudness {

/// The loudness meter's standards. Each has a programme loudness target (LUFS, integrated) and
/// a true peak ceiling (dBTP) that a deliverable should stay below.
enum class Standard {
    EbuR128, ///< EBU R128: -23 LUFS, true peak -1 dBTP
    AtscA85, ///< ATSC A/85: -24 LKFS, true peak -2 dBTP
    Streaming, ///< YouTube and other streaming services: -14 LUFS, true peak -1 dBTP
    Podcast, ///< podcasts: -16 LUFS, true peak -1 dBTP
    Custom ///< the user's own target and ceiling
};

inline constexpr int kStandardCount = 5;
/// Limits of a custom target (LUFS) and true peak ceiling (dBTP).
inline constexpr double kMinTargetLufs = -60.0;
inline constexpr double kMaxTargetLufs = 0.0;
inline constexpr double kMinCeilingDb = -20.0;
inline constexpr double kMaxCeilingDb = 0.0;
/// Within this many LU of the target a programme counts as "on target".
inline constexpr double kOnTargetLu = 1.0;

/// A loudness target: the standard and, for Custom, its own numbers.
struct Target
{
    Standard standard = Standard::EbuR128;
    double customLufs = -23.0; ///< used when standard == Custom
    double customTruePeakDb = -1.0; ///< used when standard == Custom

    /// The integrated loudness to reach (LUFS).
    [[nodiscard]] double lufs() const;
    /// The true peak ceiling (dBTP).
    [[nodiscard]] double truePeakDb() const;

    friend bool operator==(const Target&, const Target&) = default;
};

/// All standards in menu order.
[[nodiscard]] const std::array<Standard, kStandardCount>& standards();
/// Preset target of a standard (LUFS); Custom gives the EBU value as a starting point.
[[nodiscard]] double presetLufs(Standard standard);
/// Preset true peak ceiling of a standard (dBTP).
[[nodiscard]] double presetTruePeakDb(Standard standard);
/// Stable key of a standard ("ebu_r128", "atsc_a85", "streaming", "podcast", "custom").
[[nodiscard]] QString standardKey(Standard standard);
/// Parses standardKey(); unknown text gives @p fallback.
[[nodiscard]] Standard standardFromKey(const QString& key, Standard fallback);
/// Menu text of a standard, translated ("EBU R128 (-23 LUFS)").
[[nodiscard]] QString standardName(Standard standard);

/// Loudness minus target in LU (positive = louder than the target); 0 for a silent reading
/// (<= -199).
[[nodiscard]] double deviationLu(double loudnessLufs, double targetLufs);
/// "Over", "Under" or "On target" (within kOnTargetLu), translated; empty for a silent reading.
[[nodiscard]] QString deviationText(double loudnessLufs, double targetLufs);

/// Clamps a target's custom numbers to the allowed ranges.
[[nodiscard]] Target clamped(Target target);
/// Target <-> JSON-friendly strings are done by the owners; these two write / read one target
/// as a "standard|lufs|truePeak" string (QSettings, project data).
[[nodiscard]] QString targetToString(const Target& target);
[[nodiscard]] Target targetFromString(const QString& text, const Target& fallback);

} // namespace kk::loudness
