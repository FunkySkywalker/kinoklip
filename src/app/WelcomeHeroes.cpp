// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/WelcomeHeroes.h"

#include "ui/common/UiAnimation.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

namespace kk::welcomeanim {
namespace {

constexpr double kCorner = 10;
constexpr int kFadeMs = 380;

int scaled(int ms)
{
    return std::max(1, qRound(ms / uianim::speed()));
}

QColor withAlpha(QColor c, double a)
{
    c.setAlphaF(std::clamp(a, 0.0, 1.0));
    return c;
}

QColor mix(const QColor& a, const QColor& b, double t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
        a.greenF() + (b.greenF() - a.greenF()) * t, a.blueF() + (b.blueF() - a.blueF()) * t);
}

bool dark(const QPalette& palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

double smoother(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * t * (t * (t * 6 - 15) + 10);
}

/// A slow crossfade from 0 to 1 (instant when animations are off or the widget is hidden).
void fadeTo(QVariantAnimation* anim, QWidget* owner, double& value)
{
    anim->stop();
    if (!uianim::enabled() || !owner->isVisible()) {
        value = 1;
        owner->update();
        return;
    }
    value = 0;
    anim->setDuration(scaled(kFadeMs));
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setEasingCurve(QEasingCurve::InOutSine);
    anim->start();
}

QVariantAnimation* makeFade(QWidget* owner, double& value)
{
    auto* anim = new QVariantAnimation(owner);
    QObject::connect(anim, &QVariantAnimation::valueChanged, owner, [owner, &value](const QVariant& v) {
        value = v.toDouble();
        owner->update();
    });
    return anim;
}

} // namespace

// --- Hero ------------------------------------------------------------------------------------

Hero::Hero(int loopMs, QWidget* parent)
    : QWidget(parent)
    , m_loop(new QVariantAnimation(this))
    , m_loopMs(loopMs)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMaximumWidth(520);
    setFixedHeight(170);
    m_loop->setStartValue(0.0);
    m_loop->setEndValue(1.0);
    m_loop->setEasingCurve(QEasingCurve::Linear);
    m_loop->setLoopCount(-1);
    connect(m_loop, &QVariantAnimation::valueChanged, this, qOverload<>(&QWidget::update));
}

bool Hero::looping() const
{
    return m_loop->state() == QAbstractAnimation::Running;
}

double Hero::phase() const
{
    return looping() ? m_loop->currentValue().toDouble() : stillPhase();
}

QColor Hero::background() const
{
    return palette().color(QPalette::Base);
}

void Hero::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_loopMs > 0 && uianim::enabled() && !looping()) {
        m_loop->setDuration(scaled(m_loopMs));
        m_loop->start();
    }
}

void Hero::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_loop->stop(); // nothing runs while the page is not shown
}

void Hero::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF r = QRectF(rect());
    QPainterPath shape;
    shape.addRoundedRect(r, kCorner, kCorner);
    p.fillPath(shape, background());
    p.save();
    p.setClipPath(shape);
    paintContent(p, r);
    p.restore();
    p.setPen(QPen(withAlpha(palette().color(QPalette::WindowText), 0.14), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), kCorner, kCorner);
}

// --- ThemeHero -------------------------------------------------------------------------------

ThemeHero::ThemeHero(QWidget* parent)
    : Hero(0, parent)
    , m_picture(QStringLiteral(":/welcome/themes.jpg"))
{
    setObjectName(QStringLiteral("WelcomeThemeHero"));
}

void ThemeHero::paintContent(QPainter& p, const QRectF& rect)
{
    if (m_picture.isNull())
        return;
    // Cover: scale to fill the width and cut a band; the editor's viewer and timeline stay in it.
    const double aspect = rect.width() / rect.height();
    const double srcH = std::min<double>(m_picture.height(), m_picture.width() / aspect);
    const double srcW = srcH * aspect;
    const QRectF source((m_picture.width() - srcW) / 2.0, (m_picture.height() - srcH) * 0.72, srcW,
        srcH);
    p.drawPixmap(rect, m_picture, source);
}

