// SPDX-License-Identifier: GPL-3.0-or-later
// TextMesh3D implementation. Walking the outline with a bevel profile and a miter vector per
// vertex follows the idea of three.js ExtrudeGeometry (MIT) and Blender's curve extrude / bevel
// (GPL); the triangulation of the caps is mapbox/earcut.hpp (ISC). No code of the first two was
// copied.
#include "render/TextMesh3D.h"

#include <mapbox/earcut.hpp>

#include <QPolygonF>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace kk::render::scene3d {

namespace {

using P2 = QVector2D;

// Qt's normalized() returns a zero vector for anything shorter than about 0.003, which is the
// length of one flattened piece of a letter outline in scene units: these do not.
P2 unit(const P2& v)
{
    const double l = std::sqrt(double(v.x()) * double(v.x()) + double(v.y()) * double(v.y()));
    return l > 0 ? P2(float(v.x() / l), float(v.y() / l)) : P2(0, 0);
}

QVector3D unit3(const QVector3D& v)
{
    const double l = std::sqrt(double(v.x()) * double(v.x()) + double(v.y()) * double(v.y())
        + double(v.z()) * double(v.z()));
    return l > 0 ? QVector3D(float(v.x() / l), float(v.y() / l), float(v.z() / l)) : QVector3D();
}

struct Contour
{
    std::vector<P2> pts;
    double area = 0; ///< signed, positive = counter-clockwise (y up)
    int depth = 0; ///< how many larger contours enclose it
    int parent = -1; ///< the contour of depth - 1 enclosing it (holes)
    std::vector<P2> normal; ///< outward normal of edge i -> i+1
    std::vector<P2> inset; ///< inward offset vector of vertex i per unit of width
};

double signedArea(const std::vector<P2>& p)
{
    double a = 0;
    for (size_t i = 0, n = p.size(); i < n; ++i) {
        const P2& u = p[i];
        const P2& v = p[(i + 1) % n];
        a += double(u.x()) * double(v.y()) - double(v.x()) * double(u.y());
    }
    return a / 2;
}

QPolygonF toPolygon(const std::vector<P2>& p)
{
    QPolygonF poly;
    poly.reserve(qsizetype(p.size()));
    for (const P2& v : p)
        poly << QPointF(double(v.x()), double(v.y()));
    return poly;
}

void computeEdges(Contour& c)
{
    const size_t n = c.pts.size();
    c.normal.resize(n);
    c.inset.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const P2 d = c.pts[(i + 1) % n] - c.pts[i];
        // Right of the travel direction: outward for a counter-clockwise letter outline and for a
        // clockwise hole alike.
        c.normal[i] = unit(P2(d.y(), -d.x()));
    }
    for (size_t i = 0; i < n; ++i) {
        const P2& a = c.normal[(i + n - 1) % n];
        const P2& b = c.normal[i];
        const float d = std::max(1.0f + P2::dotProduct(a, b), 0.25f);
        c.inset[i] = -(a + b) / d; // length 1 / cos(half the turn) for unit normals
    }
}

std::vector<P2> insetPoints(const Contour& c, double w)
{
    std::vector<P2> r(c.pts.size());
    for (size_t i = 0; i < r.size(); ++i)
        r[i] = c.pts[i] + c.inset[i] * float(w);
    return r;
}

/// True if two edges of the closed polygon @p p cross each other.
bool selfIntersects(const std::vector<P2>& p)
{
    const size_t n = p.size();
    const auto side = [](const P2& a, const P2& b, const P2& c) {
        return double(b.x() - a.x()) * double(c.y() - a.y())
            - double(b.y() - a.y()) * double(c.x() - a.x());
    };
    for (size_t i = 0; i < n; ++i) {
        const P2& a = p[i];
        const P2& b = p[(i + 1) % n];
        for (size_t j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1)
                continue; // neighbours
            const P2& c = p[j];
            const P2& d = p[(j + 1) % n];
            if (std::max(a.x(), b.x()) < std::min(c.x(), d.x())
                || std::max(c.x(), d.x()) < std::min(a.x(), b.x())
                || std::max(a.y(), b.y()) < std::min(c.y(), d.y())
                || std::max(c.y(), d.y()) < std::min(a.y(), b.y()))
                continue;
            if (side(a, b, c) * side(a, b, d) < 0 && side(c, d, a) * side(c, d, b) < 0)
                return true;
        }
    }
    return false;
}

