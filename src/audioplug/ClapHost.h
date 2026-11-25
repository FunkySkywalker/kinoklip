// SPDX-License-Identifier: GPL-3.0-or-later
// ClapHost — CLAP effect plugins (MIT headers in third_party/clap). Internal to src/audioplug.
#pragma once

#include "audioplug/AudioPluginHost.h"

namespace kk::audioplug::clap {

BundleInfo describe(const QString& clapFile);
std::unique_ptr<Instance> create(
    const PluginInfo& plugin, const QString& clapFile, double sampleRate, QString* error);

} // namespace kk::audioplug::clap
