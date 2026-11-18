// SPDX-License-Identifier: GPL-3.0-or-later
// CompViewOptions implementation.
#include "ui/comp/CompViewOptions.h"

#include "core/CompOps.h"
#include "core/NodeRegistry.h"

#include <QSettings>

#include <algorithm>

namespace kk {

namespace {
const QString kPrefix = QStringLiteral("compositing/view/");
}

CompViewOptions::CompViewOptions()
{
    load();
}

CompViewOptions& CompViewOptions::instance()
{
    static CompViewOptions options;
    return options;
}

void CompViewOptions::load()
{
    QSettings s;
    CompViewOptionsData o;
    o.proxy = s.value(kPrefix + QStringLiteral("proxy"), false).toBool();
    o.proxyRatio = s.value(kPrefix + QStringLiteral("proxyRatio"), 2).toInt() >= 4 ? 4 : 2;
    o.autoProxy = s.value(kPrefix + QStringLiteral("autoProxy"), false).toBool();
    o.highQuality = s.value(kPrefix + QStringLiteral("highQuality"), true).toBool();
    o.motionBlur = s.value(kPrefix + QStringLiteral("motionBlur"), true).toBool();
    o.diskCache = s.value(kPrefix + QStringLiteral("diskCache"), false).toBool();
    if (o != m_options) {
        m_options = o;
        emit changed();
    }
}

void CompViewOptions::setOptions(const CompViewOptionsData& options)
{
    CompViewOptionsData o = options;
    o.proxyRatio = o.proxyRatio >= 4 ? 4 : 2;
    if (o == m_options)
        return;
    m_options = o;
    QSettings s;
    s.setValue(kPrefix + QStringLiteral("proxy"), o.proxy);
    s.setValue(kPrefix + QStringLiteral("proxyRatio"), o.proxyRatio);
    s.setValue(kPrefix + QStringLiteral("autoProxy"), o.autoProxy);
    s.setValue(kPrefix + QStringLiteral("highQuality"), o.highQuality);
    s.setValue(kPrefix + QStringLiteral("motionBlur"), o.motionBlur);
    s.setValue(kPrefix + QStringLiteral("diskCache"), o.diskCache);
    emit changed();
}

bool CompViewOptions::proxyInEffect(bool interacting) const
{
    return m_options.proxy || (m_options.autoProxy && interacting);
}

QSize CompViewOptions::renderSize(QSize size, bool interacting) const
{
    if (!proxyInEffect(interacting) || size.isEmpty())
        return size;
    const int r = m_options.proxyRatio;
    return QSize(std::min(size.width(), std::max(16, size.width() / r)),
        std::min(size.height(), std::max(16, size.height() / r)));
}

int CompViewOptions::cacheFlags() const
{
    return (m_options.highQuality ? 1 : 0) | (m_options.motionBlur ? 2 : 0);
}

Composition draftComposition(const Composition& comp, bool noMotionBlur, bool draftFilter)
{
    Composition out = comp;
    for (CompNode& n : out.nodes) {
        if (noMotionBlur && n.params.statics.contains(nodekeys::MotionBlur))
            n.params.statics[nodekeys::MotionBlur] = false;
        if (draftFilter && (n.type == nodetypes::Merge || n.type == nodetypes::Transform))
            n.params.statics[nodekeys::FilterMethod] = filtermethods::Nearest;
    }
    return out;
}

std::shared_ptr<const TimelineData> CompViewOptions::viewerTimeline(
    const std::shared_ptr<const TimelineData>& timeline, quint64 clipId) const
{
    const bool noBlur = !m_options.motionBlur || !m_options.highQuality;
    const bool draftFilter = !m_options.highQuality;
    if (!timeline || (!noBlur && !draftFilter))
        return timeline;
    // Find the clip; copy the timeline only when it has a composition worth changing.
    auto copy = std::make_shared<TimelineData>(*timeline);
    bool changed = false;
    for (auto* tracks : {&copy->videoTracks}) {
        for (TrackData& t : *tracks) {
            for (ClipData& c : t.clips) {
                if (c.id == clipId && c.composition) {
                    c.composition = draftComposition(*c.composition, noBlur, draftFilter);
                    changed = true;
                }
            }
        }
    }
    return changed ? std::shared_ptr<const TimelineData>(copy) : timeline;
}

} // namespace kk
