// SPDX-License-Identifier: GPL-3.0-or-later
// Expression compiler (recursive descent to a tree) and evaluator, see Expression.h.
#include "core/Expression.h"

#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace kk::expr {

namespace {

constexpr int kMaxLength = 2000;
constexpr int kMaxDepth = 48;
constexpr double kPi = 3.14159265358979323846;

enum class Op : quint8 {
    Num, Var, Ref, Neg, Not, Add, Sub, Mul, Div, Mod, Pow, Lt, Gt, Le, Ge, Eq, Ne, And, Or, Cond, Call
};

enum Var { VarTime, VarValue, VarN1, VarN2, VarN3, VarPi, VarE, VarPixel = 100 };

enum class Fn : quint8 {
    Sin, Cos, Tan, Asin, Acos, Atan, Atan2, Sqrt, Abs, Pow, Exp, Log, Log10, Floor, Ceil, Round,
    Fract, Sign, Min, Max, Clamp, Mix, Step, Smoothstep, Fmod, Noise, Rand, Deg, Rad, If
};

struct FnInfo
{
    const char* name;
    Fn fn;
    int args;
};

constexpr FnInfo kFunctions[] = {
    {"sin", Fn::Sin, 1}, {"cos", Fn::Cos, 1}, {"tan", Fn::Tan, 1}, {"asin", Fn::Asin, 1},
    {"acos", Fn::Acos, 1}, {"atan", Fn::Atan, 1}, {"atan2", Fn::Atan2, 2}, {"sqrt", Fn::Sqrt, 1},
    {"abs", Fn::Abs, 1}, {"pow", Fn::Pow, 2}, {"exp", Fn::Exp, 1}, {"log", Fn::Log, 1},
    {"log10", Fn::Log10, 1}, {"floor", Fn::Floor, 1}, {"ceil", Fn::Ceil, 1}, {"round", Fn::Round, 1},
    {"fract", Fn::Fract, 1}, {"sign", Fn::Sign, 1}, {"min", Fn::Min, 2}, {"max", Fn::Max, 2},
    {"clamp", Fn::Clamp, 3}, {"mix", Fn::Mix, 3}, {"lerp", Fn::Mix, 3}, {"step", Fn::Step, 2},
    {"smoothstep", Fn::Smoothstep, 3}, {"fmod", Fn::Fmod, 2}, {"noise", Fn::Noise, 1},
    {"rand", Fn::Rand, 1}, {"deg", Fn::Deg, 1}, {"rad", Fn::Rad, 1}, {"if", Fn::If, 3},
};

/// A hash of an integer to [-1, 1] (deterministic: no global state).
double hashSigned(std::int64_t i)
{
    std::uint64_t x = std::uint64_t(i) * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return double(x >> 11) / double(1ull << 52) - 1.0;
}

double smoothNoise(double x)
{
    const double f = std::floor(x);
    const double t = x - f;
    const double s = t * t * (3.0 - 2.0 * t);
    const double a = hashSigned(std::int64_t(f)), b = hashSigned(std::int64_t(f) + 1);
    return a + (b - a) * s;
}

/// Value of @p x as the shader computes it for pixel programs (see Program::compilePixel()).
double pixelPow(double x, double y)
{
    if (x >= 0.0)
        return std::pow(x, y);
    const double r = std::round(y);
    if (r != y)
        return 0.0;
    return std::fmod(std::abs(r), 2.0) == 1.0 ? -std::pow(-x, y) : std::pow(-x, y);
}

double pixelRound(double x)
{
    return x >= 0.0 ? std::floor(x + 0.5) : -std::floor(-x + 0.5);
}

double callPixelFn(Fn fn, const double* a)
{
    switch (fn) {
    case Fn::Asin: return std::asin(std::clamp(a[0], -1.0, 1.0));
    case Fn::Acos: return std::acos(std::clamp(a[0], -1.0, 1.0));
    case Fn::Atan2: return a[0] == 0.0 && a[1] == 0.0 ? 0.0 : std::atan2(a[0], a[1]);
    case Fn::Sqrt: return a[0] > 0.0 ? std::sqrt(a[0]) : 0.0;
    case Fn::Pow: return pixelPow(a[0], a[1]);
    case Fn::Exp: return std::exp(std::min(a[0], 80.0));
    case Fn::Log: return a[0] > 0.0 ? std::log(a[0]) : 0.0;
    case Fn::Log10: return a[0] > 0.0 ? std::log10(a[0]) : 0.0;
    case Fn::Round: return pixelRound(a[0]);
    case Fn::Fmod: return a[1] == 0.0 ? 0.0 : std::fmod(a[0], a[1]);
    default: break;
    }
    return 0.0;
}

double callFn(Fn fn, const double* a, bool pixel = false)
{
    if (pixel) {
        switch (fn) {
        case Fn::Asin: case Fn::Acos: case Fn::Atan2: case Fn::Sqrt: case Fn::Pow: case Fn::Exp:
        case Fn::Log: case Fn::Log10: case Fn::Round: case Fn::Fmod:
            return callPixelFn(fn, a);
        default:
            break;
        }
    }
    switch (fn) {
    case Fn::Sin: return std::sin(a[0]);
    case Fn::Cos: return std::cos(a[0]);
    case Fn::Tan: return std::tan(a[0]);
    case Fn::Asin: return std::asin(a[0]);
    case Fn::Acos: return std::acos(a[0]);
    case Fn::Atan: return std::atan(a[0]);
    case Fn::Atan2: return std::atan2(a[0], a[1]);
    case Fn::Sqrt: return std::sqrt(a[0]);
    case Fn::Abs: return std::abs(a[0]);
    case Fn::Pow: return std::pow(a[0], a[1]);
    case Fn::Exp: return std::exp(a[0]);
    case Fn::Log: return std::log(a[0]);
    case Fn::Log10: return std::log10(a[0]);
    case Fn::Floor: return std::floor(a[0]);
    case Fn::Ceil: return std::ceil(a[0]);
    case Fn::Round: return std::round(a[0]);
    case Fn::Fract: return a[0] - std::floor(a[0]);
    case Fn::Sign: return a[0] > 0 ? 1.0 : (a[0] < 0 ? -1.0 : 0.0);
    case Fn::Min: return std::min(a[0], a[1]);
    case Fn::Max: return std::max(a[0], a[1]);
    case Fn::Clamp: return std::clamp(a[0], std::min(a[1], a[2]), std::max(a[1], a[2]));
    case Fn::Mix: return a[0] + (a[1] - a[0]) * a[2];
    case Fn::Step: return a[1] < a[0] ? 0.0 : 1.0;
    case Fn::Smoothstep: {
        const double w = a[1] - a[0];
        const double t = w == 0.0 ? (a[2] < a[0] ? 0.0 : 1.0) : std::clamp((a[2] - a[0]) / w, 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }
    case Fn::Fmod: return std::fmod(a[0], a[1]);
    case Fn::Noise: return smoothNoise(a[0]);
    case Fn::Rand: return 0.5 + 0.5 * hashSigned(std::int64_t(std::floor(a[0])));
    case Fn::Deg: return a[0] * 180.0 / kPi;
    case Fn::Rad: return a[0] * kPi / 180.0;
    case Fn::If: return a[0] != 0.0 ? a[1] : a[2];
    }
    return 0.0;
}

} // namespace

struct Program::Node
{
    Op op = Op::Num;
    double num = 0.0;
    int index = 0; ///< Var / Ref index
    Fn fn = Fn::Sin;
    std::vector<std::unique_ptr<Node>> kids;
};

Program::~Program() = default;

namespace {

double evalNode(const Program::Node& n, const Program::Inputs& in)
{
    using Node = Program::Node;
    const auto kid = [&](int i) { return evalNode(*n.kids[size_t(i)], in); };
    switch (n.op) {
    case Op::Num: return n.num;
    case Op::Var:
        switch (n.index) {
        case VarTime: return in.time;
        case VarValue: return in.value;
        case VarN1: return in.n[0];
        case VarN2: return in.n[1];
        case VarN3: return in.n[2];
        case VarPi: return kPi;
        case VarE: return 2.718281828459045;
        default: return in.vars ? in.vars[n.index - VarPixel] : 0.0;
        }
    case Op::Ref: return in.refs ? in.refs[n.index] : 0.0;
    case Op::Neg: return -kid(0);
    case Op::Not: return kid(0) == 0.0 ? 1.0 : 0.0;
    case Op::Add: return kid(0) + kid(1);
    case Op::Sub: return kid(0) - kid(1);
    case Op::Mul: return kid(0) * kid(1);
    case Op::Div: {
        const double d = kid(1);
        if (d == 0.0)
            return in.pixel ? 0.0 : std::numeric_limits<double>::quiet_NaN();
        return kid(0) / d;
    }
    case Op::Mod: {
        const double d = kid(1);
        if (d == 0.0)
            return in.pixel ? 0.0 : std::numeric_limits<double>::quiet_NaN();
        return std::fmod(kid(0), d);
    }
    case Op::Pow: return in.pixel ? pixelPow(kid(0), kid(1)) : std::pow(kid(0), kid(1));
    case Op::Lt: return kid(0) < kid(1) ? 1.0 : 0.0;
    case Op::Gt: return kid(0) > kid(1) ? 1.0 : 0.0;
    case Op::Le: return kid(0) <= kid(1) ? 1.0 : 0.0;
    case Op::Ge: return kid(0) >= kid(1) ? 1.0 : 0.0;
    case Op::Eq: return kid(0) == kid(1) ? 1.0 : 0.0;
    case Op::Ne: return kid(0) != kid(1) ? 1.0 : 0.0;
    case Op::And: return kid(0) != 0.0 && kid(1) != 0.0 ? 1.0 : 0.0;
    case Op::Or: return kid(0) != 0.0 || kid(1) != 0.0 ? 1.0 : 0.0;
    case Op::Cond: return kid(0) != 0.0 ? kid(1) : kid(2);
    case Op::Call: {
        double a[3] = {0, 0, 0};
        for (size_t i = 0; i < n.kids.size() && i < 3; ++i)
            a[i] = evalNode(*n.kids[i], in);
        return callFn(n.fn, a, in.pixel);
    }
    }
    return 0.0;
}

} // namespace

double Program::evaluate(const Inputs& in) const
{
    const double v = m_root ? evalNode(*m_root, in) : 0.0;
    return std::isfinite(v) ? v : 0.0;
}

double Program::evaluatePixel(double time, const double* vars) const
{
    Inputs in;
    in.time = time;
    in.vars = vars;
    in.pixel = true;
    const double v = m_root ? evalNode(*m_root, in) : 0.0;
    return std::isfinite(v) ? v : 0.0;
}

QStringList Program::functionNames()
{
    QStringList names;
    for (const FnInfo& f : kFunctions)
        names.append(QString::fromLatin1(f.name));
    return names;
}

/// Recursive descent parser. Throws nothing: the first error is stored in m_error.
class Parser
{
public:
    explicit Parser(const QString& text, const QStringList* pixelVars = nullptr)
        : m_t(text)
        , m_pixelVars(pixelVars)
    { }

