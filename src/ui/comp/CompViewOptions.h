// SPDX-License-Identifier: GPL-3.0-or-later
// CompViewOptions — the Compositing viewers' quality switches (spec 121): Proxy, Auto Proxy, High
// Quality, Motion Blur and the disk cache. They are per user and the same in both viewers (the
// buttons are on the transport bar, like Fusion's HiQ / MB / Prx / APrx), stored in the settings.
// They only change what the viewers show: the Edit page, playback and every export render the
// composition at full quality.
#pragma once

#include "core/TimelineData.h"
#include "render/RenderTypes.h"

#include <QObject>
#include <QSize>

#include <memory>

namespace kk {

struct CompViewOptionsData
{
    bool proxy = false; ///< render at 1 / proxyRatio of the viewer's size and scale it up
    int proxyRatio = 2; ///< 2 or 4
    bool autoProxy = false; ///< proxy only while playing or changing something
    bool highQuality = true; ///< off: a draft (nearest filtering, no motion blur)
    bool motionBlur = true; ///< off: no motion blur in the viewers
    bool diskCache = false; ///< keep rendered pictures in the cache folder

    friend bool operator==(const CompViewOptionsData&, const CompViewOptionsData&) = default;
};

/// Process-wide holder of CompViewOptionsData (like ViewerOverlaySettings).
///
/// Thread-safety: GUI thread only.
class CompViewOptions : public QObject
{
    Q_OBJECT
public:
    static CompViewOptions& instance();

    [[nodiscard]] const CompViewOptionsData& options() const { return m_options; }
    /// Stores @p options (settings "compositing/view/...") and emits changed() if they differ.
    void setOptions(const CompViewOptionsData& options);
    /// Re-reads the settings (tests clear them); emits changed() if needed.
    void load();

    /// The size to render a viewer picture of @p size at: @p size, or 1 / ratio of it while a
    /// proxy is in effect (Proxy on, or Auto Proxy while @p interacting). Never below 16 x 16 or
    /// larger than @p size.
    [[nodiscard]] QSize renderSize(QSize size, bool interacting) const;
    /// True if the viewer shows a proxy now.
    [[nodiscard]] bool proxyInEffect(bool interacting) const;

    /// Flags (bit set) that go into the disk cache key: what changes the picture besides the
    /// composition, frame, node and size.
    [[nodiscard]] int cacheFlags() const;

    /// The timeline the viewers render for @p clipId: @p timeline itself, or (High Quality off,
    /// Motion Blur off) a copy in which the clip's composition has its motion blur switched off and
    /// nearest-neighbour filtering. A copy shares everything else with @p timeline.
    [[nodiscard]] std::shared_ptr<const TimelineData> viewerTimeline(
        const std::shared_ptr<const TimelineData>& timeline, quint64 clipId) const;

signals:
    void changed();

private:
    CompViewOptions();
    CompViewOptionsData m_options;
};

/// A copy of @p comp with motion blur switched off on every node and, with @p draftFilter, the
/// filter method of every node that has one set to nearest. Pure (tests).
[[nodiscard]] Composition draftComposition(const Composition& comp, bool noMotionBlur, bool draftFilter);

} // namespace kk
