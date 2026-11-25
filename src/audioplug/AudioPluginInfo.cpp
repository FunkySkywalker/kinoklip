// SPDX-License-Identifier: GPL-3.0-or-later
// AudioPluginInfo — see AudioPluginInfo.h.
#include "audioplug/AudioPluginInfo.h"

#include <QJsonArray>

#include <cmath>

namespace kk::audioplug {

namespace {

QJsonArray strings(const QStringList& l)
{
    QJsonArray a;
    for (const QString& s : l)
        a.append(s);
    return a;
}

QStringList stringsFrom(const QJsonValue& v)
{
    QStringList l;
    for (const QJsonValue& e : v.toArray())
        l.append(e.toString());
    return l;
}

/// JSON has no NaN / infinity: such values are stored as 0.
double finite(double v)
{
    return std::isfinite(v) ? v : 0.0;
}

} // namespace

QJsonObject ParamInfo::toJson() const
{
    QJsonObject o;
    o[QLatin1String("key")] = key;
    o[QLatin1String("label")] = label;
    o[QLatin1String("group")] = group;
    o[QLatin1String("unit")] = unit;
    o[QLatin1String("min")] = finite(min);
    o[QLatin1String("max")] = finite(max);
    o[QLatin1String("def")] = finite(def);
    o[QLatin1String("integer")] = integer;
    o[QLatin1String("toggle")] = toggle;
    o[QLatin1String("enumeration")] = enumeration;
    o[QLatin1String("log")] = logarithmic;
    o[QLatin1String("hidden")] = hidden;
    o[QLatin1String("readOnly")] = readOnly;
    o[QLatin1String("enumLabels")] = strings(enumLabels);
    QJsonArray values;
    for (double v : enumValues)
        values.append(finite(v));
    o[QLatin1String("enumValues")] = values;
    o[QLatin1String("id")] = double(id);
    return o;
}

ParamInfo ParamInfo::fromJson(const QJsonObject& o)
{
    ParamInfo p;
    p.key = o[QLatin1String("key")].toString();
    p.label = o[QLatin1String("label")].toString();
    p.group = o[QLatin1String("group")].toString();
    p.unit = o[QLatin1String("unit")].toString();
    p.min = o[QLatin1String("min")].toDouble();
    p.max = o[QLatin1String("max")].toDouble(1.0);
    p.def = o[QLatin1String("def")].toDouble();
    p.integer = o[QLatin1String("integer")].toBool();
    p.toggle = o[QLatin1String("toggle")].toBool();
    p.enumeration = o[QLatin1String("enumeration")].toBool();
    p.logarithmic = o[QLatin1String("log")].toBool();
    p.hidden = o[QLatin1String("hidden")].toBool();
    p.readOnly = o[QLatin1String("readOnly")].toBool();
    p.enumLabels = stringsFrom(o[QLatin1String("enumLabels")]);
    for (const QJsonValue& v : o[QLatin1String("enumValues")].toArray())
        p.enumValues.append(v.toDouble());
    p.id = quint32(o[QLatin1String("id")].toDouble());
    return p;
}

QJsonObject PluginInfo::toJson() const
{
    QJsonObject o;
    o[QLatin1String("format")] = format;
    o[QLatin1String("id")] = id;
    o[QLatin1String("name")] = name;
    o[QLatin1String("vendor")] = vendor;
    o[QLatin1String("category")] = category;
    o[QLatin1String("description")] = description;
    o[QLatin1String("version")] = version;
    o[QLatin1String("in")] = inChannels;
    o[QLatin1String("out")] = outChannels;
    QJsonArray in;
    for (quint32 p : audioInPorts)
        in.append(double(p));
    QJsonArray out;
    for (quint32 p : audioOutPorts)
        out.append(double(p));
    o[QLatin1String("inPorts")] = in;
    o[QLatin1String("outPorts")] = out;
    o[QLatin1String("latencyPort")] = latencyPort;
    o[QLatin1String("portCount")] = double(portCount);
    o[QLatin1String("tail")] = finite(tailSeconds);
    o[QLatin1String("error")] = error;
    QJsonArray ps;
    for (const ParamInfo& p : params)
        ps.append(p.toJson());
    o[QLatin1String("params")] = ps;
    return o;
}

PluginInfo PluginInfo::fromJson(const QJsonObject& o)
{
    PluginInfo p;
    p.format = o[QLatin1String("format")].toString();
    p.id = o[QLatin1String("id")].toString();
    p.name = o[QLatin1String("name")].toString();
    p.vendor = o[QLatin1String("vendor")].toString();
    p.category = o[QLatin1String("category")].toString();
    p.description = o[QLatin1String("description")].toString();
    p.version = o[QLatin1String("version")].toString();
    p.inChannels = o[QLatin1String("in")].toInt();
    p.outChannels = o[QLatin1String("out")].toInt();
    for (const QJsonValue& v : o[QLatin1String("inPorts")].toArray())
        p.audioInPorts.append(quint32(v.toDouble()));
    for (const QJsonValue& v : o[QLatin1String("outPorts")].toArray())
        p.audioOutPorts.append(quint32(v.toDouble()));
    p.latencyPort = o[QLatin1String("latencyPort")].toInt(-1);
    p.portCount = quint32(o[QLatin1String("portCount")].toDouble());
    p.tailSeconds = o[QLatin1String("tail")].toDouble();
    p.error = o[QLatin1String("error")].toString();
    for (const QJsonValue& v : o[QLatin1String("params")].toArray())
        p.params.append(ParamInfo::fromJson(v.toObject()));
    return p;
}

QJsonObject BundleInfo::toJson() const
{
    QJsonObject o;
    o[QLatin1String("path")] = path;
    o[QLatin1String("format")] = format;
    o[QLatin1String("error")] = error;
    QJsonArray ps;
    for (const PluginInfo& p : plugins)
        ps.append(p.toJson());
    o[QLatin1String("plugins")] = ps;
    return o;
}

BundleInfo BundleInfo::fromJson(const QJsonObject& o)
{
    BundleInfo b;
    b.path = o[QLatin1String("path")].toString();
    b.format = o[QLatin1String("format")].toString();
    b.error = o[QLatin1String("error")].toString();
    for (const QJsonValue& v : o[QLatin1String("plugins")].toArray())
        b.plugins.append(PluginInfo::fromJson(v.toObject()));
    return b;
}

QString plainKey(const QString& text)
{
    QString key = text;
    for (QChar& c : key) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('_'))
            c = QLatin1Char('_');
    }
    if (key.isEmpty())
        key = QStringLiteral("p");
    return key;
}

void uniqueKeys(PluginInfo* plugin)
{
    QStringList seen;
    for (ParamInfo& p : plugin->params) {
        QString key = plainKey(p.key);
        const QString base = key;
        for (int n = 2; seen.contains(key); ++n)
            key = base + QLatin1Char('_') + QString::number(n);
        seen.append(key);
        p.key = key;
    }
}

} // namespace kk::audioplug
