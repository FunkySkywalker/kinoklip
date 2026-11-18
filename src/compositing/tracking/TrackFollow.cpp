// SPDX-License-Identifier: GPL-3.0-or-later
#include "compositing/tracking/TrackFollow.h"

#include "compositing/tracking/Homography.h"
#include "compositing/tracking/TrackLink.h"
#include "core/EffectTrack.h"
#include "core/NodeRegistry.h"
#include "core/VideoEffectKeys.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace kk::tracking {

namespace {

namespace nk = nodekeys;

std::array<float, 8> cornersAt(const ParamSet& p, int64_t frame)
{
    std::array<float, 8> out {};
    for (int i = 0; i < nk::kPlanarCorners; ++i) {
        out[size_t(2 * i)] = float(p.value(nk::planarCorner(i, false), frame, 0.5));
        out[size_t(2 * i + 1)] = float(p.value(nk::planarCorner(i, true), frame, 0.5));
    }
    return out;
}

/// Keys of @p v at the track frames, values from @p valueAt, replacing everything.
template<typename F>
AnimatedValue keysAt(const std::vector<int64_t>& frames, F&& valueAt)
{
    AnimatedValue out;
    for (const int64_t f : frames)
        out.setKey(f, valueAt(f), Interp::Linear);
    return out;
}

QPointF centrePx(const CompNode& n, int64_t frame, QSizeF size)
{
    return QPointF(n.params.value(nk::CenterX, frame, 0.5) * size.width(),
        (1.0 - n.params.value(nk::CenterY, frame, 0.5)) * size.height());
}

/// The size keys of @p n: Size, or Width and Height.
std::vector<QString> sizeKeys(const CompNode& n)
{
    const NodeDescriptor* d = NodeRegistry::instance().find(n.type);
    std::vector<QString> keys;
    if (!d)
        return keys;
    if (d->param(nk::Size))
        keys.push_back(nk::Size);
    else if (d->param(nk::Width) && d->param(nk::Height) && d->category == NodeCategory::Mask) {
        keys.push_back(nk::Width);
        keys.push_back(nk::Height);
    }
    return keys;
}

/// A new AnimatedPath with @p shapeAt(frame) at every frame of @p frames.
template<typename F>
AnimatedPath pathKeys(const std::vector<int64_t>& frames, F&& shapeAt)
{
    AnimatedPath path;
    path.shapes.clear();
    path.timing = AnimatedValue();
    double index = 0;
    for (const int64_t f : frames) {
        path.shapes.append(shapeAt(f));
        path.timing.setKey(f, index++, Interp::Linear);
    }
    path.normalize();
    return path;
}

/// @p shape with every point (and its tangent control points) mapped by @p m.
MaskPath mapped(const MaskPath& shape, const QTransform& m)
{
    MaskPath out = shape;
    for (MaskPoint& pt : out) {
        const QPointF p = m.map(pt.pos());
        const QPointF in = m.map(QPointF(pt.x + pt.inX, pt.y + pt.inY));
        const QPointF o = m.map(QPointF(pt.x + pt.outX, pt.y + pt.outY));
        pt.x = p.x();
        pt.y = p.y();
        pt.inX = in.x() - p.x();
        pt.inY = in.y() - p.y();
        pt.outX = o.x() - p.x();
        pt.outY = o.y() - p.y();
    }
    return out;
}

} // namespace

matchmove::Motion sourceMotion(
    const CompNode& source, int64_t refFrame, int64_t frame, QSizeF frameSize)
{
    if (source.type == nodetypes::PlanarTracker) {
        const std::array<float, 8> a = cornersAt(source.params, refFrame);
        const std::array<float, 8> b = cornersAt(source.params, frame);
        return matchmove::motionBetween(a.data(), b.data(), matchmove::kRegionStride, frameSize);
    }
    const int patterns = std::clamp(
        int(int(std::lround(source.params.value(nk::Patterns, 0, 1)))), 1, nk::kMaxPatterns);
    if (patterns > 1) {
        // Several patterns: the best move, turn and scale of the group (spec 118.3).
        std::vector<Vec2> then, now;
        QPointF ct, cn;
        for (int i = 1; i <= patterns; ++i) {
            const double ax = source.params.value(nk::patternKey(i, false), refFrame, 0.5);
            const double ay = source.params.value(nk::patternKey(i, true), refFrame, 0.5);
            const double bx = source.params.value(nk::patternKey(i, false), frame, 0.5);
            const double by = source.params.value(nk::patternKey(i, true), frame, 0.5);
            then.push_back({ax * frameSize.width(), (1.0 - ay) * frameSize.height()});
            now.push_back({bx * frameSize.width(), (1.0 - by) * frameSize.height()});
            ct += QPointF(then.back().x, then.back().y) / patterns;
            cn += QPointF(now.back().x, now.back().y) / patterns;
        }
        matchmove::Motion m;
        m.pivotX = ct.x();
        m.pivotY = ct.y();
        m.dx = cn.x() - ct.x();
        m.dy = cn.y() - ct.y();
        if (const auto sim = similarityFromPoints(then, now)) {
            m.angleDeg = sim->angleRad * 180.0 / M_PI;
            m.scale = sim->scale;
        }
        return m;
    }
    // A point: in doubles (the float arrays of the effect tables lose digits a Tracker keeps).
    matchmove::Motion m;
    const double ax = source.params.value(nk::CenterX, refFrame, 0.5);
    const double ay = source.params.value(nk::CenterY, refFrame, 0.5);
    m.pivotX = ax * frameSize.width();
    m.pivotY = (1.0 - ay) * frameSize.height();
    m.dx = (source.params.value(nk::CenterX, frame, 0.5) - ax) * frameSize.width();
    m.dy = -(source.params.value(nk::CenterY, frame, 0.5) - ay) * frameSize.height();
    return m;
}

