// SPDX-License-Identifier: GPL-3.0-or-later
// Macro files, see MacroFiles.h.
#include "core/MacroFiles.h"

#include "core/CompGroups.h"
#include "core/CompOps.h"
#include "core/Serialization.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace kk::macrofiles {

namespace {
constexpr int kFormat = 1;
constexpr const char* kExtension = ".kkmacro";
}

QString Entry::id() const
{
    return kIdPrefix + baseName(name);
}

QString directory()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/macros");
    QDir().mkpath(dir);
    return dir;
}

QString baseName(const QString& name)
{
    QString out;
    for (const QChar c : name.trimmed())
        out.append(c.isLetterOrNumber() ? c : QLatin1Char('-'));
    return out.isEmpty() ? QStringLiteral("macro") : out;
}

QString pathFor(const QString& name)
{
    return directory() + QLatin1Char('/') + baseName(name) + QLatin1String(kExtension);
}

std::optional<MacroFile> fromGroup(const Composition& comp, quint64 group)
{
    const CompNode* g = comp.node(group);
    if (!g || !compgroups::isGroup(*g))
        return std::nullopt;
    MacroFile m;
    m.name = g->name;
    m.outputId = compgroups::outputNode(comp, group);
    QVector<quint64> ids {group};
    ids += compgroups::members(comp, group, true);
    m.nodes = compops::copyNodes(comp, ids);
    // The group node goes first (insert() returns the first node as the group) and sits at the
    // top level.
    for (CompNode& n : m.nodes) {
        if (n.id == group)
            n.group = 0;
    }
    std::stable_partition(m.nodes.begin(), m.nodes.end(),
        [group](const CompNode& n) { return n.id == group; });
    return m;
}

QString save(const MacroFile& macro, QString* error)
{
    const QString path = pathFor(macro.name);
    QJsonObject o {{QStringLiteral("kinoklipMacro"), kFormat}, {QStringLiteral("name"), macro.name},
        {QStringLiteral("outputId"), qint64(macro.outputId)},
        {QStringLiteral("nodes"), serialization::compNodesToJson(macro.nodes)}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(o).toJson()) < 0
        || !file.commit()) {
        if (error)
            *error = file.errorString();
        return {};
    }
    return path;
}

std::optional<MacroFile> load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
    if (o.value(QStringLiteral("kinoklipMacro")).toInt() < 1)
        return std::nullopt;
    MacroFile m;
    m.name = o.value(QStringLiteral("name")).toString();
    m.outputId = quint64(o.value(QStringLiteral("outputId")).toInteger());
    m.nodes = serialization::compNodesFromJson(o.value(QStringLiteral("nodes")).toArray());
    if (m.name.isEmpty() || m.nodes.isEmpty())
        return std::nullopt;
    return m;
}

QVector<Entry> list()
{
    QVector<Entry> out;
    const QDir dir(directory());
    for (const QFileInfo& f : dir.entryInfoList({QStringLiteral("*") + QLatin1String(kExtension)},
             QDir::Files, QDir::Name)) {
        if (const auto m = load(f.absoluteFilePath()))
            out.append({m->name, f.absoluteFilePath()});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    return out;
}

bool remove(const QString& name)
{
    return QFile::remove(pathFor(name));
}

QString pathOfId(const QString& id)
{
    if (!id.startsWith(kIdPrefix))
        return {};
    const QString path = directory() + QLatin1Char('/') + id.mid(kIdPrefix.size())
        + QLatin1String(kExtension);
    return QFileInfo::exists(path) ? path : QString();
}

quint64 insert(Composition& comp, const MacroFile& macro, QPointF position, quint64* newOutput)
{
    if (newOutput)
        *newOutput = 0;
    if (macro.nodes.isEmpty())
        return 0;
    // The saved positions are kept relative to the top left of the nodes.
    QPointF origin = macro.nodes.first().position;
    for (const CompNode& n : macro.nodes)
        origin = QPointF(std::min(origin.x(), n.position.x()), std::min(origin.y(), n.position.y()));
    const QVector<quint64> ids = compops::pasteNodes(comp, macro.nodes, position - origin);
    if (ids.isEmpty())
        return 0;
    if (newOutput && ids.size() == macro.nodes.size()) {
        for (qsizetype i = 0; i < ids.size(); ++i) {
            if (macro.nodes.at(i).id == macro.outputId)
                *newOutput = ids.at(i);
        }
    }
    return ids.first(); // the group node is first
}

Composition titleComposition(const MacroFile& macro)
{
    Composition c = compops::defaultComposition();
    quint64 out = 0;
    const quint64 group = insert(c, macro, QPointF(0, 3), &out);
    if (!group)
        return c;
    if (!out)
        out = compgroups::outputNode(c, group);
    if (out)
        compops::connect(c, out, 0, c.mediaOutId, nodeports::Input);
    return c;
}

} // namespace kk::macrofiles
