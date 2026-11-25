// SPDX-License-Identifier: GPL-3.0-or-later
// AudioPluginHost — the hosts of LV2 (through lilv) and CLAP audio effect plugins
// (docs/plugins/audio-plugins.md, spec 159). Two entry points:
//   describeBundle()  what a bundle contains (the probe program runs it in a process of its own)
//   Instance          one running plugin that filters interleaved stereo float in place, the
//                     shape of dsp::ClipEffect (render/AudioClipEffects.h)
// Mono plugins run as two instances (left, right); plugins with more than two channels get the
// first two, silence on the others; side-chain inputs get silence. No plugin user interface.
#pragma once

#include "audioplug/AudioPluginInfo.h"

#include <memory>

namespace kk::audioplug {

/// True if this build can host LV2 (lilv was found when Kinoklip was built).
[[nodiscard]] bool lv2Supported();

/// Loads the bundle (an .lv2 folder or a .clap file) and describes its plugins. A plugin that
/// Kinoklip cannot run has PluginInfo::error set. May crash if the plugin does: run it in the
/// probe program (src/audioplug/probe_main.cpp), never in the editor.
[[nodiscard]] BundleInfo describeBundle(const QString& format, const QString& path);

/// One running plugin instance.
class Instance
{
public:
    /// Largest block the plugin is asked to process at once; longer blocks are split.
    static constexpr int kMaxBlock = 1024;

    virtual ~Instance() = default;

    /// Creates and activates @p plugin of the bundle at @p bundlePath. On failure returns null
    /// and says why in @p error. Loads the plugin's library: may take a moment, allocates.
    [[nodiscard]] static std::unique_ptr<Instance> create(const PluginInfo& plugin,
        const QString& bundlePath, double sampleRate, QString* error);

    /// The value of parameter @p index (index into PluginInfo::params), in the plugin's units.
    /// Real-time safe: stores the value; the next process() hands it to the plugin.
    virtual void setParam(int index, double value) = 0;
    /// Filters @p frames interleaved stereo frames in place. No allocation, no lock of our own.
    virtual void process(float* io, int frames) = 0;
    /// Clears the plugin's memory (deactivates and activates it again).
    virtual void reset() = 0;
    /// The delay the plugin reports, in frames.
    [[nodiscard]] virtual int latencyFrames() const = 0;
};

} // namespace kk::audioplug
