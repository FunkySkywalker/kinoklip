// SPDX-License-Identifier: GPL-3.0-or-later
// MixerPanel — the mixer: a strip per audio track, then a strip per bus, in a scrollable row.
#pragma once

#include "core/MixerOps.h"
#include "ui/common/PanelFrame.h"

#include <QVector>

#include <optional>

class QHBoxLayout;
class QScrollArea;

namespace kk {

struct AppContext;
class MeterModel;
class MixerController;
class MixerStrip;

/// The Mixer panel of the Audio page (large) and of the Edit page (the same widget, narrower
/// by default). Strips are rebuilt when tracks or buses are added, removed or reordered, and
/// refreshed on every timeline change and playhead move (automated faders follow). The title
/// bar offers Add Bus (the registry action "audio.add_bus").
///
/// Thread-safety: GUI thread only.
class MixerPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// @p controller and @p meters are not owned and must outlive the panel.
    MixerPanel(AppContext& ctx, MixerController* controller, MeterModel* meters,
        QWidget* parent = nullptr);

    /// The strips, tracks first (A1 ... An), then the buses (Bus 1 ...).
    [[nodiscard]] const QVector<MixerStrip*>& strips() const { return m_strips; }
    /// The strip of @p target, or nullptr.
    [[nodiscard]] MixerStrip* strip(const MixTarget& target) const;

    /// The strip the Inspector shows the effects of (a click on a strip or its effect controls),
    /// or none.
    [[nodiscard]] std::optional<MixTarget> selectedTarget() const { return m_selected; }
    /// Selects @p target (draws its frame; emits stripSelected() when it changed).
    void selectTarget(const MixTarget& target);

signals:
    /// A strip was clicked: the Inspector should show that track's effects.
    void stripSelected(const MixTarget& target);
    /// An effect slot of a track strip was double-clicked: open its controls.
    void effectOpenRequested(const MixTarget& target, quint64 effectId);

private:
    void sync();
    void refreshValues();

    AppContext& m_ctx;
    MixerController* m_controller;
    MeterModel* m_meters;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_row = nullptr;
    QHBoxLayout* m_rowLayout = nullptr;
    QVector<MixerStrip*> m_strips;
    QVector<MixTarget> m_targets; ///< what m_strips show, in order
    std::optional<MixTarget> m_selected;
};

} // namespace kk
