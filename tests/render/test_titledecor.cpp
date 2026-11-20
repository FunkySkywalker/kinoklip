// SPDX-License-Identifier: GPL-3.0-or-later
// Underline and strikethrough of titles and Text nodes (roadmap 1.12): the Inspector rows, the
// rendered bars (one bar runs under spaces too, with Write On and in Text+), the project format.
#include "core/EffectRegistry.h"
#include "core/NodeRegistry.h"
#include "core/ParamKeys.h"
#include "core/Serialization.h"
#include "core/TitlePresets.h"
#include "render/TitleRenderer.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTest>

using namespace kk;
using namespace kk::render;

namespace {

TitleStyle style(const QString& text = QStringLiteral("H H"))
{
    TitleStyle s;
    s.text = text;
    s.size = 90;
    s.color = Qt::white;
    s.canvas = QSize(640, 270);
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

/// Rows {top, bottom} that hold ink (alpha > 128) in the columns [x0, x1], or {-1, -1}.
QPair<int, int> inkRows(const QImage& image, int x0, int x1)
{
    int top = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (qAlpha(image.pixel(x, y)) > 128) {
                if (top < 0)
                    top = y;
                bottom = y;
                break;
            }
        }
    }
    return {top, bottom};
}

/// Columns {left, right} that hold ink.
QPair<int, int> inkColumns(const QImage& image)
{
    int left = -1;
    int right = -1;
    for (int x = 0; x < image.width(); ++x) {
        for (int y = 0; y < image.height(); ++y) {
            if (qAlpha(image.pixel(x, y)) > 128) {
                if (left < 0)
                    left = x;
                right = x;
                break;
            }
        }
    }
    return {left, right};
}

} // namespace

class TestTitleDecor : public QObject
{
    Q_OBJECT
private slots:
    void inspectorRows()
    {
        const EffectDescriptor* title = EffectRegistry::instance().find(titles::Text);
        QVERIFY(title);
        for (const QString& key : {keys::Underline, keys::Strikethrough}) {
            const ParamDescriptor* p = title->param(key);
            QVERIFY2(p, qPrintable(key));
            QCOMPARE(p->type, ParamType::Bool);
            QCOMPARE(p->defStatic.toBool(), false);
        }
        const NodeDescriptor* text = NodeRegistry::instance().find(nodetypes::Text);
        QVERIFY(text);
        QVERIFY(text->param(keys::Underline));
        QVERIFY(text->param(keys::Strikethrough));
    }

    void offIsUnchanged()
    {
        TitleStyle a = style();
        TitleStyle b = a;
        b.underline = false;
        b.strikethrough = false;
        QCOMPARE(onCanvas(a), onCanvas(b));
    }

    void underlineRunsUnderLettersAndSpaces()
    {
        const QImage plain = onCanvas(style());
        TitleStyle u = style();
        u.underline = true;
        const QImage under = onCanvas(u);
        const auto [left, right] = inkColumns(plain);
        QVERIFY(left > 0 && right > left);
        const int mid = (left + right) / 2; // the space between the two letters
        QCOMPARE(inkRows(plain, mid - 2, mid + 2).first, -1); // nothing there without the bar
        const auto letters = inkRows(plain, left, right);
        const auto bar = inkRows(under, mid - 2, mid + 2);
        QVERIFY2(bar.first > letters.second,
            qPrintable(QStringLiteral("bar %1..%2 letters end %3")
                           .arg(bar.first)
                           .arg(bar.second)
                           .arg(letters.second)));
        QCOMPARE(bar.second - bar.first >= 1, true);
        // The bar covers the whole text, end to end.
        const auto cols = inkColumns(under);
        QVERIFY(cols.first <= left + 1 && cols.second >= right - 1);
        QVERIFY(inkRows(under, left, left + 3).second >= bar.first);
    }

