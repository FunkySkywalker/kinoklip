// SPDX-License-Identifier: GPL-3.0-or-later
// PluginValidate — see PluginValidate.h.
#include "render/PluginValidate.h"

#include "plugins/PluginRegistry.h"
#include "render/PluginShader.h"

#include <QFile>

namespace kk::render {

namespace {

constexpr qint64 kMaxShaderBytes = 256 * 1024;

} // namespace

PluginValidation validatePluginFolder(const QString& dir, bool compile)
{
    PluginValidation v;
    QFile manifestFile(dir + QLatin1Char('/') + QLatin1String(plugins::kManifestFile));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        v.problems.append({QString::fromLatin1(plugins::kManifestFile),
            QStringLiteral("there is no plugin.json in %1").arg(dir), 0});
        return v;
    }
    const plugins::ParseResult parsed = plugins::parseManifest(manifestFile.readAll());
    v.problems = parsed.problems;
    if (!parsed.ok)
        return v;
    const plugins::PluginManifest& m = parsed.manifest;
    v.pluginId = m.id;
    v.pluginName = m.name;
    plugins::PluginRegistry::checkFiles(dir, m, &v.problems);

    const auto check = [&](const QString& fullId, bool transition, const QString& file,
                           const QString& entry, const QVector<plugins::PluginParam>& params) {
        ValidatedItem item;
        item.fullId = fullId;
        item.kind = transition ? QStringLiteral("transition") : QStringLiteral("effect");
        item.file = file;
        item.desktop = item.gles = QStringLiteral("skipped");
        QFile f(dir + QLatin1Char('/') + file);
        if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxShaderBytes) {
            v.items.append(item);
            return; // checkFiles() already said why
        }
        plugins::PluginShaderSpec spec;
        spec.fullId = fullId;
        spec.pluginId = m.id;
        spec.transition = transition;
        spec.entry = entry;
        spec.params = params;
        spec.source = QString::fromUtf8(f.readAll());
        spec.file = file;
        const QVector<plugins::Problem> lint = pluginshader::lint(spec);
        v.problems += lint;
        if (compile) {
            const pluginshader::CompileCheck desktop
                = pluginshader::compileOnContext(spec, GlApi::Desktop);
            const pluginshader::CompileCheck es = pluginshader::compileOnContext(spec, GlApi::Gles);
            const auto report
                = [&](const pluginshader::CompileCheck& c, QString* state, const char* name) {
                      if (!c.attempted) {
                          *state = QStringLiteral("skipped");
                          if (v.note.isEmpty())
                              v.note = QStringLiteral("%1: %2").arg(QLatin1String(name), c.note);
                          return;
                      }
                      *state = c.compiled ? QStringLiteral("ok") : QStringLiteral("error");
                      for (plugins::Problem p : c.problems) {
                          p.message = QStringLiteral("[%1] %2").arg(QLatin1String(name), p.message);
                          // The lint usually said the same: keep the compiler's text once.
                          v.problems.append(p);
                      }
                  };
            report(desktop, &item.desktop, "OpenGL 3.3");
            report(es, &item.gles, "OpenGL ES 3.0");
        }
        v.items.append(item);
    };
    for (const plugins::PluginEffectDef& e : m.effects)
        check(e.fullId, false, e.file, e.entry, e.params);
    for (const plugins::PluginTransitionDef& t : m.transitions)
        check(t.fullId, true, t.file, {}, t.params);

    v.valid = true;
    for (const plugins::Problem& p : std::as_const(v.problems))
        v.valid &= p.warning;
    for (const ValidatedItem& item : std::as_const(v.items))
        v.valid &= item.desktop != QLatin1String("error") && item.gles != QLatin1String("error");
    return v;
}

} // namespace kk::render
