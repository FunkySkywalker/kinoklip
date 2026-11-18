// SPDX-License-Identifier: GPL-3.0-or-later
// Modifier maths and editing, see Modifiers.h.
//
// Study notes (spec 119): Blender's F-Curve Noise and Cycles modifiers (strength / scale / depth,
// cycle modes with offset repeat) and Natron's parameter expressions shaped Wiggle, Cycle and
// Expression; the maths here is our own.
#include "core/Modifiers.h"

#include "core/Expression.h"

#include <QCoreApplication>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
struct KindId
{
    ModifierKind kind;
    const char* id;
};
constexpr KindId kKindIds[] = {{ModifierKind::Perturb, "perturb"}, {ModifierKind::Wiggle, "wiggle"},
    {ModifierKind::Offset, "offset"}, {ModifierKind::Expression, "expression"},
    {ModifierKind::Cycle, "cycle"}, {ModifierKind::Path, "path"}, {ModifierKind::Connect, "connect"},
    {ModifierKind::Publish, "publish"}};
} // namespace

QString modifierKindId(ModifierKind kind)
{
    for (const KindId& k : kKindIds) {
        if (k.kind == kind)
            return QLatin1String(k.id);
    }
    return QStringLiteral("offset");
}

bool modifierKindFromId(const QString& id, ModifierKind* kind)
{
    for (const KindId& k : kKindIds) {
        if (id == QLatin1String(k.id)) {
            *kind = k.kind;
            return true;
        }
    }
    return false;
}

} // namespace kk

namespace kk::modifiers {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Modifiers", text);
}


/// Deterministic hash to [-1, 1].
double hashSigned(quint64 seed, std::int64_t i)
{
    std::uint64_t x = (std::uint64_t(i) + seed * 0x632BE59BD9B4E019ull) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return double(x >> 11) / double(1ull << 52) - 1.0;
}

/// Value noise in [-1, 1]: random targets at the integers, a smooth step between them.
double valueNoise(quint64 seed, double x)
{
    const double f = std::floor(x);
    const double t = x - f;
    const double s = t * t * (3.0 - 2.0 * t);
    const double a = hashSigned(seed, std::int64_t(f)), b = hashSigned(seed, std::int64_t(f) + 1);
    return a + (b - a) * s;
}

/// Gradient noise in about [-1, 1] (zero at the integers): rounder than value noise.
double gradientNoise(quint64 seed, double x)
{
    const double f = std::floor(x);
    const double t = x - f;
    const double fade = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    const double g0 = hashSigned(seed, std::int64_t(f)), g1 = hashSigned(seed, std::int64_t(f) + 1);
    const double a = g0 * t, b = g1 * (t - 1.0);
    return 2.0 * (a + (b - a) * fade);
}

double ctl(const ParamSet& ps, const ParamModifier& m, const char* name, double t, double def)
{
    return ps.valueAtTime(controlKey(m.id, QLatin1String(name)), t, def);
}

QString ctlChoice(const ParamSet& ps, const ParamModifier& m, const char* name, const QString& def)
{
    return ps.staticValue(controlKey(m.id, QLatin1String(name)), def).toString();
}

double evalUpTo(const ParamSet& ps, const AnimatedValue& av, int count, double t);