// --- MenusHero -------------------------------------------------------------------------------

MenusHero::MenusHero(QWidget* parent)
    : Hero(11000, parent)
    , m_titlesAnim(makeFade(this, m_titlesFade))
    , m_vimAnim(makeFade(this, m_vimFade))
{
    setObjectName(QStringLiteral("WelcomeMenusHero"));
}

void MenusHero::setTitles(const QStringList& titles)
{
    if (titles == m_titles)
        return;
    m_oldTitles = m_titles;
    m_titles = titles;
    if (m_oldTitles.isEmpty()) {
        m_titlesFade = 1;
        update();
        return;
    }
    fadeTo(m_titlesAnim, this, m_titlesFade);
}

void MenusHero::setVim(bool on)
{
    if (on == m_vim)
        return;
    m_oldVim = m_vim;
    m_vim = on;
    fadeTo(m_vimAnim, this, m_vimFade);
}

namespace {

/// The title font, shrunk until all titles fit @p avail.
QFont fitFont(const QFont& base, const QStringList& titles, double avail, double padding)
{
    QFont font = base;
    for (int i = 0; i < 16; ++i) {
        const QFontMetricsF fm(font);
        double total = 0;
        for (const QString& t : titles)
            total += fm.horizontalAdvance(t) + 2 * padding;
        if (total <= avail || font.pointSizeF() <= 7)
            break;
        font.setPointSizeF(font.pointSizeF() - 0.5);
    }
    return font;
}

/// Left edge and width of each title in a strip starting at @p x.
QList<QPair<double, double>> titleBoxes(const QFont& font, const QStringList& titles, double x,
    double padding)
{
    const QFontMetricsF fm(font);
    QList<QPair<double, double>> boxes;
    for (const QString& t : titles) {
        const double w = fm.horizontalAdvance(t) + 2 * padding;
        boxes.append({x, w});
        x += w;
    }
    return boxes;
}

} // namespace

void MenusHero::drawTitles(QPainter& p, const QRectF& strip, const QStringList& titles,
    double opacity, double pill)
{
    if (titles.isEmpty() || opacity <= 0)
        return;
    constexpr double pad = 7;
    const double x0 = strip.left() + 10;
    const QFont font = fitFont(this->font(), titles, strip.width() - 20, pad);
    const auto boxes = titleBoxes(font, titles, x0, pad);
    const QColor text = palette().color(QPalette::WindowText);
    p.save();
    p.setFont(font);
    if (pill >= 0) {
        const int i = std::min(int(pill), int(boxes.size()) - 1);
        const int j = std::min(i + 1, int(boxes.size()) - 1);
        const double f = pill - i;
        const double left = boxes[i].first + (boxes[j].first - boxes[i].first) * f;
        const double width = boxes[i].second + (boxes[j].second - boxes[i].second) * f;
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(palette().color(QPalette::Highlight), 0.28));
        const double h = strip.height() - 12;
        p.drawRoundedRect(QRectF(left, strip.center().y() - h / 2, width, h), h / 2, h / 2);
    }
    p.setPen(withAlpha(text, 0.85 * opacity));
    for (int i = 0; i < titles.size(); ++i)
        p.drawText(QRectF(boxes[i].first, strip.top(), boxes[i].second, strip.height()),
            Qt::AlignCenter, titles[i]);
    p.restore();
}

void MenusHero::drawKeys(QPainter& p, const QRectF& area, const QStringList& keys, double opacity)
{
    if (opacity <= 0)
        return;
    constexpr double capW = 48;
    constexpr double capH = 44;
    constexpr double gap = 14;
    const double total = keys.size() * capW + (keys.size() - 1) * gap;
    double x = area.center().x() - total / 2;
    const double y = area.center().y() - capH / 2 - 1;
    const QColor text = palette().color(QPalette::WindowText);
    const QColor button = palette().color(QPalette::Button);
    QFont font = this->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.25);
    p.save();
    p.setOpacity(opacity);
    p.setFont(font);
    for (const QString& key : keys) {
        const QRectF cap(x, y, capW, capH);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(text, 0.16)); // the key's lower edge
        p.drawRoundedRect(cap.translated(0, 3), 8, 8);
        p.setPen(QPen(withAlpha(text, 0.22), 1));
        p.setBrush(dark(palette()) ? button.lighter(115) : button.lighter(108));
        p.drawRoundedRect(cap, 8, 8);
        p.setPen(withAlpha(text, 0.9));
        p.drawText(cap, Qt::AlignCenter, key);
        x += capW + gap;
    }
    p.restore();
}

