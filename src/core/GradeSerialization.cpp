// SPDX-License-Identifier: GPL-3.0-or-later
// GradeSerialization — JSON form of grades (project format version 16, see
// core/Serialization.h). Only values that differ from the defaults are written.
#include "core/Serialization.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>

namespace kk::serialization {

namespace {

/// JSON keys of the curves, in GradeCurve order.
const char* const kCurveNames[kGradeCurveCount]
    = {"luma", "red", "green", "blue", "hueHue", "hueSat", "hueLum", "lumSat", "satSat"};

QJsonObject qualifierToJson(const GradeQualifier& q)
{
    const GradeQualifier d;
    QJsonObject o;
    auto flag = [&](const char* key, bool v, bool def) {
        if (v != def)
            o[QLatin1String(key)] = v;
    };
    auto num = [&](const char* key, double v, double def) {
        if (v != def)
            o[QLatin1String(key)] = v;
    };
    flag("enabled", q.enabled, d.enabled);
    flag("invert", q.invert, d.invert);
    flag("useHue", q.useHue, d.useHue);
    flag("useSat", q.useSat, d.useSat);
    flag("useLum", q.useLum, d.useLum);
    num("hueCenter", q.hueCenter, d.hueCenter);
    num("hueWidth", q.hueWidth, d.hueWidth);
    num("hueSoft", q.hueSoft, d.hueSoft);
    num("satLow", q.satLow, d.satLow);
    num("satHigh", q.satHigh, d.satHigh);
    num("satSoft", q.satSoft, d.satSoft);
    num("lumLow", q.lumLow, d.lumLow);
    num("lumHigh", q.lumHigh, d.lumHigh);
    num("lumSoft", q.lumSoft, d.lumSoft);
    if (q.mode != d.mode) {
        o[QLatin1String("mode")] = q.mode == QualifierMode::Rgb ? QLatin1String("rgb")
            : q.mode == QualifierMode::Luma                    ? QLatin1String("luma")
            : q.mode == QualifierMode::ThreeD                  ? QLatin1String("threeD")
                                                               : QLatin1String("hsl");
    }
    flag("useRed", q.useRed, d.useRed);
    flag("useGreen", q.useGreen, d.useGreen);
    flag("useBlue", q.useBlue, d.useBlue);
    num("redLow", q.redLow, d.redLow);
    num("redHigh", q.redHigh, d.redHigh);
    num("redSoft", q.redSoft, d.redSoft);
    num("greenLow", q.greenLow, d.greenLow);
    num("greenHigh", q.greenHigh, d.greenHigh);
    num("greenSoft", q.greenSoft, d.greenSoft);
    num("blueLow", q.blueLow, d.blueLow);
    num("blueHigh", q.blueHigh, d.blueHigh);
    num("blueSoft", q.blueSoft, d.blueSoft);
    if (!q.colors.isEmpty()) {
        QJsonArray colors;
        for (const GradeQualifierColor& c : q.colors)
            colors.append(QJsonArray {c.r, c.g, c.b, c.subtract ? 1 : 0});
        o[QLatin1String("colors")] = colors;
    }
    num("colorRange", q.colorRange, d.colorRange);
    num("colorSoft", q.colorSoft, d.colorSoft);
    num("cleanBlack", q.cleanBlack, d.cleanBlack);
    num("cleanWhite", q.cleanWhite, d.cleanWhite);
    num("shrinkGrow", q.shrinkGrow, d.shrinkGrow);
    num("blurRadius", q.blurRadius, d.blurRadius);
    num("inOutRatio", q.inOutRatio, d.inOutRatio);
    num("denoise", q.denoise, d.denoise);
    return o;
}

GradeQualifier qualifierFromJson(const QJsonObject& o)
{
    GradeQualifier q;
    auto flag = [&](const char* key, bool& v) { v = o.value(QLatin1String(key)).toBool(v); };
    auto num = [&](const char* key, double& v) { v = o.value(QLatin1String(key)).toDouble(v); };
    flag("enabled", q.enabled);
    flag("invert", q.invert);
    flag("useHue", q.useHue);
    flag("useSat", q.useSat);
    flag("useLum", q.useLum);
    num("hueCenter", q.hueCenter);
    num("hueWidth", q.hueWidth);
    num("hueSoft", q.hueSoft);
    num("satLow", q.satLow);
    num("satHigh", q.satHigh);
    num("satSoft", q.satSoft);
    num("lumLow", q.lumLow);
    num("lumHigh", q.lumHigh);
    num("lumSoft", q.lumSoft);
    const QString mode = o.value(QLatin1String("mode")).toString();
    q.mode = mode == QLatin1String("rgb") ? QualifierMode::Rgb
        : mode == QLatin1String("luma")   ? QualifierMode::Luma
        : mode == QLatin1String("threeD") ? QualifierMode::ThreeD
                                          : QualifierMode::Hsl;
    flag("useRed", q.useRed);
    flag("useGreen", q.useGreen);
    flag("useBlue", q.useBlue);
    num("redLow", q.redLow);
    num("redHigh", q.redHigh);
    num("redSoft", q.redSoft);
    num("greenLow", q.greenLow);
    num("greenHigh", q.greenHigh);
    num("greenSoft", q.greenSoft);
    num("blueLow", q.blueLow);
    num("blueHigh", q.blueHigh);
    num("blueSoft", q.blueSoft);
    for (const QJsonValue& v : o.value(QLatin1String("colors")).toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() >= 3 && q.colors.size() < kMaxQualifierColors) {
            q.colors.append({float(std::clamp(a[0].toDouble(), 0.0, 1.0)),
                float(std::clamp(a[1].toDouble(), 0.0, 1.0)),
                float(std::clamp(a[2].toDouble(), 0.0, 1.0)), a.size() > 3 && a[3].toInt() != 0});
        }
    }
    num("colorRange", q.colorRange);
    num("colorSoft", q.colorSoft);
    num("cleanBlack", q.cleanBlack);
    num("cleanWhite", q.cleanWhite);
    num("shrinkGrow", q.shrinkGrow);
    num("blurRadius", q.blurRadius);
    num("inOutRatio", q.inOutRatio);
    num("denoise", q.denoise);
    return q;
}

QJsonObject curvesToJson(const GradeCurves& curves)
{
    QJsonObject o;
    for (int c = 0; c < kGradeCurveCount; ++c) {
        const CurvePoints& points = curves.points[size_t(c)];
        if (points.isEmpty())
            continue;
        QJsonArray list;
        for (const CurvePoint& p : points)
            list.append(QJsonArray {p.x, p.y});
        o[QLatin1String(kCurveNames[c])] = list;
    }
    return o;
}

GradeCurves curvesFromJson(const QJsonObject& o)
{
    GradeCurves curves;
    for (int c = 0; c < kGradeCurveCount; ++c) {
        CurvePoints& points = curves.points[size_t(c)];
        for (const QJsonValue& v : o.value(QLatin1String(kCurveNames[c])).toArray()) {
            const QJsonArray a = v.toArray();
            if (a.size() < 2)
                continue;
            points.append(
                {std::clamp(a[0].toDouble(), 0.0, 1.0), std::clamp(a[1].toDouble(), 0.0, 1.0)});
        }
        std::sort(points.begin(), points.end(),
            [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    }
    return curves;
}

/// Points as [x, y, inX, inY, outX, outY, smooth 0 | 1]; a point without handles is [x, y].
QJsonArray windowPointsToJson(const GradeWindowShapePoints& points)
{
    QJsonArray out;
    for (const GradeWindowPoint& p : points) {
        QJsonArray a {p.pos.x(), p.pos.y()};
        if (p.hasHandles() || p.smooth) {
            a.append(p.inHandle.x());
            a.append(p.inHandle.y());
            a.append(p.outHandle.x());
            a.append(p.outHandle.y());
            a.append(p.smooth ? 1 : 0);
        }
        out.append(a);
    }
    return out;
}

/// Reads the points written above and the older [x, y] pairs (corner points); at most
/// kMaxWindowPoints are kept.
GradeWindowShapePoints windowPointsFromJson(const QJsonArray& array)
{
    GradeWindowShapePoints out;
    for (const QJsonValue& v : array) {
        const QJsonArray a = v.toArray();
        if (a.size() < 2 || out.size() >= kMaxWindowPoints)
            continue;
        GradeWindowPoint p;
        p.pos = QPointF(a[0].toDouble(), a[1].toDouble());
        if (a.size() >= 7) {
            p.inHandle = QPointF(a[2].toDouble(), a[3].toDouble());
            p.outHandle = QPointF(a[4].toDouble(), a[5].toDouble());
            p.smooth = a[6].toDouble() != 0.0;
        }
        out.append(p);
    }
    return out;
}

QJsonObject windowToJson(const GradeWindow& w)
{
    QJsonObject o {{QLatin1String("id"), qint64(w.id)},
        {QLatin1String("shape"), gradeWindowShapeToString(w.shape)}};
    if (!w.enabled)
        o[QLatin1String("enabled")] = false;
    if (w.invert)
        o[QLatin1String("invert")] = true;
    if (w.params != ParamSet {})
        o[QLatin1String("params")] = paramSetToJson(w.params);
    if (!w.points.isEmpty())
        o[QLatin1String("points")] = windowPointsToJson(w.points);
    if (!w.shapeKeys.isEmpty()) {
        QJsonArray keys;
        for (auto it = w.shapeKeys.cbegin(); it != w.shapeKeys.cend(); ++it) {
            keys.append(QJsonObject {{QLatin1String("frame"), qint64(it.key())},
                {QLatin1String("points"), windowPointsToJson(it.value())}});
        }
        o[QLatin1String("shapeKeys")] = keys;
    }
    return o;
}

GradeWindow windowFromJson(const QJsonObject& o)
{
    GradeWindow w;
    w.id = quint64(o.value(QLatin1String("id")).toInteger());
    w.shape = gradeWindowShapeFromString(o.value(QLatin1String("shape")).toString());
    w.enabled = o.value(QLatin1String("enabled")).toBool(true);
    w.invert = o.value(QLatin1String("invert")).toBool(false);
    w.params = paramSetFromJson(o.value(QLatin1String("params")).toObject());
    w.points = windowPointsFromJson(o.value(QLatin1String("points")).toArray());
    // Shape keys: a key whose point count differs from the static shape is dropped.
    for (const QJsonValue& v : o.value(QLatin1String("shapeKeys")).toArray()) {
        const QJsonObject key = v.toObject();
        const GradeWindowShapePoints pts = windowPointsFromJson(key.value(QLatin1String("points")).toArray());
        if (pts.size() == w.points.size() && !pts.isEmpty())
            w.shapeKeys.insert(key.value(QLatin1String("frame")).toInteger(), pts);
    }
    return w;
}

/// Tracker data: the samples as one flat array of five numbers per frame.
QJsonObject trackToJson(const GradeTrack& t)
{
    QJsonObject o {{QLatin1String("first"), qint64(t.firstFrame)},
        {QLatin1String("pivot"), QJsonArray {t.pivot.x(), t.pivot.y()}},
        {QLatin1String("pan"), t.pan}, {QLatin1String("tilt"), t.tilt},
        {QLatin1String("zoom"), t.zoom}, {QLatin1String("rotate"), t.rotate},
        {QLatin1String("perspective"), t.perspective}};
    QJsonArray samples;
    for (const GradeTrackSample& s : t.samples) {
        for (const double v : {s.dx, s.dy, s.sx, s.sy, s.rot})
            samples.append(v);
    }
    if (!samples.isEmpty())
        o[QLatin1String("samples")] = samples;
    return o;
}

GradeTrack trackFromJson(const QJsonObject& o)
{
    GradeTrack t;
    t.firstFrame = o.value(QLatin1String("first")).toInteger();
    const QJsonArray pivot = o.value(QLatin1String("pivot")).toArray();
    if (pivot.size() >= 2)
        t.pivot = QPointF(pivot[0].toDouble(0.5), pivot[1].toDouble(0.5));
    t.pan = o.value(QLatin1String("pan")).toBool(true);
    t.tilt = o.value(QLatin1String("tilt")).toBool(true);
    t.zoom = o.value(QLatin1String("zoom")).toBool(false);
    t.rotate = o.value(QLatin1String("rotate")).toBool(false);
    t.perspective = o.value(QLatin1String("perspective")).toBool(false);
    const QJsonArray a = o.value(QLatin1String("samples")).toArray();
    constexpr qsizetype kMaxSamples = 1000000;
    for (qsizetype i = 0; i + 4 < a.size() && t.samples.size() < kMaxSamples; i += 5) {
        t.samples.append({a[i].toDouble(), a[i + 1].toDouble(), a[i + 2].toDouble(1.0),
            a[i + 3].toDouble(1.0), a[i + 4].toDouble()});
    }
    return t;
}

} // namespace

QJsonObject toneMapToJson(const ToneMap& map)
{
    QJsonObject o;
    if (!map.agx)
        return o;
    o[QLatin1String("mode")] = QLatin1String("agx");
    static const char* const looks[] = {"none", "punchy", "golden", "greyscale"};
    o[QLatin1String("look")] = QLatin1String(looks[std::clamp(int(map.look), 0, 3)]);
    return o;
}

ToneMap toneMapFromJson(const QJsonObject& o)
{
    ToneMap map;
    if (o.value(QLatin1String("mode")).toString() != QLatin1String("agx"))
        return map;
    map.agx = true;
    const QString look = o.value(QLatin1String("look")).toString();
    if (look == QLatin1String("punchy"))
        map.look = AgxLook::Punchy;
    else if (look == QLatin1String("golden"))
        map.look = AgxLook::Golden;
    else if (look == QLatin1String("greyscale"))
        map.look = AgxLook::Greyscale;
    return map;
}

QJsonObject gradeToJson(const ClipGrade& grade)
{
    QJsonArray nodes;
    for (const GradeNode& n : grade.nodes) {
        QJsonObject o {{QLatin1String("id"), qint64(n.id)},
            {QLatin1String("type"), gradeNodeTypeToString(n.type)},
            {QLatin1String("pos"), QJsonArray {n.pos.x(), n.pos.y()}}};
        QJsonArray inputs;
        for (const quint64 in : n.inputs)
            inputs.append(qint64(in));
        o[QLatin1String("inputs")] = inputs;
        if (!n.label.isEmpty())
            o[QLatin1String("label")] = n.label;
        if (!n.enabled)
            o[QLatin1String("enabled")] = false;
        if (n.params != ParamSet {})
            o[QLatin1String("params")] = paramSetToJson(n.params);
        if (n.curves != GradeCurves {})
            o[QLatin1String("curves")] = curvesToJson(n.curves);
        if (n.qualifier != GradeQualifier {})
            o[QLatin1String("qualifier")] = qualifierToJson(n.qualifier);
        if (!n.windows.isEmpty()) {
            QJsonArray windows;
            for (const GradeWindow& w : n.windows)
                windows.append(windowToJson(w));
            o[QLatin1String("windows")] = windows;
        }
        if (n.track != GradeTrack {})
            o[QLatin1String("track")] = trackToJson(n.track);
        if (!n.lutPath.isEmpty())
            o[QLatin1String("lut")] = n.lutPath;
        if (n.lutInterpolation == LutInterpolation::Trilinear)
            o[QLatin1String("lutInterp")] = QStringLiteral("trilinear");
        nodes.append(o);
    }
    return QJsonObject {
        {QLatin1String("nodes"), nodes}, {QLatin1String("output"), qint64(grade.output)}};
}

ClipGrade gradeFromJson(const QJsonObject& json)
{
    ClipGrade g;
    QSet<quint64> used;
    for (const QJsonValue& v : json.value(QLatin1String("nodes")).toArray()) {
        const QJsonObject o = v.toObject();
        GradeNode n;
        n.id = quint64(o.value(QLatin1String("id")).toInteger());
        if (n.id == 0 || used.contains(n.id))
            continue; // damaged / hand-edited file: drop the node rather than guess
        used.insert(n.id);
        n.type = gradeNodeTypeFromString(o.value(QLatin1String("type")).toString());
        n.label = o.value(QLatin1String("label")).toString();
        n.enabled = o.value(QLatin1String("enabled")).toBool(true);
        n.params = paramSetFromJson(o.value(QLatin1String("params")).toObject());
        n.curves = curvesFromJson(o.value(QLatin1String("curves")).toObject());
        n.qualifier = qualifierFromJson(o.value(QLatin1String("qualifier")).toObject());
        for (const QJsonValue& w : o.value(QLatin1String("windows")).toArray())
            n.windows.append(windowFromJson(w.toObject()));
        n.track = trackFromJson(o.value(QLatin1String("track")).toObject());
        n.lutPath = o.value(QLatin1String("lut")).toString();
        n.lutInterpolation
            = o.value(QLatin1String("lutInterp")).toString() == QLatin1String("trilinear")
            ? LutInterpolation::Trilinear
            : LutInterpolation::Tetrahedral;
        n.inputs.clear();
        for (const QJsonValue& in : o.value(QLatin1String("inputs")).toArray())
            n.inputs.append(quint64(in.toInteger()));
        if (n.inputs.isEmpty() || (!n.isMixer() && n.inputs.size() > 1))
            n.inputs.resize(1);
        const QJsonArray pos = o.value(QLatin1String("pos")).toArray();
        n.pos = QPointF(pos.at(0).toDouble(), pos.at(1).toDouble());
        g.nodes.append(n);
    }
    // Inputs naming nodes that do not exist read as the source; window ids must be unique
    // within the grade (they share the node id space).
    for (GradeNode& n : g.nodes) {
        for (quint64& in : n.inputs) {
            if (in != 0 && !used.contains(in))
                in = 0;
        }
    }
    quint64 next = g.allocateId();
    QSet<quint64> windowIds;
    for (GradeNode& n : g.nodes) {
        for (GradeWindow& w : n.windows) {
            if (w.id == 0 || used.contains(w.id) || windowIds.contains(w.id))
                w.id = next++;
            windowIds.insert(w.id);
        }
    }
    g.output = quint64(json.value(QLatin1String("output")).toInteger());
    if (g.output != 0 && !used.contains(g.output))
        g.output = 0;
    return g;
}

} // namespace kk::serialization
