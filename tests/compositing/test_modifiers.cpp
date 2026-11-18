// SPDX-License-Identifier: GPL-3.0-or-later
// Parameter modifiers (Perturb, Wiggle, Offset, Expression, Cycle, XY Path), Connect To, Publish
// and the expression language (spec section 119).
#include "core/CompLinks.h"
#include "core/CompOps.h"
#include "core/Expression.h"
#include "core/Modifiers.h"
#include "core/NodeRegistry.h"
#include "core/Serialization.h"

#include <QTest>

#include <cmath>

using namespace kk;
namespace co = kk::compops;

namespace {

double calc(const QString& text, double time = 0, double value = 0, QStringList* refsOut = nullptr,
    const QVector<double>& refs = {})
{
    QString error;
    const auto p = expr::Program::compile(text, &error);
    if (!p)
        return std::nan("");
    if (refsOut)
        *refsOut = p->references();
    expr::Program::Inputs in;
    in.time = time;
    in.value = value;
    in.n[0] = 3;
    in.n[1] = 4;
    in.refs = refs.isEmpty() ? nullptr : refs.constData();
    return p->evaluate(in);
}

} // namespace

class TestModifiers : public QObject
{
    Q_OBJECT
private slots:
    void expressionMathAndPrecedence()
    {
        QCOMPARE(calc(QStringLiteral("1 + 2 * 3")), 7.0);
        QCOMPARE(calc(QStringLiteral("(1 + 2) * 3")), 9.0);
        QCOMPARE(calc(QStringLiteral("2 ^ 3 ^ 2")), 512.0); // right associative
        QCOMPARE(calc(QStringLiteral("-2 ^ 2")), -4.0);
        QCOMPARE(calc(QStringLiteral("10 % 4 + 7 / 2")), 5.5);
        QCOMPARE(calc(QStringLiteral("1.5e2 + .5")), 150.5);
        QCOMPARE(calc(QStringLiteral("3 > 2 && 2 >= 2 ? 10 : 20")), 10.0);
        QCOMPARE(calc(QStringLiteral("!0 + (1 == 2) + (1 != 2)")), 2.0);
        QCOMPARE(calc(QStringLiteral("max(2, min(9, 5)) + abs(-3) + floor(2.7) + ceil(2.1) + round(2.5)")), 5.0 + 3 + 2 + 3 + 3);
        QCOMPARE(calc(QStringLiteral("clamp(15, 0, 10) + mix(0, 10, 0.25) + step(1, 2) + sign(-4)")), 10 + 2.5 + 1 - 1);
        QVERIFY(std::abs(calc(QStringLiteral("sin(pi / 2) + cos(0) + atan2(1, 1) * 4 / pi")) - 3.0) < 1e-9);
        QCOMPARE(calc(QStringLiteral("if(time > 5, 1, 2)"), 6), 1.0);
        QCOMPARE(calc(QStringLiteral("time * 2 + value + n1 * n2"), 5, 100), 122.0);
        QCOMPARE(calc(QStringLiteral("smoothstep(0, 10, 5)")), 0.5);
        QVERIFY(std::abs(calc(QStringLiteral("deg(pi)")) - 180.0) < 1e-9);
        // The same text and inputs always give the same noise.
        QCOMPARE(calc(QStringLiteral("noise(3.3) + rand(7)")), calc(QStringLiteral("noise(3.3) + rand(7)")));
        const double r = calc(QStringLiteral("rand(2)"));
        QVERIFY(r >= 0.0 && r <= 1.0);
    }