/// True if insetting every contour by @p w keeps the letters sane: letters shrink without turning
/// inside out, holes grow, and a hole stays inside its letter.
bool insetFits(const std::vector<Contour>& cs, double w)
{
    std::vector<std::vector<P2>> in(cs.size());
    for (size_t i = 0; i < cs.size(); ++i) {
        in[i] = insetPoints(cs[i], w);
        const double a = signedArea(in[i]);
        const double o = cs[i].area;
        if (a * o <= 0 || selfIntersects(in[i]))
            return false;
        // No edge may turn round or vanish: a thin stroke inset past its middle has that.
        const size_t n = in[i].size();
        for (size_t e = 0; e < n; ++e) {
            const P2 now = in[i][(e + 1) % n] - in[i][e];
            const P2 was = cs[i].pts[(e + 1) % n] - cs[i].pts[e];
            if (P2::dotProduct(now, was) <= 1e-12f * was.lengthSquared() + 1e-14f)
                return false;
        }
        if (cs[i].depth % 2 == 0 ? std::abs(a) >= std::abs(o) : std::abs(a) <= std::abs(o))
            return false;
    }
    for (size_t i = 0; i < cs.size(); ++i) {
        if (cs[i].parent < 0)
            continue;
        const QPolygonF outer = toPolygon(in[size_t(cs[i].parent)]);
        for (const P2& v : in[i])
            if (!outer.containsPoint(QPointF(double(v.x()), double(v.y())), Qt::OddEvenFill))
                return false;
    }
    return true;
}

struct Sink
{
    Mesh& m;
    quint32 add(const QVector3D& p, const QVector3D& n)
    {
        m.positions << p;
        m.normals << n;
        m.uvs << QVector2D(0, 0);
        return quint32(m.positions.size() - 1);
    }
};

/// One piece of the profile along the thickness: from (inset0, z0) to (inset1, z1) with the
/// surface normals (outward part, z part) at both ends.
struct Strip
{
    float inset0, z0, inset1, z1;
    float nOut0, nZ0, nOut1, nZ1;
    bool isSide;
};

std::vector<Strip> makeProfile(const TextExtrusion& o, double w, double bd)
{
    const float half = float(o.depth / 2);
    std::vector<Strip> s;
    if (w <= 0 || bd <= 0) {
        s.push_back({0, -half, 0, half, 1, 0, 1, 0, true});
        return s;
    }
    const float zs = half - float(bd);
    struct Pt
    {
        float inset, z, nOut, nZ;
    };
    const bool flat = o.style == BevelStyle::Flat;
    std::vector<Pt> f; // front half: from the wall (0, zs) to the cap (w, half)
    if (flat) {
        const QVector2D n = unit(QVector2D(float(bd), float(w)));
        f.push_back({0, zs, n.x(), n.y()});
        f.push_back({float(w), half, n.x(), n.y()});
    } else {
        const int n = std::clamp(o.segments, 1, 16);
        for (int k = 0; k <= n; ++k) {
            const double a = 1.5707963267948966 * k / n;
            const QVector2D nn
                = unit(QVector2D(float(bd * std::cos(a)), float(w * std::sin(a))));
            // The ends are exact so the bevel meets the wall and the cap without a crack.
            f.push_back({k == n ? float(w) : float(w * (1 - std::cos(a))),
                k == n ? half : k == 0 ? zs : zs + float(bd * std::sin(a)), nn.x(), nn.y()});
        }
    }
    // The back half mirrored (from the back cap to the wall), the wall, then the front half.
    const size_t n = f.size();
    for (size_t k = 0; k + 1 < n; ++k) {
        const Pt& a = f[n - 1 - k];
        const Pt& b = f[n - 2 - k];
        s.push_back({a.inset, -a.z, b.inset, -b.z, a.nOut, -a.nZ, flat ? a.nOut : b.nOut,
            flat ? -a.nZ : -b.nZ, false});
    }
    if (zs > 1e-9f) // no wall when the two bevels meet
        s.push_back({0, -zs, 0, zs, 1, 0, 1, 0, true});
    for (size_t k = 0; k + 1 < n; ++k)
        s.push_back({f[k].inset, f[k].z, f[k + 1].inset, f[k + 1].z, f[k].nOut, f[k].nZ,
            flat ? f[k].nOut : f[k + 1].nOut, flat ? f[k].nZ : f[k + 1].nZ, false});
    return s;
}

} // namespace

