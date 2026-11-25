// SPDX-License-Identifier: GPL-3.0-or-later
// Commands of the plugin system (docs/plugins/writing-plugins.md, "Commands for AI assistants"):
// plugin.list, plugin.info, plugin.create, plugin.copy_builtin, plugin.validate, plugin.install,
// plugin.uninstall, plugin.enable, plugin.disable, plugin.reload, plugin.pack,
// plugin.render_preview. They need no project; the command line, the MCP server and the Plugins
// dialog's own buttons all use the same PluginRegistry.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "app/commands/ParamSupport.h"
#include "core/EffectRegistry.h"
#include "plugins/PluginRegistry.h"
#include "render/PluginPreview.h"
#include "render/PluginValidate.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <algorithm>

namespace kk::commands {

namespace {

using plugins::LoadedPlugin;
using plugins::PluginRegistry;

PluginRegistry& registry()
{
    PluginRegistry& r = PluginRegistry::instance();
    r.load();
    return r;
}

QString jsonString(const QString& text)
{
    QString out
        = QString::fromUtf8(QJsonDocument(QJsonArray {text}).toJson(QJsonDocument::Compact));
    return out.mid(1, out.size() - 2);
}

QJsonObject problemJson(const plugins::Problem& p)
{
    QJsonObject o {{QStringLiteral("where"), p.where}, {QStringLiteral("message"), p.message}};
    if (p.line > 0)
        o.insert(QStringLiteral("line"), p.line);
    if (p.warning)
        o.insert(QStringLiteral("warning"), true);
    return o;
}

QJsonArray problemsJson(const QVector<plugins::Problem>& problems)
{
    QJsonArray a;
    for (const plugins::Problem& p : problems)
        a.append(problemJson(p));
    return a;
}

QJsonObject pluginSummary(const LoadedPlugin& p)
{
    QJsonObject o {{QStringLiteral("id"), p.id()},
        {QStringLiteral("name"), p.valid ? p.manifest.name : p.folderName},
        {QStringLiteral("version"), p.manifest.version},
        {QStringLiteral("author"), p.manifest.author},
        {QStringLiteral("license"), p.manifest.license},
        {QStringLiteral("source"),
            p.source == plugins::PluginSource::Bundled ? QStringLiteral("bundled")
                                                       : QStringLiteral("user")},
        {QStringLiteral("enabled"), p.enabled}, {QStringLiteral("valid"), p.valid},
        {QStringLiteral("kinds"), QJsonArray::fromStringList(p.manifest.kinds())},
        {QStringLiteral("folder"), p.dir}};
    if (!p.valid)
        o.insert(QStringLiteral("problems"), problemsJson(p.problems));
    return o;
}

// ---- Scaffolding ----

struct ScaffoldParam
{
    QString key;
    QString type;
    QString min, max, def;
    QString label;
    QStringList choices;
};

/// "key:type:min:max:default[:label]" and the other forms (see the command's description).
bool parseParamSpec(const QString& text, ScaffoldParam& out, QString* error)
{
    const QStringList f = text.split(QLatin1Char(':'));
    auto bad = [&](const QString& why) {
        *error = QStringLiteral("param \"%1\": %2").arg(text, why);
        return false;
    };
    if (f.size() < 3)
        return bad(QStringLiteral("use key:type:... (number key:number:min:max:default, "
                                  "boolean key:boolean:true, color key:color:#rrggbb, choice "
                                  "key:choice:a|b|c:default)"));
    out.key = f[0];
    out.type = f[1];
    if (!plugins::isValidParamKey(out.key))
        return bad(QStringLiteral("the key must start with a letter and have letters and digits "
                                  "only"));
    const auto number = [](const QString& s) {
        bool ok = false;
        s.toDouble(&ok);
        return ok;
    };
    if (out.type == QLatin1String("number") || out.type == QLatin1String("integer")) {
        if (f.size() < 5 || !number(f[2]) || !number(f[3]) || !number(f[4]))
            return bad(QStringLiteral("a number needs key:number:min:max:default"));
        out.min = f[2];
        out.max = f[3];
        out.def = f[4];
        out.label = f.value(5);
    } else if (out.type == QLatin1String("boolean")) {
        out.def = f[2] == QLatin1String("true") ? QStringLiteral("true") : QStringLiteral("false");
        out.label = f.value(3);
    } else if (out.type == QLatin1String("color")) {
        out.def = f[2];
        if (!QColor::fromString(out.def).isValid())
            return bad(QStringLiteral("colour as #rrggbb"));
        out.label = f.value(3);
    } else if (out.type == QLatin1String("choice")) {
        out.choices = f[2].split(QLatin1Char('|'), Qt::SkipEmptyParts);
        out.def = f.value(3);
        if (out.choices.isEmpty() || !out.choices.contains(out.def))
            return bad(QStringLiteral("a choice needs key:choice:a|b|c:default (default one of "
                                      "the list)"));
        out.label = f.value(4);
    } else {
        return bad(QStringLiteral("type must be number, integer, boolean, color or choice"));
    }
    if (out.label.isEmpty()) {
        out.label = out.key;
        out.label[0] = out.label[0].toUpper();
    }
    return true;
}

QString paramJson(const ScaffoldParam& p, const QString& indent)
{
    QStringList lines;
    lines << QStringLiteral("\"key\": %1").arg(jsonString(p.key));
    lines << QStringLiteral("\"label\": %1").arg(jsonString(p.label));
    lines << QStringLiteral("\"type\": %1").arg(jsonString(p.type));
    if (p.type == QLatin1String("number") || p.type == QLatin1String("integer")) {
        lines << QStringLiteral("\"min\": %1").arg(p.min);
        lines << QStringLiteral("\"max\": %1").arg(p.max);
        lines << QStringLiteral("\"default\": %1").arg(p.def);
    } else if (p.type == QLatin1String("boolean")) {
        lines << QStringLiteral("\"default\": %1").arg(p.def);
    } else if (p.type == QLatin1String("color")) {
        lines << QStringLiteral("\"default\": %1").arg(jsonString(p.def));
    } else {
        QStringList c;
        for (const QString& v : p.choices)
            c << jsonString(v);
        lines << QStringLiteral("\"choices\": [%1]").arg(c.join(QStringLiteral(", ")));
        lines << QStringLiteral("\"default\": %1").arg(jsonString(p.def));
    }
    return indent + QStringLiteral("{ ") + lines.join(QStringLiteral(", ")) + QStringLiteral(" }");
}

QString uniformComment(const QVector<ScaffoldParam>& params)
{
    if (params.isEmpty())
        return QStringLiteral("// This effect has no parameters yet: add them to plugin.json.\n");
    QString out = QStringLiteral("// Parameters (Kinoklip declares these uniforms for you):\n");
    for (const ScaffoldParam& p : params) {
        QString type = p.type == QLatin1String("number") ? QStringLiteral("float")
            : p.type == QLatin1String("color")   ? QStringLiteral("vec4, straight RGBA 0..1")
            : p.type == QLatin1String("boolean") ? QStringLiteral("bool")
                                                 : QStringLiteral("int");
        out += QStringLiteral("//   u_%1  (%2)\n").arg(p.key, type);
    }
    return out;
}

bool writeFile(const QString& path, const QString& text, QString* error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(text.toUtf8()) < 0) {
        *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    return true;
}

QString slug(const QString& name)
{
    QString s;
    for (const QChar c : name.toLower())
        s += (c.isLetterOrNumber() && c.unicode() < 128) ? c : QChar::fromLatin1('_');
    s.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    while (s.startsWith(QLatin1Char('_')))
        s.remove(0, 1);
    while (s.endsWith(QLatin1Char('_')))
        s.chop(1);
    return s.isEmpty() ? QStringLiteral("item") : s.left(40);
}

CommandResult runCreate(CommandContext&, const CommandArgs& args)
{
    PluginRegistry& reg = registry();
    const QString id = args.string(QStringLiteral("id"));
    if (!plugins::isValidPluginId(id))
        return fail("bad_value",
            QStringLiteral("\"%1\" is not a plugin id: 2 to 64 characters, lower case letters, "
                           "digits, '.', '-', '_' (for example com.example.tint)")
                .arg(id));
    const QString kind = args.string(QStringLiteral("kind"), QStringLiteral("effect"));
    const QString name = args.string(QStringLiteral("name"), id);
    const QString item
        = args.has(QStringLiteral("item")) ? args.string(QStringLiteral("item")) : slug(name);
    if (!plugins::isValidItemId(item))
        return fail("bad_value",
            QStringLiteral("item \"%1\": lower case letters, digits and _ only").arg(item));
    const QString entry = args.string(QStringLiteral("entry"), QStringLiteral("main"));
    const QString description = args.string(QStringLiteral("description"), name);
    const QString author = args.string(QStringLiteral("author"), QStringLiteral("Unknown"));
    const QString license
        = args.string(QStringLiteral("license"), QStringLiteral("GPL-3.0-or-later"));
    const QString group = args.string(QStringLiteral("group"));
    QVector<ScaffoldParam> params;
    for (const QString& text : args.strings(QStringLiteral("param"))) {
        ScaffoldParam p;
        QString error;
        if (!parseParamSpec(text, p, &error))
            return fail("bad_value", error);
        params.append(p);
    }
    const QString dir = reg.userDir() + QLatin1Char('/') + id;
    if (QFileInfo::exists(dir) && !args.boolean(QStringLiteral("overwrite")))
        return fail("exists",
            QStringLiteral("%1 already exists: pick another id, edit it, or pass overwrite")
                .arg(dir));
    if (const LoadedPlugin* clash = reg.find(id);
        clash && clash->source == plugins::PluginSource::Bundled)
        return fail("exists", QStringLiteral("a bundled plugin has the id \"%1\"").arg(id));

    QStringList paramLines;
    for (const ScaffoldParam& p : params)
        paramLines << paramJson(p, QStringLiteral("        "));
    const QString paramsBlock = params.isEmpty()
        ? QString()
        : QStringLiteral(",\n      \"params\": [\n%1\n      ]")
              .arg(paramLines.join(QStringLiteral(",\n")));

    QString fileName;
    QString fileText;
    QString section;
    const QString groupLine
        = group.isEmpty() ? QString() : QStringLiteral("\"group\": %1, ").arg(jsonString(group));
    if (kind == QLatin1String("effect")) {
        fileName = QStringLiteral("%1.glsl").arg(item);
        const bool color = entry == QLatin1String("color");
        fileText = QStringLiteral("// %1\n// Effect shader. Kinoklip adds the version line, the "
                                  "uniforms and main().\n")
                       .arg(name)
            + QStringLiteral("// Standard uniforms: uTex (the picture, premultiplied RGBA), "
                             "uResolution, uScale, uTime, uFrame, uFps, uContent.\n")
            + uniformComment(params);
        if (color)
            fileText += QStringLiteral(
                "\n// entry \"color\": c is the straight (not premultiplied) colour 0..1; return "
                "the new colour.\nvec3 apply(vec3 c)\n{\n    return c;\n}\n");
        else
            fileText
                += QStringLiteral("\n// entry \"main\": uv is the texture coordinate (0,0 = top "
                                  "left). Return premultiplied RGBA.\nvec4 effect(vec2 uv)\n{\n    "
                                  "vec4 c = texture(uTex, uv);\n    return c;\n}\n");
        section = QStringLiteral(
            ",\n  \"effects\": [\n    {\n      \"id\": %1,\n      \"name\": %2,\n      "
            "\"description\": %3,\n      %4\"entry\": %5,\n      \"file\": %6%7\n    }\n  ]")
                      .arg(jsonString(item), jsonString(name), jsonString(description),
                          groupLine.isEmpty() ? QString() : groupLine + QStringLiteral("\n      "),
                          jsonString(color ? QStringLiteral("color") : QStringLiteral("main")),
                          jsonString(fileName), paramsBlock);
    } else if (kind == QLatin1String("transition")) {
        fileName = QStringLiteral("%1.glsl").arg(item);
        fileText = QStringLiteral("// %1\n// Transition shader (gl-transitions style). Kinoklip "
                                  "adds the version line, the uniforms and main().\n")
                       .arg(name)
            + QStringLiteral(
                "// Available: progress (0..1), ratio (width / height), getFromColor(uv), "
                "getToColor(uv); uv has its origin at the bottom left.\n")
            + uniformComment(params)
            + QStringLiteral("\nvec4 transition(vec2 uv)\n{\n    return mix(getFromColor(uv), "
                             "getToColor(uv), progress);\n}\n");
        section
            = QStringLiteral(",\n  \"transitions\": [\n    {\n      \"id\": %1,\n      \"name\": "
                             "%2,\n      \"description\": %3,\n      %4\"file\": %5%6\n    }\n  ]")
                  .arg(jsonString(item), jsonString(name), jsonString(description),
                      groupLine.isEmpty() ? QString() : groupLine + QStringLiteral("\n      "),
                      jsonString(fileName), paramsBlock);
    } else if (kind == QLatin1String("preset")) {
        fileName = QStringLiteral("%1.json").arg(item);
        fileText
            = QStringLiteral("{\n  \"commands\": [\n    {\n      \"command\": \"effect.add\",\n    "
                             "  \"args\": { \"clip\": \"$clip\", \"type\": \"gaussian_blur\", "
                             "\"set\": [\"radius=12\"] }\n    }\n  ]\n}\n");
        section = QStringLiteral(
            ",\n  \"presets\": [\n    {\n      \"id\": %1,\n      \"name\": %2,\n      "
            "\"description\": %3,\n      \"target\": \"clip\",\n      \"file\": %4\n    }\n  ]")
                      .arg(jsonString(item), jsonString(name), jsonString(description),
                          jsonString(fileName));
    } else if (kind == QLatin1String("script")) {
        fileName = QStringLiteral("%1.py").arg(item);
        fileText = QStringLiteral(
            "# %1\n# Runs as python3 when you choose it in Workspace > Scripts (after you enabled "
            "the plugin).\nimport kinoklip\n\nclips = kinoklip.call(\"clip.list\").get(\"clips\", "
            "[])\nkinoklip.log(\"%2: %3 clips on the timeline\" % (%4, len(clips)))\n")
                       .arg(name, QStringLiteral("%s"), QStringLiteral("%d"), jsonString(name));
        section = QStringLiteral(
            ",\n  \"permissions\": [\"project.read\"],\n  \"scripts\": [\n    {\n      \"id\": "
            "%1,\n      \"name\": %2,\n      \"description\": %3,\n      \"file\": %4,\n      "
            "\"permissions\": [\"project.read\"]\n    }\n  ]")
                      .arg(jsonString(item), jsonString(name), jsonString(description),
                          jsonString(fileName));
    } else {
        return fail(
            "bad_value", QStringLiteral("kind must be effect, transition, preset or script"));
    }
    const QString manifest
        = QStringLiteral(
            "{\n  \"id\": %1,\n  \"name\": %2,\n  \"version\": \"1.0.0\",\n  \"author\": %3,\n  "
            "\"license\": %4,\n  \"description\": %5,\n  \"api\": %6%7\n}\n")
              .arg(jsonString(id), jsonString(name), jsonString(author), jsonString(license),
                  jsonString(description), QString::number(plugins::kApiVersion), section);
    QString error;
    if (!writeFile(dir + QStringLiteral("/plugin.json"), manifest, &error)
        || !writeFile(dir + QLatin1Char('/') + fileName, fileText, &error))
        return fail("write_failed", error);
    reg.reload();
    const LoadedPlugin* made = reg.find(id);
    QJsonObject data {{QStringLiteral("id"), id}, {QStringLiteral("folder"), dir},
        {QStringLiteral("files"),
            QJsonArray {dir + QStringLiteral("/plugin.json"), dir + QLatin1Char('/') + fileName}},
        {QStringLiteral("itemId"), plugins::fullId(id, item)},
        {QStringLiteral("enabled"), made && made->enabled}};
    return CommandResult::success(data,
        QStringLiteral(
            "created %1 (%2) in %3: edit %4, then plugin.reload and plugin.render_preview")
            .arg(id, kind, dir, fileName));
}

CommandResult runList(CommandContext&, const CommandArgs&)
{
    QJsonArray list;
    QString text;
    for (const LoadedPlugin& p : registry().plugins()) {
        list.append(pluginSummary(p));
        text += QStringLiteral("\n  %1  %2 %3  [%4]%5")
                    .arg(p.id(), -34)
                    .arg(p.valid ? p.manifest.name : QStringLiteral("(broken)"), p.manifest.version,
                        p.source == plugins::PluginSource::Bundled ? QStringLiteral("bundled")
                                                                   : QStringLiteral("user"),
                        p.valid ? (p.enabled ? QString() : QStringLiteral("  off"))
                                : QStringLiteral("  has problems"));
    }
    return CommandResult::success(
        {{QStringLiteral("plugins"), list}, {QStringLiteral("userFolder"), registry().userDir()}},
        QStringLiteral("%1 plugin(s)%2").arg(list.size()).arg(text));
}

CommandResult runInfo(CommandContext&, const CommandArgs& args)
{
    const QString id = args.string(QStringLiteral("id"));
    const LoadedPlugin* p = registry().find(id);
    if (!p)
        return fail("unknown_plugin", QStringLiteral("no plugin \"%1\" (see plugin.list)").arg(id));
    QJsonObject o = pluginSummary(*p);
    o.insert(QStringLiteral("problems"), problemsJson(p->problems));
    o.insert(QStringLiteral("description"), p->manifest.description);
    o.insert(QStringLiteral("permissions"), QJsonArray::fromStringList(p->manifest.permissions));
    QJsonArray items;
    const auto add = [&](const QString& kind, const QString& fullId, const QString& name,
                         const QString& file) {
        QJsonObject item {{QStringLiteral("kind"), kind}, {QStringLiteral("id"), fullId},
            {QStringLiteral("name"), name}, {QStringLiteral("file"), file}};
        const QString err = registry().shaderError(fullId);
        if (!err.isEmpty())
            item.insert(QStringLiteral("compileError"), err);
        items.append(item);
    };
    for (const auto& e : p->manifest.effects)
        add(QStringLiteral("effect"), e.fullId, e.name, e.file);
    for (const auto& t : p->manifest.transitions)
        add(QStringLiteral("transition"), t.fullId, t.name, t.file);
    for (const auto& pr : p->manifest.presets)
        add(QStringLiteral("preset"), pr.fullId, pr.name, pr.file);
    for (const auto& s : p->manifest.scripts)
        add(QStringLiteral("script"), s.fullId, s.name, s.file);
    o.insert(QStringLiteral("items"), items);
    o.insert(QStringLiteral("manifest"), p->manifest.raw);
    return CommandResult::success(o,
        QStringLiteral("%1 %2 by %3 (%4): %5 item(s), %6 problem(s)")
            .arg(p->manifest.name, p->manifest.version, p->manifest.author, p->manifest.license)
            .arg(items.size())
            .arg(p->problems.size()));
}

/// Runs `kinoklip plugin <verb> <options>` as a child under xvfb-run (a session without a display
/// has no OpenGL: the GL checks of validate and render_preview need one). Returns false when
/// that is not possible (no xvfb-run, or this already is the child).
bool runUnderXvfb(const QStringList& cliArguments, QByteArray* output)
{
    if (!qEnvironmentVariableIsEmpty("KK_NO_XVFB"))
        return false;
    const QString xvfb = QStandardPaths::findExecutable(QStringLiteral("xvfb-run"));
    if (xvfb.isEmpty())
        return false;
    const QByteArray appImage = qgetenv("APPIMAGE");
    QStringList args {QStringLiteral("-a"), QStringLiteral("env"),
        QStringLiteral("QT_QPA_PLATFORM=xcb"), QStringLiteral("KK_NO_XVFB=1"),
        appImage.isEmpty() ? QCoreApplication::applicationFilePath()
                           : QString::fromLocal8Bit(appImage)};
    args += cliArguments;
    QProcess child;
    child.start(xvfb, args);
    if (!child.waitForStarted() || !child.waitForFinished(120000))
        return false;
    *output = child.readAllStandardOutput();
    return child.exitStatus() == QProcess::NormalExit;
}

/// The options of @p args as command-line words: --name value, repeated for lists, a bare flag
/// for a true boolean.
QStringList cliOptions(const QJsonObject& values)
{
    QStringList out;
    for (auto it = values.begin(); it != values.end(); ++it) {
        const QString name = QStringLiteral("--") + it.key();
        const QJsonValue v = it.value();
        if (v.isBool()) {
            if (v.toBool())
                out << name;
        } else if (v.isArray()) {
            for (const QJsonValue& e : v.toArray())
                out << name << e.toString();
        } else {
            out << name << (v.isDouble() ? QString::number(v.toDouble(), 'g', 17) : v.toString());
        }
    }
    return out;
}

CommandResult runValidate(CommandContext&, const CommandArgs& args)
{
    PluginRegistry& reg = registry();
    const QString target = args.string(QStringLiteral("target"));
    QString dir = target;
    if (const LoadedPlugin* p = reg.find(target))
        dir = p->dir;
    else if (target.endsWith(QLatin1String(plugins::kPackageSuffix)) && QFileInfo(target).isFile())
        return fail("bad_value",
            QStringLiteral("validate a folder or an installed plugin: install the package first "
                           "(plugin.install), then validate its id"));
    if (!QFileInfo(dir).isDir())
        return fail("unknown_plugin",
            QStringLiteral("\"%1\" is neither an installed plugin id nor a folder").arg(target));
    const render::PluginValidation v = render::validatePluginFolder(
        QFileInfo(dir).absoluteFilePath(), !args.boolean(QStringLiteral("static")));
    QJsonArray items;
    for (const render::ValidatedItem& i : v.items)
        items.append(QJsonObject {{QStringLiteral("id"), i.fullId},
            {QStringLiteral("kind"), i.kind}, {QStringLiteral("file"), i.file},
            {QStringLiteral("opengl33"), i.desktop}, {QStringLiteral("opengles30"), i.gles}});
    // No OpenGL context here (a headless session): the same check under xvfb-run, once.
    if (!args.boolean(QStringLiteral("static")) && !v.items.isEmpty()
        && std::all_of(v.items.begin(), v.items.end(),
            [](const render::ValidatedItem& i) { return i.desktop == QLatin1String("skipped"); })) {
        QByteArray out;
        QJsonObject shown = args.values();
        shown.insert(QStringLiteral("target"), dir);
        if (runUnderXvfb(QStringList {QStringLiteral("plugin"), QStringLiteral("validate"),
                             QStringLiteral("--json")}
                    + cliOptions(shown),
                &out)) {
            const QJsonObject reply = QJsonDocument::fromJson(out).object();
            if (reply.value(QStringLiteral("ok")).toBool())
                return CommandResult::success(reply.value(QStringLiteral("result")).toObject(),
                    QStringLiteral("(checked under xvfb-run) valid: %1")
                        .arg(reply.value(QStringLiteral("result"))
                                    .toObject()
                                    .value(QStringLiteral("valid"))
                                    .toBool()
                                ? QStringLiteral("yes")
                                : QStringLiteral("no")));
        }
    }
    QJsonObject data {{QStringLiteral("valid"), v.valid}, {QStringLiteral("plugin"), v.pluginId},
        {QStringLiteral("problems"), problemsJson(v.problems)}, {QStringLiteral("items"), items}};
    if (!v.note.isEmpty())
        data.insert(QStringLiteral("note"), v.note);
    QString text = v.valid ? QStringLiteral("valid") : QStringLiteral("NOT valid");
    for (const plugins::Problem& p : v.problems)
        text += QStringLiteral("\n  ") + p.text();
    for (const render::ValidatedItem& i : v.items)
        text += QStringLiteral("\n  %1: OpenGL 3.3 %2, OpenGL ES 3.0 %3")
                    .arg(i.fullId, i.desktop, i.gles);
    if (!v.note.isEmpty())
        text += QStringLiteral("\n  note: ") + v.note;
    return CommandResult::success(data, text);
}

CommandResult runInstall(CommandContext&, const CommandArgs& args)
{
    QString error;
    QString id;
    if (!registry().install(args.string(QStringLiteral("path")), &error, &id))
        return fail("install_failed", error);
    const LoadedPlugin* p = registry().find(id);
    return CommandResult::success(
        {{QStringLiteral("id"), id}, {QStringLiteral("enabled"), p && p->enabled}},
        p && !p->enabled && p->manifest.hasScripts()
            ? QStringLiteral("installed %1; it has scripts, so it is off until you enable it "
                             "(Workspace > Plugins)")
                  .arg(id)
            : QStringLiteral("installed %1").arg(id));
}

CommandResult runUninstall(CommandContext&, const CommandArgs& args)
{
    QString error;
    const QString id = args.string(QStringLiteral("id"));
    if (!registry().uninstall(id, &error))
        return fail("uninstall_failed", error);
    return CommandResult::success(
        {{QStringLiteral("id"), id}}, QStringLiteral("removed %1").arg(id));
}

CommandResult setEnabled(CommandContext& ctx, const CommandArgs& args, bool on)
{
    const QString id = args.string(QStringLiteral("id"));
    const LoadedPlugin* p = registry().find(id);
    if (!p)
        return fail("unknown_plugin", QStringLiteral("no plugin \"%1\" (see plugin.list)").arg(id));
    // A plugin with scripts runs programs: switching it on is the person's decision, made in the
    // Plugins dialog or on their own command line, not by an assistant over the live socket.
    if (on && ctx.live && p->valid && p->manifest.hasScripts())
        return fail("needs_consent",
            QStringLiteral("%1 has scripts: enable it yourself in Workspace > Plugins (it shows "
                           "what the scripts may do)")
                .arg(id));
    registry().setEnabled(id, on);
    return CommandResult::success({{QStringLiteral("id"), id}, {QStringLiteral("enabled"), on}},
        QStringLiteral("%1 %2").arg(
            id, on ? QStringLiteral("enabled") : QStringLiteral("disabled")));
}

CommandResult runReload(CommandContext&, const CommandArgs&)
{
    registry().reload();
    int problems = 0;
    for (const LoadedPlugin& p : registry().plugins())
        problems += p.valid ? 0 : 1;
    return CommandResult::success({{QStringLiteral("plugins"), registry().plugins().size()},
                                      {QStringLiteral("broken"), problems}},
        QStringLiteral("reloaded %1 plugin(s), %2 with problems")
            .arg(registry().plugins().size())
            .arg(problems));
}

CommandResult runPack(CommandContext&, const CommandArgs& args)
{
    QString error;
    const QString file = QFileInfo(args.string(QStringLiteral("file"))).absoluteFilePath();
    if (!file.endsWith(QLatin1String(plugins::kPackageSuffix)))
        return fail("bad_value", QStringLiteral("the file name must end in .kkplugin"));
    if (!registry().exportPackage(args.string(QStringLiteral("id")), file, &error))
        return fail("pack_failed", error);
    return CommandResult::success(
        {{QStringLiteral("file"), file}}, QStringLiteral("wrote %1").arg(file));
}

CommandResult runCopyBuiltin(CommandContext&, const CommandArgs& args)
{
    PluginRegistry& reg = registry();
    const QString builtin = args.string(QStringLiteral("builtin"));
    const QString newId = args.string(QStringLiteral("id"));
    if (!plugins::isValidPluginId(newId))
        return fail("bad_value", QStringLiteral("\"%1\" is not a plugin id").arg(newId));
    const QString examples = PluginRegistry::examplesDir();
    QStringList available;
    QString source;
    for (const QFileInfo& e : QDir(examples).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile f(e.absoluteFilePath() + QStringLiteral("/plugin.json"));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        for (const char* key : {"effects", "transitions"}) {
            for (const QJsonValue& v : root.value(QLatin1String(key)).toArray()) {
                const QString from = v.toObject().value(QStringLiteral("ported_from")).toString();
                if (from.isEmpty())
                    continue;
                available << from;
                if (from == builtin)
                    source = e.absoluteFilePath();
            }
        }
    }
    if (source.isEmpty()) {
        // Say what is possible: only built-ins that are plain shaders have a plugin form.
        const EffectDescriptor* d = EffectRegistry::instance().find(builtin);
        return fail("not_exportable",
            QStringLiteral(
                "\"%1\" %2. Built-ins with a plugin form: %3. For anything else start "
                "from plugin.create and write the shader (docs/plugins/writing-plugins.md)")
                .arg(builtin,
                    d ? QStringLiteral("has no plugin version yet")
                      : QStringLiteral("is not a built-in effect or transition (see effect.list)"),
                    available.isEmpty()
                        ? QStringLiteral("none found (examples folder: %1)").arg(examples)
                        : available.join(QStringLiteral(", "))));
    }
    const QString dir = reg.userDir() + QLatin1Char('/') + newId;
    if (QFileInfo::exists(dir) && !args.boolean(QStringLiteral("overwrite")))
        return fail("exists", QStringLiteral("%1 already exists").arg(dir));
    if (QFileInfo::exists(dir))
        QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    for (const QFileInfo& f : QDir(source).entryInfoList(QDir::Files)) {
        QFile in(f.absoluteFilePath());
        if (!in.open(QIODevice::ReadOnly))
            continue;
        QString text = QString::fromUtf8(in.readAll());
        if (f.fileName() == QLatin1String("plugin.json")) {
            text.replace(QRegularExpression(QStringLiteral("^  \"id\": \".*\",$"),
                             QRegularExpression::MultilineOption),
                QStringLiteral("  \"id\": %1,").arg(jsonString(newId)));
            if (args.has(QStringLiteral("name")))
                text.replace(QRegularExpression(QStringLiteral("^  \"name\": \".*\",$"),
                                 QRegularExpression::MultilineOption),
                    QStringLiteral("  \"name\": %1,")
                        .arg(jsonString(args.string(QStringLiteral("name")))));
        }
        QString error;
        if (!writeFile(dir + QLatin1Char('/') + f.fileName(), text, &error))
            return fail("write_failed", error);
    }
    reg.reload();
    return CommandResult::success({{QStringLiteral("id"), newId}, {QStringLiteral("folder"), dir},
                                      {QStringLiteral("copiedFrom"), builtin}},
        QStringLiteral("copied the plugin form of %1 to %2: edit its .glsl and plugin.json, then "
                       "plugin.reload and plugin.render_preview")
            .arg(builtin, dir));
}

CommandResult runRenderPreview(CommandContext&, const CommandArgs& args)
{
    PluginRegistry& reg = registry();
    const QString pluginId = args.string(QStringLiteral("plugin"));
    const LoadedPlugin* p = reg.find(pluginId);
    if (!p || !p->valid)
        return fail("unknown_plugin",
            QStringLiteral("no valid plugin \"%1\" (plugin.list; plugin.validate shows problems)")
                .arg(pluginId));
    if (!p->enabled)
        return fail(
            "plugin_disabled", QStringLiteral("%1 is disabled (plugin.enable)").arg(pluginId));
    // Which item: the named one, or the only shader item.
    QStringList ids;
    for (const auto& e : p->manifest.effects)
        ids << e.fullId;
    for (const auto& t : p->manifest.transitions)
        ids << t.fullId;
    QString fullId;
    if (args.has(QStringLiteral("item"))) {
        const QString item = args.string(QStringLiteral("item"));
        fullId = ids.contains(item) ? item : plugins::fullId(pluginId, item);
    } else if (ids.size() == 1) {
        fullId = ids.first();
    }
    if (!ids.contains(fullId))
        return fail("unknown_item",
            QStringLiteral("give item: one of %1").arg(ids.join(QStringLiteral(", "))));
    const EffectDescriptor* d = EffectRegistry::instance().find(fullId);
    if (!d || d->unavailable)
        return fail("unknown_item", QStringLiteral("%1 is not registered").arg(fullId));
    QVector<ParamSetting> settings;
    if (auto bad = checkSettings(*d, settingsOf(args), settings))
        return *bad;
    ParamSet params = EffectRegistry::instance().defaultParams(fullId);
    for (const ParamSetting& s : std::as_const(settings)) {
        if (s.descriptor->isScalar())
            params.setBase(s.descriptor->key, s.number);
        else
            params.setStatic(s.descriptor->key, s.value);
    }

    const int width = int(args.integer(QStringLiteral("width"), 640));
    if (width < 64 || width > 2048)
        return fail("out_of_range", QStringLiteral("width must be between 64 and 2048"));
    QImage source;
    if (args.has(QStringLiteral("source"))) {
        source = QImage(args.string(QStringLiteral("source")));
        if (source.isNull())
            return fail("bad_value",
                QStringLiteral("cannot read the image %1")
                    .arg(args.string(QStringLiteral("source"))));
        source = source.scaledToWidth(width, Qt::SmoothTransformation);
    } else {
        source = render::previewTestChart(QSize(width, width * 9 / 16));
    }
    render::PluginPreviewRig rig;
    QString why;
    if (!rig.initialize(&why)) {
        // Headless (no display): the same command in a child under xvfb-run, picture through a
        // file.
        QTemporaryFile png(QDir::tempPath() + QStringLiteral("/kinoklip-preview-XXXXXX.png"));
        QByteArray out;
        if (png.open()) {
            QJsonObject shown = args.values();
            shown.insert(QStringLiteral("file"), png.fileName());
            if (runUnderXvfb(QStringList {QStringLiteral("plugin"),
                                 QStringLiteral("render_preview"), QStringLiteral("--json")}
                        + cliOptions(shown),
                    &out)) {
                const QJsonObject reply = QJsonDocument::fromJson(out).object();
                if (!reply.value(QStringLiteral("ok")).toBool()
                    && reply.contains(QStringLiteral("error"))) {
                    const QJsonObject e = reply.value(QStringLiteral("error")).toObject();
                    return CommandResult::failure(e.value(QStringLiteral("code")).toString(),
                        e.value(QStringLiteral("message")).toString());
                }
                QImage image(png.fileName());
                if (reply.value(QStringLiteral("ok")).toBool() && !image.isNull()) {
                    QJsonObject data = reply.value(QStringLiteral("result")).toObject();
                    data.remove(QStringLiteral("file"));
                    if (args.has(QStringLiteral("file"))) {
                        const QString file
                            = QFileInfo(args.string(QStringLiteral("file"))).absoluteFilePath();
                        image.convertedTo(QImage::Format_ARGB32).save(file, "PNG");
                        data.insert(QStringLiteral("file"), file);
                    }
                    CommandResult result = CommandResult::success(data,
                        QStringLiteral("rendered %1 under xvfb-run (%2x%3)")
                            .arg(data.value(QStringLiteral("item")).toString())
                            .arg(image.width())
                            .arg(image.height()));
                    result.image = image.convertedTo(QImage::Format_ARGB32);
                    return result;
                }
            }
        }
        return fail("no_opengl",
            why
                + QStringLiteral(". Install xvfb-run, or use it through the open window (Local "
                                 "Control)"));
    }
    QImage image;
    const bool transition = d->category == EffectCategory::Transition;
    if (transition) {
        QImage b = source;
        b.invertPixels(QImage::InvertRgb); // the incoming picture: the inverted chart
        image = rig.renderTransition(
            fullId, params, args.number(QStringLiteral("progress"), 0.5), source, b, &why);
    } else {
        image = rig.renderEffect(
            fullId, params, source, args.integer(QStringLiteral("at"), 0), 25.0, &why);
    }
    if (image.isNull())
        return fail(
            "shader_error", QStringLiteral("%1 (plugin.validate shows the lines)").arg(why));
    QJsonObject data {{QStringLiteral("item"), fullId}, {QStringLiteral("width"), image.width()},
        {QStringLiteral("height"), image.height()},
        {QStringLiteral("api"),
            rig.isEs() ? QStringLiteral("OpenGL ES 3.0") : QStringLiteral("OpenGL 3.3")}};
    if (args.has(QStringLiteral("file"))) {
        const QString file = QFileInfo(args.string(QStringLiteral("file"))).absoluteFilePath();
        if (!file.endsWith(QLatin1String(".png"), Qt::CaseInsensitive))
            return fail("bad_value", QStringLiteral("the file name must end in .png"));
        if (!image.convertedTo(QImage::Format_ARGB32).save(file, "PNG"))
            return fail("write_failed", QStringLiteral("cannot write %1").arg(file));
        data.insert(QStringLiteral("file"), file);
    }
    CommandResult result = CommandResult::success(data,
        QStringLiteral("rendered %1 (%2x%3)%4")
            .arg(fullId)
            .arg(image.width())
            .arg(image.height())
            .arg(data.contains(QStringLiteral("file"))
                    ? QStringLiteral(" to ") + data.value(QStringLiteral("file")).toString()
                    : QString()));
    result.image = image.convertedTo(QImage::Format_ARGB32);
    return result;
}

const ParamSpec kIdParam {QStringLiteral("id"), ParamType::String,
    QStringLiteral("plugin id (see plugin.list)"), true, {}, {}};

} // namespace

void registerPluginCommands(CommandRegistry& r)
{
    r.add({{QStringLiteral("plugin.list"),
               QStringLiteral("List the installed plugins (bundled and user): id, name, version, "
                              "kinds, whether enabled and valid, and where the user folder is."),
               {}, ProjectUse::None},
        runList});
    r.add({{QStringLiteral("plugin.info"),
               QStringLiteral("Everything about one plugin: manifest, items with their ids, "
                              "declared permissions, manifest problems and shader compile errors."),
               {kIdParam}, ProjectUse::None},
        runInfo});
    r.add({{QStringLiteral("plugin.create"),
               QStringLiteral("Make a new plugin from a template in the user plugin folder and "
                              "install it: kind effect (GLSL shader), transition (GLSL), preset "
                              "(recipe of commands) or script (python3). Then edit its files and "
                              "run plugin.reload, plugin.validate and plugin.render_preview."),
               {{QStringLiteral("id"), ParamType::String,
                    QStringLiteral("plugin id, reverse-DNS style: com.example.tint"), true, {}, {}},
                   {QStringLiteral("name"), ParamType::String, QStringLiteral("display name"),
                       false, {}, {}},
                   {QStringLiteral("kind"), ParamType::String, QStringLiteral("what to make"),
                       false, QStringLiteral("effect"),
                       {QStringLiteral("effect"), QStringLiteral("transition"),
                           QStringLiteral("preset"), QStringLiteral("script")}},
                   {QStringLiteral("entry"), ParamType::String,
                       QStringLiteral(
                           "effects: main (vec4 effect(vec2 uv)) or color (vec3 apply(vec3 c))"),
                       false, QStringLiteral("main"),
                       {QStringLiteral("main"), QStringLiteral("color")}},
                   {QStringLiteral("item"), ParamType::String,
                       QStringLiteral("id of the item inside the plugin (default: from the name)"),
                       false, {}, {}},
                   {QStringLiteral("description"), ParamType::String,
                       QStringLiteral("one-line description"), false, {}, {}},
                   {QStringLiteral("author"), ParamType::String, QStringLiteral("author name"),
                       false, {}, {}},
                   {QStringLiteral("license"), ParamType::String,
                       QStringLiteral("SPDX licence of the plugin"), false,
                       QStringLiteral("GPL-3.0-or-later"), {}},
                   {QStringLiteral("group"), ParamType::String,
                       QStringLiteral(
                           "Effects Library folder (blur, color, distort, ... or any text)"),
                       false, {}, {}},
                   {QStringLiteral("param"), ParamType::StringList,
                       QStringLiteral("a parameter: key:number:min:max:default[:label], "
                                      "key:integer:min:max:default, key:boolean:true, "
                                      "key:color:#rrggbb, key:choice:a|b|c:default (repeat)"),
                       false, {}, {}},
                   {QStringLiteral("overwrite"), ParamType::Boolean,
                       QStringLiteral("replace an existing plugin folder with this id"), false, {},
                       {}}},
               ProjectUse::None},
        runCreate});
    r.add({{QStringLiteral("plugin.copy_builtin"),
               QStringLiteral("Copy the plugin form of a built-in effect or transition into a new "
                              "plugin of your own, to change it (only built-ins that are plain "
                              "shaders have one; the answer lists them)."),
               {{QStringLiteral("builtin"), ParamType::String,
                    QStringLiteral("built-in id (effect.list, transition.list), e.g. invert"), true,
                    {}, {}},
                   {QStringLiteral("id"), ParamType::String, QStringLiteral("id of the new plugin"),
                       true, {}, {}},
                   {QStringLiteral("name"), ParamType::String,
                       QStringLiteral("display name of the new plugin"), false, {}, {}},
                   {QStringLiteral("overwrite"), ParamType::Boolean,
                       QStringLiteral("replace an existing folder"), false, {}, {}}},
               ProjectUse::None},
        runCopyBuiltin});
    r.add({{QStringLiteral("plugin.validate"),
               QStringLiteral("Check a plugin (an installed id or a folder): the manifest, its "
                              "files, the shader rules, and a compile of every shader for OpenGL "
                              "3.3 and OpenGL ES 3.0 where this machine has them. Returns every "
                              "problem with file and line."),
               {{QStringLiteral("target"), ParamType::String,
                    QStringLiteral("installed plugin id, or a plugin folder"), true, {}, {}},
                   {QStringLiteral("static"), ParamType::Boolean,
                       QStringLiteral("only the checks that need no OpenGL"), false, {}, {}}},
               ProjectUse::None},
        runValidate});
    r.add({{QStringLiteral("plugin.install"),
               QStringLiteral("Install a plugin from a .kkplugin file or a plugin folder into the "
                              "user plugin folder (a plugin with scripts starts switched off). "
                              "Nothing of the plugin runs."),
               {{QStringLiteral("path"), ParamType::Path,
                   QStringLiteral(".kkplugin file or folder"), true, {}, {}}},
               ProjectUse::None},
        runInstall});
    r.add(
        {{QStringLiteral("plugin.uninstall"),
             QStringLiteral("Delete an installed user plugin (bundled ones can only be disabled). "
                            "Projects keep the effect's settings and show it as a missing plugin."),
             {kIdParam}, ProjectUse::None},
            runUninstall});
    r.add({{QStringLiteral("plugin.enable"),
               QStringLiteral("Switch a plugin on (its effects appear in the Effects library). A "
                              "plugin with scripts cannot be switched on over the live connection: "
                              "the person does that in Workspace > Plugins."),
               {kIdParam}, ProjectUse::None},
        [](CommandContext& c, const CommandArgs& a) { return setEnabled(c, a, true); }});
    r.add({{QStringLiteral("plugin.disable"),
               QStringLiteral(
                   "Switch a plugin off; effects using it render as bypass until it is on again."),
               {kIdParam}, ProjectUse::None},
        [](CommandContext& c, const CommandArgs& a) { return setEnabled(c, a, false); }});
    r.add({{QStringLiteral("plugin.reload"),
               QStringLiteral("Read all plugin folders again: new plugins, edited manifests and "
                              "shaders take effect (hot reload for plugin developers)."),
               {}, ProjectUse::None},
        runReload});
    r.add({{QStringLiteral("plugin.pack"),
               QStringLiteral("Pack an installed plugin into a .kkplugin file to share."),
               {kIdParam,
                   {QStringLiteral("file"), ParamType::Path,
                       QStringLiteral("the .kkplugin file to write"), true, {}, {}}},
               ProjectUse::None},
        runPack});
    r.add(
        {{QStringLiteral("plugin.render_preview"),
             QStringLiteral("Render one frame of a test picture (or your own image) through a "
                            "plugin effect or transition with the given parameters, so you can "
                            "see the result. The picture comes back with the answer; also "
                            "written to `file` when given. Needs OpenGL (use xvfb-run when "
                            "headless)."),
             {{QStringLiteral("plugin"), ParamType::String, QStringLiteral("installed plugin id"),
                  true, {}, {}},
                 {QStringLiteral("item"), ParamType::String,
                     QStringLiteral("the effect or transition (item id or full id); not needed "
                                    "when the plugin has one"),
                     false, {}, {}},
                 setListParam("effect or transition"),
                 {QStringLiteral("source"), ParamType::Path,
                     QStringLiteral("an image to use instead of the built-in test chart"), false,
                     {}, {}},
                 {QStringLiteral("width"), ParamType::Integer,
                     QStringLiteral("picture width, 64 to 2048"), false, QStringLiteral("640"), {}},
                 {QStringLiteral("at"), ParamType::Integer,
                     QStringLiteral(
                         "effects: the clip-local frame (for keyframed or animated looks)"),
                     false, QStringLiteral("0"), {}},
                 {QStringLiteral("progress"), ParamType::Number,
                     QStringLiteral("transitions: 0 (all outgoing) to 1 (all incoming)"), false,
                     QStringLiteral("0.5"), {}},
                 {QStringLiteral("file"), ParamType::Path,
                     QStringLiteral("also write the picture as PNG"), false, {}, {}}},
             ProjectUse::None},
            runRenderPreview});
}

} // namespace kk::commands
