// SPDX-License-Identifier: GPL-3.0-or-later
// Extruded, bevelled 3D text (1.13): the mesh built from glyph outlines (closed, outward normals,
// depth and bevel change the bounds, holes of letters), the Text3D parameters in a saved project
// and in the scene.
#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "core/Serialization.h"
#include "render/CompScene3D.h"
#include "render/TextMesh3D.h"

#include <QFont>
#include <QTest>

#include <cmath>
#include <map>
#include <tuple>

using namespace kk;
using namespace kk::render::scene3d;
namespace co = kk::compops;
namespace k = kk::nodekeys;

namespace {

/// A square of side 1 with a square hole of side 0.5, in a y-up plane.
QPainterPath ringPath()
{
    QPainterPath p;
    p.setFillRule(Qt::OddEvenFill);
    p.addRect(0, 0, 1, 1);
    p.addRect(0.25, 0.25, 0.5, 0.5);
    return p;
}

QPainterPath textPath(const QString& text)
{
    QPainterPath p;
    p.setFillRule(Qt::WindingFill);
    QFont f(QStringLiteral("Sans Serif"));
    f.setPixelSize(100);
    p.addText(0, 100, f, text);
    return p;
}

/// The letter H as a polygon, 1 wide and 1 high, bars 0.3 thick.
QPainterPath hShape()
{
    QPainterPath p;
    p.addPolygon(QPolygonF({{0, 0}, {0.3, 0}, {0.3, 0.35}, {0.7, 0.35}, {0.7, 0}, {1, 0}, {1, 1},
        {0.7, 1}, {0.7, 0.65}, {0.3, 0.65}, {0.3, 1}, {0, 1}}));
    p.closeSubpath();
    return p;
}

QTransform pixelsToScene()
{
    QTransform t;
    t.scale(1.0 / 100, -1.0 / 100);
    return t;
}

QVector<const Mesh*> parts(const ExtrudedText& t)
{
    return {&t.front, &t.back, &t.side, &t.bevel};
}

struct Bounds
{
    QVector3D lo {1e9f, 1e9f, 1e9f}, hi {-1e9f, -1e9f, -1e9f};
};

Bounds boundsOf(const Mesh& m)
{
    Bounds b;
    for (const QVector3D& p : m.positions)
        for (int i = 0; i < 3; ++i) {
            b.lo[i] = std::min(b.lo[i], p[i]);
            b.hi[i] = std::max(b.hi[i], p[i]);
        }
    return b;
}

Bounds boundsOf(const ExtrudedText& t)
{
    Bounds b;
    for (const Mesh* m : parts(t)) {
        const Bounds x = boundsOf(*m);
        for (int i = 0; i < 3; ++i) {
            b.lo[i] = std::min(b.lo[i], x.lo[i]);
            b.hi[i] = std::max(b.hi[i], x.hi[i]);
        }
    }
    return b;
}

using Key = std::tuple<long, long, long>;

Key keyOf(const QVector3D& p)
{
    return {std::lround(p.x() * 1e4), std::lround(p.y() * 1e4), std::lround(p.z() * 1e4)};
}

/// Every undirected edge of the whole surface is used by exactly two triangles.
bool isClosed(const ExtrudedText& t, QString* why = nullptr)
{
    std::map<std::pair<Key, Key>, int> edges;
    for (const Mesh* m : parts(t))
        for (int i = 0; i + 2 < m->indices.size(); i += 3)
            for (int e = 0; e < 3; ++e) {
                Key a = keyOf(m->positions[int(m->indices[i + e])]);
                Key b = keyOf(m->positions[int(m->indices[i + (e + 1) % 3])]);
                if (a == b)
                    continue;
                if (b < a)
                    std::swap(a, b);
                ++edges[{a, b}];
            }
    for (const auto& [e, n] : edges)
        if (n != 2) {
            if (why)
                *why = QStringLiteral("an edge is used %1 times").arg(n);
            return false;
        }
    return !edges.empty();
}

double volumeOf(const ExtrudedText& t)
{
    double v = 0;
    for (const Mesh* m : parts(t))
        for (int i = 0; i + 2 < m->indices.size(); i += 3) {
            const QVector3D a = m->positions[int(m->indices[i])];
            const QVector3D b = m->positions[int(m->indices[i + 1])];
            const QVector3D c = m->positions[int(m->indices[i + 2])];
            v += double(QVector3D::dotProduct(a, QVector3D::crossProduct(b, c))) / 6;
        }
    return v;
}

double areaOf(const Mesh& m)
{
    double a = 0;
    for (int i = 0; i + 2 < m.indices.size(); i += 3) {
        const QVector3D p = m.positions[int(m.indices[i])];
        a += double(QVector3D::crossProduct(m.positions[int(m.indices[i + 1])] - p,
                                               m.positions[int(m.indices[i + 2])] - p)
                        .length())
            / 2;
    }
    return a;
}

/// Every vertex normal is a unit vector on the side of the triangle's own (winding) normal.
bool normalsFaceOut(const ExtrudedText& t, QString* why = nullptr)
{
    int part = 0;
    for (const Mesh* m : parts(t)) {
        ++part;
        if (m->positions.size() != m->normals.size())
            return false;
        for (int i = 0; i + 2 < m->indices.size(); i += 3) {
            const QVector3D p = m->positions[int(m->indices[i])];
            const QVector3D g = QVector3D::crossProduct(m->positions[int(m->indices[i + 1])] - p,
                m->positions[int(m->indices[i + 2])] - p);
            if (g.length() < 1e-8f)
                continue;
            for (int c = 0; c < 3; ++c) {
                const QVector3D n = m->normals[int(m->indices[i + c])];
                const float d = QVector3D::dotProduct(g / g.length(), n);
                if (std::abs(n.length() - 1) > 1e-3f || d < 0.2f) {
                    if (why)
                        *why = QStringLiteral("part %1 triangle %2 corner %3 dot %4")
                                   .arg(part).arg(i / 3).arg(c).arg(double(d));
                    return false;
                }
            }
        }
    }
    return true;
}

TextExtrusion extrusion(double depth, double bevelDepth, double bevelWidth,
    BevelStyle style = BevelStyle::Smooth)
{
    TextExtrusion e;
    e.depth = depth;
    e.bevelDepth = bevelDepth;
    e.bevelWidth = bevelWidth;
    e.style = style;
    return e;
}

} // namespace

