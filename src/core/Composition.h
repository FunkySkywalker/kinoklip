// SPDX-License-Identifier: GPL-3.0-or-later
// Composition — the node graph of the Compositing page (docs/compositing-page.md).
// Value types only; the node catalogue is in core/NodeRegistry.h, evaluation in src/render.
#pragma once

#include "core/Animation.h"
#include "core/Mask.h"

#include <QColor>
#include <QMap>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>

namespace kk {

/// Where an input of a node takes its image from: output @p outputIndex of node @p nodeId.
struct NodeLink
{
    quint64 nodeId = 0; ///< 0 = not connected
    int outputIndex = 0;

    [[nodiscard]] bool isConnected() const { return nodeId != 0; }
    friend bool operator==(const NodeLink&, const NodeLink&) = default;
};

/// One node of a composition. `type` is a NodeRegistry descriptor id ("media_in", "merge",
/// "transform", or "fx:<EffectRegistry id>" for every Edit page video effect used as a node).
/// A control a Macro shows (spec 120): a number on the macro node (ParamSet key @p key) to which
/// parameter @p paramKey of the member node @p node is connected.
struct MacroControl
{
    QString key; ///< key of the number in the macro node's ParamSet ("c1", "c2", ...)
    QString label; ///< what the Inspector shows
    quint64 node = 0; ///< the member node whose parameter it drives
    QString paramKey; ///< that parameter

    friend bool operator==(const MacroControl&, const MacroControl&) = default;
};

struct CompNode
{
    quint64 id = 0; ///< unique within the composition (Composition::allocateId())
    QString type;
    QString name; ///< shown on the tile, unique within the composition ("Merge1", "Blur2")
    QPointF position; ///< node editor position in grid units (1 unit = one tile height)
    bool enabled = true; ///< false = pass-through: the main input goes straight to the output
    bool locked = false; ///< parameters cannot be edited
    QColor color; ///< tile colour set by the user (Set Color); invalid = the default look
    bool thumbnail = false; ///< show the node's picture on its tile (node editor)
    ParamSet params; ///< keys from the descriptor's parameters; keyframes in comp frames
    /// Inputs by port id from the descriptor ("background", "foreground", "mask", "input").
    /// Ports without an entry are unconnected.
    QMap<QString, NodeLink> inputs;
    /// MediaIn only: MediaItem::id of the picture it reads; empty = the clip that owns the
    /// composition (its source at the clip's time, with the clip's speed and retime).
    QString mediaId;
    /// MediaIn only, in a transition's composition (spec 128): 0 = an ordinary MediaIn, 1 = MediaIn1
    /// (the outgoing clip), 2 = MediaIn2 (the incoming clip). Such a node ignores mediaId.
    int transitionSide = 0;
    /// Polygon mask node only: the closed point path (core/Mask.h AnimatedPath, shape
    /// keyframes in comp frames). Points are mask-local frame pixels (timeline pixels, origin
    /// at the node's centre, y down), like MaskData::path.
    AnimatedPath path;
    /// Group / Macro (spec 120): the Group node this node is inside (0 = top level). The graph
    /// stays flat; a group only decides what the node editor draws and what moves together.
    quint64 group = 0;
    /// Group node only: draw it as one tile and hide its members.
    bool collapsed = false;
    /// Group node only: it is a Macro (it has controls).
    bool macro = false;
    /// Macro node only: its published controls (numbers in params under MacroControl::key).
    QVector<MacroControl> controls;
    /// Instance (spec 120): the node whose parameters this node uses (0 = an ordinary node).
    quint64 instanceOf = 0;

    friend bool operator==(const CompNode&, const CompNode&) = default;
};

/// A note or group frame in the node editor (no effect on the picture).
struct CompNote
{
    quint64 id = 0;
    QPointF position;
    QSizeF size;
    QString text;
    QColor color;
    /// An Underlay (spec 120): drawn behind the nodes it covers, which move with it.
    bool underlay = false;

    friend bool operator==(const CompNote&, const CompNote&) = default;
};

/// The node graph of one clip. A clip with a composition shows the composition's output
/// (the image arriving at its MediaOut node) instead of its source picture; the Edit page
/// effects, masks, transform, opacity and compositing of the clip then apply on top of it.
/// Frames are clip-local ("comp frames": 0 = the clip's first frame), like every keyframe.
struct Composition
{
    QVector<CompNode> nodes;
    QVector<CompNote> notes;
    quint64 mediaOutId = 0; ///< the node whose image is the result (type "media_out")
    quint64 nextId = 1; ///< next id handed out by allocateId()
    /// Node editor view state (saved so reopening shows the same place).
    QPointF viewCentre;
    double viewZoom = 1.0;
    quint64 leftViewerNode = 0; ///< node shown in the left viewer (0 = none)
    quint64 rightViewerNode = 0; ///< node shown in the right viewer (0 = MediaOut)
    /// MediaIn > Audio (Resolve: MediaIn Inspector, Audio tab): false = the Compositing page
    /// plays the timeline's mix (default); true = only the clip's own sound (the audio clips
    /// linked to it). Applies only while the Compositing page plays.
    bool soloAudio = false;
    /// MediaIn > Audio > Sound Offset: timeline frames (fractions allowed, positive = the sound
    /// comes later) the sound is moved against the picture on the Compositing page only.
    double soundOffset = 0.0;

    /// A new id, unique within this composition.
    quint64 allocateId() { return nextId++; }
    /// Node with id @p id, or nullptr.
    [[nodiscard]] const CompNode* node(quint64 id) const
    {
        for (const CompNode& n : nodes) {
            if (n.id == id) {
                return &n;
            }
        }
        return nullptr;
    }
    /// Mutable node with id @p id, or nullptr.
    [[nodiscard]] CompNode* node(quint64 id)
    {
        for (CompNode& n : nodes) {
            if (n.id == id) {
                return &n;
            }
        }
        return nullptr;
    }

    friend bool operator==(const Composition&, const Composition&) = default;
};

} // namespace kk