    void strikethroughRunsThroughTheMiddle()
    {
        const QImage plain = onCanvas(style());
        TitleStyle t = style();
        t.strikethrough = true;
        const QImage struck = onCanvas(t);
        const auto [left, right] = inkColumns(plain);
        const int mid = (left + right) / 2;
        const auto letters = inkRows(plain, left, right);
        const auto bar = inkRows(struck, mid - 2, mid + 2);
        QVERIFY(bar.first > letters.first && bar.second < letters.second);
        const int centre = (bar.first + bar.second) / 2;
        QVERIFY2(centre > letters.first + (letters.second - letters.first) / 4
                && centre < letters.second - (letters.second - letters.first) / 4,
            qPrintable(QString::number(centre)));
    }

    void bothBarsAtOnce()
    {
        TitleStyle t = style();
        t.underline = true;
        t.strikethrough = true;
        const QImage both = onCanvas(t);
        const auto [left, right] = inkColumns(onCanvas(style()));
        const int mid = (left + right) / 2;
        int rowsWithInk = 0;
        for (int y = 0; y < both.height(); ++y)
            if (qAlpha(both.pixel(mid, y)) > 128)
                ++rowsWithInk;
        QVERIFY(rowsWithInk >= 2);
        QVERIFY(onCanvas(t) != onCanvas([&] {
            TitleStyle s = style();
            s.underline = true;
            return s;
        }()));
    }

    void everyLineGetsABar()
    {
        TitleStyle u = style(QStringLiteral("H H\nH H"));
        const QImage plain = onCanvas(u);
        u.underline = true;
        const QImage under = onCanvas(u);
        const auto [left, right] = inkColumns(plain);
        const int mid = (left + right) / 2;
        int bars = 0;
        bool inBar = false;
        for (int y = 0; y < under.height(); ++y) {
            const bool ink = qAlpha(under.pixel(mid, y)) > 128;
            if (ink && !inBar)
                ++bars;
            inBar = ink;
        }
        QCOMPARE(bars, 2);
    }

    void writeOnKeepsTheBarUnderTheShownLettersOnly()
    {
        TitleStyle u = style(QStringLiteral("HHHH"));
        u.underline = true;
        const QImage full = onCanvas(u);
        u.writeOnEnd = 0.5; // two of four letters
        const QImage half = onCanvas(u);
        const auto fullCols = inkColumns(full);
        const auto halfCols = inkColumns(half);
        QVERIFY(halfCols.second < fullCols.second - (fullCols.second - fullCols.first) / 4);
        QVERIFY(halfCols.first <= fullCols.first + 1);
    }

    void textPlusBarsFollowTheGlyphs()
    {
        TitleStyle u = style();
        u.plus.layout = TextPlus::Layout::Frame;
        u.plus.frameWidth = 600;
        QVERIFY(u.plus.active());
        const QImage plain = onCanvas(u);
        u.underline = true;
        const QImage under = onCanvas(u);
        const auto [left, right] = inkColumns(plain);
        const int mid = (left + right) / 2;
        QCOMPARE(inkRows(plain, mid - 2, mid + 2).first, -1);
        const auto letters = inkRows(plain, left, right);
        const auto bar = inkRows(under, mid - 2, mid + 2);
        QVERIFY(bar.first > letters.second);
    }

    void projectFormatRoundTrip()
    {
        ParamSet p;
        p.setStatic(keys::Underline, true);
        p.setStatic(keys::Strikethrough, true);
        const ParamSet back = serialization::paramSetFromJson(serialization::paramSetToJson(p));
        QCOMPARE(back, p);
        QVERIFY(back.staticValue(keys::Underline, false).toBool());
        QVERIFY(back.staticValue(keys::Strikethrough, false).toBool());
        ClipData clip;
        const TitleStyle s
            = resolveLayerStyle(back, TitleLayerKind::Text, clip, 0, QSize(640, 270), 1.0);
        QVERIFY(s.underline && s.strikethrough);
        QVERIFY(s
            != resolveLayerStyle(ParamSet {}, TitleLayerKind::Text, clip, 0, QSize(640, 270), 1.0));
    }
};

QTEST_MAIN(TestTitleDecor)
#include "test_titledecor.moc"
