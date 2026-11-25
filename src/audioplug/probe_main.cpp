// SPDX-License-Identifier: GPL-3.0-or-later
// kinoklip-audio-probe <lv2|clap> <bundle> <result file> — loads one LV2 bundle or CLAP file in a
// process of its own, instantiates every plugin and runs one block, and writes what the host
// learned as JSON. Kinoklip runs it at scan time: a plugin that crashes, hangs or aborts while
// loading kills only this process and the bundle goes on the bad-plugin list (spec 159).
#include "audioplug/AudioPluginHost.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 4) {
        std::fprintf(stderr, "usage: kinoklip-audio-probe <lv2|clap> <bundle> <result file>\n");
        return 2;
    }
    const kk::audioplug::BundleInfo info
        = kk::audioplug::describeBundle(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]));
    QFile out(QString::fromLocal8Bit(argv[3]));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 3;
    out.write(QJsonDocument(info.toJson()).toJson(QJsonDocument::Compact));
    out.close();
    // Leave without running the plugins' static destructors (some crash at exit).
    std::fflush(nullptr);
    std::_Exit(0);
}
