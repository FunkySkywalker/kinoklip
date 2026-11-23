// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Sizing.h"

#include <algorithm>
#include <cmath>

namespace kk::sizing {

const QStringList& sizingKeys()
{
    static const QStringList list {
        keys::PosX, keys::PosY, keys::ZoomX, keys::ZoomY, keys::Rotation};
    return list;
}

keyrows::Slices rowOf(ParamSet& params)
{
    return {{&params, sizingKeys()}};
}

keyrows::ConstSlices rowOf(const ParamSet& params)
{
    return {{&params, sizingKeys()}};
}

int addKeys(ParamSet& params, int64_t frame)
{
    return keyrows::addKeys(rowOf(params), frame, [](const QString& key) { return keys::defaultValue(key); });
}

double zoomOf(const ParamSet& params, int64_t frame)
{
    const double x = std::abs(params.value(keys::ZoomX, frame));
    const double y = std::abs(params.value(keys::ZoomY, frame));
    return std::sqrt(x * y);
}

void setAt(ParamSet& params, const QString& key, int64_t frame, double value, bool forceKey)
{
    AnimatedValue& v = params.animated(key, keys::defaultValue(key));
    if (forceKey)
        v.setKey(frame, value);
    else
        v.setValue(frame, value);
}

void setZoomAt(ParamSet& params, int64_t frame, double zoom, bool forceKey)
{
    const double x = params.value(keys::ZoomX, frame);
    const double y = params.value(keys::ZoomY, frame);
    const double now = std::sqrt(std::abs(x * y));
    if (now < 1e-9) {
        setAt(params, keys::ZoomX, frame, zoom, forceKey);
        setAt(params, keys::ZoomY, frame, zoom, forceKey);
        return;
    }
    const double f = zoom / now;
    setAt(params, keys::ZoomX, frame, x * f, forceKey);
    setAt(params, keys::ZoomY, frame, y * f, forceKey);
}

bool isIdentity(const ParamSet& params, int64_t frame)
{
    auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    return same(params.value(keys::PosX, frame), 0.0) && same(params.value(keys::PosY, frame), 0.0)
        && same(params.value(keys::ZoomX, frame), 1.0)
        && same(params.value(keys::ZoomY, frame), 1.0)
        && same(params.value(keys::Rotation, frame), 0.0)
        && !params.staticValue(keys::FlipH, false).toBool()
        && !params.staticValue(keys::FlipV, false).toBool();
}

bool isEmpty(const ParamSet& params)
{
    return params.values.isEmpty() && params.statics.isEmpty();
}

QTransform outputTransform(
    const ParamSet& params, int64_t frame, QSizeF outSize, QSizeF timelineSize)
{
    if (isEmpty(params) || isIdentity(params, frame))
        return QTransform();
    const double s = timelineSize.width() > 0.0 ? outSize.width() / timelineSize.width() : 1.0;
    const double px = params.value(keys::PosX, frame) * s;
    const double py = params.value(keys::PosY, frame) * s;
    double zx = params.value(keys::ZoomX, frame);
    double zy = params.value(keys::ZoomY, frame);
    if (params.staticValue(keys::FlipH, false).toBool())
        zx = -zx;
    if (params.staticValue(keys::FlipV, false).toBool())
        zy = -zy;
    const double cx = outSize.width() / 2.0;
    const double cy = outSize.height() / 2.0;
    QTransform t;
    t.translate(cx + px, cy + py);
    t.rotate(params.value(keys::Rotation, frame));
    t.scale(zx, zy);
    t.translate(-cx, -cy);
    return t;
}

} // namespace kk::sizing
