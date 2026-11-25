// SPDX-License-Identifier: GPL-3.0-or-later
// PluginShader — see PluginShader.h.
#include "render/PluginShader.h"

#include "render/GlEffectBase.h"
#include "render/GlShaderEffect.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QRegularExpression>
#include <QSet>

namespace kk::render::pluginshader {

namespace {

using plugins::ParamKind;
using plugins::PluginParam;
using plugins::Problem;

/// GLSL declarations of the parameter uniforms: Number float, Integer / Choice int, Boolean bool,
/// Color vec4 (straight RGBA, 0..1). Strings are not passed to shaders.
std::string paramUniforms(const QVector<PluginParam>& params)
{
    std::string out;
    for (const PluginParam& p : params) {
        const char* type = nullptr;
        switch (p.type) {
        case ParamKind::Number:
            type = "float";
            break;
        case ParamKind::Integer:
        case ParamKind::Choice:
            type = "int";
            break;
        case ParamKind::Boolean:
            type = "bool";
            break;
        case ParamKind::Color:
            type = "vec4";
            break;
        case ParamKind::String:
            break;
        }
        if (type)
            out += std::string("uniform ") + type + " " + uniformName(p.key).toStdString() + ";\n";
    }
    return out;
}

constexpr char kEffectPrelude[] = R"(
// ---- Kinoklip plugin prelude (not part of your file) ----
uniform sampler2D uTex;   // the clip's picture, premultiplied RGBA
uniform vec2 uResolution; // layer size in pixels
uniform float uScale;     // layer pixels per timeline pixel
uniform float uTime;      // seconds since the start of the clip
uniform float uFrame;     // frames since the start of the clip
uniform float uFps;       // timeline frame rate
uniform vec4 uContent;    // picture rectangle in uv: x0, y0, x1, y1 (the layer minus padding)
)";

constexpr char kTransitionPrelude[] = R"(
// ---- Kinoklip plugin prelude (not part of your file) ----
uniform sampler2D uA;     // outgoing picture, premultiplied RGBA
uniform sampler2D uB;     // incoming picture, premultiplied RGBA
uniform float uProgress;  // 0 = all A, 1 = all B
uniform vec2 uResolution; // output size in pixels
#define progress uProgress
#define ratio (uResolution.x / uResolution.y)
// gl-transitions convention: uv has its origin at the bottom left.
vec4 getFromColor(vec2 uv) { return texture(uA, vec2(uv.x, 1.0 - uv.y)); }
vec4 getToColor(vec2 uv) { return texture(uB, vec2(uv.x, 1.0 - uv.y)); }
)";

QString stripComments(const QString& text)
{
    // Comments and string-like text become spaces; newlines stay so line numbers hold.
    QString out = text;
    bool line = false;
    bool block = false;
    for (qsizetype i = 0; i < out.size(); ++i) {
        const QChar c = out[i];
        const QChar next = i + 1 < out.size() ? out[i + 1] : QChar();
        if (line) {
            if (c == QLatin1Char('\n'))
                line = false;
            else
                out[i] = QLatin1Char(' ');
        } else if (block) {
            if (c == QLatin1Char('*') && next == QLatin1Char('/')) {
                out[i] = out[i + 1] = QLatin1Char(' ');
                ++i;
                block = false;
            } else if (c != QLatin1Char('\n')) {
                out[i] = QLatin1Char(' ');
            }
        } else if (c == QLatin1Char('/') && next == QLatin1Char('/')) {
            line = true;
            out[i] = out[i + 1] = QLatin1Char(' ');
            ++i;
        } else if (c == QLatin1Char('/') && next == QLatin1Char('*')) {
            block = true;
            out[i] = out[i + 1] = QLatin1Char(' ');
            ++i;
        }
    }
    return out;
}

} // namespace

QString uniformName(const QString& key)
{
    return QStringLiteral("u_") + key;
}

std::string composeEffect(const plugins::PluginShaderSpec& spec)
{
    std::string s = fx::kGlslCommon;
    s += fx::kGlslHash;
    s += kEffectPrelude;
    s += paramUniforms(spec.params);
    s += spec.entry == QLatin1String("color") ? "vec3 apply(vec3 c);\n" : "vec4 effect(vec2 uv);\n";
    s += "#line 1\n";
    s += spec.source.toStdString();
    s += "\n";
    if (spec.entry == QLatin1String("color")) {
        // The same wrapper as the built-in colour effects: straight colour in, straight out,
        // transparent stays transparent, premultiplied again.
        s += "void main() {\n"
             "    vec4 t = texture(uTex, vUv);\n"
             "    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }\n"
             "    fragColor = vec4(clamp(apply(straight(t)), 0.0, 1.0) * t.a, t.a);\n"
             "}\n";
    } else {
        s += "void main() { fragColor = effect(vUv); }\n";
    }
    return s;
}

