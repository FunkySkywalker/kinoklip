// SPDX-License-Identifier: GPL-3.0-or-later
// ColorKeyframes — the Color page's Keyframes palette: a strip with one row per corrector node of
// the open grade, Input Sizing and Output Sizing, plus a Master row holding every key. Keys are
// Static (hold their value until the next key, drawn as squares) or Dynamic (interpolate, drawn
// as diamonds) with a dissolve type (Linear, Ease In, Ease Out, Ease In and Out, Smooth). Click a
// lane to move the playhead, a key to select it, drag a key to move it, double-click a lane to
// add a key, Delete removes the selected key. The Auto toggle makes every edit on the page key
// the parameter at the playhead. All edits go through ColorSession (one undo step each).
#pragma once

#include "ui/color/ColorSession.h"

#include <QPointer>
#include <QWidget>

#include <optional>

class QComboBox;
class QScrollArea;
class QToolButton;

namespace kk {

class ColorKeyStrip;

class ColorKeyframesPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ColorKeyframesPanel(ColorSession* session, QWidget* parent = nullptr);

    [[nodiscard]] ColorKeyStrip* strip() const { return m_strip; }
    [[nodiscard]] QToolButton* autoButton() const { return m_auto; }
    [[nodiscard]] QToolButton* addButton() const { return m_add; }
    [[nodiscard]] QToolButton* deleteButton() const { return m_delete; }
    [[nodiscard]] QToolButton* previousButton() const { return m_prev; }
    [[nodiscard]] QToolButton* nextButton() const { return m_next; }
    [[nodiscard]] QToolButton* staticButton() const { return m_static; }
    [[nodiscard]] QToolButton* dynamicButton() const { return m_dynamic; }
    [[nodiscard]] QComboBox* dissolveCombo() const { return m_dissolve; }
    /// The palette bar's Reset: removes every key of the open grade's selected node row and of
    /// the sizing rows' keys (values stay as they are at the playhead).
    void resetAll();

private:
    void updateControls();
    void addKeys();
    void deleteKeys();
    void step(bool forward);
    void setMode(keyrows::Mode mode);
    void setDissolve(int index);

    QPointer<ColorSession> m_session;
    ColorKeyStrip* m_strip = nullptr;
    QToolButton* m_auto = nullptr;
    QToolButton* m_add = nullptr;
    QToolButton* m_delete = nullptr;
    QToolButton* m_prev = nullptr;
    QToolButton* m_next = nullptr;
    QToolButton* m_static = nullptr;
    QToolButton* m_dynamic = nullptr;
    QComboBox* m_dissolve = nullptr;
    bool m_updating = false;
};

/// The painted rows of the Keyframes palette. Row 0 is Master, then ColorSession::keyRows().
class ColorKeyStrip : public QWidget
{
    Q_OBJECT
public:
    /// A selected key: @c row 0 = Master (all rows with a key there), else keyRows()[row - 1].
    struct Selection
    {
        int row = -1; ///< -1 = none
        int64_t frame = 0;
        friend bool operator==(const Selection&, const Selection&) = default;
    };

    ColorKeyStrip(ColorSession* session, QWidget* parent = nullptr);

    [[nodiscard]] int rowCount() const { return int(m_rows.size()) + 1; }
    [[nodiscard]] QString rowLabel(int row) const;
    /// Strip frames of the keys of @p row (Master: the union).
    [[nodiscard]] QVector<int64_t> frames(int row) const;
    [[nodiscard]] Selection selection() const { return m_selection; }
    void select(int row, int64_t frame);
    void clearSelection();
    /// The session rows a selection acts on (Master: every row with a key at the frame).
    [[nodiscard]] QVector<ColorSession::KeyRow> targets(const Selection& sel) const;
    /// Widget geometry for tests: the lane rectangle of @p row, the x of strip frame @p frame
    /// and the frame under x.
    [[nodiscard]] QRect laneRect(int row) const;
    [[nodiscard]] double frameToX(int64_t frame) const;
    [[nodiscard]] int64_t xToFrame(double x) const;
    [[nodiscard]] int rowAt(int y) const;

    /// Edits on the selected key (Master: every row with a key there; several rows are one undo
    /// step). Each returns false when nothing changed.
    bool deleteSelected();
    bool setSelectedMode(keyrows::Mode mode);
    bool setSelectedInterp(Interp interp);
    /// Adds a key at strip frame @p frame to @p row (0: every row) and selects it.
    bool addKey(int row, int64_t frame);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

Q_SIGNALS:
    void selectionChanged();
    void deleteRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    void rebuild();
    [[nodiscard]] std::optional<int64_t> keyAt(int row, const QPoint& pos) const;
    [[nodiscard]] QRect lanes() const;

    QPointer<ColorSession> m_session;
    QVector<ColorSession::KeyRow> m_rows;
    Selection m_selection;
    bool m_dragging = false;
    bool m_dragStarted = false;
    int64_t m_dragFrame = 0; ///< where the dragged key is now
};

} // namespace kk
