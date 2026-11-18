// SPDX-License-Identifier: GPL-3.0-or-later
// ParamEditHost — what a parameter editor row needs from its owner (the InspectorPanel):
// clip lookup, playhead-relative frames and the undoable edit protocol.
#pragma once

#include "ui/inspector/ParamAccess.h"

#include <cstdint>
#include <functional>

class QColor;
class QMenu;
class QString;

namespace kk {

/// Services the inspector offers its editor rows. Keeping rows behind this interface means
/// they never talk to Project/Timeline directly, so the undo grouping lives in one place
/// (InspectorPanel) and rows can be tested with a fake host.
class ParamEditHost
{
public:
    virtual ~ParamEditHost() = default;

    /// Current state of clip @p clipId, or nullptr if it no longer exists.
    [[nodiscard]] virtual const ClipData* clip(quint64 clipId) const = 0;
    /// Clip-local frame of the playhead for clip @p clipId, clamped into the clip.
    [[nodiscard]] virtual int64_t localFrame(quint64 clipId) const = 0;

    /// Opens an undo group labelled @p label (nesting-safe). Must be paired with endParamEdit().
    virtual void beginParamEdit(const QString& label) = 0;
    /// Closes the undo group opened by beginParamEdit().
    virtual void endParamEdit() = 0;
    /// Applies @p mutate to clip @p clipId inside an open edit and notifies views live.
    virtual void modifyClip(quint64 clipId, const std::function<void(ClipData&)>& mutate) = 0;
    /// One-shot change: beginParamEdit(@p label), modifyClip(), endParamEdit() = one undo step.
    virtual void editClip(
        const QString& label, quint64 clipId, const std::function<void(ClipData&)>& mutate)
        = 0;

    /// Moves the playhead to clip-local frame @p localFrame of clip @p clipId.
    virtual void seekLocalFrame(quint64 clipId, int64_t localFrame) = 0;
    /// Remembers the parameter the user last touched (target of the Add Keyframe shortcut).
    virtual void setActiveParam(const ParamRef& ref, const ParamDescriptor& desc) = 0;
    /// True if the clips an edit of @p ref applies to (the selected clips of the same kind,
    /// spec 24.1) hold different values of it; the row then shows "mixed" instead of a value.
    [[nodiscard]] virtual bool isMixed(const ParamRef& ref, const ParamDescriptor& desc) const
    {
        (void)ref;
        (void)desc;
        return false;
    }
    /// Turns the timeline viewer's eyedropper for colour parameter @p ref on or off
    /// (SelectionModel::colorPicker()). Default: no eyedropper.
    virtual void setColorPicking(const ParamRef& ref, bool on)
    {
        (void)ref;
        (void)on;
    }
    /// True while the viewer's eyedropper writes @p ref.
    [[nodiscard]] virtual bool isColorPicking(const ParamRef& ref) const
    {
        (void)ref;
        return false;
    }
    /// True if the host supports parameter modifiers (the Compositing page Inspector): number
    /// rows then offer the right-click menu and accept a typed "= formula".
    [[nodiscard]] virtual bool supportsModifiers() const { return false; }
    /// Adds the host's actions for parameter @p ref (Modify With, Connect To, Remove ...) to the
    /// right-click menu of its row.
    virtual void fillParamMenu(const ParamRef& ref, const ParamDescriptor& desc, QMenu& menu)
    {
        (void)ref;
        (void)desc;
        (void)menu;
    }
    /// A formula was typed into the value field of @p ref (@p text without the "=").
    virtual void setParamExpression(const ParamRef& ref, const ParamDescriptor& desc, const QString& text)
    {
        (void)ref;
        (void)desc;
        (void)text;
    }
    /// The formula of the Expression modifier of @p ref, or empty.
    [[nodiscard]] virtual QString paramExpression(const ParamRef& ref) const
    {
        (void)ref;
        return {};
    }
    /// A short description of what modifies @p ref ("Expression", "Connected to Blur1 > Size"),
    /// or empty when nothing does.
    [[nodiscard]] virtual QString paramModifierNote(const ParamRef& ref) const
    {
        (void)ref;
        return {};
    }
    /// Asks the user for a colour (modal). Returns an invalid colour if cancelled.
    [[nodiscard]] virtual QColor pickColor(const QColor& initial, const QString& title) = 0;
    /// Asks the user for a file to open (modal; File parameters). @p filter is a file dialog
    /// name filter. Returns an empty string if cancelled. Default: cancelled.
    [[nodiscard]] virtual QString pickFile(
        const QString& current, const QString& title, const QString& filter)
    {
        (void)current;
        (void)title;
        (void)filter;
        return {};
    }
};

} // namespace kk
