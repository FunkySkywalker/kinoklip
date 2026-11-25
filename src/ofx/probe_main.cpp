// SPDX-License-Identifier: GPL-3.0-or-later
// kinoklip-ofx-probe <bundle folder> <result file> — loads one OpenFX bundle in a process of its
// own and writes what the host learned of its plugins as JSON. Kinoklip runs it at scan time: a
// plugin that crashes, hangs or aborts while loading kills only this process and the bundle goes
// on the bad-plugin list (spec 158).
#include "ofx/OfxHost.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3) {
        std::fprintf(stderr, "usage: kinoklip-ofx-probe <bundle folder> <result file>\n");
        return 2;
    }
    const kk::ofx::BundleInfo info = kk::ofx::describeBundle(QString::fromLocal8Bit(argv[1]));
    QFile out(QString::fromLocal8Bit(argv[2]));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 3;
    out.write(QJsonDocument(info.toJson()).toJson(QJsonDocument::Compact));
    out.close();
    // Leave without running the plugins' static destructors (some crash at exit).
    std::fflush(nullptr);
    std::_Exit(0);
}
