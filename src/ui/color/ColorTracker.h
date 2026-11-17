// SPDX-License-Identifier: GPL-3.0-or-later
// ColorTracker — the Tracker palette of the Color page (spec section 107), Resolve's Tracker
// with its Window and Stabilizer modes. Window mode: Track Forward / Track Reverse follow the
// subject of the selected node's window through the clip with the TrackerRunner (the playback
// engine's pictures, without grades) and store the result as the node's tracker data (GradeTrack,
// one undo step); Pan / Tilt / Zoom / Rotate / 3D choose what the windows take from it, Clip /
// Frame whether a run covers the rest of the clip or one frame, and a graph shows the track.
// Stabilizer mode measures the clip's camera movement (the Inspector's Stabilization) and sets
// its Mode, Zoom, Smooth and Strength.
#pragma once

#include "compositing/tracking/TrackerRunner.h"
#include "compositing/tracking/WindowTrack.h"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <optional>
#include <future>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QStackedWidget;
class QToolButton;

namespace kk {

struct ClipData;
class ColorSession;
class PlaybackEngine;
class Project;
class ValueField;

namespace tracking {
class StabilizeRunner;
}

/// Graph of the node's tracker data: Pan (x), Tilt (y), and Zoom and Rotate when they are
/// switched on, over the track's frames, with the playhead marked.
class TrackGraph : public QWidget
{
    Q_OBJECT
public:
    explicit TrackGraph(ColorSession* session, QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override { return {260, 90}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPointer<ColorSession> m_session;
};

class ColorTrackerPanel : public QWidget
{
    Q_OBJECT
public:
    /// @p engine renders the pictures that are tracked (may be null: tracking is then refused).
    ColorTrackerPanel(
        ColorSession* session, PlaybackEngine* engine, Project* project, QWidget* parent = nullptr);
    ~ColorTrackerPanel() override;

    /// The picture of a timeline frame at a size, for tests and engines without a compositor.
    using FrameSource = std::function<std::future<RenderedFramePtr>(int64_t timelineFrame, QSize size)>;
    void setFrameSourceOverride(FrameSource source) { m_override = std::move(source); }

    enum class Mode {
        Window,
        Stabilizer
    };
    [[nodiscard]] Mode mode() const;
    void setMode(Mode mode);

    /// Track Forward / Track Reverse (as the buttons). False when nothing was started; the
    /// reason is in status().
    bool trackFromPlayhead(bool forward);
    void stopTracking();
    /// Removes the tracked frames of the selected node.
    void clearTrack();
    [[nodiscard]] bool isTracking() const;
    [[nodiscard]] QString status() const;

    [[nodiscard]] QToolButton* modeButton(Mode m) const { return m_modeButtons[int(m)]; }
    [[nodiscard]] QCheckBox* panBox() const { return m_pan; }
    [[nodiscard]] QCheckBox* tiltBox() const { return m_tilt; }
    [[nodiscard]] QCheckBox* zoomBox() const { return m_zoom; }
    [[nodiscard]] QCheckBox* rotateBox() const { return m_rotate; }
    [[nodiscard]] QCheckBox* perspectiveBox() const { return m_perspective; }
    [[nodiscard]] QToolButton* clipModeButton() const { return m_clipMode; }
    [[nodiscard]] QToolButton* frameModeButton() const { return m_frameMode; }
    [[nodiscard]] QToolButton* forwardButton() const { return m_forward; }
    [[nodiscard]] QToolButton* reverseButton() const { return m_reverse; }
    [[nodiscard]] QToolButton* stopButton() const { return m_stop; }
    [[nodiscard]] QToolButton* clearButton() const { return m_clear; }
    [[nodiscard]] TrackGraph* graph() const { return m_graph; }
    [[nodiscard]] QComboBox* stabilizerMode() const { return m_stabMode; }
    [[nodiscard]] QCheckBox* stabilizerZoom() const { return m_stabZoom; }
    [[nodiscard]] ValueField* stabilizerSmooth() const { return m_smooth; }
    [[nodiscard]] ValueField* stabilizerStrength() const { return m_strength; }
    [[nodiscard]] QToolButton* stabilizeButton() const { return m_stabilize; }
    [[nodiscard]] QToolButton* stabilizerReset() const { return m_stabReset; }
    /// Sets every tracker option and the stabilization of the selected node / clip back.
    void resetAll();

private:
    void refresh();
    void setOption(const QString& label, const std::function<void(GradeTrack&)>& edit);
    void onSample();
    void onFinished(tracking::TrackOutcome outcome, const QString& reason);
    void setStatus(const QString& text);
    void editStabilization(
        const QString& label, const std::function<void(ClipData&)>& edit);
    void startStabilize();
    void onStabilizeFinished(quint64 clipId);

    QPointer<ColorSession> m_session;
    QPointer<PlaybackEngine> m_engine;
    Project* m_project = nullptr;
    FrameSource m_override;
    tracking::TrackerRunner* m_runner = nullptr;
    tracking::StabilizeRunner* m_stabilizer = nullptr;

    QToolButton* m_modeButtons[2] {};
    QStackedWidget* m_stack = nullptr;
    QCheckBox* m_pan = nullptr;
    QCheckBox* m_tilt = nullptr;
    QCheckBox* m_zoom = nullptr;
    QCheckBox* m_rotate = nullptr;
    QCheckBox* m_perspective = nullptr;
    QToolButton* m_clipMode = nullptr;
    QToolButton* m_frameMode = nullptr;
    QToolButton* m_forward = nullptr;
    QToolButton* m_reverse = nullptr;
    QToolButton* m_stop = nullptr;
    QToolButton* m_clear = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_status = nullptr;
    TrackGraph* m_graph = nullptr;
    QComboBox* m_stabMode = nullptr;
    QCheckBox* m_stabZoom = nullptr;
    ValueField* m_smooth = nullptr;
    ValueField* m_strength = nullptr;
    QToolButton* m_stabilize = nullptr;
    QToolButton* m_stabCancel = nullptr;
    QToolButton* m_stabReset = nullptr;
    QProgressBar* m_stabProgress = nullptr;
    QLabel* m_stabStatus = nullptr;

    // The run in progress.
    std::optional<tracking::WindowTrackPlan> m_plan;
    quint64 m_runNode = 0;
    int64_t m_runOffset = 0;
    bool m_refreshing = false;
};

} // namespace kk