QTransform motionTransform(
    const matchmove::Motion& m, const FollowOptions& o, QPointF fallbackPivot)
{
    const QPointF pivot = o.position ? QPointF(m.pivotX, m.pivotY) : fallbackPivot;
    QTransform t;
    t.translate(pivot.x() + (o.position ? m.dx : 0.0), pivot.y() + (o.position ? m.dy : 0.0));
    if (o.angle)
        t.rotate(m.angleDeg);
    if (o.size)
        t.scale(m.scale, m.scale);
    t.translate(-pivot.x(), -pivot.y());
    return t;
}

bool hasPathPoints(const CompNode& node)
{
    if (node.type == nodetypes::PolygonMask || node.type == nodetypes::BSplineMask
        || node.type == nodetypes::BezierMask)
        return node.path.pointCount() > 0;
    return node.type == nodetypes::Text
        && node.params.staticValue(nk::LayoutType, textlayouts::Point).toString()
        == textlayouts::Path
        && node.path.pointCount() > 0;
}

bool canFollowSize(const CompNode& node)
{
    return !sizeKeys(node).empty();
}

bool canFollowAngle(const CompNode& node)
{
    const NodeDescriptor* d = NodeRegistry::instance().find(node.type);
    return d && d->param(nk::Angle);
}

bool followTrack(CompNode& target, const CompNode& source, int64_t offsetFrame, QSizeF frameSize,
    const FollowOptions& o)
{
    if (target.locked || source.locked || !canFollowTrack(target) || !isTrackSource(source)
        || frameSize.isEmpty())
        return false;
    const std::vector<int64_t> frames = trackFrames(source);
    if (frames.empty())
        return false;
    const QPointF c0 = centrePx(target, offsetFrame, frameSize);

    if (o.points) {
        if (!hasPathPoints(target))
            return false;
        // Local point -> picture pixel: centre + R(-angle) * (size * point); the path is then
        // mapped by the track's motion and back.
        const double size0 = target.params.value(nk::Size, offsetFrame, 1.0);
        const double angle0 = target.params.value(nk::Angle, offsetFrame, 0.0);
        QTransform local;
        local.translate(c0.x(), c0.y());
        local.rotate(-angle0);
        local.scale(size0, size0);
        bool invertible = false;
        const QTransform back = local.inverted(&invertible);
        if (!invertible)
            return false;
        const MaskPath base = target.path.at(double(offsetFrame));
        target.path = pathKeys(frames, [&](int64_t f) {
            const matchmove::Motion m = sourceMotion(source, offsetFrame, f, frameSize);
            return mapped(base, local * motionTransform(m, o, c0) * back);
        });
        return true;
    }

    const auto motionAt = [&](int64_t f) { return sourceMotion(source, offsetFrame, f, frameSize); };
    if (o.position) {
        target.params.animated(nk::CenterX, 0.5) = keysAt(frames, [&](int64_t f) {
            const QPointF p = motionTransform(motionAt(f), o, c0).map(c0);
            return p.x() / frameSize.width();
        });
        target.params.animated(nk::CenterY, 0.5) = keysAt(frames, [&](int64_t f) {
            const QPointF p = motionTransform(motionAt(f), o, c0).map(c0);
            return 1.0 - p.y() / frameSize.height();
        });
    }
    if (o.size) {
        for (const QString& key : sizeKeys(target)) {
            const double v0 = target.params.value(key, offsetFrame, 1.0);
            target.params.animated(key, v0) = keysAt(frames, [&](int64_t f) {
                return v0 * std::max(motionAt(f).scale, 1e-6);
            });
        }
    }
    if (o.angle && canFollowAngle(target)) {
        const double a0 = target.params.value(nk::Angle, offsetFrame, 0.0);
        // The track turns clockwise on the screen, the node's angle counter-clockwise.
        target.params.animated(nk::Angle, 0.0) = keysAt(
            frames, [&](int64_t f) { return a0 - motionAt(f).angleDeg; });
    }
    return true;
}