double applyOne(const ParamSet& ps, const AnimatedValue& av, int index, double t)
{
    const ParamModifier& m = av.modifiers.at(index);
    const auto below = [&](double at) { return evalUpTo(ps, av, index, at); };
    switch (m.kind) {
    case ModifierKind::Perturb: {
        const double period = std::max(0.01, ctl(ps, m, "period", t, 12.0));
        const double strength = ctl(ps, m, "strength", t, 1.0);
        const quint64 seed = quint64(std::llround(ctl(ps, m, "seed", t, 1.0)));
        return below(t) + strength * valueNoise(seed, t / period);
    }
    case ModifierKind::Wiggle: {
        const double wave = std::max(0.01, ctl(ps, m, "wavelength", t, 24.0));
        const double amp = ctl(ps, m, "amplitude", t, 1.0);
        const int octaves = std::clamp(int(std::lround(ctl(ps, m, "octaves", t, 2.0))), 1, 6);
        const quint64 seed = quint64(std::llround(ctl(ps, m, "seed", t, 1.0)));
        double sum = 0.0, norm = 0.0, a = 1.0, f = 1.0;
        for (int o = 0; o < octaves; ++o) {
            sum += a * gradientNoise(seed + quint64(o) * 7919, t / wave * f);
            norm += a;
            a *= 0.5;
            f *= 2.0;
        }
        return below(t) + amp * sum / norm;
    }
    case ModifierKind::Offset: {
        const double dt = ctl(ps, m, "timeOffset", t, 0.0);
        const double scale = ctl(ps, m, "scale", t, 1.0);
        const double offset = ctl(ps, m, "offset", t, 0.0);
        return below(t + dt) * scale + offset;
    }
    case ModifierKind::Expression: {
        QString error;
        const auto program = expr::Program::compile(m.text, &error);
        if (!program)
            return below(t);
        QVector<double> refs;
        for (const QString& r : program->references())
            refs.append(ps.bound.values.value(m.id + QLatin1Char('/') + r, 0.0));
        expr::Program::Inputs in;
        in.time = t;
        in.value = below(t);
        in.n[0] = ctl(ps, m, "n1", t, 0.0);
        in.n[1] = ctl(ps, m, "n2", t, 0.0);
        in.n[2] = ctl(ps, m, "n3", t, 0.0);
        in.refs = refs.isEmpty() ? nullptr : refs.constData();
        return program->evaluate(in);
    }
    case ModifierKind::Cycle: {
        const double s = ctl(ps, m, "start", t, 0.0);
        const double e = ctl(ps, m, "end", t, 24.0);
        const double len = e - s;
        if (len < 1e-6 || t < s)
            return below(t);
        const int repeats = int(std::lround(ctl(ps, m, "repeats", t, 0.0)));
        const QString mode = ctlChoice(ps, m, "mode", QStringLiteral("loop"));
        double k = std::floor((t - s) / len);
        double at;
        if (repeats > 0 && k >= repeats) {
            k = repeats - 1;
            at = mode == QLatin1String("pingPong") && int(k) % 2 == 1 ? s : e;
            if (mode == QLatin1String("loop"))
                at = e;
        } else if (mode == QLatin1String("pingPong")) {
            const double phase = std::fmod(t - s, 2.0 * len);
            at = phase < len ? s + phase : e - (phase - len);
        } else {
            at = s + std::fmod(t - s, len);
        }
        double v = below(at);
        if (mode == QLatin1String("relative"))
            v += k * (below(e) - below(s));
        return v;
    }
    case ModifierKind::Path:
        return ps.valueAtTime(controlKey(m.id, QStringLiteral("path")), t, below(t));
    case ModifierKind::Connect: {
        const auto it = ps.bound.values.constFind(m.id);
        return it == ps.bound.values.cend() ? below(t) : *it;
    }
    case ModifierKind::Publish:
        return below(t);
    }
    return below(t);
}

double evalUpTo(const ParamSet& ps, const AnimatedValue& av, int count, double t)
{
    // Start from the last modifier that replaces the value: the ones under it cannot matter.
    if (count <= 0) {
        AnimatedValue plain;
        plain.base = av.base;
        plain.keys = av.keys;
        plain.pre = av.pre;
        plain.post = av.post;
        return plain.valueAtTime(t);
    }
    if (!av.modifiers.at(count - 1).enabled)
        return evalUpTo(ps, av, count - 1, t);
    return applyOne(ps, av, count - 1, t);
}

} // namespace

QString controlKey(const QString& id, const QString& name)
{
    return QLatin1Char('~') + id + QLatin1Char('.') + name;
}

bool isControlKey(const QString& key)
{
    return key.startsWith(QLatin1Char('~'));
}

double evaluate(const ParamSet& params, const AnimatedValue& value, double frame)
{
    return evalUpTo(params, value, int(value.modifiers.size()), frame);
}

