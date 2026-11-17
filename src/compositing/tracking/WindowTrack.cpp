// SPDX-License-Identifier: GPL-3.0-or-later
// WindowTrack implementation. See the header.
#include "compositing/tracking/WindowTrack.h"

#include <QObject>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::tracking {

namespace {

struct P
{
    double x = 0, y = 0;
};

/// The corner @p i (top-left, top-right, bottom-right, bottom-left) of a TrackSample / job
/// corner array as a point in picture-height units (x * aspect, y down).
P corner(const std::array<double, 8>& c, size_t i, double aspect)
{
    return {c[2 * i] * aspect, 1.0 - c[2 * i + 1]};
}

P mid(P a, P b)
{
    return {(a.x + b.x) / 2, (a.y + b.y) / 2};
}

P sub(P a, P b)
{
    return {a.x - b.x, a.y - b.y};
}

double len(P a)
{
    return std::hypot(a.x, a.y);
}

} // namespace

std::optional<WindowTrackPlan> planWindowTrack(const GradeNode& node, const GradeWindow& w,
    int64_t startFrame, int64_t endFrame, double aspect, QString* why)
{
    auto fail = [&](const QString& text) -> std::optional<WindowTrackPlan> {
        if (why)
            *why = text;
        return std::nullopt;
    };
    if (w.shape == GradeWindowShape::Gradient)
        return fail(QObject::tr("a gradient window has no subject to follow"));
    if (aspect <= 0.0)
        return fail(QObject::tr("the picture has no size"));
    WindowTrackPlan plan;
    plan.aspect = aspect;
    plan.hadData = node.track.hasData();
    plan.base = plan.hadData ? node.track.at(startFrame) : GradeTrackSample {};
    plan.start = grademath::trackedWindowGeometry(node, w, startFrame, aspect);
    const grademath::WindowGeometry& g = plan.start;
    const double width = std::abs(g.width), height = std::abs(g.height);
    if (width < 0.01 || height < 0.01)
        return fail(QObject::tr("the window is too small"));
    if (g.cx < 0 || g.cx > 1 || g.cy < 0 || g.cy > 1)
        return fail(QObject::tr("the window is outside the picture"));
    plan.pivot = plan.hadData ? node.track.pivot : QPointF(g.cx, g.cy);
    plan.planar = node.track.zoom || node.track.rotate || node.track.perspective;

    TrackJob& job = plan.job;
    job.firstFrame = startFrame;
    job.lastFrame = endFrame;
    job.startU = g.cx;
    job.startV = 1.0 - g.cy;
    const double smaller = std::min(width, height / aspect); // fraction of the picture width
    if (plan.planar) {
        // The window's box (turned like the window) is the region; patches are a quarter of its
        // smaller side.
        const double a = g.rotation * std::numbers::pi / 180.0;
        const double cs = std::cos(a), sn = std::sin(a);
        const double hx = width * aspect / 2, hy = height / 2; // picture-height units
        const double local[4][2] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
        for (size_t i = 0; i < 4; ++i) {
            const double x = cs * local[i][0] - sn * local[i][1];
            const double y = sn * local[i][0] + cs * local[i][1];
            job.startCorners[2 * i] = std::clamp(g.cx + x / aspect, 0.005, 0.995);
            job.startCorners[2 * i + 1] = std::clamp(1.0 - (g.cy + y), 0.005, 0.995);
        }
        job.planar = true;
        job.patternFraction = std::clamp(smaller / 4.0, 0.02, 0.15);
        job.searchFraction = 0.2;
    } else {
        job.patternFraction = std::clamp(smaller * 0.6, 0.03, 0.25);
        job.searchFraction = std::clamp(job.patternFraction * 3.0, 0.12, 0.4);
    }
    return plan;
}

GradeTrackSample absoluteSample(const WindowTrackPlan& plan, const TrackSample& s)
{
    GradeTrackSample rel;
    if (plan.job.planar && s.planar) {
        const double a = plan.aspect;
        const std::array<double, 8>& c0 = plan.job.startCorners;
        const std::array<double, 8>& c1 = s.corners;
        const P top0 = sub(corner(c0, 1, a), corner(c0, 0, a));
        const P bottom0 = sub(corner(c0, 2, a), corner(c0, 3, a));
        const P left0 = sub(corner(c0, 3, a), corner(c0, 0, a));
        const P right0 = sub(corner(c0, 2, a), corner(c0, 1, a));
        const P top1 = sub(corner(c1, 1, a), corner(c1, 0, a));
        const P bottom1 = sub(corner(c1, 2, a), corner(c1, 3, a));
        const P left1 = sub(corner(c1, 3, a), corner(c1, 0, a));
        const P right1 = sub(corner(c1, 2, a), corner(c1, 1, a));
        const P w0 = mid(top0, bottom0), w1 = mid(top1, bottom1);
        const P h0 = mid(left0, right0), h1 = mid(left1, right1);
        const P centre0 = mid(mid(corner(c0, 0, a), corner(c0, 2, a)),
            mid(corner(c0, 1, a), corner(c0, 3, a)));
        const P centre1 = mid(mid(corner(c1, 0, a), corner(c1, 2, a)),
            mid(corner(c1, 1, a), corner(c1, 3, a)));
        rel.dx = (centre1.x - centre0.x) / a;
        rel.dy = centre1.y - centre0.y;
        if (len(w0) > 1e-9)
            rel.sx = std::clamp(len(w1) / len(w0), 0.05, 20.0);
        if (len(h0) > 1e-9)
            rel.sy = std::clamp(len(h1) / len(h0), 0.05, 20.0);
        rel.rot = (std::atan2(w1.y, w1.x) - std::atan2(w0.y, w0.x)) * 180.0 / std::numbers::pi;
        while (rel.rot > 180.0)
            rel.rot -= 360.0;
        while (rel.rot < -180.0)
            rel.rot += 360.0;
    } else {
        rel.dx = s.u - plan.job.startU;
        rel.dy = -(s.v - plan.job.startV);
    }
    GradeTrackSample out;
    out.dx = plan.base.dx + rel.dx;
    out.dy = plan.base.dy + rel.dy;
    out.sx = plan.base.sx * rel.sx;
    out.sy = plan.base.sy * rel.sy;
    out.rot = plan.base.rot + rel.rot;
    return out;
}

void mergeSamples(
    GradeTrack& track, const WindowTrackPlan& plan, const std::vector<TrackSample>& samples)
{
    if (samples.empty())
        return;
    int64_t lo = samples.front().frame, hi = lo;
    for (const TrackSample& s : samples) {
        lo = std::min(lo, s.frame);
        hi = std::max(hi, s.frame);
    }
    const bool had = track.hasData();
    if (!had)
        track.pivot = plan.pivot;
    const int64_t first = had ? std::min(lo, track.firstFrame) : lo;
    const int64_t last = had ? std::max(hi, track.lastFrame()) : hi;
    std::vector<const TrackSample*> byFrame(size_t(hi - lo + 1), nullptr);
    for (const TrackSample& s : samples)
        byFrame[size_t(s.frame - lo)] = &s;
    QVector<GradeTrackSample> merged;
    merged.reserve(int(last - first + 1));
    for (int64_t f = first; f <= last; ++f) {
        const TrackSample* found = f >= lo && f <= hi ? byFrame[size_t(f - lo)] : nullptr;
        merged.append(found ? absoluteSample(plan, *found) : track.at(f));
    }
    track.firstFrame = first;
    track.samples = std::move(merged);
}

} // namespace kk::tracking
