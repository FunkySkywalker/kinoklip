// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/KeyRows.h"

#include <algorithm>

namespace kk::keyrows {

namespace {

/// Calls @p fn(iterator) for every stored parameter of the row.
template<typename SliceT, typename Fn>
void forEachValue(const std::vector<SliceT>& row, Fn&& fn)
{
    for (const SliceT& s : row) {
        if (!s.set)
            continue;
        for (auto it = s.set->values.begin(); it != s.set->values.end(); ++it) {
            if (s.keys.isEmpty() || s.keys.contains(it.key()))
                fn(it);
        }
    }
}

bool isDynamicPreset(Interp i)
{
    return i == Interp::Linear || i == Interp::EaseIn || i == Interp::EaseOut
        || i == Interp::EaseInOut || i == Interp::Smooth;
}

} // namespace

QVector<int64_t> frames(const ConstSlices& row)
{
    QVector<int64_t> out;
    forEachValue(row, [&](auto it) {
        for (const Keyframe& k : it.value().keys)
            out.append(k.frame);
    });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool hasKeyAt(const ConstSlices& row, int64_t frame)
{
    bool found = false;
    forEachValue(row, [&](auto it) { found = found || it.value().hasKeyAt(frame); });
    return found;
}

Mode modeAt(const ConstSlices& row, int64_t frame)
{
    bool any = false, allHold = true;
    forEachValue(row, [&](auto it) {
        const AnimatedValue& v = it.value();
        const qsizetype i = v.indexOf(frame);
        if (i >= 0) {
            any = true;
            allHold = allHold && v.keys[i].hold;
        }
    });
    return any && allHold ? Mode::Static : Mode::Dynamic;
}

Interp interpAt(const ConstSlices& row, int64_t frame)
{
    bool any = false, mixed = false;
    Interp shared = Interp::Custom;
    forEachValue(row, [&](auto it) {
        const AnimatedValue& v = it.value();
        const qsizetype i = v.indexOf(frame);
        if (i < 0 || v.keys[i].hold)
            return; // only the Dynamic keys count
        const Interp kind = keyInterp(v.keys[i]);
        if (!any) {
            shared = kind;
            any = true;
        } else if (kind != shared) {
            mixed = true;
        }
    });
    return any && !mixed ? shared : Interp::Custom;
}

int addKeys(const Slices& row, int64_t frame, double (*defaultOf)(const QString&))
{
    int n = 0;
    for (const Slice& s : row) {
        if (!s.set)
            continue;
        QStringList keys = s.keys;
        if (keys.isEmpty())
            keys = s.set->values.keys();
        for (const QString& key : std::as_const(keys)) {
            AnimatedValue& v = s.set->animated(key, defaultOf ? defaultOf(key) : 0.0);
            if (v.hasKeyAt(frame))
                continue;
            v.setKey(frame, v.valueAt(frame));
            ++n;
        }
    }
    return n;
}

int deleteKeys(const Slices& row, int64_t frame)
{
    int n = 0;
    forEachValue(row, [&](auto it) {
        if (it.value().hasKeyAt(frame)) {
            it.value().removeKey(frame);
            ++n;
        }
    });
    return n;
}

int moveKeys(const Slices& row, int64_t from, int64_t to)
{
    if (from == to)
        return 0;
    int n = 0;
    forEachValue(row, [&](auto it) {
        AnimatedValue& v = it.value();
        const qsizetype i = v.indexOf(from);
        if (i < 0)
            return;
        Keyframe k = v.keys[i];
        v.keys.remove(i);
        const qsizetype j = v.indexOf(to);
        if (j >= 0)
            v.keys.remove(j);
        k.frame = to;
        v.putKey(k);
        ++n;
    });
    return n;
}

int setMode(const Slices& row, int64_t frame, Mode mode)
{
    int n = 0;
    forEachValue(row, [&](auto it) {
        AnimatedValue& v = it.value();
        const qsizetype i = v.indexOf(frame);
        if (i < 0)
            return;
        const bool hold = mode == Mode::Static;
        if (v.keys[i].hold != hold) {
            v.keys[i].hold = hold;
            ++n;
        }
    });
    return n;
}

int setInterp(const Slices& row, int64_t frame, Interp interp)
{
    if (!isDynamicPreset(interp))
        return 0;
    int n = 0;
    forEachValue(row, [&](auto it) {
        AnimatedValue& v = it.value();
        const qsizetype i = v.indexOf(frame);
        if (i < 0)
            return;
        Keyframe k = v.keys[i];
        applyInterp(k, interp);
        k.hold = false;
        if (!(k == v.keys[i])) {
            v.keys[i] = k;
            ++n;
        }
    });
    return n;
}

} // namespace kk::keyrows