    void expressionReferencesAndErrors()
    {
        QStringList refs;
        QCOMPARE(calc(QStringLiteral("Blur1.size * 2 + width"), 0, 0, &refs, {5.0, 1.0}), 11.0);
        QCOMPARE(refs, (QStringList {QStringLiteral("Blur1.size"), QStringLiteral("width")}));
        // Division by zero and other non-finite maths give 0, not a broken value.
        QCOMPARE(calc(QStringLiteral("1 / 0")), 0.0);
        QCOMPARE(calc(QStringLiteral("sqrt(-1)")), 0.0);
        QCOMPARE(calc(QStringLiteral("5 % 0")), 0.0);
        // Errors: no program, a message, never a crash.
        for (const QString& bad : {QStringLiteral(""), QStringLiteral("1 +"), QStringLiteral("(1"),
                 QStringLiteral("foo("), QStringLiteral("sin()"), QStringLiteral("sin(1, 2)"),
                 QStringLiteral("1 2"), QStringLiteral("@"), QStringLiteral("min(1)")}) {
            QString error;
            QVERIFY2(!expr::Program::compile(bad, &error), qPrintable(bad));
            QVERIFY2(!error.isEmpty(), qPrintable(bad));
        }
        // Limits: a very long formula and a very deep one are refused, not run.
        QVERIFY(!expr::Program::compile(QString(3000, QLatin1Char('1'))));
        QVERIFY(!expr::Program::compile(QString(300, QLatin1Char('(')) + QStringLiteral("1") + QString(300, QLatin1Char(')'))));
        QVERIFY(!expr::Program::compile(QString(300, QLatin1Char('-')) + QStringLiteral("1")));
        QVERIFY(expr::Program::functionNames().contains(QStringLiteral("smoothstep")));
    }

    void offsetScalesAndShiftsTime()
    {
        ParamSet p;
        p.setBase(nodekeys::Size, 1.0);
        AnimatedValue& v = p.animated(nodekeys::Size, 1.0);
        v.setKey(0, 0.0);
        v.setKey(10, 10.0);
        QCOMPARE(p.value(nodekeys::Size, 5, 0.0), 5.0);
        const QString id = modifiers::add(p, nodekeys::Size, ModifierKind::Offset);
        p.setBase(modifiers::controlKey(id, QStringLiteral("offset")), 100.0);
        p.setBase(modifiers::controlKey(id, QStringLiteral("scale")), 2.0);
        QCOMPARE(p.value(nodekeys::Size, 5, 0.0), 110.0);
        // Time Offset reads the curve later.
        p.setBase(modifiers::controlKey(id, QStringLiteral("timeOffset")), 2.0);
        QCOMPARE(p.value(nodekeys::Size, 5, 0.0), 114.0);
        QCOMPARE(p.valueAtTime(nodekeys::Size, 5.5, 0.0), 115.0);
        // Disabled: the keyframed value again.
        p.values[nodekeys::Size].modifiers[0].enabled = false;
        QCOMPARE(p.value(nodekeys::Size, 5, 0.0), 5.0);
        // Removing it drops its controls too.
        QVERIFY(modifiers::remove(p, nodekeys::Size, id));
        QVERIFY(p.values.size() == 1);
        QVERIFY(!modifiers::remove(p, nodekeys::Size, id));
    }

    void perturbAndWiggleAreRandomButRepeatable()
    {
        for (const ModifierKind kind : {ModifierKind::Perturb, ModifierKind::Wiggle}) {
            ParamSet p;
            p.setBase(nodekeys::Size, 50.0);
            const QString id = modifiers::add(p, nodekeys::Size, kind);
            p.setBase(modifiers::controlKey(id, kind == ModifierKind::Perturb ? QStringLiteral("strength")
                                                                               : QStringLiteral("amplitude")),
                10.0);
            double lo = 1e9, hi = -1e9;
            for (int f = 0; f < 400; ++f) {
                const double v = p.value(nodekeys::Size, f, 0.0);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                QCOMPARE(v, p.value(nodekeys::Size, f, 0.0)); // repeatable
                QVERIFY(std::abs(v - 50.0) <= 10.0 + 1e-9);
            }
            QVERIFY2(hi - lo > 5.0, "it moves"); // moves by a good part of the strength
            QVERIFY2(lo < 50.0 && hi > 50.0, "both sides");
            // Smooth: neighbouring frames differ by little.
            for (int f = 0; f < 200; ++f)
                QVERIFY(std::abs(p.value(nodekeys::Size, f + 1, 0.0) - p.value(nodekeys::Size, f, 0.0)) < 4.0);
            // Another seed, another sequence.
            ParamSet q = p;
            q.setBase(modifiers::controlKey(id, QStringLiteral("seed")), 777.0);
            int different = 0;
            for (int f = 0; f < 100; ++f)
                different += std::abs(p.value(nodekeys::Size, f, 0.0) - q.value(nodekeys::Size, f, 0.0)) > 1e-6;
            QVERIFY(different > 80);
        }
        // The X and Y of a point do not get the same seed.
        ParamSet p;
        const QString a = modifiers::add(p, nodekeys::CenterX, ModifierKind::Perturb);
        const QString b = modifiers::add(p, nodekeys::CenterY, ModifierKind::Perturb);
        QVERIFY(a != b);
        QVERIFY(p.value(modifiers::controlKey(a, QStringLiteral("seed")), 0, -1)
            != p.value(modifiers::controlKey(b, QStringLiteral("seed")), 0, -1));
    }

