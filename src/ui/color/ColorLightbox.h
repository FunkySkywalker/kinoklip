// SPDX-License-Identifier: GPL-3.0-or-later
// ColorLightbox — View > Lightbox on the Color page: a full-viewer grid of the graded
// thumbnail of every clip on the timeline, to judge the whole grade at a glance.
#pragma once

#include <QAbstractScrollArea>
#include <QPointer>
#include <QRect>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;

namespace kk {

class ColorFrameSource;
class ColorSession;
class Project;

/// The grid of ColorLightbox (a scroll area): one cell per clip of the clips strip, in strip
/// order. A click grades that clip (the playhead moves to it), Ctrl-click adds or removes it
/// from the selected clips, a double-click also asks to leave the Lightbox (closeRequested()).
/// Options: thumbnail size (Small / Medium / Large), clip labels (number, track, timecode,
/// name) and grade info (node count, LUT, bypass, "ungraded"). The pictures are
/// ColorFrameSource detail pictures of the visible clips (graded, the clip's middle frame).
class ColorLightboxGrid : public QAbstractScrollArea
{
    Q_OBJECT
public:
    enum class Size { Small, Medium, Large };

    ColorLightboxGrid(ColorSession* session, Project* project, ColorFrameSource* frames,
        QWidget* parent = nullptr);

    void setThumbnailSize(Size size);
    [[nodiscard]] Size thumbnailSize() const { return m_size; }
    /// Thumbnail width of @p size in pixels.
    [[nodiscard]] static int widthOf(Size size);
    void setShowLabels(bool on);
    [[nodiscard]] bool showLabels() const { return m_labels; }
    void setShowGradeInfo(bool on);
    [[nodiscard]] bool showGradeInfo() const { return m_info; }

    /// The clips in the grid (clips strip order).
    [[nodiscard]] const QVector<quint64>& clips() const { return m_clips; }
    /// Cell rectangle of clip @p clipId in viewport pixels (scrolled; null when not in the grid).
    [[nodiscard]] QRect cellRect(quint64 clipId) const;
    /// The clip at viewport position @p pos (0: none).
    [[nodiscard]] quint64 clipAt(QPoint pos) const;
    /// Number of columns at the current width and size.
    [[nodiscard]] int columns() const;
    /// Asks the frame source for the pictures of the clips in view (also called on scrolling).
    void requestPictures();

Q_SIGNALS:
    void closeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void rebuild();
    void relayout();
    [[nodiscard]] QSize cellSize() const;
    [[nodiscard]] int labelHeight() const;
    [[nodiscard]] QString infoOf(quint64 clipId) const;

    QPointer<ColorSession> m_session;
    Project* m_project = nullptr;
    QPointer<ColorFrameSource> m_frames;
    QVector<quint64> m_clips;
    Size m_size = Size::Medium;
    bool m_labels = true;
    bool m_info = true;
};

/// The Lightbox page of the viewer: an options bar (Size, Labels, Grade Info) over the grid.
/// The options are remembered between runs.
class ColorLightbox : public QWidget
{
    Q_OBJECT
public:
    ColorLightbox(ColorSession* session, Project* project, ColorFrameSource* frames,
        QWidget* parent = nullptr);

    [[nodiscard]] ColorLightboxGrid* grid() const { return m_grid; }
    [[nodiscard]] QComboBox* sizeBox() const { return m_sizeBox; }
    [[nodiscard]] QCheckBox* labelsBox() const { return m_labelsBox; }
    [[nodiscard]] QCheckBox* infoBox() const { return m_infoBox; }

Q_SIGNALS:
    void closeRequested();

private:
    ColorLightboxGrid* m_grid = nullptr;
    QComboBox* m_sizeBox = nullptr;
    QCheckBox* m_labelsBox = nullptr;
    QCheckBox* m_infoBox = nullptr;
};

} // namespace kk
