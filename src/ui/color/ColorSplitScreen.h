// SPDX-License-Identifier: GPL-3.0-or-later
// ColorSplitScreen — the Color page viewer's Split Screen (View > Split Screen): the graded
// pictures of several clips side by side over the viewer picture, to match shots.
#pragma once

#include <QPointer>
#include <QRectF>
#include <QVector>
#include <QWidget>

namespace kk {

class ColorFrameSource;
class ColorSession;
class Project;
class ViewerSurface;

/// Transparent widget over the viewer surface (a child of it, like ColorViewerOverlay), drawn
/// in widget pixels over the viewer's picture rectangle.
///
/// **Modes.**
/// - SelectedClips: the clips selected in the clips strip, each whole in a tile of a grid (as
///   square as possible, strip order). With fewer than two selected the clip under the
///   playhead is added, so there is always something to compare.
/// - Timeline: the clip under the playhead and its neighbours on the timeline (clips strip
///   order, up to kNeighbours before and after) in vertical strips, each strip showing its
///   part of its clip's picture, so a grade difference shows as a hard edge in the picture.
/// Every tile carries the clip's number and name; the clip being graded is framed in the
/// highlight colour. A click on a tile grades that clip.
///
/// The pictures are ColorFrameSource detail pictures (graded like the viewer, the clip's middle
/// frame; the clip under the playhead at the playhead).
///
/// Thread-safety: GUI thread only.
class ColorSplitScreen : public QWidget
{
    Q_OBJECT
public:
    enum class Mode { Off, SelectedClips, Timeline };
    /// Neighbouring clips shown on each side in Timeline mode.
    static constexpr int kNeighbours = 2;

    /// One tile as drawn (widget pixels).
    struct Tile
    {
        quint64 clip = 0;
        QRectF rect; ///< where the tile is drawn
        QRectF source; ///< the part of the clip's picture it shows, in fractions (0..1)
    };

    ColorSplitScreen(
        ColorSession* session, Project* project, ColorFrameSource* frames, ViewerSurface* surface);

    void setMode(Mode mode);
    [[nodiscard]] Mode mode() const { return m_mode; }
    /// The clips the current mode shows, in order (empty when off).
    [[nodiscard]] QVector<quint64> clips() const;
    /// The layout of clips() over the viewer picture (empty when off).
    [[nodiscard]] QVector<Tile> tiles() const;
    /// Asks the frame source for the pictures of clips() and repaints (called when the clips,
    /// the grade or the page changes).
    void updateWanted();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] QString labelOf(quint64 clipId, int number) const;

    QPointer<ColorSession> m_session;
    Project* m_project = nullptr;
    QPointer<ColorFrameSource> m_frames;
    ViewerSurface* m_surface = nullptr;
    Mode m_mode = Mode::Off;
};

} // namespace kk