    void cycleLoopsPingPongsAndRepeatsRelative()
    {
        ParamSet p;
        AnimatedValue& v = p.animated(nodekeys::Size, 0.0);
        v.setKey(0, 0.0);
        v.setKey(10, 10.0); // a ramp 0 -> 10
        const QString id = modifiers::add(p, nodekeys::Size, ModifierKind::Cycle);
        const auto set = [&](const char* n, double x) { p.setBase(modifiers::controlKey(id, QLatin1String(n)), x); };
        set("start", 0);
        set("end", 10);
        // Loop
        QCOMPARE(p.value(nodekeys::Size, 4, 0), 4.0);
        QCOMPARE(p.value(nodekeys::Size, 14, 0), 4.0);
        QCOMPARE(p.value(nodekeys::Size, 23, 0), 3.0);
        // Ping-Pong
        p.setStatic(modifiers::controlKey(id, QStringLiteral("mode")), QStringLiteral("pingPong"));
        QCOMPARE(p.value(nodekeys::Size, 4, 0), 4.0);
        QCOMPARE(p.value(nodekeys::Size, 14, 0), 6.0);
        QCOMPARE(p.value(nodekeys::Size, 20, 0), 0.0);
        QCOMPARE(p.value(nodekeys::Size, 23, 0), 3.0);
        // Relative: every round starts where the last one ended.
        p.setStatic(modifiers::controlKey(id, QStringLiteral("mode")), QStringLiteral("relative"));
        QCOMPARE(p.value(nodekeys::Size, 14, 0), 14.0);
        QCOMPARE(p.value(nodekeys::Size, 23, 0), 23.0);
        // Repeats: three rounds (0..30), then it holds the end.
        set("repeats", 3);
        QCOMPARE(p.value(nodekeys::Size, 25, 0), 25.0);
        QCOMPARE(p.value(nodekeys::Size, 60, 0), 30.0);
        p.setStatic(modifiers::controlKey(id, QStringLiteral("mode")), QStringLiteral("loop"));
        QCOMPARE(p.value(nodekeys::Size, 60, 0), 10.0);
        // An empty range does nothing.
        set("end", 0);
        QCOMPARE(p.value(nodekeys::Size, 14, 0), 10.0);
    }

    void expressionModifierUsesTimeValueAndNumbers()
    {
        ParamSet p;
        p.setBase(nodekeys::Size, 7.0);
        const QString id = modifiers::add(p, nodekeys::Size, ModifierKind::Expression);
        p.setBase(modifiers::controlKey(id, QStringLiteral("n1")), 3.0);
        p.values[nodekeys::Size].modifiers[0].text = QStringLiteral("value * n1 + time");
        QCOMPARE(p.value(nodekeys::Size, 5, 0), 26.0);
        // A broken formula leaves the value alone.
        p.values[nodekeys::Size].modifiers[0].text = QStringLiteral("value *");
        QCOMPARE(p.value(nodekeys::Size, 5, 0), 7.0);
        // n1 is keyframeable like any parameter.
        p.values[nodekeys::Size].modifiers[0].text = QStringLiteral("n1");
        p.animated(modifiers::controlKey(id, QStringLiteral("n1")), 0).setKey(0, 0.0);
        p.animated(modifiers::controlKey(id, QStringLiteral("n1")), 0).setKey(10, 100.0);
        QCOMPARE(p.value(nodekeys::Size, 5, 0), 50.0);
    }

