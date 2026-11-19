// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/PaintStrokes.h"

#include "core/NodeRegistry.h"

#include <QCache>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>

namespace kk::paint {

QString toolId(Tool tool)
{
    switch (tool) {
    case Tool::Stroke:
        return painttools::Stroke;
    case Tool::Clone:
        return painttools::Clone;
    case Tool::Eraser:
        return painttools::Eraser;
    case Tool::Smear:
        return painttools::Smear;
    }
    return painttools::Stroke;
}

Tool toolFromId(const QString& id)
{
    if (id == painttools::Clone)
        return Tool::Clone;
    if (id == painttools::Eraser)
        return Tool::Eraser;
    if (id == painttools::Smear)
        return Tool::Smear;
    return Tool::Stroke;
}

QString toText(const QVector<Stroke>& strokes)
{
    if (strokes.isEmpty())
        return {};
    QJsonArray list;
    for (const Stroke& s : strokes) {
        QJsonObject o;
        o[QLatin1String("t")] = toolId(s.tool);
        o[QLatin1String("size")] = s.size;
        o[QLatin1String("soft")] = s.softness;
        o[QLatin1String("op")] = s.opacity;
        o[QLatin1String("color")] = s.color.name(QColor::HexArgb);
        if (s.tool == Tool::Clone) {
            o[QLatin1String("cx")] = s.cloneX;
            o[QLatin1String("cy")] = s.cloneY;
        }
        o[QLatin1String("from")] = qint64(s.first);
        o[QLatin1String("to")] = qint64(s.last);
        QJsonArray pts;
        for (const Point& p : s.points) {
            pts.append(double(p.x));
            pts.append(double(p.y));
            pts.append(double(p.pressure));
        }
        o[QLatin1String("pts")] = pts;
        list.append(o);
    }
    return QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact));
}

QVector<Stroke> fromText(const QString& text)
{
    QVector<Stroke> out;
    if (text.isEmpty())
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        Stroke s;
        s.tool = toolFromId(o.value(QLatin1String("t")).toString());
        s.size = std::clamp(o.value(QLatin1String("size")).toDouble(0.03), 0.0001, 4.0);
        s.softness = std::clamp(o.value(QLatin1String("soft")).toDouble(0.3), 0.0, 1.0);
        s.opacity = std::clamp(o.value(QLatin1String("op")).toDouble(1.0), 0.0, 1.0);
        const QColor c(o.value(QLatin1String("color")).toString());
        if (c.isValid())
            s.color = c;
        s.cloneX = o.value(QLatin1String("cx")).toDouble(0.0);
        s.cloneY = o.value(QLatin1String("cy")).toDouble(0.0);
        s.first = o.value(QLatin1String("from")).toInteger(0);
        s.last = o.value(QLatin1String("to")).toInteger(s.first);
        const QJsonArray pts = o.value(QLatin1String("pts")).toArray();
        for (qsizetype i = 0; i + 2 < pts.size(); i += 3) {
            s.points.append(Point {float(pts.at(i).toDouble()), float(pts.at(i + 1).toDouble()),
                float(std::clamp(pts.at(i + 2).toDouble(1.0), 0.0, 1.0))});
        }
        out.append(s);
    }
    return out;
}

QVector<Stroke> strokesOf(const ParamSet& params)
{
    const QString text = params.staticValue(nodekeys::PaintStrokes, QString()).toString();
    if (text.isEmpty())
        return {};
    static QMutex mutex;
    static QCache<QString, QVector<Stroke>> cache(32);
    {
        QMutexLocker lock(&mutex);
        if (const QVector<Stroke>* hit = cache.object(text))
            return *hit;
    }
    const QVector<Stroke> parsed = fromText(text);
    QMutexLocker lock(&mutex);
    cache.insert(text, new QVector<Stroke>(parsed), int(std::max<qsizetype>(1, text.size() / 1024)));
    return parsed;
}

void setStrokes(ParamSet& params, const QVector<Stroke>& strokes)
{
    params.setStatic(nodekeys::PaintStrokes, toText(strokes));
}

Stroke newStroke(const ParamSet& params, int64_t frame)
{
    Stroke s;
    s.tool = toolFromId(params.staticValue(nodekeys::PaintTool, painttools::Stroke).toString());
    s.size = std::clamp(params.value(nodekeys::BrushSize, frame, 0.03), 0.001, 1.0);
    s.softness = std::clamp(params.value(nodekeys::BrushSoftness, frame, 0.3), 0.0, 1.0);
    s.opacity = std::clamp(params.value(nodekeys::BrushOpacity, frame, 1.0), 0.0, 1.0);
    s.color = params.staticValue(nodekeys::Color, QColor(255, 0, 0)).value<QColor>();
    s.cloneX = params.value(nodekeys::CloneOffsetX, frame, 0.1);
    s.cloneY = params.value(nodekeys::CloneOffsetY, frame, 0.0);
    const QString mode
        = params.staticValue(nodekeys::StrokeDuration, paintdurations::OneFrame).toString();
    if (mode == paintdurations::Whole) {
        s.first = -kForever;
        s.last = kForever;
    } else if (mode == paintdurations::Frames) {
        const int n = std::max(1, int(params.value(nodekeys::StrokeFrames, frame, 10)));
        s.first = frame;
        s.last = frame + n - 1;
    } else {
        s.first = frame;
        s.last = frame;
    }
    return s;
}

} // namespace kk::paint