    std::unique_ptr<Program::Node> parse(QStringList* refs, QString* error)
    {
        m_refs = refs;
        auto n = ternary(0);
        skip();
        if (m_error.isEmpty() && m_i < m_t.size())
            fail(QCoreApplication::translate("Expression", "Unexpected \"%1\"").arg(m_t.at(m_i)));
        if (!m_error.isEmpty()) {
            if (error)
                *error = m_error;
            return nullptr;
        }
        return n;
    }

private:
    using NodeP = std::unique_ptr<Program::Node>;

    void fail(const QString& why)
    {
        if (m_error.isEmpty())
            m_error = why;
    }
    void skip()
    {
        while (m_i < m_t.size() && m_t.at(m_i).isSpace())
            ++m_i;
    }
    bool take(const QString& s)
    {
        skip();
        if (m_t.mid(m_i, s.size()) == s) {
            m_i += s.size();
            return true;
        }
        return false;
    }
    static NodeP make(Op op)
    {
        auto n = std::make_unique<Program::Node>();
        n->op = op;
        return n;
    }
    NodeP binary(Op op, NodeP a, NodeP b)
    {
        auto n = make(op);
        n->kids.push_back(std::move(a));
        n->kids.push_back(std::move(b));
        return n;
    }

    NodeP ternary(int depth)
    {
        if (depth > kMaxDepth) {
            fail(QCoreApplication::translate("Expression", "The formula is nested too deeply"));
            return make(Op::Num);
        }
        NodeP c = logicOr(depth);
        if (take(QStringLiteral("?"))) {
            NodeP a = ternary(depth + 1);
            if (!take(QStringLiteral(":")))
                fail(QCoreApplication::translate("Expression", "Expected \":\""));
            NodeP b = ternary(depth + 1);
            auto n = make(Op::Cond);
            n->kids.push_back(std::move(c));
            n->kids.push_back(std::move(a));
            n->kids.push_back(std::move(b));
            return n;
        }
        return c;
    }
    NodeP logicOr(int d)
    {
        NodeP a = logicAnd(d);
        while (take(QStringLiteral("||")))
            a = binary(Op::Or, std::move(a), logicAnd(d));
        return a;
    }
    NodeP logicAnd(int d)
    {
        NodeP a = equality(d);
        while (take(QStringLiteral("&&")))
            a = binary(Op::And, std::move(a), equality(d));
        return a;
    }
    NodeP equality(int d)
    {
        NodeP a = relational(d);
        for (;;) {
            if (take(QStringLiteral("==")))
                a = binary(Op::Eq, std::move(a), relational(d));
            else if (take(QStringLiteral("!=")))
                a = binary(Op::Ne, std::move(a), relational(d));
            else
                return a;
        }
    }
    NodeP relational(int d)
    {
        NodeP a = additive(d);
        for (;;) {
            if (take(QStringLiteral("<=")))
                a = binary(Op::Le, std::move(a), additive(d));
            else if (take(QStringLiteral(">=")))
                a = binary(Op::Ge, std::move(a), additive(d));
            else if (take(QStringLiteral("<")))
                a = binary(Op::Lt, std::move(a), additive(d));
            else if (take(QStringLiteral(">")))
                a = binary(Op::Gt, std::move(a), additive(d));
            else
                return a;
        }
    }
    NodeP additive(int d)
    {
        NodeP a = term(d);
        for (;;) {
            if (take(QStringLiteral("+")))
                a = binary(Op::Add, std::move(a), term(d));
            else if (take(QStringLiteral("-")))
                a = binary(Op::Sub, std::move(a), term(d));
            else
                return a;
        }
    }
    NodeP term(int d)
    {
        NodeP a = unary(d);
        for (;;) {
            if (take(QStringLiteral("*")))
                a = binary(Op::Mul, std::move(a), unary(d));
            else if (take(QStringLiteral("/")))
                a = binary(Op::Div, std::move(a), unary(d));
            else if (take(QStringLiteral("%")))
                a = binary(Op::Mod, std::move(a), unary(d));
            else
                return a;
        }
    }
    NodeP unary(int d)
    {
        if (d > kMaxDepth) {
            fail(QCoreApplication::translate("Expression", "The formula is nested too deeply"));
            return make(Op::Num);
        }
        if (take(QStringLiteral("-"))) {
            auto n = make(Op::Neg);
            n->kids.push_back(unary(d + 1));
            return n;
        }
        if (take(QStringLiteral("+")))
            return unary(d + 1);
        if (take(QStringLiteral("!"))) {
            auto n = make(Op::Not);
            n->kids.push_back(unary(d + 1));
            return n;
        }
        return power(d);
    }
    NodeP power(int d)
    {
        NodeP a = primary(d);
        if (take(QStringLiteral("^")))
            return binary(Op::Pow, std::move(a), unary(d + 1)); // right associative
        return a;
    }
    NodeP primary(int d)
    {
        skip();
        if (m_i >= m_t.size()) {
            fail(QCoreApplication::translate("Expression", "The formula ends too early"));
            return make(Op::Num);
        }
        const QChar c = m_t.at(m_i);
        if (c == QLatin1Char('(')) {
            ++m_i;
            NodeP n = ternary(d + 1);
            if (!take(QStringLiteral(")")))
                fail(QCoreApplication::translate("Expression", "Expected \")\""));
            return n;
        }
        if (c.isDigit() || c == QLatin1Char('.')) {
            int j = m_i;
            while (j < m_t.size() && (m_t.at(j).isDigit() || m_t.at(j) == QLatin1Char('.')))
                ++j;
            if (j < m_t.size() && (m_t.at(j) == QLatin1Char('e') || m_t.at(j) == QLatin1Char('E'))) {
                int k = j + 1;
                if (k < m_t.size() && (m_t.at(k) == QLatin1Char('+') || m_t.at(k) == QLatin1Char('-')))
                    ++k;
                if (k < m_t.size() && m_t.at(k).isDigit()) {
                    while (k < m_t.size() && m_t.at(k).isDigit())
                        ++k;
                    j = k;
                }
            }
            bool ok = false;
            const double v = m_t.mid(m_i, j - m_i).toDouble(&ok);
            if (!ok)
                fail(QCoreApplication::translate("Expression", "Bad number \"%1\"").arg(m_t.mid(m_i, j - m_i)));
            m_i = j;
            auto n = make(Op::Num);
            n->num = v;
            return n;
        }
        if (c.isLetter() || c == QLatin1Char('_')) {
            int j = m_i;
            while (j < m_t.size()
                && (m_t.at(j).isLetterOrNumber() || m_t.at(j) == QLatin1Char('_')
                    || (m_t.at(j) == QLatin1Char('.') && j + 1 < m_t.size()
                        && (m_t.at(j + 1).isLetter() || m_t.at(j + 1) == QLatin1Char('_')))))
                ++j;
            const QString name = m_t.mid(m_i, j - m_i);
            m_i = j;
            skip();
            if (m_i < m_t.size() && m_t.at(m_i) == QLatin1Char('(')) {
                ++m_i;
                const FnInfo* info = nullptr;
                for (const FnInfo& f : kFunctions) {
                    if (name.compare(QLatin1String(f.name), Qt::CaseInsensitive) == 0)
                        info = &f;
                }
                if (!info) {
                    fail(QCoreApplication::translate("Expression", "Unknown function \"%1\"").arg(name));
                    return make(Op::Num);
                }
                if (m_pixelVars && (info->fn == Fn::Noise || info->fn == Fn::Rand)) {
                    fail(QCoreApplication::translate(
                             "Expression", "%1 is not available in a Custom Tool").arg(name));
                    return make(Op::Num);
                }
                auto n = make(Op::Call);
                n->fn = info->fn;
                skip();
                if (!take(QStringLiteral(")"))) {
                    for (;;) {
                        n->kids.push_back(ternary(d + 1));
                        if (take(QStringLiteral(",")))
                            continue;
                        if (!take(QStringLiteral(")")))
                            fail(QCoreApplication::translate("Expression", "Expected \")\""));
                        break;
                    }
                }
                if (int(n->kids.size()) != info->args)
                    fail(QCoreApplication::translate("Expression", "%1 takes %2 argument(s)")
                             .arg(name)
                             .arg(info->args));
                return n;
            }
            if (m_pixelVars) {
                const QString low = name.toLower();
                if (low == QLatin1String("pi") || low == QLatin1String("e")) {
                    auto n = make(Op::Var);
                    n->index = low == QLatin1String("pi") ? VarPi : VarE;
                    return n;
                }
                if (low == QLatin1String("frame"))
                    return pixelVar(m_pixelVars->indexOf(QStringLiteral("time")));
                for (int i = 0; i < m_pixelVars->size(); ++i) {
                    if (m_pixelVars->at(i).compare(name, Qt::CaseInsensitive) == 0)
                        return pixelVar(i);
                }
                fail(QCoreApplication::translate("Expression", "Unknown name \"%1\"").arg(name));
                return make(Op::Num);
            }
            static const QHash<QString, int> vars = {{QStringLiteral("time"), VarTime},
                {QStringLiteral("frame"), VarTime}, {QStringLiteral("value"), VarValue},
                {QStringLiteral("n1"), VarN1}, {QStringLiteral("n2"), VarN2},
                {QStringLiteral("n3"), VarN3}, {QStringLiteral("pi"), VarPi}, {QStringLiteral("e"), VarE}};
            const auto v = vars.constFind(name.toLower());
            if (v != vars.constEnd()) {
                auto n = make(Op::Var);
                n->index = *v;
                return n;
            }
            auto n = make(Op::Ref);
            int idx = m_refs->indexOf(name);
            if (idx < 0) {
                m_refs->append(name);
                idx = int(m_refs->size()) - 1;
            }
            n->index = idx;
            return n;
        }
        fail(QCoreApplication::translate("Expression", "Unexpected \"%1\"").arg(c));
        ++m_i;
        return make(Op::Num);
    }