    void xyPathReplacesBothAxes()
    {
        ParamSet p;
        p.setBase(nodekeys::CenterX, 0.5);
        p.setBase(nodekeys::CenterY, 0.5);
        const QString x = modifiers::add(p, nodekeys::CenterX, ModifierKind::Path);
        const QString y = modifiers::add(p, nodekeys::CenterY, ModifierKind::Path);
        modifiers::seedPath(p, nodekeys::CenterX, x, 0);
        modifiers::seedPath(p, nodekeys::CenterY, y, 0);
        QCOMPARE(p.value(nodekeys::CenterX, 3, 0), 0.5);
        p.animated(modifiers::controlKey(x, QStringLiteral("path")), 0).setKey(10, 0.9);
        p.animated(modifiers::controlKey(y, QStringLiteral("path")), 0).setKey(10, 0.1);
        QVERIFY(std::abs(p.value(nodekeys::CenterX, 5, 0) - 0.7) < 1e-9);
        QVERIFY(std::abs(p.value(nodekeys::CenterY, 5, 0) - 0.3) < 1e-9);
        QVERIFY(modifiers::replacesValue(ModifierKind::Path));
        QVERIFY(!modifiers::replacesValue(ModifierKind::Perturb));
    }

    void publishChangesNothingAndIsListed()
    {
        Composition c = co::defaultComposition();
        const quint64 t = co::addNode(c, nodetypes::Transform, {});
        CompNode& n = *c.node(t);
        n.params.setBase(nodekeys::Size, 2.0);
        const QString id = modifiers::add(n.params, nodekeys::Size, ModifierKind::Publish);
        QCOMPARE(modifiers::add(n.params, nodekeys::Size, ModifierKind::Publish), id); // once
        QCOMPARE(n.params.value(nodekeys::Size, 0, 0), 2.0);
        QVERIFY(!modifiers::isModified(n.params, nodekeys::Size));
        const auto pub = complinks::published(c);
        QCOMPARE(pub.size(), 1);
        QCOMPARE(pub[0].key, nodekeys::Size);
        QVERIFY(pub[0].label.contains(n.name));
    }

    void connectToFollowsAnotherParameter()
    {
        Composition c = co::defaultComposition();
        const quint64 a = co::addNode(c, nodetypes::Transform, {});
        const quint64 b = co::addNode(c, nodetypes::Transform, {});
        AnimatedValue& src = c.node(a)->params.animated(nodekeys::Size, 1.0);
        src.setKey(0, 1.0);
        src.setKey(10, 3.0);
        QVERIFY(!complinks::hasLinks(c));
        QCOMPARE(complinks::connectParam(c, b, nodekeys::Size, {a, nodekeys::Size}), complinks::LinkError::None);
        QVERIFY(complinks::hasLinks(c));
        // Unresolved the link has no value yet; resolved it is the other parameter's.
        const Composition r5 = complinks::resolved(c, 5);
        QCOMPARE(r5.node(b)->params.value(nodekeys::Size, 5, 0), 2.0);
        const Composition r10 = complinks::resolved(c, 10);
        QCOMPARE(r10.node(b)->params.value(nodekeys::Size, 10, 0), 3.0);
        // The stored composition is untouched.
        QVERIFY(c.node(b)->params.bound.values.isEmpty());
        // Chains: c follows b follows a.
        const quint64 d = co::addNode(c, nodetypes::Transform, {});
        QCOMPARE(complinks::connectParam(c, d, nodekeys::Size, {b, nodekeys::Size}), complinks::LinkError::None);
        QCOMPARE(complinks::resolved(c, 10).node(d)->params.value(nodekeys::Size, 10, 0), 3.0);
        // Circles are refused; so is a link to itself or to something that is not a number.
        QCOMPARE(complinks::connectParam(c, a, nodekeys::Size, {d, nodekeys::Size}), complinks::LinkError::Cycle);
        QCOMPARE(complinks::connectParam(c, a, nodekeys::Size, {a, nodekeys::Size}), complinks::LinkError::ToItself);
        QCOMPARE(complinks::connectParam(c, a, nodekeys::Size, {999, nodekeys::Size}), complinks::LinkError::Missing);
        QCOMPARE(complinks::connectParam(c, a, nodekeys::Size, {d, QStringLiteral("nothing")}), complinks::LinkError::NotNumber);
        // A second Connect To replaces the first.
        QCOMPARE(complinks::connectParam(c, d, nodekeys::Size, {a, nodekeys::Size}), complinks::LinkError::None);
        QCOMPARE(c.node(d)->params.values[nodekeys::Size].modifiers.size(), 1);
        // Disconnect keeps the value it showed.
        QVERIFY(complinks::disconnectParam(c, b, nodekeys::Size, 10));
        QCOMPARE(c.node(b)->params.value(nodekeys::Size, 3, 0), 3.0);
        QVERIFY(c.node(b)->params.values[nodekeys::Size].modifiers.isEmpty());
        QVERIFY(!complinks::disconnectParam(c, b, nodekeys::Size, 10));
        // A link to a deleted node falls back to the parameter's own value.
        c.nodes.removeIf([&](const CompNode& n) { return n.id == a; });
        QCOMPARE(complinks::resolved(c, 4).node(d)->params.value(nodekeys::Size, 4, 0), c.node(d)->params.values[nodekeys::Size].base);
    }