class TestTextMesh3D : public QObject
{
    Q_OBJECT

private slots:
    void flatTextMakesNoMesh()
    {
        QVERIFY(extrudeText(ringPath(), QTransform(), extrusion(0, 0, 0)).isEmpty());
    }

    void holeIsLeftOpen()
    {
        const ExtrudedText t = extrudeText(ringPath(), QTransform(), extrusion(0.2, 0, 0));
        QVERIFY(!t.isEmpty());
        QVERIFY(std::abs(areaOf(t.front) - 0.75) < 1e-5);
        QVERIFY(std::abs(areaOf(t.back) - 0.75) < 1e-5);
        // The walls: 2 x (outer 4 + inner 2) sides of length 1 / 0.5, 0.2 high.
        QVERIFY(std::abs(areaOf(t.side) - 0.2 * (4.0 + 2.0)) < 1e-5);
        QString why;
        QVERIFY2(isClosed(t, &why), qPrintable(why));
        QVERIFY(volumeOf(t) > 0);
        QVERIFY(std::abs(volumeOf(t) - 0.75 * 0.2) < 1e-5);
        QVERIFY2(normalsFaceOut(t, &why), qPrintable(why));
    }

    void bevelledSquareIsClosedWithRightVolume()
    {
        QPainterPath p;
        p.addRect(0, 0, 1, 1);
        for (const BevelStyle style : {BevelStyle::Flat, BevelStyle::Smooth}) {
            const ExtrudedText t
                = extrudeText(p, QTransform(), extrusion(0.4, 0.1, 0.1, style));
            QString why;
            QVERIFY2(isClosed(t, &why), qPrintable(why));
            QVERIFY2(normalsFaceOut(t, &why), qPrintable(why));
            QVERIFY(!t.bevel.positions.isEmpty());
            // A flat chamfer takes a quarter of the 0.1 x 0.1 corner prism away per edge; a round
            // one a little less.
            const double full = 0.4;
            QVERIFY(volumeOf(t) < full);
            QVERIFY(volumeOf(t) > full - 0.1);
            QVERIFY(std::abs(t.bevelWidthUsed - 0.1) < 1e-6);
        }
    }

