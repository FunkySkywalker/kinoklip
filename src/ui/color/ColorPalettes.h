// SPDX-License-Identifier: GPL-3.0-or-later
// ColorPalettes — the Qualifier, Windows, LUTs and Key palettes of the Color page. Like the
// Primaries and Curves palettes (ColorWidgets.h) they edit the selected node only through
// ColorSession.
#pragma once

#include "core/Grade.h"
#include "ui/color/QualifierPick.h"

#include <QPointer>
#include <QWidget>

#include <optional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace kk {

class ColorFrameSource;
class ColorSession;
class ValueField;

/// One HSL channel of the qualifier as a painted bar: the channel's scale (hues, a saturation
/// ramp or a grey ramp), the selected range and its soft edges. Drag the inside to move the
/// range, its ends to resize it.
class QualifierRangeBar : public QWidget
{
    Q_OBJECT
public:
    enum class Channel {
        Hue,
        Sat,
        Lum
    };
    QualifierRangeBar(Channel channel, QWidget* parent = nullptr);

    /// Hue: @p a centre, @p b width; Sat / Lum: @p a low, @p b high. @p soft: soft edge width.
    void setRange(double a, double b, double soft);
    [[nodiscard]] double a() const { return m_a; }
    [[nodiscard]] double b() const { return m_b; }
    [[nodiscard]] QRectF barRect() const;

    [[nodiscard]] QSize sizeHint() const override { return {240, 24}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {80, 18}; }

Q_SIGNALS:
    void dragStarted();
    void rangeChanged(double a, double b);
    void dragFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    /// Range ends in bar fractions (hue: may lie outside 0..1, the bar wraps).
    [[nodiscard]] std::pair<double, double> ends() const;

    Channel m_channel;
    double m_a = 0, m_b = 1, m_soft = 0;
    enum class Drag {
        None,
        Move,
        Low,
        High
    } m_drag = Drag::None;
    double m_pressX = 0;
    double m_pressA = 0, m_pressB = 0;
};

/// The Qualifier palette: the eyedroppers (pick, add, remove: click or drag over the viewer),
/// the mode (HSL, RGB, Luma, 3D) with its ranges, bars and fields, invert, the highlight
/// toggle with its mode, and the Matte Finesse values that clean the key (the palette bar's
/// Reset clears the qualifier).
class QualifierPanel : public QWidget
{
    Q_OBJECT
public:
    explicit QualifierPanel(ColorSession* session, QWidget* parent = nullptr);

    /// The active eyedropper (none: the viewer's normal tools).
    [[nodiscard]] std::optional<qualifierpick::Mode> pickMode() const;
    void setPickMode(std::optional<qualifierpick::Mode> mode);
    [[nodiscard]] QToolButton* highlightButton() const { return m_highlight; }
    /// Highlight in black and white (else grey background).
    [[nodiscard]] bool highlightBlackWhite() const;
    [[nodiscard]] ValueField* field(const QString& name) const;
    [[nodiscard]] QualifierRangeBar* bar(QualifierRangeBar::Channel c) const;

Q_SIGNALS:
    void pickModeChanged();
    void highlightModeChanged();

private:
    void refresh();
    /// Applies @p edit to a copy of the selected node's qualifier and commits it.
    template<typename Fn>
    void edit(const QString& label, Fn&& fn);

    QPointer<ColorSession> m_session;
    QButtonGroup* m_picks = nullptr;
    QToolButton* m_highlight = nullptr;
    QComboBox* m_highlightMode = nullptr;
    QCheckBox* m_enable = nullptr;
    QCheckBox* m_invert = nullptr;
    QCheckBox* m_use[3] {};
    QCheckBox* m_useRgb[3] {};
    QComboBox* m_mode = nullptr;
    QStackedWidget* m_ranges = nullptr;
    QList<QWidget*> m_rowWidgets[3]; ///< the widgets of each HSL row (Luma hides Hue and Sat)
    QLabel* m_colourCount = nullptr;
    QToolButton* m_clearColours = nullptr;
    QualifierRangeBar* m_bars[3] {};
    QVector<std::pair<QString, ValueField*>> m_fields;
    bool m_refreshing = false;
};

/// The Windows palette: create Circle / Linear / Polygon / Gradient windows on the selected
/// node, the node's windows (enable check box, select), invert, delete, and the selected
/// window's transform (size, aspect, pan, tilt, rotate) and softness / opacity fields. The
/// shapes are edited in the viewer (ColorViewerOverlay).
class WindowsPanel : public QWidget
{
    Q_OBJECT
public:
    explicit WindowsPanel(ColorSession* session, QWidget* parent = nullptr);

    [[nodiscard]] QToolButton* shapeButton(GradeWindowShape shape) const;
    [[nodiscard]] QListWidget* list() const { return m_list; }
    [[nodiscard]] ValueField* field(const QString& key) const;

private:
    void refresh();

    QPointer<ColorSession> m_session;
    QToolButton* m_shapes[5] {};
    QListWidget* m_list = nullptr;
    QToolButton* m_invert = nullptr;
    QToolButton* m_delete = nullptr;
    QToolButton* m_keyShape = nullptr;
    QToolButton* m_removeShapeKey = nullptr;
    QToolButton* m_clearShapeKeys = nullptr;
    QWidget* m_fieldsBox = nullptr;
    QVector<std::pair<QString, ValueField*>> m_fields;
    bool m_refreshing = false;
};