QVector<Control> controls(ModifierKind kind)
{
    const auto num = [](const char* name, const QString& label, const QString& tip, double mn,
                         double mx, double def, double step, const QString& unit = {},
                         bool isInt = false) {
        Control c;
        c.name = QLatin1String(name);
        c.label = label;
        c.tooltip = tip;
        c.min = mn;
        c.max = mx;
        c.def = def;
        c.step = step;
        c.unit = unit;
        c.isInt = isInt;
        return c;
    };
    switch (kind) {
    case ModifierKind::Perturb:
        return {num("period", tr("Period"),
                    tr("Frames between two random targets; the value glides from one to the next"),
                    1, 2000, 12, 1, tr(" fr")),
            num("strength", tr("Strength"),
                tr("How far the value moves away from where it would be, in the parameter's own units"),
                -100000, 100000, 1, 0.1),
            num("seed", tr("Random Seed"), tr("Another number gives another random sequence"), 0,
                65535, 1, 1, {}, true)};
    case ModifierKind::Wiggle:
        return {num("wavelength", tr("Wavelength"),
                    tr("Frames of the slowest wiggle; each octave is twice as fast"), 1, 5000, 24,
                    1, tr(" fr")),
            num("amplitude", tr("Amplitude"),
                tr("How far the value wiggles, in the parameter's own units"), -100000, 100000, 1,
                0.1),
            num("octaves", tr("Octaves"), tr("Layers of finer, weaker wiggle (1 to 6)"), 1, 6, 2,
                1, {}, true),
            num("seed", tr("Random Seed"), tr("Another number gives another random sequence"), 0,
                65535, 1, 1, {}, true)};
    case ModifierKind::Offset:
        return {num("offset", tr("Offset"), tr("Added to the value"), -100000, 100000, 0, 0.1),
            num("scale", tr("Scale"), tr("The value is multiplied by this before the Offset is added"),
                -1000, 1000, 1, 0.01),
            num("timeOffset", tr("Time Offset"),
                tr("Reads the value this many frames later (negative: earlier)"), -100000, 100000,
                0, 1, tr(" fr"))};
    case ModifierKind::Expression:
        return {num("n1", tr("Number In 1"), tr("Available in the formula as n1"), -1e6, 1e6, 0, 0.1),
            num("n2", tr("Number In 2"), tr("Available in the formula as n2"), -1e6, 1e6, 0, 0.1),
            num("n3", tr("Number In 3"), tr("Available in the formula as n3"), -1e6, 1e6, 0, 0.1)};
    case ModifierKind::Cycle: {
        Control mode;
        mode.name = QStringLiteral("mode");
        mode.label = tr("Mode");
        mode.tooltip = tr("Loop: start again at the Start frame. Ping-Pong: run backwards and "
                          "forwards. Relative: start again from where the last round ended.");
        mode.choices = {QStringLiteral("loop"), QStringLiteral("pingPong"), QStringLiteral("relative")};
        mode.choiceLabels = {tr("Loop"), tr("Ping-Pong"), tr("Relative")};
        mode.defChoice = QStringLiteral("loop");
        return {num("start", tr("Start Frame"), tr("First frame of the repeated range"), -100000,
                    100000, 0, 1, tr(" fr")),
            num("end", tr("End Frame"), tr("Last frame of the repeated range"), -100000, 100000, 24,
                1, tr(" fr")),
            mode,
            num("repeats", tr("Repeats"), tr("How many rounds to run; 0 repeats for ever"), 0, 10000,
                0, 1, {}, true)};
    }
    case ModifierKind::Path:
        return {num("path", tr("Path"), tr("The path of this axis over time; add keyframes to move it"),
            -100000, 100000, 0, 0.01)};
    case ModifierKind::Connect:
    case ModifierKind::Publish:
        return {};
    }
    return {};
}

QString kindName(ModifierKind kind)
{
    switch (kind) {
    case ModifierKind::Perturb: return tr("Perturb");
    case ModifierKind::Wiggle: return tr("Wiggle");
    case ModifierKind::Offset: return tr("Offset");
    case ModifierKind::Expression: return tr("Expression");
    case ModifierKind::Cycle: return tr("Cycle");
    case ModifierKind::Path: return tr("XY Path");
    case ModifierKind::Connect: return tr("Connect To");
    case ModifierKind::Publish: return tr("Publish");
    }
    return {};
}

QString kindDescription(ModifierKind kind)
{
    switch (kind) {
    case ModifierKind::Perturb:
        return tr("Moves the value by smooth random amounts, a new random target every Period frames.");
    case ModifierKind::Wiggle:
        return tr("Shakes the value with layered noise of a chosen Wavelength and Amplitude.");
    case ModifierKind::Offset:
        return tr("Adds an amount to the value, scales it, and can read it earlier or later in time.");
    case ModifierKind::Expression:
        return tr("Computes the value with a formula that can read other parameters.");
    case ModifierKind::Cycle:
        return tr("Repeats a range of frames of the value as a loop, a ping-pong or a relative repeat.");
    case ModifierKind::Path:
        return tr("Replaces a position's X and Y with two paths of their own.");
    case ModifierKind::Connect:
        return tr("Makes the value follow another node's parameter.");
    case ModifierKind::Publish:
        return tr("Offers the parameter in the Connect To lists and on a Macro.");
    }
    return {};
}