    void lettersWithHolesAreClosedAndFaceOut()
    {
        for (const QString& text : {QStringLiteral("O"), QStringLiteral("A"), QStringLiteral("B"),
                 QStringLiteral("8Q%")}) {
            const QPainterPath path = textPath(text);
            if (path.isEmpty())
                QSKIP("no font that can draw the test letters");
            for (const double bevel : {0.0, 0.03}) {
                for (const BevelStyle style : {BevelStyle::Smooth, BevelStyle::Flat}) {
                    const ExtrudedText t = extrudeText(path, pixelsToScene(),
                        extrusion(0.2, bevel, bevel, style));
                    QVERIFY(!t.isEmpty());
                    QString why;
                    QVERIFY2(isClosed(t, &why),
                        qPrintable(text + QStringLiteral(" bevel %1: ").arg(bevel) + why));
                    QVERIFY2(normalsFaceOut(t, &why), qPrintable(text + why));
                    QVERIFY(volumeOf(t) > 0);
                }
            }
        }
    }

    void holeOfOReducesTheCapArea()
    {
        const QPainterPath path = textPath(QStringLiteral("O"));
        if (path.isEmpty())
            QSKIP("no font that can draw the test letters");
        const ExtrudedText t = extrudeText(path, pixelsToScene(), extrusion(0.1, 0, 0));
        const Bounds b = boundsOf(t.front);
        const double box = double(b.hi.x() - b.lo.x()) * double(b.hi.y() - b.lo.y());
        const double cap = areaOf(t.front);
        // A filled ellipse takes pi / 4 = 0.785 of its box; a ring (the O of a normal font) much less.
        QVERIFY2(cap < 0.7 * box, qPrintable(QString::number(cap / box)));
        QVERIFY(cap > 0.2 * box);
    }

    void depthSetsTheThickness()
    {
        const QPainterPath path = textPath(QStringLiteral("H"));
        if (path.isEmpty())
            QSKIP("no font that can draw the test letters");
        for (const double depth : {0.05, 0.2, 0.6}) {
            const Bounds b = boundsOf(extrudeText(path, pixelsToScene(), extrusion(depth, 0, 0)));
            QVERIFY(std::abs(b.hi.z() - float(depth / 2)) < 1e-6f);
            QVERIFY(std::abs(b.lo.z() + float(depth / 2)) < 1e-6f);
        }
    }

    void bevelPullsTheCapsInButKeepsTheOutline()
    {
        const QPainterPath path = hShape();
        const ExtrudedText plain = extrudeText(path, QTransform(), extrusion(0.2, 0, 0));
        const ExtrudedText bev = extrudeText(path, QTransform(), extrusion(0.2, 0.05, 0.02));
        const Bounds bp = boundsOf(plain), bb = boundsOf(bev);
        // The outline (the widest and tallest part) is the same, the depth too.
        for (int i = 0; i < 3; ++i) {
            QVERIFY(std::abs(bp.lo[i] - bb.lo[i]) < 1e-5f);
            QVERIFY(std::abs(bp.hi[i] - bb.hi[i]) < 1e-5f);
        }
        // The front face is smaller: by the bevel width on every side.
        const Bounds fp = boundsOf(plain.front), fb = boundsOf(bev.front);
        QVERIFY(std::abs(fb.lo.x() - (fp.lo.x() + 0.02f)) < 1e-4f);
        QVERIFY(std::abs(fb.hi.x() - (fp.hi.x() - 0.02f)) < 1e-4f);
        QVERIFY(areaOf(bev.front) < areaOf(plain.front));
        QVERIFY(std::abs(bev.bevelWidthUsed - 0.02) < 1e-6);
        // A wider bevel pulls it in further.
        const ExtrudedText wide = extrudeText(path, QTransform(), extrusion(0.2, 0.05, 0.04));
        QVERIFY(areaOf(wide.front) < areaOf(bev.front));
        // A deeper bevel leaves a shorter wall.
        const ExtrudedText deep = extrudeText(path, QTransform(), extrusion(0.2, 0.09, 0.02));
        QVERIFY(areaOf(deep.side) < areaOf(bev.side));
        QString why;
        QVERIFY2(isClosed(bev, &why), qPrintable(why));
        QVERIFY2(isClosed(deep, &why), qPrintable(why));
    }

