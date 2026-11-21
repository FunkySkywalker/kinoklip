// SPDX-License-Identifier: GPL-3.0-or-later
// Spec 128.10: Saver output as OpenEXR (FFmpeg's exr encoder): half and float, the compression
// choices, linear light, alpha, and a round trip through the decoder.
#include "core/NodeRegistry.h"
#include "render/CompSaver.h"
#include "render/ExrWriter.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace kk;
namespace exr = kk::render::exr;

namespace {

/// 16 x 8: left half opaque (200, 100, 50), right half half transparent (premultiplied 50% of
/// the same colour), bottom row fully transparent.
QImage testPicture()
{
    QImage image(16, 8, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 16; ++x) {
            image.setPixel(x, y,
                x < 8 ? qPremultiply(qRgba(200, 100, 50, 255)) : qPremultiply(qRgba(200, 100, 50, 128)));
        }
    }
    return image;
}

float srgbToLinear(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

} // namespace

class TestExr : public QObject
{
    Q_OBJECT
private slots:
    void theEncoderIsThere() { QVERIFY(exr::available()); }

    void roundTripInEveryDepthAndCompression()
    {
        QTemporaryDir dir;
        const QImage image = testPicture();
        for (const QString& depth : {exr::kHalf, exr::kFloat}) {
            for (const QString& compression : {exr::kNone, exr::kRle, exr::kZip1, exr::kZip16}) {
                exr::Options o;
                o.depth = depth;
                o.compression = compression;
                o.linear = false;
                const QString file = QDir(dir.path()).filePath(depth + compression + QStringLiteral(".exr"));
                QString error;
                QVERIFY2(exr::write(image, file, o, &error), qPrintable(error));
                const exr::Info info = exr::read(file);
                QVERIFY2(info.ok, qPrintable(file));
                QCOMPARE(info.width, 16);
                QCOMPARE(info.height, 8);
                QVERIFY(info.hasAlpha);
                QCOMPARE(info.bitsPerChannel, depth == exr::kHalf ? 16 : 32);
                QCOMPARE(info.compression, compression);
                // Opaque pixel: the colour as it is.
                const QVector<float> p = info.pixel(2, 3);
                QVERIFY(std::abs(p[0] - 200.f / 255.f) < 0.002f);
                QVERIFY(std::abs(p[1] - 100.f / 255.f) < 0.002f);
                QVERIFY(std::abs(p[2] - 50.f / 255.f) < 0.002f);
                QCOMPARE(p[3], 1.0f);
                // Half transparent: premultiplied colour, alpha 128 / 255.
                const QVector<float> q = info.pixel(12, 3);
                QVERIFY(std::abs(q[3] - 128.f / 255.f) < 0.002f);
                QVERIFY(std::abs(q[0] - 200.f / 255.f * 128.f / 255.f) < 0.004f);
                // Transparent row: all zero.
                const QVector<float> t = info.pixel(5, 7);
                QCOMPARE(t, QVector<float>({0.f, 0.f, 0.f, 0.f}));
            }
        }
    }

    void halfIsSmallerThanFloatAndZipSmallerThanNone()
    {
        QTemporaryDir dir;
        QImage flat(128, 64, QImage::Format_ARGB32_Premultiplied);
        flat.fill(QColor(30, 90, 160));
        auto size = [&](const QString& depth, const QString& compression) {
            exr::Options o;
            o.depth = depth;
            o.compression = compression;
            const QString file = QDir(dir.path()).filePath(depth + compression + QStringLiteral(".exr"));
            QString error;
            if (!exr::write(flat, file, o, &error))
                return qint64(-1);
            return QFileInfo(file).size();
        };
        const qint64 halfNone = size(exr::kHalf, exr::kNone);
        const qint64 floatNone = size(exr::kFloat, exr::kNone);
        const qint64 halfZip = size(exr::kHalf, exr::kZip16);
        QVERIFY(halfNone > 0 && floatNone > 0 && halfZip > 0);
        QVERIFY(halfNone < floatNone);
        QVERIFY(halfZip < halfNone);
    }

    void linearLightIsTheDefault()
    {
        QTemporaryDir dir;
        QImage image(4, 4, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(128, 128, 128));
        const QString file = QDir(dir.path()).filePath(QStringLiteral("lin.exr"));
        QString error;
        QVERIFY2(exr::write(image, file, exr::Options {exr::kFloat, exr::kNone, true}, &error),
            qPrintable(error));
        const QVector<float> p = exr::read(file).pixel(1, 1);
        QVERIFY(!p.isEmpty());
        QVERIFY(std::abs(p[0] - srgbToLinear(128.f / 255.f)) < 0.001f);
        QVERIFY(p[0] < 0.25f); // 128 in sRGB is 0.216 in linear light
    }

    void theSaverWritesExrFiles()
    {
        QTemporaryDir dir;
        const QString base = QDir(dir.path()).filePath(QStringLiteral("shot.png"));
        QCOMPARE(QFileInfo(render::compsaver::fileName(base, 7, saverformats::Exr)).fileName(),
            QStringLiteral("shot0007.exr"));
        const QString file = render::compsaver::fileName(base, 7, saverformats::Exr);
        exr::Options o;
        o.depth = exr::kFloat;
        o.compression = exr::kZip1;
        QString error;
        QVERIFY2(render::compsaver::writeImage(testPicture(), file, saverformats::Exr, &error, &o),
            qPrintable(error));
        const exr::Info info = exr::read(file);
        QVERIFY(info.ok);
        QCOMPARE(info.bitsPerChannel, 32);
        QCOMPARE(info.compression, exr::kZip1);
        // Defaults without options: half, ZIP.
        const QString other = render::compsaver::fileName(base, 8, saverformats::Exr);
        QVERIFY(render::compsaver::writeImage(testPicture(), other, saverformats::Exr, &error));
        QCOMPARE(exr::read(other).bitsPerChannel, 16);
    }

    void aMissingPictureOrFolderIsAnError()
    {
        QString error;
        QVERIFY(!exr::write(QImage(), QStringLiteral("/nonexistent/x.exr"), {}, &error));
        QVERIFY(!error.isEmpty());
        error.clear();
        QVERIFY(!exr::write(testPicture(), QStringLiteral("/nonexistent-dir/x.exr"), {}, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!exr::read(QStringLiteral("/nonexistent/x.exr")).ok);
    }
};

QTEST_MAIN(TestExr)
#include "test_exr.moc"