/// The LUTs palette: a browser of .cube files in the LUT folders (settings key
/// "color/lutFolders"; the user's LUT folder by default), applied to the selected node with a
/// double-click or Apply; Remove takes the node's LUT away; the node's LUT and interpolation
/// are shown.
class LutBrowser : public QWidget
{
    Q_OBJECT
public:
    explicit LutBrowser(ColorSession* session, QWidget* parent = nullptr);

    /// The folders shown (settings, plus the default folder).
    [[nodiscard]] QStringList folders() const;
    /// Adds @p folder to the list (remembered).
    void addFolder(const QString& folder);
    /// Rescans the folders.
    void rescan();
    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }
    /// Shows only the LUTs whose name holds every word of the search field.
    void applyFilter();
    /// The default LUT folder (created when missing).
    [[nodiscard]] static QString defaultFolder();

private:
    void refresh();
    void applyItem(QTreeWidgetItem* item);

    QPointer<ColorSession> m_session;
    QTreeWidget* m_tree = nullptr;
    QLineEdit* m_search = nullptr;
    QLabel* m_current = nullptr;
    QComboBox* m_interpolation = nullptr;
    QToolButton* m_remove = nullptr;
    bool m_refreshing = false;
};

/// The Key palette: the node's key output gain (Mix), the qualifier's invert and a small view
/// of the node's key (the Black / White highlight of ColorFrameSource).
class KeyPanel : public QWidget
{
    Q_OBJECT
public:
    KeyPanel(ColorSession* session, ColorFrameSource* frames, QWidget* parent = nullptr);

    [[nodiscard]] ValueField* gainField() const { return m_gain; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void refresh();
    void updateMatte();

    QPointer<ColorSession> m_session;
    QPointer<ColorFrameSource> m_frames;
    ValueField* m_gain = nullptr;
    QCheckBox* m_invert = nullptr;
    QWidget* m_matteArea = nullptr;
    QImage m_matte;
    bool m_refreshing = false;
};

/// The Blur palette (Resolve's Blur / Sharpen / Mist): three modes with their Radius (Blur and
/// Sharpen: R, G, B with a link button, 0.5 = no change), H/V Ratio and Scaling fields, the
/// Sharpen Coring Softness and Level and the Mist Mix. They edit the selected node's blur
/// parameters (gradekeys), which blur the node's picture inside its windows and qualifier; the
/// palette bar's Reset sets all of them back.
class BlurPanel : public QWidget
{
    Q_OBJECT
public:
    explicit BlurPanel(ColorSession* session, QWidget* parent = nullptr);

    /// The field of parameter @p key (gradekeys::BlurRadiusR, ...), or nullptr.
    [[nodiscard]] ValueField* field(const QString& key) const;
    /// The mode buttons: 0 Blur, 1 Sharpen, 2 Mist.
    [[nodiscard]] QToolButton* modeButton(int mode) const { return m_modeButtons[mode]; }
    [[nodiscard]] int mode() const;
    void setMode(int mode);
    /// The R / G / B link button of the Blur radius (checked = a radius edit sets all three).
    [[nodiscard]] QToolButton* linkButton(int mode) const { return m_links[mode]; }
    /// Sets every blur parameter of the selected node back to its default.
    void resetAll();

private:
    void bindField(ValueField* field, const QString& key);
    void setValueOf(const QString& key, double value);
    void refresh();

    QPointer<ColorSession> m_session;
    QToolButton* m_modeButtons[3] {};
    QToolButton* m_links[2] {};
    QStackedWidget* m_stack = nullptr;
    QVector<std::pair<QString, ValueField*>> m_fields;
    bool m_refreshing = false;
};

/// The RGB Mixer palette (Resolve's RGB Mixer): three output tabs Red / Green / Blue, each with
/// three sliders (how much of the input Red, Green, Blue goes into that output channel), a
/// Monochrome check box (the tabs give way to one Gray row of weights) and a Preserve Luminance
/// check box. They edit the selected node's mixer parameters (gradekeys); the palette bar's
/// Reset sets all of them back.
class RgbMixerPanel : public QWidget
{
    Q_OBJECT
public:
    explicit RgbMixerPanel(ColorSession* session, QWidget* parent = nullptr);

    /// The field of parameter @p key (gradekeys::MixerRR, ...), or nullptr.
    [[nodiscard]] ValueField* field(const QString& key) const;
    /// The output tab buttons: 0 Red, 1 Green, 2 Blue.
    [[nodiscard]] QToolButton* tabButton(int output) const { return m_tabs[output]; }
    [[nodiscard]] QCheckBox* monochrome() const { return m_mono; }
    [[nodiscard]] QCheckBox* preserveLuminance() const { return m_preserve; }
    /// The page shown: 0..2 an output tab, 3 the Gray row (Monochrome on).
    [[nodiscard]] int page() const;
    /// Sets every mixer parameter of the selected node back to its default.
    void resetAll();

private:
    void bindField(ValueField* field, const QString& key);
    void setValueOf(const QString& key, double value);
    void refresh();

    QPointer<ColorSession> m_session;
    QToolButton* m_tabs[3] {};
    QCheckBox* m_mono = nullptr;
    QCheckBox* m_preserve = nullptr;
    QStackedWidget* m_stack = nullptr;
    int m_tab = 0;
    QVector<std::pair<QString, ValueField*>> m_fields;
    bool m_refreshing = false;
};

} // namespace kk