ExtrudedText extrudeText(
    const QPainterPath& outline, const QTransform& toScene, const TextExtrusion& options)
{
    ExtrudedText result;
    if (options.depth <= 0)
        return result;

    // ---- Outlines -> contours ----
    std::vector<Contour> cs;
    for (const QPolygonF& poly : outline.simplified().toSubpathPolygons()) {
        Contour c;
        for (const QPointF& q : poly) {
            const QPointF m = toScene.map(q);
            const P2 v(float(m.x()), float(m.y()));
            if (c.pts.empty() || (v - c.pts.back()).lengthSquared() > 1e-14f)
                c.pts.push_back(v);
        }
        while (c.pts.size() > 1 && (c.pts.back() - c.pts.front()).lengthSquared() <= 1e-14f)
            c.pts.pop_back();
        // Points on a straight line carry nothing, and the triangulator drops them from the caps:
        // the walls must not keep them or the surface would have a seam there.
        for (size_t i = 0; i < c.pts.size() && c.pts.size() > 3;) {
            const P2 a = c.pts[(i + c.pts.size() - 1) % c.pts.size()], b = c.pts[i],
                     d = c.pts[(i + 1) % c.pts.size()];
            const P2 u = b - a, v = d - b;
            const double cross = double(u.x()) * double(v.y()) - double(u.y()) * double(v.x());
            const double len = std::sqrt(double(u.lengthSquared()) * double(v.lengthSquared()));
            if (len > 0 && std::abs(cross) < 1e-4 * len && P2::dotProduct(u, v) > 0)
                c.pts.erase(c.pts.begin() + std::ptrdiff_t(i));
            else
                ++i;
        }
        if (c.pts.size() < 3)
            continue;
        c.area = signedArea(c.pts);
        if (std::abs(c.area) < 1e-12)
            continue;
        cs.push_back(std::move(c));
    }
    if (cs.empty())
        return result;
    // Nesting: an even depth is a letter, an odd one a hole in it.
    std::vector<QPolygonF> polys;
    for (const Contour& c : cs)
        polys.push_back(toPolygon(c.pts));
    const auto probeOf = [&](size_t i) {
        return QPointF(double(cs[i].pts[0].x()), double(cs[i].pts[0].y()));
    };
    for (size_t i = 0; i < cs.size(); ++i)
        for (size_t j = 0; j < cs.size(); ++j)
            if (i != j && std::abs(cs[j].area) > std::abs(cs[i].area)
                && polys[j].containsPoint(probeOf(i), Qt::OddEvenFill))
                ++cs[i].depth;
    for (size_t i = 0; i < cs.size(); ++i) {
        if (cs[i].depth % 2 == 0)
            continue;
        double best = 1e300;
        for (size_t j = 0; j < cs.size(); ++j)
            if (cs[j].depth == cs[i].depth - 1 && std::abs(cs[j].area) > std::abs(cs[i].area)
                && std::abs(cs[j].area) < best && polys[j].containsPoint(probeOf(i), Qt::OddEvenFill)) {
                best = std::abs(cs[j].area);
                cs[i].parent = int(j);
            }
    }
    for (Contour& c : cs) {
        if ((c.area > 0) != (c.depth % 2 == 0)) {
            std::reverse(c.pts.begin(), c.pts.end());
            c.area = -c.area;
        }
        computeEdges(c);
    }

    // ---- Bevel size ----
    const double bd = std::clamp(options.bevelDepth, 0.0, options.depth / 2);
    double w = bd > 0 ? std::max(0.0, options.bevelWidth) : 0.0;
    for (int tries = 0; w > 1e-7 && !insetFits(cs, w * 1.25); ++tries) // with a margin: no hair-thin caps
        w = tries >= 40 ? 0 : w * 0.85;
    if (w < 1e-7)
        w = 0;
    result.bevelWidthUsed = w;
    const std::vector<Strip> profile = makeProfile(options, w, w > 0 ? bd : 0);
    const float half = float(options.depth / 2);

    // ---- Walls and bevel rings ----
    const float cornerCos = 0.82f; // about 35 degrees: sharper corners keep a hard edge
    Sink sideSink {result.side}, bevelSink {result.bevel};
    for (const Contour& c : cs) {
        const size_t n = c.pts.size();
        for (size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            const P2& ne = c.normal[i];
            const P2& np = c.normal[(i + n - 1) % n];
            const P2& nn = c.normal[j];
            const P2 n0 = P2::dotProduct(ne, np) > cornerCos ? unit(ne + np) : ne;
            const P2 n1 = P2::dotProduct(ne, nn) > cornerCos ? unit(ne + nn) : ne;
            for (const Strip& s : profile) {
                Sink& sink = s.isSide ? sideSink : bevelSink;
                const auto vert = [&](size_t v, float inset, float z, float nOut, float nZ, const P2& nv) {
                    const P2 p = c.pts[v] + c.inset[v] * inset;
                    return sink.add(QVector3D(p.x(), p.y(), z),
                        unit3(QVector3D(nv.x() * nOut, nv.y() * nOut, nZ)));
                };
                const quint32 a = vert(i, s.inset0, s.z0, s.nOut0, s.nZ0, n0);
                const quint32 b = vert(j, s.inset0, s.z0, s.nOut0, s.nZ0, n1);
                const quint32 cc = vert(j, s.inset1, s.z1, s.nOut1, s.nZ1, n1);
                const quint32 d = vert(i, s.inset1, s.z1, s.nOut1, s.nZ1, n0);
                sink.m.indices << a << b << cc << a << cc << d;
            }
        }
    }

    // ---- Caps ----
    using Pt = std::array<double, 2>;
    std::vector<std::vector<size_t>> holesOf(cs.size());
    for (size_t i = 0; i < cs.size(); ++i)
        if (cs[i].depth % 2 == 1 && cs[i].parent >= 0)
            holesOf[size_t(cs[i].parent)].push_back(i);
    Sink front {result.front}, back {result.back};
    for (size_t i = 0; i < cs.size(); ++i) {
        if (cs[i].depth % 2 == 1)
            continue;
        std::vector<std::vector<Pt>> rings;
        std::vector<P2> flat;
        const auto addRing = [&](const Contour& c) {
            std::vector<Pt> ring;
            for (const P2& p : insetPoints(c, w)) {
                ring.push_back({double(p.x()), double(p.y())});
                flat.push_back(p);
            }
            rings.push_back(std::move(ring));
        };
        addRing(cs[i]);
        for (size_t h : holesOf[i])
            addRing(cs[h]);
        const std::vector<uint32_t> tri = mapbox::earcut<uint32_t>(rings);
        const quint32 base = quint32(result.front.positions.size());
        for (const P2& p : flat) {
            front.add(QVector3D(p.x(), p.y(), half), QVector3D(0, 0, 1));
            back.add(QVector3D(p.x(), p.y(), -half), QVector3D(0, 0, -1));
        }
        for (size_t t = 0; t + 2 < tri.size(); t += 3) {
            quint32 a = tri[t], b = tri[t + 1], c = tri[t + 2];
            const P2 u = flat[b] - flat[a], v = flat[c] - flat[a];
            if (u.x() * v.y() - u.y() * v.x() < 0)
                std::swap(b, c); // counter-clockwise seen from +Z
            result.front.indices << base + a << base + b << base + c;
            result.back.indices << base + a << base + c << base + b;
        }
    }
    return result;
}

} // namespace kk::render::scene3d
