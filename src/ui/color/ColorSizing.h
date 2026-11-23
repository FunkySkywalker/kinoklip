// SPDX-License-Identifier: GPL-3.0-or-later
// ColorSizing — the Color page's Sizing palette: Zoom, Width, Height, Pan, Tilt, Rotate and
// Flip H / V, in two tabs. Input is the open clip's own transform (the Edit page Inspector's
// Position, Zoom, Rotation and Flip: one source of truth), Output is the timeline's Output
// Sizing (the whole picture after the timeline grade). Every edit goes through ColorSession as
// one undo step; with Auto Keyframe on (Keyframes palette) or on a keyed parameter an edit sets
// a key at the playhead.
#pragma once

#include "ui/color/ColorSession.h"

#include <QPointer>
#include <QWidget>

class QToolButton;

namespace kk {

class ValueField;

class SizingPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SizingPanel(ColorSession* session, QWidget* parent = nullptr);

    /// The field named @p name: "zoom", "width", "height", "pan", "tilt" or "rotate".
    [[nodiscard]] ValueField* field(const QString& name) const;
    /// The tab buttons: 0 Input, 1 Output.
    [[nodiscard]] QToolButton* scopeButton(int scope) const { return m_scopeButtons[scope]; }
    /// Flip Horizontal (@p vertical false) or Vertical.
    [[nodiscard]] QToolButton* flipButton(bool vertical) const { return m_flips[vertical ? 1 : 0]; }
    [[nodiscard]] ColorSession::SizingScope scope() const { return m_scope; }
    void setScope(ColorSession::SizingScope scope);
    /// Sets the shown scope back (the palette bar's Reset).
    void resetAll();

private:
    void bind(ValueField* field, const QString& name);
    void apply(const QString& name, double value);
    void refresh();

    QPointer<ColorSession> m_session;
    ColorSession::SizingScope m_scope = ColorSession::SizingScope::Input;
    QToolButton* m_scopeButtons[2] {};
    QToolButton* m_flips[2] {};
    QVector<std::pair<QString, ValueField*>> m_fields;
    bool m_refreshing = false;
};

} // namespace kk
