// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
// Product identity comes from CMake (KK_APP_NAME / KK_APP_ID / KK_APP_VERSION).
namespace kk::appinfo {
inline constexpr const char* name = KK_APP_NAME;
inline constexpr const char* id = KK_APP_ID;
inline constexpr const char* version = KK_APP_VERSION; // base version; see prerelease
/// Pre-release tag shown in the About box and window title (empty for a final release).
inline constexpr const char* prerelease = "";
/// Short commit of a local dev build, "" for every other build. Only scripts/dev_build.sh
/// (through KK_DEV_BUILD in scripts/package_linux.sh) writes the marker file
/// <bundle>/usr/share/kinoklip/dev-build; the compiled code is the same as a
/// release, so a dev package never needs a different build configuration.
inline QString devBuildCommit()
{
    static const QString commit = [] {
        // The launcher (packaging/linux/apprun.sh) exports KINOKLIP_APPDIR; /proc/self/exe is no
        // use there (the bundled glibc's loader runs the binary).
        QString dir = qEnvironmentVariable("KINOKLIP_APPDIR");
        if (dir.isEmpty()) {
            const QString exe = QFile::symLinkTarget(QStringLiteral("/proc/self/exe"));
            if (exe.isEmpty())
                return QString();
            dir = QFileInfo(exe).absolutePath() + QStringLiteral("/..");
        }
        QFile f(QDir::cleanPath(dir + QStringLiteral("/usr/share/kinoklip/dev-build")));
        if (!f.open(QIODevice::ReadOnly))
            return QString();
        return QString::fromUtf8(f.readLine(64)).trimmed();
    }();
    return commit;
}
/// The name as shown to the user: "Kinoklip", or "Kinoklip Dev" in a dev build, so the two are
/// told apart in window titles, menus and the launcher. Settings and data keep `name`, so a dev
/// copy shares them with the release.
inline QString displayName()
{
    return devBuildCommit().isEmpty() ? QLatin1String(name) : QLatin1String(name) + QStringLiteral(" Dev");
}

/// Version as shown to the user: "0.1.0-alpha.1" (base version + "-" + prerelease if set);
/// a local dev build shows "1.8.2 dev (abc1234)".
inline QString displayVersion()
{
    const QString base = QLatin1String(version);
    QString v = *prerelease ? base + QLatin1Char('-') + QLatin1String(prerelease) : base;
    if (const QString dev = devBuildCommit(); !dev.isEmpty())
        v += QStringLiteral(" dev (") + dev + QLatin1Char(')');
    return v;
}
/// Permanent invite to the community Discord server (Help > Join the Community on Discord).
inline constexpr const char* communityUrl = "https://discord.gg/pCeHnvDn3x";
inline constexpr const char* projectExtension = "kkproj";
/// Reverse-DNS application id: desktop entry name, Wayland app_id, icon name, MIME package
/// (packaging/linux/org.kinoklip.Kinoklip.desktop).
inline constexpr const char* desktopId = "org.kinoklip.Kinoklip";
} // namespace kk::appinfo