std::string composeTransition(const plugins::PluginShaderSpec& spec)
{
    std::string s = fx::kGlslCommon;
    s += fx::kGlslHash;
    s += kTransitionPrelude;
    s += paramUniforms(spec.params);
    s += "vec4 transition(vec2 uv);\n";
    s += "#line 1\n";
    s += spec.source.toStdString();
    s += "\nvoid main() { fragColor = transition(vec2(vUv.x, 1.0 - vUv.y)); }\n";
    return s;
}

std::string compose(const plugins::PluginShaderSpec& spec)
{
    return spec.transition ? composeTransition(spec) : composeEffect(spec);
}

QVector<Problem> lint(const plugins::PluginShaderSpec& spec)
{
    QVector<Problem> out;
    const QString clean = stripComments(spec.source);
    const QStringList lines = clean.split(QLatin1Char('\n'));
    const auto add = [&](int line, const QString& message, bool warning = false) {
        out.append({spec.file, message, line, warning});
    };
    // Words GLSL ES 3.00 or desktop GLSL 3.30 reserve (they may not be used as names); `patch`
    // and `sample` are the usual accidents.
    static const QSet<QString> reserved
        = {QStringLiteral("patch"), QStringLiteral("sample"), QStringLiteral("subroutine"),
            QStringLiteral("common"), QStringLiteral("partition"), QStringLiteral("active"),
            QStringLiteral("asm"), QStringLiteral("class"), QStringLiteral("union"),
            QStringLiteral("enum"), QStringLiteral("typedef"), QStringLiteral("template"),
            QStringLiteral("this"), QStringLiteral("resource"), QStringLiteral("goto"),
            QStringLiteral("inline"), QStringLiteral("noinline"), QStringLiteral("public"),
            QStringLiteral("static"), QStringLiteral("extern"), QStringLiteral("external"),
            QStringLiteral("interface"), QStringLiteral("long"), QStringLiteral("short"),
            QStringLiteral("double"), QStringLiteral("half"), QStringLiteral("fixed"),
            QStringLiteral("unsigned"), QStringLiteral("superp"), QStringLiteral("input"),
            QStringLiteral("output"), QStringLiteral("filter"), QStringLiteral("sizeof"),
            QStringLiteral("cast"), QStringLiteral("namespace"), QStringLiteral("using")};
    static const QRegularExpression word(QStringLiteral("\\b[A-Za-z_][A-Za-z0-9_]*\\b"));
    static const QRegularExpression version(QStringLiteral("^\\s*#\\s*version\\b"));
    static const QRegularExpression extension(QStringLiteral("^\\s*#\\s*extension\\b"));
    static const QRegularExpression legacy(
        QStringLiteral("\\b(gl_FragColor|gl_FragData|texture2D|texture3D|textureCube|varying|"
                       "attribute|gl_TexCoord|gl_Color)\\b"));
    static const QRegularExpression mainFn(QStringLiteral("\\bvoid\\s+main\\s*\\("));
    static const QRegularExpression uniformParam(
        QStringLiteral("\\buniform\\s+\\w+\\s+(u_\\w+|uTex|uA|uB|uProgress|uResolution|uScale|"
                       "uTime|uFrame|uFps|uContent)\\b"));
    static const QRegularExpression intToFloat(
        QStringLiteral("\\b(float|vec[234])\\s+\\w+\\s*=\\s*[0-9]+\\s*;"));
    static const QRegularExpression floatLiteralSuffix(
        QStringLiteral("\\b[0-9]+\\.?[0-9]*[fF]\\b"));
    static const QRegularExpression doubleType(QStringLiteral("\\b(dvec[234]|dmat[234])\\b"));
    static const QRegularExpression layoutLoc(QStringLiteral("\\blayout\\s*\\("));
    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines[i];
        const int n = i + 1;
        if (version.match(line).hasMatch())
            add(n, QStringLiteral("remove #version: Kinoklip adds it (330 core or 300 es)"));
        if (extension.match(line).hasMatch())
            add(n,
                QStringLiteral("#extension does not work on every GPU (and not on OpenGL ES "
                               "3.0): avoid it"),
                true);
        if (const auto m = legacy.match(line); m.hasMatch())
            add(n,
                QStringLiteral("%1 is old GLSL: use texture() and write the result by "
                               "returning it from your entry function")
                    .arg(m.captured(1)));
        if (mainFn.match(line).hasMatch())
            add(n,
                QStringLiteral("do not define main(): Kinoklip adds it and calls your "
                               "entry function (effect / apply / transition)"));
        if (const auto m = uniformParam.match(line); m.hasMatch())
            add(n,
                QStringLiteral("do not declare the uniform %1: Kinoklip declares the "
                               "standard and the parameter uniforms for you")
                    .arg(m.captured(1)));
        if (intToFloat.match(line).hasMatch())
            add(n, QStringLiteral("OpenGL ES does not convert an int to a float: write 1.0, not 1"),
                true);
        if (floatLiteralSuffix.match(line).hasMatch())
            add(n,
                QStringLiteral("the 'f' suffix of a float literal is not allowed in GLSL ES 3.00"));
        if (doubleType.match(line).hasMatch())
            add(n, QStringLiteral("double precision types do not exist on OpenGL ES"));
        if (layoutLoc.match(line).hasMatch())
            add(n, QStringLiteral("layout(...) qualifiers are not needed in a plugin shader"),
                true);
        QRegularExpressionMatchIterator it = word.globalMatch(line);
        while (it.hasNext()) {
            const QString w = it.next().captured(0);
            if (reserved.contains(w))
                add(n,
                    QStringLiteral("\"%1\" is a reserved word in GLSL (it breaks on OpenGL ES "
                                   "or desktop GL): use another name")
                        .arg(w));
        }
    }
    // The entry function.
    static const QRegularExpression effectFn(
        QStringLiteral("\\bvec4\\s+effect\\s*\\(\\s*vec2\\s+\\w+\\s*\\)\\s*\\{"));
    static const QRegularExpression colorFn(
        QStringLiteral("\\bvec3\\s+apply\\s*\\(\\s*vec3\\s+\\w+\\s*\\)\\s*\\{"));
    static const QRegularExpression transitionFn(
        QStringLiteral("\\bvec4\\s+transition\\s*\\(\\s*vec2\\s+\\w+\\s*\\)\\s*\\{"));
    if (spec.transition) {
        if (!transitionFn.match(clean).hasMatch())
            add(0, QStringLiteral("define the entry function: vec4 transition(vec2 uv) { ... }"));
    } else if (spec.entry == QLatin1String("color")) {
        if (!colorFn.match(clean).hasMatch())
            add(0,
                QStringLiteral("define the entry function: vec3 apply(vec3 c) { ... } "
                               "(entry \"color\")"));
    } else if (!effectFn.match(clean).hasMatch()) {
        add(0,
            QStringLiteral("define the entry function: vec4 effect(vec2 uv) { ... } "
                           "(entry \"main\")"));
    }
    return out;
}