void MenusHero::paintContent(QPainter& p, const QRectF& rect)
{
    const double width = std::min(rect.width() - 24, 480.0);
    const QRectF window(rect.center().x() - width / 2, rect.top() + 18, width, rect.height() - 36);
    const QColor text = palette().color(QPalette::WindowText);
    const QColor base = palette().color(QPalette::Window);
    // The mock window with its title strip.
    p.setPen(QPen(withAlpha(text, 0.2), 1));
    p.setBrush(base);
    p.drawRoundedRect(window, 8, 8);
    const QRectF strip(window.left(), window.top(), window.width(), 34);
    p.save();
    QPainterPath shape;
    shape.addRoundedRect(window, 8, 8);
    p.setClipPath(shape);
    p.fillRect(strip, withAlpha(text, 0.06));
    p.restore();
    p.setPen(QPen(withAlpha(text, 0.12), 1));
    p.drawLine(QPointF(strip.left() + 0.5, strip.bottom()), QPointF(strip.right() - 0.5, strip.bottom()));

    const int n = std::max<int>(1, m_titles.size());
    const double pill = (n - 1) * (0.5 - 0.5 * std::cos(2 * M_PI * phase()));
    if (m_titlesFade < 1)
        drawTitles(p, strip, m_oldTitles, 1.0 - m_titlesFade, -1);
    drawTitles(p, strip, m_titles, m_titlesFade, pill);

    const QRectF keysArea(window.left(), strip.bottom(), window.width(), window.bottom() - strip.bottom());
    static const QStringList kNormal {QStringLiteral("I"), QStringLiteral("O"), QStringLiteral("F9"),
        QStringLiteral("B")};
    static const QStringList kVim {QStringLiteral("h"), QStringLiteral("j"), QStringLiteral("k"),
        QStringLiteral("l")};
    if (m_vimFade < 1)
        drawKeys(p, keysArea, m_oldVim ? kVim : kNormal, 1.0 - m_vimFade);
    drawKeys(p, keysArea, m_vim ? kVim : kNormal, m_vimFade);
}

// --- TimelineHero ----------------------------------------------------------------------------

TimelineHero::TimelineHero(QWidget* parent)
    : Hero(5000, parent)
{
    setObjectName(QStringLiteral("WelcomeTimelineHero"));
}

void TimelineHero::setZoomAroundMouse(bool mouse)
{
    m_mouse = mouse;
    update();
}