QString add(ParamSet& params, const QString& key, ModifierKind kind)
{
    AnimatedValue& av = params.animated(key, 0.0);
    if (kind == ModifierKind::Publish) {
        for (const ParamModifier& m : av.modifiers) {
            if (m.kind == kind)
                return m.id;
        }
    }
    // The first free "m<n>" among all modifiers of the set.
    QSet<QString> used;
    for (const AnimatedValue& v : std::as_const(params.values)) {
        for (const ParamModifier& m : v.modifiers)
            used.insert(m.id);
    }
    int n = 1;
    while (used.contains(QStringLiteral("m%1").arg(n)))
        ++n;
    ParamModifier m;
    m.kind = kind;
    m.id = QStringLiteral("m%1").arg(n);
    for (const Control& c : controls(kind)) {
        const QString k = controlKey(m.id, c.name);
        if (c.choices.isEmpty())
            params.setBase(k, c.def);
        else
            params.setStatic(k, c.defChoice);
    }
    if (kind == ModifierKind::Perturb || kind == ModifierKind::Wiggle) {
        // The Y of a point gets another sequence than its X.
        quint32 h = 7;
        for (const QChar c : key)
            h = h * 31 + c.unicode();
        const double seed = double(1 + (h % 9000));
        params.setBase(controlKey(m.id, QStringLiteral("seed")), seed);
    }
    if (kind == ModifierKind::Path)
        params.setBase(controlKey(m.id, QStringLiteral("path")), av.base);
    AnimatedValue& target = params.animated(key, 0.0);
    target.modifiers.append(m);
    return m.id;
}

bool remove(ParamSet& params, const QString& key, const QString& id)
{
    const auto it = params.values.find(key);
    if (it == params.values.end())
        return false;
    for (qsizetype i = 0; i < it->modifiers.size(); ++i) {
        if (it->modifiers.at(i).id != id)
            continue;
        const ParamModifier m = it->modifiers.at(i);
        it->modifiers.removeAt(i);
        for (const Control& c : controls(m.kind)) {
            params.values.remove(controlKey(id, c.name));
            params.statics.remove(controlKey(id, c.name));
        }
        params.bound.values.remove(id);
        const QString prefix = id + QLatin1Char('/');
        for (auto b = params.bound.values.begin(); b != params.bound.values.end();) {
            if (b.key().startsWith(prefix))
                b = params.bound.values.erase(b);
            else
                ++b;
        }
        return true;
    }
    return false;
}

const ParamModifier* find(const ParamSet& params, const QString& key, const QString& id)
{
    const auto it = params.values.constFind(key);
    if (it == params.values.cend())
        return nullptr;
    for (const ParamModifier& m : it->modifiers) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

ParamModifier* find(ParamSet& params, const QString& key, const QString& id)
{
    const auto it = params.values.find(key);
    if (it == params.values.end())
        return nullptr;
    for (ParamModifier& m : it->modifiers) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

const ParamModifier* first(const ParamSet& params, const QString& key, ModifierKind kind)
{
    const auto it = params.values.constFind(key);
    if (it == params.values.cend())
        return nullptr;
    for (const ParamModifier& m : it->modifiers) {
        if (m.kind == kind)
            return &m;
    }
    return nullptr;
}

bool replacesValue(ModifierKind kind)
{
    return kind == ModifierKind::Expression || kind == ModifierKind::Path
        || kind == ModifierKind::Connect;
}

bool isModified(const ParamSet& params, const QString& key)
{
    const auto it = params.values.constFind(key);
    if (it == params.values.cend())
        return false;
    return std::any_of(it->modifiers.cbegin(), it->modifiers.cend(),
        [](const ParamModifier& m) { return m.kind != ModifierKind::Publish; });
}

QStringList modifiedKeys(const ParamSet& params)
{
    QStringList keys;
    for (auto it = params.values.cbegin(); it != params.values.cend(); ++it) {
        if (!it->modifiers.isEmpty())
            keys.append(it.key());
    }
    return keys;
}

void seedPath(ParamSet& params, const QString& key, const QString& id, int64_t frame)
{
    const ParamModifier* m = find(params, key, id);
    if (!m || m->kind != ModifierKind::Path)
        return;
    const QString ck = controlKey(id, QStringLiteral("path"));
    // The curve's value now: the parameter without this modifier.
    AnimatedValue plain = params.values.value(key);
    plain.modifiers.clear();
    params.animated(ck, 0.0).setKey(frame, plain.valueAt(frame));
}

} // namespace kk::modifiers