bool followPoints(CompNode& target, const std::vector<int>& indices, const CompNode& source,
    int64_t offsetFrame, QSizeF frameSize)
{
    if (target.locked || source.locked || !isTrackSource(source) || !hasPathPoints(target)
        || frameSize.isEmpty() || indices.empty())
        return false;
    const std::vector<int64_t> frames = trackFrames(source);
    if (frames.empty())
        return false;
    const MaskPath base = target.path.at(double(offsetFrame));
    for (const int i : indices) {
        if (i < 0 || i >= base.size())
            return false;
    }
    const double size0 = std::max(1e-6, target.params.value(nk::Size, offsetFrame, 1.0));
    const double angle0 = target.params.value(nk::Angle, offsetFrame, 0.0);
    // A shift in picture pixels (y down) as a shift of mask-local points.
    QTransform turn;
    turn.rotate(angle0); // inverse of the node's rotation(-angle)
    target.path = pathKeys(frames, [&](int64_t f) {
        const matchmove::Motion m = sourceMotion(source, offsetFrame, f, frameSize);
        const QPointF d = turn.map(QPointF(m.dx, m.dy)) / size0;
        MaskPath shape = base;
        for (const int i : indices) {
            shape[i].x += d.x();
            shape[i].y += d.y();
        }
        return shape;
    });
    return true;
}

bool followMatchMove(MaskData& mask, const ClipData& clip, const EffectInstance& fx,
    int64_t offsetFrame, QSizeF frameSize, Rational fps, const FollowOptions& o)
{
    if (fx.type != QLatin1String(fxid::MatchMove) || !fps.isValid() || frameSize.isEmpty()
        || clip.duration < 1)
        return false;
    const auto table = efftrack::decode(fx.params.staticValue(fxkeys::TrackData).toString());
    const int stride = table ? table->stride() : 0;
    if (!table || (stride != matchmove::kPointStride && stride != matchmove::kRegionStride))
        return false;
    if (o.points && !mask.isPathShape())
        return false;
    const auto secondOf = [&](int64_t local) {
        return double(clip.sourceTimeUsAt(clip.start + local, fps)) / 1e6;
    };
    std::array<float, 8> ref {};
    table->at(secondOf(offsetFrame), ref.data());
    const auto motionAt = [&](int64_t local) {
        std::array<float, 8> cur {};
        table->at(secondOf(local), cur.data());
        return matchmove::motionBetween(ref.data(), cur.data(), stride, frameSize);
    };
    std::vector<int64_t> frames;
    for (int64_t t = 0; t < clip.duration; ++t)
        frames.push_back(t);
    const QPointF half(frameSize.width() / 2, frameSize.height() / 2);
    // Mask-local -> picture pixels: the clip centre, the mask position, rotation and scale.
    const double px = mask.value(maskkeys::PosX, offsetFrame);
    const double py = mask.value(maskkeys::PosY, offsetFrame);
    const double sx = mask.value(maskkeys::ScaleX, offsetFrame);
    const double sy = mask.value(maskkeys::ScaleY, offsetFrame);
    const double rot = mask.value(maskkeys::Rotation, offsetFrame);
    const QPointF c0 = half + QPointF(px, py);

    if (o.points) {
        QTransform local;
        local.translate(c0.x(), c0.y());
        local.rotate(rot);
        local.scale(sx, sy);
        bool invertible = false;
        const QTransform back = local.inverted(&invertible);
        if (!invertible)
            return false;
        const MaskPath base = mask.path.at(double(offsetFrame));
        mask.path = pathKeys(frames, [&](int64_t f) {
            return mapped(base, local * motionTransform(motionAt(f), o, c0) * back);
        });
        return true;
    }
    if (o.position) {
        mask.params.animated(maskkeys::PosX, px) = keysAt(frames, [&](int64_t f) {
            return motionTransform(motionAt(f), o, c0).map(c0).x() - half.x();
        });
        mask.params.animated(maskkeys::PosY, py) = keysAt(frames, [&](int64_t f) {
            return motionTransform(motionAt(f), o, c0).map(c0).y() - half.y();
        });
    }
    if (o.size) {
        const auto scaleKeys = [&](const QString& key, double v0) {
            mask.params.animated(key, v0) = keysAt(
                frames, [&](int64_t f) { return v0 * std::max(motionAt(f).scale, 1e-6); });
        };
        scaleKeys(maskkeys::ScaleX, sx);
        scaleKeys(maskkeys::ScaleY, sy);
    }
    if (o.angle) {
        mask.params.animated(maskkeys::Rotation, rot) = keysAt(
            frames, [&](int64_t f) { return rot + motionAt(f).angleDeg; });
    }
    return true;
}

} // namespace kk::tracking