void TimelineHero::paintContent(QPainter& p, const QRectF& rect)
{
    const QColor text = palette().color(QPalette::WindowText);
    const bool isDark = dark(palette());

    // Zoom in, hold, out, hold; about 4 s of a 5 s loop.
    const double f = phase();
    double u = 0;
    if (f < 0.4)
        u = smoother(f / 0.4);
    else if (f < 0.55)
        u = 1;
    else if (f < 0.95)
        u = 1 - smoother((f - 0.55) / 0.4);
    const double s = 1.0 + 1.3 * u;

    const double x0 = rect.left() + 46;
    const double x1 = rect.right() - 12;
    const double w = x1 - x0;
    constexpr double playhead = 0.36; // in world coordinates (0..1 across the unzoomed width)
    const double anchor = x0 + (m_mouse ? 0.72 : playhead) * w;
    const auto sx = [&](double world) { return anchor + (x0 + world * w - anchor) * s; };

    const double rulerBase = rect.top() + 34;
    struct Lane
    {
        const char* name;
        double y, h;
    };
    const Lane lanes[] = {{"V2", rect.top() + 42, 32}, {"V1", rect.top() + 78, 32},
        {"A1", rect.top() + 114, 36}};

    // Lanes and their names.
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    p.setFont(small);
    for (const Lane& lane : lanes) {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(text, 0.10));
        p.drawRoundedRect(QRectF(rect.left() + 12, lane.y, 28, lane.h), 4, 4);
        p.setBrush(withAlpha(text, 0.045));
        p.drawRoundedRect(QRectF(x0, lane.y, w, lane.h), 4, 4);
        p.setPen(withAlpha(text, 0.6));
        p.drawText(QRectF(rect.left() + 12, lane.y, 28, lane.h), Qt::AlignCenter,
            QLatin1String(lane.name));
    }

    p.save();
    p.setClipRect(QRectF(x0, rect.top(), w, rect.height()));

    // Ruler: the finer ticks appear as the zoom grows.
    const double fine = std::clamp((s - 1.15) / 0.6, 0.0, 1.0);
    for (int k = 0; k <= 40; ++k) {
        const double world = k * 0.025;
        const bool major = k % 4 == 0;
        const bool mid = k % 2 == 0;
        const double a = major ? 0.55 : mid ? 0.3 + 0.2 * fine : 0.35 * fine;
        if (a <= 0.01)
            continue;
        const double x = std::round(sx(world)) + 0.5;
        p.setPen(QPen(withAlpha(text, a), 1));
        p.drawLine(QPointF(x, rulerBase), QPointF(x, rulerBase - (major ? 11 : mid ? 7 : 4)));
    }
    p.setPen(QPen(withAlpha(text, 0.15), 1));
    p.drawLine(QPointF(x0, rulerBase + 0.5), QPointF(x1, rulerBase + 0.5));

    // Clips in soft colours like Resolve's clip colours.
    const auto tone = [&](const char* hex) {
        const QColor c = QColor::fromString(QLatin1String(hex));
        return isDark ? c : c.lighter(112);
    };
    struct Clip
    {
        int lane;
        double a, b;
        const char* colour;
    };
    const Clip clips[] = {
        {0, 0.12, 0.33, "#8a6bb5"}, {0, 0.47, 0.64, "#4a7fb8"},
        {1, 0.03, 0.30, "#4a7fb8"}, {1, 0.305, 0.58, "#3d9a8e"}, {1, 0.62, 0.96, "#4a7fb8"},
        {2, 0.03, 0.58, "#4e9f64"}, {2, 0.62, 0.96, "#4e9f64"},
    };
    for (const Clip& clip : clips) {
        const Lane& lane = lanes[clip.lane];
        const QRectF box(sx(clip.a), lane.y + 1.5, sx(clip.b) - sx(clip.a), lane.h - 3);
        if (box.right() < x0 || box.left() > x1)
            continue;
        const QColor c = tone(clip.colour);
        QLinearGradient g(box.topLeft(), box.bottomLeft());
        g.setColorAt(0, c.lighter(115));
        g.setColorAt(1, c);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(box, 4, 4);
        if (clip.lane == 2) { // a waveform, fixed to the clip so it scales with the zoom
            p.setPen(QPen(withAlpha(Qt::white, 0.55), 1.4, Qt::SolidLine, Qt::RoundCap));
            const double mid = box.center().y();
            const double step = 0.004;
            for (double wd = clip.a + step; wd < clip.b - step; wd += step) {
                const double x = sx(wd);
                if (x < x0 || x > x1)
                    continue;
                const double amp = 0.25 + 0.75 * std::abs(std::sin(wd * 190) * std::sin(wd * 47 + 1));
                const double hh = amp * (box.height() / 2 - 5);
                p.drawLine(QPointF(x, mid - hh), QPointF(x, mid + hh));
            }
        } else { // a title bar on a video clip
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(Qt::white, 0.22));
            p.drawRoundedRect(QRectF(box.left() + 5, box.top() + 5, std::min(34.0, box.width() - 10), 3),
                1.5, 1.5);
        }
    }

    // Playhead (it stays put when zooming around it, and drifts when zooming around the pointer).
    const double px = std::round(sx(playhead)) + 0.5;
    const QColor red(QStringLiteral("#e5484d"));
    p.setPen(QPen(red, 1.5));
    p.drawLine(QPointF(px, rulerBase - 14), QPointF(px, rect.top() + 154));
    p.setPen(Qt::NoPen);
    p.setBrush(red);
    QPainterPath head;
    head.addRoundedRect(QRectF(px - 6, rulerBase - 22, 12, 9), 2, 2);
    head.moveTo(px - 6, rulerBase - 14);
    head.lineTo(px + 6, rulerBase - 14);
    head.lineTo(px, rulerBase - 8);
    head.closeSubpath();
    p.drawPath(head.simplified());
    p.restore();

    if (m_mouse) { // the pointer the zoom is centred on
        p.save();
        p.translate(anchor, lanes[1].y + 6);
        QPainterPath arrow;
        arrow.moveTo(0, 0);
        arrow.lineTo(0, 17);
        arrow.lineTo(4.3, 13.3);
        arrow.lineTo(7.2, 20);
        arrow.lineTo(9.8, 18.8);
        arrow.lineTo(7, 12.4);
        arrow.lineTo(12.5, 12.4);
        arrow.closeSubpath();
        p.setPen(QPen(QColor(20, 20, 20, 230), 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::white);
        p.drawPath(arrow);
        p.restore();
    }
}

