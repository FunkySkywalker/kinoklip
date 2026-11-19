// SPDX-License-Identifier: GPL-3.0-or-later
// AutomationLanes — the automation lane that opens below an audio track on the timeline (spec
// 138): open / closed state and chosen parameter per track, lane heights for TimelineLayout,
// the lane widget placement, and AutomationLaneWidget, the curve editor of one track.
#pragma once

#include "core/Animation.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>

class QComboBox;

namespace kk {

struct AppContext;
struct TimelineData;
struct TrackData;
class TimelineLayout;

/// The editor of one track's automation curve (a QWidget over the lane rect of the timeline).
///
/// The left part (the width of the track headers) holds the parameter picker; the rest draws
/// the curve of the chosen parameter on the timeline's own time axis: points with straight (or
/// shaped, see Interp) segments between them, the static value as a dashed line when there is
/// no curve. Click on empty space adds a point, a drag moves a point (in time and value) or, on
/// a segment, moves both its points up or down; Delete removes the selected point; the
/// context menu sets the curve of the segment that leaves a point. Every gesture is one undo
/// step of the Project.
/// Thread-safety: GUI thread only.
class AutomationLaneWidget : public QWidget
{
    Q_OBJECT
public:
    /// Height of the lane (px).
    static constexpr int kLaneHeight = 112;

    AutomationLaneWidget(
        AppContext& ctx, const TimelineLayout& layout, quint64 trackId, QWidget* parent);

    [[nodiscard]] quint64 trackId() const { return m_trackId; }
    /// The parameter shown (a mixkeys key, see MixerOps).
    [[nodiscard]] QString parameterKey() const { return m_key; }
    void setParameterKey(const QString& key);
    /// The parameter picker (tests).
    [[nodiscard]] QComboBox* picker() const { return m_picker; }
    /// Shows the part of @p lane (a lane rect of the timeline viewport) that lies below
    /// @p minTop (under the ruler), or hides the widget.
    void placeLane(const QRect& lane, int minTop);

    /// Widget position of a point (frame, value) and the inverse (the value of the shown
    /// parameter at widget y; bottom edge of a dB parameter = its minimum, "-inf").
    [[nodiscard]] QPointF pointPos(double frame, double value) const;
    [[nodiscard]] double valueAtY(double y) const;
    [[nodiscard]] double frameAtX(double x) const;

    /// The frame of the selected point, if any.
    [[nodiscard]] std::optional<int64_t> selectedPoint() const { return m_selected; }
    void selectPoint(std::optional<int64_t> frame);
    /// Deletes the selected point (one undo step). Returns whether one was deleted.
    bool deleteSelected();
    /// Sets the curve of the segment leaving the selected point.
    bool setSelectedInterp(Interp interp);
    /// Removes the whole curve of the shown parameter (one undo step).
    bool clearCurve();

signals:
    /// The user chose another parameter in the picker.
    void parameterChanged(quint64 trackId, const QString& key);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    enum class Drag {
        None,
        Point,
        PendingSegment,
        Segment
    };
    [[nodiscard]] const TrackData* track() const;
    [[nodiscard]] const AnimatedValue* curve() const;
    [[nodiscard]] QRectF canvas() const; ///< logical (before the vertical window) canvas rect
    [[nodiscard]] std::pair<double, double> displayRange() const;
    [[nodiscard]] double paramMin() const;
    [[nodiscard]] double paramMax() const;
    [[nodiscard]] std::optional<int64_t> pointAt(QPointF pos) const;
    /// Index of the first key of the segment under @p pos, or -1.
    [[nodiscard]] int segmentAt(QPointF pos) const;
    void refreshPicker();
    void layoutPicker();
    void addPointAt(QPointF pos);
    /// Runs @p change on the timeline data as one undo step named @p label.
    bool edit(const QString& label, const std::function<bool(TimelineData&)>& change);
    void liveChange(const std::function<bool(TimelineData&)>& change);

    AppContext& m_ctx;
    const TimelineLayout& m_layout;
    quint64 m_trackId;
    QString m_key;
    QComboBox* m_picker = nullptr;
    int m_yOffset = 0;
    std::optional<int64_t> m_selected;
    Drag m_drag = Drag::None;
    QPointF m_pressPos;
    int64_t m_dragFrame = 0;
    int64_t m_segA = 0, m_segB = 0;
    double m_segValueA = 0.0, m_segValueB = 0.0;
};

/// Owns the automation lanes of a TimelineWidget (one per track with its lane open).
///
/// A lane is opened with the Automation button of a track header (Audio page) or the header's
/// context menu; the state is view state, not saved in the project. The lane grows the track's
/// row (TimelineLayout::setAutomationLaneHeights()).
/// Thread-safety: GUI thread only.
class AutomationLanes : public QObject
{
    Q_OBJECT
public:
    AutomationLanes(
        AppContext& ctx, const TimelineLayout& layout, QWidget* viewport, QObject* parent = nullptr);
    ~AutomationLanes() override;

    [[nodiscard]] bool isOpen(quint64 trackId) const { return m_open.contains(trackId); }
    /// The open lanes' tracks (for painting the header buttons).
    [[nodiscard]] const QSet<quint64>& openTracks() const { return m_open; }
    /// Opens / closes the lane of audio track @p trackId.
    void setOpen(quint64 trackId, bool open);
    void toggle(quint64 trackId) { setOpen(trackId, !isOpen(trackId)); }
    /// The parameter chosen for @p trackId (Volume until the user picks another).
    [[nodiscard]] QString parameter(quint64 trackId) const;
    void setParameter(quint64 trackId, const QString& key);

    /// Lane heights per track id for TimelineLayout::setAutomationLaneHeights() (drops tracks
    /// that no longer exist).
    [[nodiscard]] QHash<quint64, int> laneHeights(const TimelineData& data);
    /// Places, shows and hides the lane widgets after a layout / scroll / zoom change.
    void syncGeometry();
    /// Closes every lane (another timeline became active).
    void reset();
    /// The lane widget of @p trackId (tests), or nullptr.
    [[nodiscard]] AutomationLaneWidget* widget(quint64 trackId) const;

signals:
    /// Lane heights changed: the timeline must rebuild its layout (and call syncGeometry()).
    void layoutChanged();

private:
    AppContext& m_ctx;
    const TimelineLayout& m_layout;
    QWidget* m_viewport;
    QSet<quint64> m_open;
    QHash<quint64, QString> m_parameter;
    QHash<quint64, QPointer<AutomationLaneWidget>> m_widgets;
};

} // namespace kk
