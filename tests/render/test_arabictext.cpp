// SPDX-License-Identifier: GPL-3.0-or-later
// Cursive and bidirectional text in titles (issue 12): the title is shaped as whole runs, so
// Arabic letters join, tracking 0 changes nothing, and mixed right-to-left / left-to-right text
// matches what Qt draws for the same string.
#include "render/TitleRenderer.h"

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTest>

using namespace kk;
using namespace kk::render;

namespace {

const QString kArabic = QString::fromUtf8("السلام عليكم");
const QString kMixed = QString::fromUtf8("Kinoklip السلام 123");

QString arabicFamily()
{
    const QStringList wanted{"Noto Sans Arabic", "Noto Naskh Arabic", "Noto Sans"};
    const QStringList have = QFontDatabase::families(QFontDatabase::Arabic);
    for (const QString& w : wanted)
        if (have.contains(w))
            return w;
    return {};
}

TitleStyle style(const QString& text, const QString& family, double tracking = 0)
{
    TitleStyle s;
    s.text = text;
    s.font = family;
    s.size = 120;
    s.color = Qt::white;
    s.canvas = QSize(1280, 360);
    s.tracking = tracking;
    return s;
}

QImage onCanvas(const TitleStyle& s)
{
    const TitleImage t = renderTitle(s);
    QImage canvas(s.canvas, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    if (!t.image.isNull()) {
        QPainter p(&canvas);
        p.drawImage(t.rect.topLeft(), t.image);
    }
    return canvas;
}

/// The same string drawn by Qt's own text layout, centred like the title (centre alignment).
QImage reference(const TitleStyle& s)
{
    QFont font(s.font);
    font.setPixelSize(int(s.size));
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF fm(font);
    QImage canvas(s.canvas, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    p.setFont(font);
    p.setPen(Qt::white);
    const double w = fm.horizontalAdvance(s.text);
    const double top = (1.0 - s.posY) * s.canvas.height() - (fm.ascent() + fm.descent()) * 0.5;
    p.drawText(QPointF(s.posX * s.canvas.width() - w * 0.5, top + fm.ascent()), s.text);
    return canvas;
}

int inkPixels(const QImage& image)
{
    int n = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            n += qAlpha(image.pixel(x, y)) > 128;
    return n;
}

/// Pixels where exactly one of the two images has ink.
int inkDifference(const QImage& a, const QImage& b)
{
    int n = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            n += (qAlpha(a.pixel(x, y)) > 128) != (qAlpha(b.pixel(x, y)) > 128);
    return n;
}

} // namespace

class ArabicTextTest : public QObject
{
    Q_OBJECT
private slots:
    void matchesQtLayout_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("arabic") << kArabic;
        QTest::newRow("mixed") << kMixed;
    }
    void matchesQtLayout()
    {
        const QString family = arabicFamily();
        if (family.isEmpty())
            QSKIP("no Arabic font installed");
        QFETCH(QString, text);
        const TitleStyle s = style(text, family);
        const QImage got = onCanvas(s);
        const QImage want = reference(s);
        const QString dir = qEnvironmentVariable("KK_ARABIC_OUT");
        if (!dir.isEmpty()) {
            got.save(dir + "/title-" + QString::number(text.size()) + ".png");
            want.save(dir + "/ref-" + QString::number(text.size()) + ".png");
        }
        QVERIFY(inkPixels(want) > 1000);
        QVERIFY2(inkDifference(got, want) < inkPixels(want) / 50,
            qPrintable(QString("diff %1 of %2").arg(inkDifference(got, want)).arg(inkPixels(want))));
    }
    void trackingZeroIsPlainText()
    {
        const QString family = arabicFamily();
        if (family.isEmpty())
            QSKIP("no Arabic font installed");
        TitleStyle plain;
        plain.text = kArabic;
        plain.font = family;
        plain.size = 120;
        plain.color = Qt::white;
        plain.canvas = QSize(1280, 360);
        QCOMPARE(onCanvas(style(kArabic, family, 0)), onCanvas(plain));
    }
    void trackingKeepsJoinsClosed()
    {
        const QString family = arabicFamily();
        if (family.isEmpty())
            QSKIP("no Arabic font installed");
        // A joined word keeps its ink in one piece: tracking must not split it into more
        // separate strokes than the untracked word has.
        const QImage plain = onCanvas(style(QString::fromUtf8("عليكم"), family, 0));
        const QImage tracked = onCanvas(style(QString::fromUtf8("عليكم"), family, 20));
        QVERIFY(inkPixels(tracked) > 0);
        QVERIFY(std::abs(inkPixels(tracked) - inkPixels(plain)) < inkPixels(plain) / 20);
    }
    void writeOnRevealsFromTheRight()
    {
        const QString family = arabicFamily();
        if (family.isEmpty())
            QSKIP("no Arabic font installed");
        TitleStyle s = style(kArabic, family);
        const QImage full = onCanvas(s);
        s.writeOnEnd = 0.5;
        const QImage half = onCanvas(s);
        const int mid = s.canvas.width() / 2;
        int left = 0;
        int right = 0;
        for (int y = 0; y < half.height(); ++y)
            for (int x = 0; x < half.width(); ++x)
                if (qAlpha(half.pixel(x, y)) > 128)
                    (x < mid ? left : right)++;
        QVERIFY(right > 1000);
        QVERIFY(left < right / 10); // right-to-left: the first letters are on the right
        // What is shown is the same ink as in the full text (no re-shaping of a part).
        int extra = 0;
        for (int y = 0; y < half.height(); ++y)
            for (int x = 0; x < half.width(); ++x)
                extra += qAlpha(half.pixel(x, y)) > 128 && qAlpha(full.pixel(x, y)) <= 128;
        QVERIFY(extra < 30);
    }
    void latinUnchanged()
    {
        const TitleStyle s = style("Kinoklip title", "Noto Sans");
        const QImage got = onCanvas(s);
        const QImage want = reference(s);
        QVERIFY(inkPixels(want) > 500);
        QVERIFY(inkDifference(got, want) < inkPixels(want) / 50);
    }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ArabicTextTest t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_arabictext.moc"