    void expressionsReadOtherParameters()
    {
        Composition c = co::defaultComposition();
        const quint64 a = co::addNode(c, nodetypes::Transform, {});
        const quint64 b = co::addNode(c, nodetypes::Transform, {});
        const QString an = c.node(a)->name;
        c.node(a)->params.setBase(nodekeys::Size, 4.0);
        c.node(a)->params.setBase(nodekeys::Angle, 30.0);
        const QString id = modifiers::add(c.node(b)->params, nodekeys::Size, ModifierKind::Expression);
        modifiers::find(c.node(b)->params, nodekeys::Size, id)->text
            = an.toLower() + QStringLiteral(".Size * 2 + angle / 10 + ") + an + QStringLiteral(".Angle");
        QVERIFY(complinks::hasLinks(c));
        // size is this node's own (default 1) here: 4 * 2 + angle(0)/10 + 30 = 38.
        QCOMPARE(complinks::resolved(c, 0).node(b)->params.value(nodekeys::Size, 0, 0), 38.0);
        // Parameter labels work too, in any case and without the spaces.
        modifiers::find(c.node(b)->params, nodekeys::Size, id)->text = an + QStringLiteral(".size + n1");
        QCOMPARE(complinks::resolved(c, 0).node(b)->params.value(nodekeys::Size, 0, 0), 4.0);
        // A formula that reads its own parameter does not run away: the reference stays 0.
        modifiers::find(c.node(b)->params, nodekeys::Size, id)->text = QStringLiteral("size + 1");
        QCOMPARE(complinks::resolved(c, 0).node(b)->params.value(nodekeys::Size, 0, 0), 1.0);
        QVERIFY(complinks::dependsOn(c, {b, nodekeys::Size}, {b, nodekeys::Size}));
        // Reading a parameter that reads this one is a circle.
        modifiers::find(c.node(b)->params, nodekeys::Size, id)->text = an + QStringLiteral(".Size");
        QVERIFY(!complinks::dependsOn(c, {b, nodekeys::Size}, {b, nodekeys::Size}));
        const QString id2 = modifiers::add(c.node(a)->params, nodekeys::Size, ModifierKind::Expression);
        modifiers::find(c.node(a)->params, nodekeys::Size, id2)->text = c.node(b)->name + QStringLiteral(".Size");
        QVERIFY(complinks::dependsOn(c, {b, nodekeys::Size}, {b, nodekeys::Size}));
        QCOMPARE(complinks::resolved(c, 0).node(b)->params.value(nodekeys::Size, 0, 0) >= 0.0, true); // ends
        // Unknown names are 0, not errors.
        c.node(a)->params.values[nodekeys::Size].modifiers.clear();
        modifiers::find(c.node(b)->params, nodekeys::Size, id)->text = QStringLiteral("nosuch.size + 5");
        QCOMPARE(complinks::resolved(c, 0).node(b)->params.value(nodekeys::Size, 0, 0), 5.0);
    }