    NodeP pixelVar(int i)
    {
        auto n = make(Op::Var);
        n->index = VarPixel + std::max(i, 0);
        return n;
    }

    QString m_t;
    int m_i = 0;
    QString m_error;
    QStringList* m_refs = nullptr;
    const QStringList* m_pixelVars = nullptr; ///< pixel programs: the variable names
};

std::shared_ptr<const Program> Program::compile(const QString& text, QString* error)
{
    static QMutex mutex;
    static QHash<QString, std::shared_ptr<const Program>> cache;
    const QString src = text.trimmed();
    if (src.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("Expression", "The formula is empty");
        return nullptr;
    }
    if (src.size() > kMaxLength) {
        if (error)
            *error = QCoreApplication::translate("Expression", "The formula is too long");
        return nullptr;
    }
    {
        QMutexLocker lock(&mutex);
        const auto it = cache.constFind(src);
        if (it != cache.constEnd())
            return *it;
    }
    std::shared_ptr<Program> p(new Program);
    Parser parser(src);
    QString why;
    p->m_root = parser.parse(&p->m_refs, &why);
    if (!p->m_root) {
        if (error)
            *error = why;
        return nullptr;
    }
    QMutexLocker lock(&mutex);
    if (cache.size() > 512)
        cache.clear();
    cache.insert(src, p);
    return p;
}

std::shared_ptr<const Program> Program::compilePixel(
    const QString& text, const QStringList& variables, QString* error)
{
    static QMutex mutex;
    static QHash<QString, std::shared_ptr<const Program>> cache;
    const QString src = text.trimmed();
    if (src.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("Expression", "The formula is empty");
        return nullptr;
    }
    if (src.size() > kMaxLength) {
        if (error)
            *error = QCoreApplication::translate("Expression", "The formula is too long");
        return nullptr;
    }
    const QString key = variables.join(QLatin1Char(',')) + QLatin1Char('|') + src;
    {
        QMutexLocker lock(&mutex);
        const auto it = cache.constFind(key);
        if (it != cache.constEnd())
            return *it;
    }
    std::shared_ptr<Program> p(new Program);
    p->m_pixel = true;
    Parser parser(src, &variables);
    QString why;
    p->m_root = parser.parse(&p->m_refs, &why);
    if (!p->m_root) {
        if (error)
            *error = why;
        return nullptr;
    }
    QMutexLocker lock(&mutex);
    if (cache.size() > 512)
        cache.clear();
    cache.insert(key, p);
    return p;
}

namespace {

QString glslNumber(double v)
{
    QString s = QString::number(v, 'g', 9);
    if (!s.contains(QLatin1Char('.')) && !s.contains(QLatin1Char('e')) && !s.contains(QLatin1Char('n')))
        s += QStringLiteral(".0");
    return s;
}

QString glslOf(const Program::Node& n, const QStringList& vars)
{
    const auto kid = [&](size_t i) { return glslOf(*n.kids[i], vars); };
    const auto bin = [&](const char* op) {
        return QStringLiteral("(%1 %2 %3)").arg(kid(0), QLatin1String(op), kid(1));
    };
    const auto cmp = [&](const char* op) {
        return QStringLiteral("((%1 %2 %3) ? 1.0 : 0.0)").arg(kid(0), QLatin1String(op), kid(1));
    };
    switch (n.op) {
    case Op::Num: return QStringLiteral("(%1)").arg(glslNumber(n.num));
    case Op::Var:
        if (n.index == VarPi)
            return QStringLiteral("(3.14159265)");
        if (n.index == VarE)
            return QStringLiteral("(2.71828183)");
        return QStringLiteral("v_%1").arg(vars.value(n.index - VarPixel));
    case Op::Ref: return QStringLiteral("0.0");
    case Op::Neg: return QStringLiteral("(-%1)").arg(kid(0));
    case Op::Not: return QStringLiteral("((%1 == 0.0) ? 1.0 : 0.0)").arg(kid(0));
    case Op::Add: return bin("+");
    case Op::Sub: return bin("-");
    case Op::Mul: return bin("*");
    case Op::Div: return QStringLiteral("kk_div(%1, %2)").arg(kid(0), kid(1));
    case Op::Mod: return QStringLiteral("kk_fmod(%1, %2)").arg(kid(0), kid(1));
    case Op::Pow: return QStringLiteral("kk_pow(%1, %2)").arg(kid(0), kid(1));
    case Op::Lt: return cmp("<");
    case Op::Gt: return cmp(">");
    case Op::Le: return cmp("<=");
    case Op::Ge: return cmp(">=");
    case Op::Eq: return cmp("==");
    case Op::Ne: return cmp("!=");
    case Op::And:
        return QStringLiteral("(((%1 != 0.0) && (%2 != 0.0)) ? 1.0 : 0.0)").arg(kid(0), kid(1));
    case Op::Or:
        return QStringLiteral("(((%1 != 0.0) || (%2 != 0.0)) ? 1.0 : 0.0)").arg(kid(0), kid(1));
    case Op::Cond:
        return QStringLiteral("((%1 != 0.0) ? %2 : %3)").arg(kid(0), kid(1), kid(2));
    case Op::Call: {
        QStringList args;
        for (size_t i = 0; i < n.kids.size(); ++i)
            args.append(kid(i));
        const QString a = args.join(QStringLiteral(", "));
        switch (n.fn) {
        case Fn::Sin: return QStringLiteral("sin(%1)").arg(a);
        case Fn::Cos: return QStringLiteral("cos(%1)").arg(a);
        case Fn::Tan: return QStringLiteral("tan(%1)").arg(a);
        case Fn::Asin: return QStringLiteral("asin(clamp(%1, -1.0, 1.0))").arg(a);
        case Fn::Acos: return QStringLiteral("acos(clamp(%1, -1.0, 1.0))").arg(a);
        case Fn::Atan: return QStringLiteral("atan(%1)").arg(a);
        case Fn::Atan2: return QStringLiteral("kk_atan2(%1)").arg(a);
        case Fn::Sqrt: return QStringLiteral("kk_sqrt(%1)").arg(a);
        case Fn::Abs: return QStringLiteral("abs(%1)").arg(a);
        case Fn::Pow: return QStringLiteral("kk_pow(%1)").arg(a);
        case Fn::Exp: return QStringLiteral("exp(min(%1, 80.0))").arg(a);
        case Fn::Log: return QStringLiteral("kk_log(%1)").arg(a);
        case Fn::Log10: return QStringLiteral("(kk_log(%1) * 0.4342944819)").arg(a);
        case Fn::Floor: return QStringLiteral("floor(%1)").arg(a);
        case Fn::Ceil: return QStringLiteral("ceil(%1)").arg(a);
        case Fn::Round: return QStringLiteral("kk_round(%1)").arg(a);
        case Fn::Fract: return QStringLiteral("kk_fract(%1)").arg(a);
        case Fn::Sign: return QStringLiteral("sign(%1)").arg(a);
        case Fn::Min: return QStringLiteral("min(%1)").arg(a);
        case Fn::Max: return QStringLiteral("max(%1)").arg(a);
        case Fn::Clamp: return QStringLiteral("kk_clamp(%1)").arg(a);
        case Fn::Mix: return QStringLiteral("mix(%1)").arg(a);
        case Fn::Step: return QStringLiteral("kk_step(%1)").arg(a);
        case Fn::Smoothstep: return QStringLiteral("kk_smoothstep(%1)").arg(a);
        case Fn::Fmod: return QStringLiteral("kk_fmod(%1)").arg(a);
        case Fn::Deg: return QStringLiteral("((%1) * 57.2957795)").arg(a);
        case Fn::Rad: return QStringLiteral("((%1) * 0.0174532925)").arg(a);
        case Fn::If: return QStringLiteral("kk_if(%1)").arg(a);
        case Fn::Noise:
        case Fn::Rand: break;
        }
        return QStringLiteral("0.0");
    }
    }
    return QStringLiteral("0.0");
}

} // namespace

QString Program::toGlsl(const QStringList& variables) const
{
    return QStringLiteral("kk_finite(%1)").arg(m_root ? glslOf(*m_root, variables) : QStringLiteral("0.0"));
}

QString Program::pixelGlslHelpers()
{
    return QStringLiteral(R"(
float kk_div(float a, float b) { return b == 0.0 ? 0.0 : a / b; }
float kk_fmod(float a, float b) { return b == 0.0 ? 0.0 : a - b * trunc(a / b); }
float kk_pow(float x, float y)
{
    if (x >= 0.0)
        return pow(x, y);
    float r = floor(y + 0.5);
    if (r != y)
        return 0.0;
    float p = pow(-x, y);
    return mod(abs(r), 2.0) == 1.0 ? -p : p;
}
float kk_atan2(float a, float b) { return (a == 0.0 && b == 0.0) ? 0.0 : atan(a, b); }
float kk_sqrt(float a) { return a > 0.0 ? sqrt(a) : 0.0; }
float kk_log(float a) { return a > 0.0 ? log(a) : 0.0; }
float kk_round(float x) { return x >= 0.0 ? floor(x + 0.5) : -floor(-x + 0.5); }
float kk_fract(float x) { return x - floor(x); }
float kk_clamp(float x, float a, float b) { return clamp(x, min(a, b), max(a, b)); }
float kk_step(float e, float x) { return x < e ? 0.0 : 1.0; }
float kk_smoothstep(float a, float b, float x)
{
    float w = b - a;
    float t = w == 0.0 ? (x < a ? 0.0 : 1.0) : clamp((x - a) / w, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
float kk_if(float c, float a, float b) { return c != 0.0 ? a : b; }
float kk_finite(float x) { return (x == x && abs(x) < 3.0e38) ? x : 0.0; }
)");
}

} // namespace kk::expr
