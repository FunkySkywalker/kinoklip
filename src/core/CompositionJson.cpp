// SPDX-License-Identifier: GPL-3.0-or-later
// CompositionJson — project-file form of the Compositing page node graph (core/Composition.h,
// format 15; see serialization::compositionToJson()).
#include "core/Serialization.h"

#include <QJsonArray>
#include <QSet>

#include <cmath>

namespace kk::serialization {

namespace {

QJsonArray pointToJson(QPointF p)
{
    return QJsonArray {p.x(), p.y()};
}

QPointF pointFromJson(const QJsonValue& v)
{
    const QJsonArray a = v.toArray();
    return QPointF(a.at(0).toDouble(), a.at(1).toDouble());
}

QJsonObject nodeToJson(const CompNode& n, bool pictureOnly)
{
    QJsonObject o {{QLatin1String("id"), qint64(n.id)}, {QLatin1String("type"), n.type}};
    if (!pictureOnly) {
        o[QLatin1String("name")] = n.name;
        o[QLatin1String("pos")] = pointToJson(n.position);
        if (n.locked)
            o[QLatin1String("locked")] = true;
        if (n.color.isValid())
            o[QLatin1String("color")] = n.color.name(QColor::HexArgb);
        if (n.thumbnail)
            o[QLatin1String("thumb")] = true;
        if (n.group != 0)
            o[QLatin1String("group")] = qint64(n.group);
        if (n.collapsed)
            o[QLatin1String("collapsed")] = true;
        if (n.macro)
            o[QLatin1String("macro")] = true;
        if (!n.controls.isEmpty()) {
            QJsonArray controls;
            for (const MacroControl& c : n.controls)
                controls.append(QJsonObject {{QLatin1String("key"), c.key},
                    {QLatin1String("label"), c.label}, {QLatin1String("node"), qint64(c.node)},
                    {QLatin1String("param"), c.paramKey}});
            o[QLatin1String("controls")] = controls;
        }
    }
    if (n.instanceOf != 0)
        o[QLatin1String("instanceOf")] = qint64(n.instanceOf);
    if (!n.enabled)
        o[QLatin1String("enabled")] = false;
    if (n.params != ParamSet {})
        o[QLatin1String("params")] = paramSetToJson(n.params);
    QJsonObject inputs;
    for (auto it = n.inputs.cbegin(); it != n.inputs.cend(); ++it) {
        if (it->isConnected())
            inputs[it.key()] = QJsonArray {qint64(it->nodeId), it->outputIndex};
    }
    if (!inputs.isEmpty())
        o[QLatin1String("inputs")] = inputs;
    if (!n.mediaId.isEmpty())
        o[QLatin1String("mediaId")] = n.mediaId;
    if (n.transitionSide != 0)
        o[QLatin1String("side")] = n.transitionSide;
    if (n.path != AnimatedPath {})
        animatedPathToJson(n.path, o);
    return o;
}

CompNode nodeFromJson(const QJsonObject& o)
{
    CompNode n;
    n.id = quint64(o.value(QLatin1String("id")).toInteger());
    n.type = o.value(QLatin1String("type")).toString();
    n.name = o.value(QLatin1String("name")).toString();
    n.position = pointFromJson(o.value(QLatin1String("pos")));
    n.enabled = o.value(QLatin1String("enabled")).toBool(true);
    n.locked = o.value(QLatin1String("locked")).toBool(false);
    if (o.contains(QLatin1String("color"))) {
        const QColor c(o.value(QLatin1String("color")).toString());
        if (c.isValid())
            n.color = c;
    }
    n.thumbnail = o.value(QLatin1String("thumb")).toBool(false);
    n.group = quint64(o.value(QLatin1String("group")).toInteger());
    n.collapsed = o.value(QLatin1String("collapsed")).toBool(false);
    n.macro = o.value(QLatin1String("macro")).toBool(false);
    for (const QJsonValue& cv : o.value(QLatin1String("controls")).toArray()) {
        const QJsonObject co = cv.toObject();
        MacroControl c;
        c.key = co.value(QLatin1String("key")).toString();
        c.label = co.value(QLatin1String("label")).toString();
        c.node = quint64(co.value(QLatin1String("node")).toInteger());
        c.paramKey = co.value(QLatin1String("param")).toString();
        if (!c.key.isEmpty())
            n.controls.append(c);
    }
    n.instanceOf = quint64(o.value(QLatin1String("instanceOf")).toInteger());
    n.params = paramSetFromJson(o.value(QLatin1String("params")).toObject());
    const QJsonObject inputs = o.value(QLatin1String("inputs")).toObject();
    for (auto it = inputs.constBegin(); it != inputs.constEnd(); ++it) {
        const QJsonArray a = it->toArray();
        NodeLink link;
        link.nodeId = quint64(a.at(0).toInteger());
        link.outputIndex = std::max(0, a.at(1).toInt());
        if (link.isConnected())
            n.inputs.insert(it.key(), link);
    }
    n.mediaId = o.value(QLatin1String("mediaId")).toString();
    n.transitionSide = std::clamp(o.value(QLatin1String("side")).toInt(), 0, 2);
    if (o.contains(QLatin1String("points")) || o.contains(QLatin1String("shapeKeys")))
        animatedPathFromJson(o, n.path);
    return n;
}

} // namespace

QJsonArray compNodesToJson(const QVector<CompNode>& nodes)
{
    QJsonArray a;
    for (const CompNode& n : nodes)
        a.append(nodeToJson(n, false));
    return a;
}

QVector<CompNode> compNodesFromJson(const QJsonArray& json)
{
    QVector<CompNode> nodes;
    nodes.reserve(json.size());
    for (const QJsonValue& v : json)
        nodes.append(nodeFromJson(v.toObject()));
    return nodes;
}

QJsonObject compositionToJson(const Composition& comp, bool pictureOnly)
{
    QJsonObject o;
    QJsonArray nodes;
    for (const CompNode& n : comp.nodes)
        nodes.append(nodeToJson(n, pictureOnly));
    o[QLatin1String("nodes")] = nodes;
    o[QLatin1String("mediaOut")] = qint64(comp.mediaOutId);
    if (pictureOnly)
        return o;
    o[QLatin1String("nextId")] = qint64(comp.nextId);
    if (!comp.notes.isEmpty()) {
        QJsonArray notes;
        for (const CompNote& n : comp.notes) {
            QJsonObject no {{QLatin1String("id"), qint64(n.id)},
                {QLatin1String("pos"), pointToJson(n.position)},
                {QLatin1String("size"), QJsonArray {n.size.width(), n.size.height()}},
                {QLatin1String("text"), n.text}};
            if (n.color.isValid())
                no[QLatin1String("color")] = variantToJson(n.color);
            if (n.underlay)
                no[QLatin1String("underlay")] = true;
            notes.append(no);
        }
        o[QLatin1String("notes")] = notes;
    }
    o[QLatin1String("view")] = QJsonObject {{QLatin1String("centre"), pointToJson(comp.viewCentre)},
        {QLatin1String("zoom"), comp.viewZoom},
        {QLatin1String("left"), qint64(comp.leftViewerNode)},
        {QLatin1String("right"), qint64(comp.rightViewerNode)}};
    if (comp.soloAudio || comp.soundOffset != 0.0) {
        QJsonObject audio;
        if (comp.soloAudio)
            audio[QLatin1String("solo")] = true;
        if (comp.soundOffset != 0.0)
            audio[QLatin1String("offset")] = comp.soundOffset;
        o[QLatin1String("audio")] = audio;
    }
    return o;
}

Composition compositionFromJson(const QJsonObject& o)
{
    Composition comp;
    quint64 maxId = 0;
    QSet<quint64> ids;
    for (const QJsonValue& v : o.value(QLatin1String("nodes")).toArray()) {
        CompNode n = nodeFromJson(v.toObject());
        if (n.id == 0 || ids.contains(n.id) || n.type.isEmpty())
            continue; // damaged entry
        ids.insert(n.id);
        maxId = std::max(maxId, n.id);
        comp.nodes.append(std::move(n));
    }
    // Group / instance references to nodes that do not exist are dropped, and a group cannot
    // contain itself.
    for (CompNode& n : comp.nodes) {
        if (n.group != 0 && (!ids.contains(n.group) || n.group == n.id))
            n.group = 0;
        if (n.instanceOf != 0 && (!ids.contains(n.instanceOf) || n.instanceOf == n.id))
            n.instanceOf = 0;
    }
    // Links to nodes that do not exist are dropped (a hand-edited or damaged file).
    for (CompNode& n : comp.nodes)
        n.inputs.removeIf([&ids](QMap<QString, NodeLink>::iterator it) {
            return !ids.contains(it.value().nodeId);
        });
    for (const QJsonValue& v : o.value(QLatin1String("notes")).toArray()) {
        const QJsonObject no = v.toObject();
        CompNote note;
        note.id = quint64(no.value(QLatin1String("id")).toInteger());
        note.position = pointFromJson(no.value(QLatin1String("pos")));
        const QPointF size = pointFromJson(no.value(QLatin1String("size")));
        note.size = QSizeF(size.x(), size.y());
        note.text = no.value(QLatin1String("text")).toString();
        if (no.contains(QLatin1String("color")))
            note.color = variantFromJson(no.value(QLatin1String("color"))).value<QColor>();
        note.underlay = no.value(QLatin1String("underlay")).toBool(false);
        maxId = std::max(maxId, note.id);
        comp.notes.append(note);
    }
    comp.mediaOutId = quint64(o.value(QLatin1String("mediaOut")).toInteger());
    const CompNode* out = comp.node(comp.mediaOutId);
    if (!out || out->type != QLatin1String("media_out")) {
        comp.mediaOutId = 0;
        for (const CompNode& n : comp.nodes) {
            if (n.type == QLatin1String("media_out")) {
                comp.mediaOutId = n.id;
                break;
            }
        }
    }
    comp.nextId
        = std::max<quint64>(maxId + 1, quint64(o.value(QLatin1String("nextId")).toInteger()));
    const QJsonObject view = o.value(QLatin1String("view")).toObject();
    comp.viewCentre = pointFromJson(view.value(QLatin1String("centre")));
    comp.viewZoom = view.value(QLatin1String("zoom")).toDouble(1.0);
    if (!(comp.viewZoom > 0.0))
        comp.viewZoom = 1.0;
    comp.leftViewerNode = quint64(view.value(QLatin1String("left")).toInteger());
    comp.rightViewerNode = quint64(view.value(QLatin1String("right")).toInteger());
    if (!ids.contains(comp.leftViewerNode))
        comp.leftViewerNode = 0;
    if (!ids.contains(comp.rightViewerNode))
        comp.rightViewerNode = 0;
    const QJsonObject audio = o.value(QLatin1String("audio")).toObject();
    comp.soloAudio = audio.value(QLatin1String("solo")).toBool(false);
    const double offset = audio.value(QLatin1String("offset")).toDouble(0.0);
    comp.soundOffset = std::isfinite(offset) ? offset : 0.0;
    return comp;
}

} // namespace kk::serialization