    void modifiersAreSavedAndOlderFilesLoad()
    {
        Composition c = co::defaultComposition();
        const quint64 a = co::addNode(c, nodetypes::Transform, {});
        const quint64 b = co::addNode(c, nodetypes::Transform, {});
        const QString p = modifiers::add(c.node(a)->params, nodekeys::Size, ModifierKind::Perturb);
        c.node(a)->params.setBase(modifiers::controlKey(p, QStringLiteral("strength")), 0.25);
        const QString cy = modifiers::add(c.node(a)->params, nodekeys::Angle, ModifierKind::Cycle);
        c.node(a)->params.setStatic(modifiers::controlKey(cy, QStringLiteral("mode")), QStringLiteral("pingPong"));
        const QString e = modifiers::add(c.node(a)->params, nodekeys::CenterX, ModifierKind::Expression);
        modifiers::find(c.node(a)->params, nodekeys::CenterX, e)->text = QStringLiteral("0.5 + sin(time) / 10");
        modifiers::find(c.node(a)->params, nodekeys::CenterX, e)->enabled = false;
        QVERIFY(complinks::connectParam(c, b, nodekeys::Size, {a, nodekeys::Size}) == complinks::LinkError::None);
        modifiers::add(c.node(b)->params, nodekeys::Angle, ModifierKind::Publish);
        const QJsonObject json = serialization::compositionToJson(c);
        const Composition back = serialization::compositionFromJson(json);
        QCOMPARE(back.node(a)->params, c.node(a)->params);
        QCOMPARE(back.node(b)->params, c.node(b)->params);
        const ParamModifier* m = modifiers::first(back.node(b)->params, nodekeys::Size, ModifierKind::Connect);
        QVERIFY(m && m->targetNode == a && m->targetKey == nodekeys::Size);
        QCOMPARE(back.node(a)->params.value(nodekeys::Size, 3, 0), c.node(a)->params.value(nodekeys::Size, 3, 0));
        QVERIFY(!modifiers::find(back.node(a)->params, nodekeys::CenterX, e)->enabled);
        // A file without modifiers (older versions) loads with none.
        Composition plain = co::defaultComposition();
        co::addNode(plain, nodetypes::Transform, {});
        const Composition plainBack = serialization::compositionFromJson(serialization::compositionToJson(plain));
        for (const CompNode& n : plainBack.nodes)
            QVERIFY(modifiers::modifiedKeys(n.params).isEmpty());
        // A modifier of an unknown kind (a newer file) is dropped; the value stays.
        QJsonObject copy = json;
        QString text = QString::fromUtf8(QJsonDocument(copy).toJson());
        text.replace(QStringLiteral("\"perturb\""), QStringLiteral("\"from_the_future\""));
        const Composition future = serialization::compositionFromJson(QJsonDocument::fromJson(text.toUtf8()).object());
        QVERIFY(future.node(a)->params.values[nodekeys::Size].modifiers.isEmpty());
    }

    void nodeCopiesKeepModifiers()
    {
        Composition c = co::defaultComposition();
        const quint64 a = co::addNode(c, nodetypes::Transform, {});
        modifiers::add(c.node(a)->params, nodekeys::Size, ModifierKind::Wiggle);
        const Composition copy = c;
        QCOMPARE(copy, c);
        Composition other = c;
        other.node(a)->params.values[nodekeys::Size].modifiers[0].enabled = false;
        QVERIFY(!(other == c));
        // The values filled in for a frame are not part of what makes two compositions differ.
        Composition bound = c;
        bound.node(a)->params.bound.values.insert(QStringLiteral("m1"), 3.0);
        QCOMPARE(bound, c);
    }
};

QTEST_MAIN(TestModifiers)
#include "test_modifiers.moc"
