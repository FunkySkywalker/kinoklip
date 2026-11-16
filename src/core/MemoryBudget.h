// SPDX-License-Identifier: GPL-3.0-or-later
// MemoryBudget — how much memory Kinoklip may use on this machine, and the caps that follow.
//
// The caches (decoded tiles, waveforms, still images, render targets, output frames, open
// decoders) were sized for a machine with 8 GB or more. On a smaller one they are scaled down in
// proportion, so a 4 GB laptop does not swap while editing. The base is the machine's RAM, or
// the value of Preferences > System > Memory and GPU > "Limit Kinoklip memory usage to"
// (Resolve has the same setting, "Limit Resolve memory usage to"). Machines at or above
// kFullSpeedBytes keep every cap as it was.
//
// This file also holds the allocator settings that keep freed memory from piling up (glibc
// keeps freed heap pages in per-thread arenas; a video editor frees large frames all the time).
// Idea from: Firefox / Chromium (bounded arenas, malloc_trim when idle), mpv (frame buffers
// from a pool, large buffers outside the heap).
#pragma once

#include <QString>

#include <cstdint>

namespace kk::membudget {

/// RAM at or above which no cap is reduced.
inline constexpr int64_t kFullSpeedBytes = 8LL << 30;
/// No cap is scaled below this fraction of its full size (a 1 GB machine still works).
inline constexpr double kMinScale = 0.125;

/// Physical memory (cgroup limit when lower), bytes; 0 when unknown.
[[nodiscard]] int64_t totalRamBytes();
/// "MemTotal" of a /proc/meminfo text in bytes; -1 when missing.
[[nodiscard]] int64_t totalFromMeminfo(const QByteArray& meminfo);

/// The memory Kinoklip budgets for: the user's limit when set, else totalRamBytes() (all of
/// it when unknown, so nothing is reduced).
[[nodiscard]] int64_t limitBytes();
/// Sets the user's limit in GiB (0 = automatic). Call before the caches are created.
void setLimitGiB(int gib);
[[nodiscard]] int limitGiB();
/// What limitBytes() would be for @p ramBytes and @p userLimitGiB (pure; tests).
[[nodiscard]] int64_t limitFor(int64_t ramBytes, int userLimitGiB);
/// min(1, limit / kFullSpeedBytes), at least kMinScale. @p limit in bytes.
[[nodiscard]] double scaleFor(int64_t limit);
[[nodiscard]] double scale();

/// @p full scaled by scale(), not below @p floor (and never above @p full).
[[nodiscard]] int64_t scaledBytes(int64_t full, int64_t floor);
[[nodiscard]] int scaledCount(int full, int floor);

/// Allocator settings, once at start (glibc; no-op elsewhere): large blocks (frames, images)
/// come from mmap and go back to the system when freed instead of fragmenting the heap, and
/// the number of arenas is bounded. Environment tunables (GLIBC_TUNABLES) win.
void tuneAllocator();
/// Gives freed heap pages back to the system (glibc malloc_trim); cheap enough to call when
/// idle, never on the playback path.
void trimHeap();

/// One line for the log: RAM, limit and the scale in force.
[[nodiscard]] QString describe();

} // namespace kk::membudget
