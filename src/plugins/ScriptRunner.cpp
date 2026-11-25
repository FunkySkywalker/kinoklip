// SPDX-License-Identifier: GPL-3.0-or-later
// ScriptRunner — see ScriptRunner.h.
#include "plugins/ScriptRunner.h"

#include "platform/ChildProcessEnv.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

namespace kk::plugins {

namespace {

constexpr char kRequestPrefix[] = "@kk ";
constexpr int kMaxRequests = 100000; ///< a runaway loop stops here
constexpr int kMaxLineBytes = 8 * 1024 * 1024;

// Starts the script with the bridge folder first on the path. -I (isolated mode) keeps the
// script's own folder and the PYTHON* variables out of the module search, so a file placed next
// to the script cannot shadow the standard library.
constexpr char kBootstrap[] = "import sys, os, runpy\n"
                              "sdk, script = sys.argv[1], sys.argv[2]\n"
                              "sys.path.insert(0, sdk)\n"
                              "sys.path.insert(1, os.path.dirname(script))\n"
                              "sys.argv = sys.argv[2:]\n"
                              "runpy.run_path(script, run_name='__main__')\n";

} // namespace

bool permissionAllows(const QStringList& permissions, const QString& need)
{
    if (need == QLatin1String("none"))
        return true;
    if (need == QLatin1String("read"))
        return permissions.contains(QLatin1String("project.read"))
            || permissions.contains(QLatin1String("project.edit"));
    return permissions.contains(QLatin1String("project.edit"));
}

ScriptRunner::ScriptRunner(CommandExecutor executor, QObject* parent)
    : QObject(parent)
    , m_executor(std::move(executor))
{ }

ScriptRunner::~ScriptRunner()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

QString ScriptRunner::pythonProgram()
{
    return QStandardPaths::findExecutable(QStringLiteral("python3"));
}

QStringList ScriptRunner::launchArguments(const QString& sdkDir, const QString& scriptFile)
{
    return {QStringLiteral("-I"), QStringLiteral("-c"), QString::fromLatin1(kBootstrap), sdkDir,
        scriptFile};
}

bool ScriptRunner::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

bool ScriptRunner::start(const ScriptEntry& script, QString* error)
{
    auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (isRunning())
        return fail(QStringLiteral("a script is already running"));
    const QString python = pythonProgram();
    if (python.isEmpty())
        return fail(QStringLiteral("python3 was not found: install Python 3 to run scripts"));
    const QString file = script.dir + QLatin1Char('/') + script.def.file;
    if (!QFileInfo(file).isFile())
        return fail(QStringLiteral("the script file %1 is missing").arg(script.def.file));
    // The bridge module, written fresh for each run (it comes from the program, not the plugin).
    m_sdkDir = std::make_unique<QTemporaryDir>();
    if (!m_sdkDir->isValid())
        return fail(QStringLiteral("cannot make a temporary folder"));
    QFile bridge(QStringLiteral(":/plugin-sdk/kinoklip.py"));
    QFile target(m_sdkDir->path() + QStringLiteral("/kinoklip.py"));
    if (!bridge.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)
        || target.write(bridge.readAll()) <= 0)
        return fail(QStringLiteral("the script bridge is not available in this build"));
    target.close();

    m_permissions = script.def.permissions;
    m_title = script.def.name;
    m_buffer.clear();
    m_output.clear();
    m_timedOut = false;
    m_cancelled = false;
    m_requests = 0;
    delete m_process;
    m_process = new QProcess(this);
    m_process->setWorkingDirectory(script.dir);
    platform::prepareChildProcess(*m_process);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, &ScriptRunner::readOutput);
    connect(m_process, &QProcess::finished, this, &ScriptRunner::finish);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finish(-1, QProcess::CrashExit);
    });
    m_process->start(python, launchArguments(m_sdkDir->path(), file));
    QTimer::singleShot(kMaxRunMs, m_process, [this] {
        if (isRunning()) {
            m_timedOut = true;
            m_process->kill();
        }
    });
    return true;
}

void ScriptRunner::cancel()
{
    if (!isRunning())
        return;
    m_cancelled = true;
    m_process->terminate();
    QTimer::singleShot(2000, m_process, [this] {
        if (isRunning())
            m_process->kill();
    });
}

void ScriptRunner::readOutput()
{
    m_buffer += m_process->readAll();
    if (m_buffer.size() > kMaxLineBytes && !m_buffer.contains('\n')) {
        m_process->kill(); // a line that never ends
        return;
    }
    qsizetype nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        if (line.startsWith(kRequestPrefix)) {
            handleRequest(line.mid(int(sizeof(kRequestPrefix)) - 1));
        } else {
            const QString text = QString::fromUtf8(line);
            m_output += text + QLatin1Char('\n');
            emit outputLine(text);
        }
    }
}

void ScriptRunner::handleRequest(const QByteArray& line)
{
    QJsonObject reply;
    const QJsonObject request = QJsonDocument::fromJson(line).object();
    reply.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
    const QString command = request.value(QStringLiteral("command")).toString();
    const QJsonObject args = request.value(QStringLiteral("args")).toObject();
    const auto deny = [&reply](const QString& code, const QString& message) {
        reply.insert(QStringLiteral("ok"), false);
        reply.insert(QStringLiteral("error"),
            QJsonObject {{QStringLiteral("code"), code}, {QStringLiteral("message"), message}});
    };
    if (command.isEmpty()) {
        deny(QStringLiteral("bad_request"), QStringLiteral("the request has no command"));
    } else if (++m_requests > kMaxRequests) {
        deny(QStringLiteral("too_many_requests"), QStringLiteral("too many commands in one run"));
    } else {
        // The executor reports a permission problem itself (it knows what a command does).
        const QJsonObject result = m_executor(command, args);
        reply.insert(QStringLiteral("ok"), result.value(QStringLiteral("ok")).toBool());
        if (result.value(QStringLiteral("ok")).toBool())
            reply.insert(QStringLiteral("result"), result.value(QStringLiteral("result")));
        else
            reply.insert(QStringLiteral("error"), result.value(QStringLiteral("error")));
    }
    m_process->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
}

void ScriptRunner::finish(int exitCode, QProcess::ExitStatus status)
{
    if (!m_process)
        return;
    readOutput();
    QString summary;
    if (m_timedOut)
        summary = tr("Stopped: the script ran longer than %1 minutes.").arg(kMaxRunMs / 60000);
    else if (m_cancelled)
        summary = tr("Stopped.");
    else if (status == QProcess::CrashExit && exitCode < 0
        && m_process->error() == QProcess::FailedToStart)
        summary = tr("python3 could not be started.");
    else if (exitCode != 0)
        summary = tr("The script ended with an error (exit code %1).").arg(exitCode);
    else
        summary = tr("Done.");
    emit finished(m_timedOut || m_cancelled ? -1 : exitCode, summary);
}

} // namespace kk::plugins