    void bevelDepthIsLimitedToHalfTheThickness()
    {
        QPainterPath p;
        p.addRect(0, 0, 1, 1);
        const ExtrudedText t = extrudeText(p, QTransform(), extrusion(0.2, 5, 0.05));
        QString why;
        QVERIFY2(isClosed(t, &why), qPrintable(why));
        QVERIFY(areaOf(t.side) < 1e-6); // the wall is gone: the two bevels meet
    }

    void tooWideABevelShrinksToFitThinStrokes()
    {
        // Two bars 0.04 thick: a bevel of 0.5 cannot be wider than half of that.
        QPainterPath p;
        p.addRect(0, 0, 0.04, 1);
        p.addRect(0.3, 0, 0.04, 1);
        const ExtrudedText t = extrudeText(p, QTransform(), extrusion(0.2, 0.05, 0.5));
        QVERIFY(t.bevelWidthUsed > 0.005);
        QVERIFY(t.bevelWidthUsed < 0.02);
        QString why;
        QVERIFY2(isClosed(t, &why), qPrintable(why));
        QVERIFY(areaOf(t.front) > 1e-3);
        // The same with real letters, whatever the font draws.
        const QPainterPath letters = textPath(QStringLiteral("Il"));
        if (!letters.isEmpty()) {
            const ExtrudedText l = extrudeText(letters, pixelsToScene(), extrusion(0.2, 0.05, 1.0));
            QVERIFY2(isClosed(l, &why), qPrintable(why));
            QVERIFY(areaOf(l.front) > 1e-4);
        }
    }

    void flatBevelHasOneNormalPerFaceSmoothHasMany()
    {
        QPainterPath p;
        p.addRect(0, 0, 1, 1);
        const ExtrudedText flat
            = extrudeText(p, QTransform(), extrusion(0.4, 0.1, 0.1, BevelStyle::Flat));
        // All bevel normals of an edge of a square: 45 degrees between out and +-Z.
        for (const QVector3D& n : flat.bevel.normals)
            QVERIFY(std::abs(std::abs(n.z()) - std::sqrt(0.5f)) < 1e-3f);
        TextExtrusion e = extrusion(0.4, 0.1, 0.1, BevelStyle::Smooth);
        e.segments = 6;
        const ExtrudedText smooth = extrudeText(p, QTransform(), e);
        float lo = 1, hi = -1;
        for (const QVector3D& n : smooth.bevel.normals) {
            if (n.z() >= 0) {
                lo = std::min(lo, n.z());
                hi = std::max(hi, n.z());
            }
        }
        QVERIFY(lo < 0.05f && hi > 0.95f); // from the wall round to the face
    }