QVector<Problem> parseCompileLog(const QString& log, const QString& file, int userLineCount)
{
    QVector<Problem> out;
    // Mesa "0:12(5): error: ...", ANGLE / glslang "ERROR: 0:12: '...' : ...",
    // NVIDIA "0(12) : error C1008: ...".
    static const QRegularExpression colon(
        QStringLiteral("(?:ERROR:\\s*|WARNING:\\s*)?\\b(\\d+):(\\d+)(?:\\(\\d+\\))?:\\s*(.*)"));
    static const QRegularExpression paren(QStringLiteral("\\b(\\d+)\\((\\d+)\\)\\s*:\\s*(.*)"));
    for (const QString& raw : log.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        int number = -1;
        QString message = line;
        if (const auto m = colon.match(line); m.hasMatch()) {
            number = m.captured(2).toInt();
            message = m.captured(3);
        } else if (const auto m2 = paren.match(line); m2.hasMatch()) {
            number = m2.captured(2).toInt();
            message = m2.captured(3);
        }
        if (number > userLineCount || number < 0) {
            if (number > userLineCount)
                message += QStringLiteral(" (in the code Kinoklip adds after your file: check "
                                          "that the entry function exists and has the right "
                                          "signature)");
            number = 0;
        }
        out.append({file, message, number});
    }
    return out;
}

CompileCheck compileOnContext(const plugins::PluginShaderSpec& spec, GlApi api)
{
    CompileCheck check;
    check.api = api;
    if (!QGuiApplication::instance()) {
        check.note = QStringLiteral("no application object");
        return check;
    }
    QOpenGLContext context;
    context.setFormat(glFormat(api));
    if (!context.create()) {
        check.note = QStringLiteral("no %1 context on this machine").arg(glApiName(api));
        return check;
    }
    const bool es = context.isOpenGLES();
    if ((api == GlApi::Gles) != es) {
        check.note = QStringLiteral("no %1 context on this machine").arg(glApiName(api));
        return check;
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface)) {
        check.note = QStringLiteral("cannot make an OpenGL context current (headless session "
                                    "without OpenGL: run under xvfb-run)");
        return check;
    }
    check.attempted = true;
    QOpenGLExtraFunctions gl(&context);
    GlProgram program;
    QString log;
    const std::string source = compose(spec);
    check.compiled = program.build(&gl, spec.fullId.toUtf8().constData(), source.c_str(), &log);
    if (check.compiled) {
        program.destroy(&gl);
    } else {
        check.note = log;
        const int lines = spec.source.count(QLatin1Char('\n')) + 1;
        check.problems = parseCompileLog(log, spec.file, lines);
    }
    context.doneCurrent();
    return check;
}

} // namespace kk::render::pluginshader
