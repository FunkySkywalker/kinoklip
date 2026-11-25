// SPDX-License-Identifier: GPL-3.0-or-later
// Lv2Host — LV2 effect plugins through lilv (ISC). Without lilv at build time (KK_HAVE_LV2 unset)
// every function reports that LV2 is not available. Internal to src/audioplug.
#pragma once

#include "audioplug/AudioPluginHost.h"

namespace kk::audioplug::lv2 {

bool supported();
BundleInfo describe(const QString& bundleFolder);
std::unique_ptr<Instance> create(
    const PluginInfo& plugin, const QString& bundleFolder, double sampleRate, QString* error);

} // namespace kk::audioplug::lv2