// --- SystemHero ------------------------------------------------------------------------------

SystemHero::SystemHero(QWidget* parent)
    : Hero(7000, parent)
    , m_notifyAnim(new QVariantAnimation(this))
{
    setObjectName(QStringLiteral("WelcomeSystemHero"));
    connect(m_notifyAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_notifyShown = v.toDouble();
        update();
    });
}

void SystemHero::setNotifications(bool on)
{
    if (on == m_notify)
        return;
    m_notify = on;
    m_notifyAnim->stop();
    const double target = on ? 1.0 : 0.0;
    if (!uianim::enabled() || !isVisible()) {
        m_notifyShown = target;
        update();
        return;
    }
    m_notifyAnim->setDuration(scaled(300));
    m_notifyAnim->setStartValue(m_notifyShown);
    m_notifyAnim->setEndValue(target);
    m_notifyAnim->setEasingCurve(QEasingCurve::InOutSine);
    m_notifyAnim->start();
}

void SystemHero::paintContent(QPainter& p, const QRectF& rect)
{
    const QColor text = palette().color(QPalette::WindowText);
    const QColor accent = palette().color(QPalette::Highlight);
    const QColor window = palette().color(QPalette::Window);
    const bool isDark = dark(palette());
    const double cx = rect.center().x();
    const double cy = rect.top() + 76;
    const double f = phase();

    // Frames travel left to right under the chip: grey before it, coloured after it.
    static const char* const kColours[] = {"#4a7fb8", "#3d9a8e", "#8a6bb5", "#d08a3c"};
    constexpr int count = 4;
    const double xs = rect.left() + 10;
    const double xe = rect.right() - 10;
    for (int k = 0; k < count; ++k) {
        const double u = std::fmod((k + f) / count + 1.0, 1.0);
        const double cxF = xs + (xe - xs) * u;
        const double edge = std::min({1.0, u / 0.1, (1.0 - u) / 0.1});
        const bool after = cxF > cx;
        QColor c = after ? QColor(QLatin1String(kColours[k])) : mix(window, text, 0.35);
        if (!after && !isDark)
            c = mix(window, text, 0.28);
        const QRectF frame(cxF - 23, cy - 15, 46, 30);
        p.save();
        p.setOpacity(edge);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(frame, 5, 5);
        p.setBrush(withAlpha(Qt::white, after ? 0.6 : 0.4));
        p.drawEllipse(QPointF(frame.right() - 11, frame.top() + 9), 3.5, 3.5);
        QPainterPath hills;
        hills.moveTo(frame.left() + 4, frame.bottom() - 4);
        hills.lineTo(frame.left() + 16, frame.bottom() - 15);
        hills.lineTo(frame.left() + 25, frame.bottom() - 8);
        hills.lineTo(frame.left() + 31, frame.bottom() - 12);
        hills.lineTo(frame.right() - 4, frame.bottom() - 4);
        hills.closeSubpath();
        p.setBrush(withAlpha(Qt::black, 0.22));
        p.drawPath(hills);
        p.restore();
    }

    // The chip: pins, body and a die with a small grid.
    constexpr double size = 84;
    const QRectF body(cx - size / 2, cy - size / 2, size, size);
    p.setPen(Qt::NoPen);
    p.setBrush(withAlpha(text, 0.42));
    for (int i = -2; i <= 2; ++i) {
        const double o = i * 14;
        p.drawRoundedRect(QRectF(cx + o - 2, body.top() - 8, 4, 9), 1.5, 1.5);
        p.drawRoundedRect(QRectF(cx + o - 2, body.bottom() - 1, 4, 9), 1.5, 1.5);
        p.drawRoundedRect(QRectF(body.left() - 8, cy + o - 2, 9, 4), 1.5, 1.5);
        p.drawRoundedRect(QRectF(body.right() - 1, cy + o - 2, 9, 4), 1.5, 1.5);
    }
    p.setPen(QPen(withAlpha(text, 0.35), 1));
    p.setBrush(mix(background(), text, isDark ? 0.16 : 0.08));
    p.drawRoundedRect(body, 14, 14);
    const QRectF die = body.adjusted(15, 15, -15, -15);
    QLinearGradient g(die.topLeft(), die.bottomRight());
    g.setColorAt(0, accent.lighter(120));
    g.setColorAt(1, accent.darker(110));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(die, 8, 8);
    p.setBrush(withAlpha(palette().color(QPalette::HighlightedText), 0.85));
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            p.drawRoundedRect(QRectF(die.left() + 8 + col * 11, die.top() + 8 + row * 11, 6, 6), 1.5, 1.5);

    // The notification card: fades in and out once per loop (still when the loop is not running).
    double a = 1;
    if (looping()) {
        if (f < 0.5)
            a = 0;
        else if (f < 0.6)
            a = smoother((f - 0.5) / 0.1);
        else if (f < 0.9)
            a = 1;
        else
            a = 1.0 - smoother((f - 0.9) / 0.1);
    }
    a *= m_notifyShown;
    if (a > 0.01) {
        const QRectF card(rect.right() - 12 - 164, rect.bottom() - 12 - 40 + 6 * (1 - a), 164, 40);
        p.save();
        p.setOpacity(a);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 40));
        p.drawRoundedRect(card.translated(0, 2), 9, 9);
        p.setPen(QPen(withAlpha(text, 0.22), 1));
        p.setBrush(isDark ? window.lighter(125) : window.lighter(106));
        p.drawRoundedRect(card, 9, 9);
        const QPointF ic(card.left() + 22, card.center().y());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(QStringLiteral("#4e9f64")));
        p.drawEllipse(ic, 10, 10);
        p.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF({ic + QPointF(-4.5, 0.5), ic + QPointF(-1.5, 3.5), ic + QPointF(4.5, -3.5)}));
        QFont bold = font();
        bold.setBold(true);
        bold.setPointSizeF(bold.pointSizeF() * 0.95);
        p.setFont(bold);
        p.setPen(withAlpha(text, 0.9));
        p.drawText(QRectF(card.left() + 40, card.top() + 5, card.width() - 48, 18), Qt::AlignVCenter | Qt::AlignLeft,
            QCoreApplication::translate("WelcomeHeroes", "Render finished"));
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(text, 0.16));
        p.drawRoundedRect(QRectF(card.left() + 40, card.top() + 26, 70, 5), 2.5, 2.5);
        p.restore();
    }
}

} // namespace kk::welcomeanim
