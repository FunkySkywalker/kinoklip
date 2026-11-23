// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GradeKeyframes.h"

#include <algorithm>

namespace kk::gradekeyframes {

namespace {

QStringList nodeKeys()
{
    QStringList keys;
    for (const QString& k : gradekeys::allKeys())
        keys.append(k);
    return keys;
}

QStringList windowKeys()
{
    QStringList keys;
    for (const QString& k : gradewindowkeys::allKeys())
        keys.append(k);
    return keys;
}

double nodeDefault(const QString& key)
{
    return gradekeys::defaultValue(key);
}

double windowDefault(const QString& key)
{
    return gradewindowkeys::defaultValue(key);
}

} // namespace

keyrows::ConstSlices rowOf(const GradeNode& node)
{
    keyrows::ConstSlices row;
    row.push_back({&node.params, {}});
    for (const GradeWindow& w : node.windows)
        row.push_back({&w.params, {}});
    return row;
}

keyrows::Slices rowOf(GradeNode& node)
{
    keyrows::Slices row;
    row.push_back({&node.params, {}});
    for (GradeWindow& w : node.windows)
        row.push_back({&w.params, {}});
    return row;
}

QVector<int64_t> frames(const GradeNode& node)
{
    QVector<int64_t> out = keyrows::frames(rowOf(node));
    for (const GradeWindow& w : node.windows) {
        for (auto it = w.shapeKeys.cbegin(); it != w.shapeKeys.cend(); ++it)
            out.append(it.key());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool hasKeyAt(const GradeNode& node, int64_t frame)
{
    if (keyrows::hasKeyAt(rowOf(node), frame))
        return true;
    return std::any_of(node.windows.cbegin(), node.windows.cend(),
        [frame](const GradeWindow& w) { return w.shapeKeys.contains(frame); });
}

keyrows::Mode modeAt(const GradeNode& node, int64_t frame)
{
    return keyrows::modeAt(rowOf(node), frame);
}

Interp interpAt(const GradeNode& node, int64_t frame)
{
    return keyrows::interpAt(rowOf(node), frame);
}

int addKeys(GradeNode& node, int64_t frame)
{
    int n = keyrows::addKeys({{&node.params, nodeKeys()}}, frame, nodeDefault);
    for (GradeWindow& w : node.windows) {
        n += keyrows::addKeys({{&w.params, windowKeys()}}, frame, windowDefault);
        if (!w.shapeKeys.isEmpty() && !w.shapeKeys.contains(frame)) {
            // The shape shown at @p frame: the nearest key before it, else the first.
            auto it = w.shapeKeys.upperBound(frame);
            if (it != w.shapeKeys.begin())
                --it;
            w.shapeKeys.insert(frame, it.value());
            ++n;
        }
    }
    return n;
}

int deleteKeys(GradeNode& node, int64_t frame)
{
    int n = keyrows::deleteKeys(rowOf(node), frame);
    for (GradeWindow& w : node.windows)
        n += int(w.shapeKeys.remove(frame));
    return n;
}

int moveKeys(GradeNode& node, int64_t from, int64_t to)
{
    int n = keyrows::moveKeys(rowOf(node), from, to);
    if (from == to)
        return n;
    for (GradeWindow& w : node.windows) {
        if (auto it = w.shapeKeys.find(from); it != w.shapeKeys.end()) {
            const GradeWindowShapePoints points = it.value();
            w.shapeKeys.erase(it);
            w.shapeKeys.insert(to, points);
            ++n;
        }
    }
    return n;
}

int setMode(GradeNode& node, int64_t frame, keyrows::Mode mode)
{
    return keyrows::setMode(rowOf(node), frame, mode);
}

int setInterp(GradeNode& node, int64_t frame, Interp interp)
{
    return keyrows::setInterp(rowOf(node), frame, interp);
}

} // namespace kk::gradekeyframes
