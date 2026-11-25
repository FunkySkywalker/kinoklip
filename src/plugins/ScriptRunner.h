// SPDX-License-Identifier: GPL-3.0-or-later
// ScriptRunner — runs the script of a plugin (docs/plugins/writing-plugins.md, "Scripts"): an
// external `python3` process that talks to Kinoklip's command layer through a JSON-lines bridge.
// The script prints "@kk {json}" lines (resources/plugin-sdk/kinoklip.py does it), the runner
// executes each as a command through the application's executor and writes the JSON reply on
// the script's standard input. Everything else the script prints is output text.
//
// Safety: only the scripts of enabled plugins run (the caller checks), Python starts in
// isolated mode (no current-directory modules, no PYTHON* variables), the working folder is the
// plugin folder, the run ends after kMaxRunMs, and commands are filtered by the plugin's
// declared permissions. A script is still a program: it can do what the user can; the
// permissions the person sees say so.
#pragma once

#include "plugins/PluginRegistry.h"

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>

#include <functional>
#include <memory>

class QTemporaryDir;

namespace kk::plugins {

/// Longest a script may run.
inline constexpr int kMaxRunMs = 15 * 60 * 1000;

/// Runs one command of the command layer; returns CommandResult::toJson() form
/// ({"ok":true,"result":{...}} or {"ok":false,"error":{"code","message"}}).
using CommandExecutor = std::function<QJsonObject(const QString& command, const QJsonObject& args)>;

/// Thread-safety: GUI thread (the executor runs there).
class ScriptRunner : public QObject
{
    Q_OBJECT
public:
    explicit ScriptRunner(CommandExecutor executor, QObject* parent = nullptr);
    ~ScriptRunner() override;

    /// Starts @p script. Returns false with @p error when python3 or the file is missing or a
    /// script is already running.
    bool start(const ScriptEntry& script, QString* error);
    [[nodiscard]] bool isRunning() const;
    /// Stops the script (terminate, then kill).
    void cancel();
    /// Everything the script printed so far.
    [[nodiscard]] QString output() const { return m_output; }

    /// The python3 program name, or empty when none is installed.
    [[nodiscard]] static QString pythonProgram();
    /// Writes the bridge module into @p dir and returns the argument list that runs @p scriptFile
    /// with it (everything after the program name).
    [[nodiscard]] static QStringList launchArguments(
        const QString& sdkDir, const QString& scriptFile);

signals:
    /// A line of output (print() of the script, or a note from the runner).
    void outputLine(const QString& line);
    /// The script ended: exit code (-1 when killed), and a one-line summary.
    void finished(int exitCode, const QString& summary);

private:
    void readOutput();
    void handleRequest(const QByteArray& line);
    void finish(int exitCode, QProcess::ExitStatus status);

    CommandExecutor m_executor;
    QProcess* m_process = nullptr;
    QByteArray m_buffer;
    QString m_output;
    QStringList m_permissions;
    QString m_title;
    bool m_timedOut = false;
    bool m_cancelled = false;
    std::unique_ptr<QTemporaryDir> m_sdkDir;
    int m_requests = 0;
};

/// True if a script with @p permissions may run a command that needs @p need: "read" (commands
/// that only read the project), "edit" (commands that change it), "none" (no project).
[[nodiscard]] bool permissionAllows(const QStringList& permissions, const QString& need);

} // namespace kk::plugins
