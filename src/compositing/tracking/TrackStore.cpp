// SPDX-License-Identifier: GPL-3.0-or-later
#include "compositing/tracking/TrackStore.h"

#include "core/NodeRegistry.h"

#include <algorithm>

namespace kk::tracking {

bool storeTrack(CompNode& node, const std::vector<TrackSample>& samples, int64_t referenceFrame,
    int pattern)
{
    if (samples.empty())
        return false;
    if (node.type == nodetypes::PlanarTracker) {
        if (!samples.front().planar)
            return false;
        AnimatedValue& first = node.params.animated(nodekeys::planarCorner(0, false), 0.25);
        if (!first.isAnimated())
            node.params.setBase(nodekeys::RefFrame, double(referenceFrame));
        for (const TrackSample& s : samples) {
            for (int i = 0; i < nodekeys::kPlanarCorners; ++i) {
                node.params.animated(nodekeys::planarCorner(i, false), 0.5)
                    .setKey(s.frame, s.corners[size_t(2 * i)], Interp::Linear);
                node.params.animated(nodekeys::planarCorner(i, true), 0.5)
                    .setKey(s.frame, s.corners[size_t(2 * i + 1)], Interp::Linear);
            }
        }
        return true;
    }
    pattern = std::clamp(pattern, 1, nodekeys::kMaxPatterns);
    AnimatedValue& x = node.params.animated(nodekeys::patternKey(pattern, false), 0.5);
    AnimatedValue& y = node.params.animated(nodekeys::patternKey(pattern, true), 0.5);
    bool anyTracked = false;
    for (int i = 1; i <= nodekeys::kMaxPatterns; ++i)
        anyTracked = anyTracked
            || node.params.values.value(nodekeys::patternKey(i, false)).isAnimated();
    if (!anyTracked)
        node.params.setBase(nodekeys::RefFrame, double(referenceFrame));
    for (const TrackSample& s : samples) {
        x.setKey(s.frame, s.u, Interp::Linear);
        y.setKey(s.frame, s.v, Interp::Linear);
    }
    return true;
}

void clearTrack(CompNode& node, int64_t frame, int pattern)
{
    if (node.type == nodetypes::PlanarTracker) {
        for (int i = 0; i < nodekeys::kPlanarCorners; ++i) {
            for (const bool y : {false, true}) {
                const QString key = nodekeys::planarCorner(i, y);
                node.params.animated(key, 0.5) = AnimatedValue(node.params.value(key, frame, 0.5));
            }
        }
        return;
    }
    for (int i = 1; i <= nodekeys::kMaxPatterns; ++i) {
        if (pattern != 0 && i != pattern)
            continue;
        const QString kx = nodekeys::patternKey(i, false), ky = nodekeys::patternKey(i, true);
        const double u = node.params.value(kx, frame, 0.5);
        const double v = node.params.value(ky, frame, 0.5);
        node.params.animated(kx, 0.5) = AnimatedValue(u);
        node.params.animated(ky, 0.5) = AnimatedValue(v);
    }
}

} // namespace kk::tracking