    void saveAndLoadKeepEveryTextParameter()
    {
        Composition c = co::defaultComposition();
        const quint64 tx = co::addNode(c, nodetypes::Text3D, {});
        ParamSet& p = c.node(tx)->params;
        p.setBase(k::ExtrudeDepth, 0.12);
        p.setBase(k::BevelDepth, 0.03);
        p.setBase(k::BevelWidth, 0.02);
        p.setBase(k::BevelSegments, 7);
        p.setStatic(k::BevelStyle, bevelstyles::Flat);
        p.setStatic(k::SeparateColors, true);
        p.setStatic(k::SideColor, QColor(10, 20, 30));
        p.setStatic(k::BevelColor, QColor(200, 100, 50));
        p.setStatic(k::BackColor, QColor(1, 2, 3));
        p.animated(k::ExtrudeDepth, 0).setKey(0, 0.0);
        p.animated(k::ExtrudeDepth, 0).setKey(20, 0.4);
        const Composition back
            = serialization::compositionFromJson(serialization::compositionToJson(c));
        const ParamSet& q = back.node(tx)->params;
        QCOMPARE(q.value(k::BevelDepth, 0, 0), 0.03);
        QCOMPARE(q.value(k::BevelWidth, 0, 0), 0.02);
        QCOMPARE(q.value(k::BevelSegments, 0, 0), 7.0);
        QCOMPARE(q.staticValue(k::BevelStyle).toString(), bevelstyles::Flat);
        QVERIFY(q.staticValue(k::SeparateColors).toBool());
        QCOMPARE(q.staticValue(k::SideColor).value<QColor>(), QColor(10, 20, 30));
        QCOMPARE(q.staticValue(k::BevelColor).value<QColor>(), QColor(200, 100, 50));
        QCOMPARE(q.staticValue(k::BackColor).value<QColor>(), QColor(1, 2, 3));
        QVERIFY(std::abs(q.value(k::ExtrudeDepth, 10, 0) - 0.2) < 0.02); // keyframed, halfway
    }

    void everyNewControlIsKeyframableOrStatic()
    {
        const NodeDescriptor* d = NodeRegistry::instance().find(nodetypes::Text3D);
        QVERIFY(d);
        for (const QString& key : {k::ExtrudeDepth, k::BevelDepth, k::BevelWidth, k::BevelSegments}) {
            const ParamDescriptor* pd = d->param(key);
            QVERIFY2(pd, qPrintable(key));
            QVERIFY2(pd->animatable, qPrintable(key));
            QVERIFY(!pd->tooltip.isEmpty());
        }
        for (const QString& key : {k::BevelStyle, k::SeparateColors, k::BevelColor, k::SideColor,
                 k::BackColor, k::Specular, k::Shininess})
            QVERIFY2(d->param(key) && !d->param(key)->tooltip.isEmpty(), qPrintable(key));
    }

    void sceneHoldsAFlatPlaneOrTheSolidParts()
    {
        Composition c = co::defaultComposition();
        const quint64 tx = co::addNode(c, nodetypes::Text3D, {});
        c.node(tx)->params.setStatic(k::Text, QStringLiteral("OA"));
        c.node(tx)->params.setBase(k::FontSize, 80);
        const QSize size(320, 180);
        Scene flat = buildScene(c, tx, 0, size, QSizeF(size), 1.0);
        if (flat.meshes.isEmpty())
            QSKIP("no font");
        QCOMPARE(flat.meshes.size(), 1); // the old flat picture
        QVERIFY(flat.meshes[0].textured);
        c.node(tx)->params.setBase(k::ExtrudeDepth, 0.1);
        Scene one = buildScene(c, tx, 0, size, QSizeF(size), 1.0);
        QCOMPARE(one.meshes.size(), 4); // front, bevel, side, back
        c.node(tx)->params.setStatic(k::SeparateColors, true);
        c.node(tx)->params.setStatic(k::SideColor, QColor(0, 255, 0));
        c.node(tx)->params.setBase(k::BevelDepth, 0);
        const Scene two = buildScene(c, tx, 0, size, QSizeF(size), 1.0);
        bool green = false;
        for (const Mesh& m : two.meshes)
            green = green || m.material.color == QVector3D(0, 1, 0);
        QVERIFY(green);
        // Keyframed depth: the mesh grows with the frame.
        c.node(tx)->params.animated(k::ExtrudeDepth, 0).setKey(0, 0.05);
        c.node(tx)->params.animated(k::ExtrudeDepth, 0).setKey(10, 0.5);
        const Scene a = buildScene(c, tx, 0, size, QSizeF(size), 1.0);
        const Scene b = buildScene(c, tx, 10, size, QSizeF(size), 1.0);
        QVERIFY(boundsOf(b.meshes[0]).hi.z() > boundsOf(a.meshes[0]).hi.z() * 5);
    }
};

QTEST_MAIN(TestTextMesh3D)
#include "test_textmesh3d.moc"
