// SPDX-License-Identifier: GPL-3.0-or-later
// ColorWidgets — wheels, bars, the Primaries palette, the curve editor and the Curves palette.
#include "ui/color/ColorWidgets.h"

#include "core/GradeMath.h"
#include "ui/color/ColorSession.h"
#include "ui/common/StyleTokens.h"
#include "ui/common/VimRowOps.h"
#include "ui/inspector/ValueField.h"

#include <QButtonGroup>
#include <QConicalGradient>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk {

namespace {

namespace gk = gradekeys;
using gk::Wheel;

constexpr double kCbScale = 1.8556;
constexpr double kCrScale = 1.5748;
/// Strip height under each wheel (px).
constexpr double kStripHeight = 12.0;
/// Title height above each wheel (px).
constexpr double kTitleHeight = 15.0;
/// Least mouse travel (pixels) for a full puck throw / for the master strip's range of
/// -range() .. +range() (wheel drag sensitivity, independent of the wheel's size).
constexpr double kPuckThrowPx = 200.0;
constexpr double kMasterThrowPx = 400.0;
/// Pick radius of curve points (px).
constexpr double kPointRadius = 6.0;
constexpr Wheel kWheels[] = {Wheel::Lift, Wheel::Gamma, Wheel::Gain, Wheel::Offset};
constexpr Wheel kLogWheels[] = {
    Wheel::LogShadow, Wheel::LogMidtone, Wheel::LogHighlight, Wheel::LogOffset};

QString wheelName(Wheel w)
{
    switch (w) {
    case Wheel::Lift:
        return ColorWheel::tr("Lift");
    case Wheel::Gamma:
        return ColorWheel::tr("Gamma");
    case Wheel::Gain:
        return ColorWheel::tr("Gain");
    case Wheel::Offset:
    case Wheel::LogOffset:
        return ColorWheel::tr("Offset");
    case Wheel::LogShadow:
    case Wheel::HdrShadow:
        return ColorWheel::tr("Shadow");
    case Wheel::HdrGlobal:
        return ColorWheel::tr("Global");
    case Wheel::HdrLight:
        return ColorWheel::tr("Light");
    case Wheel::HdrSpecular:
        return ColorWheel::tr("Specular");
    case Wheel::HdrBlack:
        return ColorWheel::tr("Black Offset");
    case Wheel::LogMidtone:
        return ColorWheel::tr("Midtone");
    case Wheel::LogHighlight:
    case Wheel::HdrHighlight:
        return ColorWheel::tr("Highlight");
    }
    return {};
}

/// Name for object names: unique across both palettes.
QString wheelId(Wheel w)
{
    if (int(w) >= int(Wheel::HdrGlobal))
        return QStringLiteral("Hdr") + QString::number(int(w) - int(Wheel::HdrGlobal));
    return int(w) >= int(Wheel::LogShadow) ? QStringLiteral("Log") + wheelName(w) : wheelName(w);
}

double neutralOf(Wheel w)
{
    return w == Wheel::Gain ? 1.0 : 0.0;
}

/// Colours of the Y, R, G, B channels (fields, bars, curves).
QColor channelColour(int channel)
{
    switch (channel) {
    case 0:
        return QColor(232, 86, 80);
    case 1:
        return QColor(96, 200, 100);
    case 2:
        return QColor(92, 140, 240);
    default:
        return QColor(220, 220, 220);
    }
}

QToolButton* modeButton(const QString& text, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setProperty("kkTab", true); // Kinoklip style: flat with a red underline when checked
    return b;
}

} // namespace

// ---- ColorWheel ----

ColorWheel::ColorWheel(Wheel wheel, QWidget* parent)
    : QWidget(parent)
    , m_wheel(wheel)
{
    setObjectName(QStringLiteral("ColorWheel_") + wheelId(wheel));
    setToolTip(tr("%1: drag the centre to balance the colour, drag the bar below for the master "
                  "level; Shift for fine control; the arrow at the top right or a "
                  "double-click resets")
                   .arg(wheelName(wheel)));
}

double ColorWheel::masterRange() const
{
    if (m_wheel == Wheel::HdrBlack)
        return 0.25;
    return int(m_wheel) >= int(Wheel::HdrGlobal) ? 1.0 : range(); // HDR master: exposure, stops
}

double ColorWheel::range() const
{
    return m_wheel == Wheel::Gain ? 0.5 : 0.25;
}

QPointF ColorWheel::puckFor(const std::array<double, 3>& d) const
{
    const double y = grademath::kLumaR * d[0] + grademath::kLumaG * d[1] + grademath::kLumaB * d[2];
    const double cb = (d[2] - y) / kCbScale;
    const double cr = (d[0] - y) / kCrScale;
    return QPointF(cb, cr) / range();
}

std::array<double, 3> ColorWheel::deltaFor(QPointF puck) const
{
    // Luma-neutral: red and blue from Cr / Cb, green balancing the luma.
    const double cb = puck.x() * range();
    const double cr = puck.y() * range();
    const double r = cr * kCrScale;
    const double b = cb * kCbScale;
    const double g = -(grademath::kLumaR * r + grademath::kLumaB * b) / grademath::kLumaG;
    return {r, g, b};
}

void ColorWheel::setValues(const std::array<double, 3>& rgbDelta, double masterDelta)
{
    if (rgbDelta == m_rgb && masterDelta == m_master)
        return;
    m_rgb = rgbDelta;
    m_master = masterDelta;
    update();
}

QRectF ColorWheel::discRect() const
{
    const double top = kTitleHeight;
    const double side
        = std::max(10.0, std::min(double(width()) - 6, height() - top - kStripHeight - 8));
    return QRectF((width() - side) / 2, top, side, side);
}

double ColorWheel::puckThrowPx() const
{
    return std::max(discRect().width() / 2 - 6, kPuckThrowPx);
}

double ColorWheel::masterThrowPx() const
{
    return std::max(stripRect().width(), kMasterThrowPx);
}

QRectF ColorWheel::resetRect() const
{
    const QRectF d = discRect();
    return QRectF(
        std::min(d.right(), double(width())) - kTitleHeight, 0, kTitleHeight, kTitleHeight);
}

QRectF ColorWheel::stripRect() const
{
    const QRectF d = discRect();
    const double w = std::min(double(width()) - 8, d.width() + 8);
    return QRectF((width() - w) / 2, d.bottom() + 5, w, kStripHeight);
}

void ColorWheel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    p.setPen(pal.color(QPalette::WindowText));
    p.drawText(QRectF(0, 0, width(), kTitleHeight - 2), Qt::AlignHCenter | Qt::AlignVCenter,
        wheelName(m_wheel));
    {
        // The wheel's reset button (top right, as in Resolve); bright when there is something
        // to reset.
        const bool changed
            = m_master != 0.0 || m_rgb[0] != 0.0 || m_rgb[1] != 0.0 || m_rgb[2] != 0.0;
        QColor c = pal.color(QPalette::WindowText);
        c.setAlphaF(changed ? 1.0f : 0.35f);
        p.setPen(c);
        p.drawText(resetRect(), Qt::AlignCenter, QStringLiteral("\u21BA"));
        p.setPen(pal.color(QPalette::WindowText));
    }
    const QRectF d = discRect();
    // Hue ring: Cr up, Cb right, so a hue sits where the puck adds it.
    QConicalGradient ring(d.center(), 0);
    for (int i = 0; i <= 24; ++i) {
        const double a = i / 24.0 * 2 * std::numbers::pi;
        const std::array<double, 3> c = deltaFor(QPointF(std::cos(a), std::sin(a)));
        const double k = 0.5 / std::max({std::abs(c[0]), std::abs(c[1]), std::abs(c[2]), 1e-6});
        ring.setColorAt(i / 24.0,
            QColor::fromRgbF(float(std::clamp(0.5 + c[0] * k, 0.0, 1.0)),
                float(std::clamp(0.5 + c[1] * k, 0.0, 1.0)),
                float(std::clamp(0.5 + c[2] * k, 0.0, 1.0))));
    }
    const bool kk = style::kinoklipActive();
    const style::Colors kc = style::colors(pal);
    if (kk) {
        // Resolve's wheel: a dark outer ring, a thin hue ring and a dark disc inside it.
        p.setPen(Qt::NoPen);
        p.setBrush(kc.wheelRingOuter);
        p.drawEllipse(d);
        const QRectF hue = d.adjusted(2, 2, -2, -2);
        p.setBrush(ring);
        p.drawEllipse(hue);
        p.setBrush(kc.wheelDisc);
        const double rw = style::kWheelRing;
        p.drawEllipse(hue.adjusted(rw, rw, -rw, -rw));
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(ring);
        p.drawEllipse(d);
        // The inner disc: dark with a faint tint towards the rim.
        QRadialGradient fade(d.center(), d.width() / 2);
        fade.setColorAt(0.0, QColor(34, 34, 38, 255));
        fade.setColorAt(0.80, QColor(34, 34, 38, 235));
        fade.setColorAt(0.93, QColor(34, 34, 38, 120));
        fade.setColorAt(1.0, QColor(34, 34, 38, 0));
        p.setBrush(fade);
        p.drawEllipse(d.adjusted(2, 2, -2, -2));
    }
    p.setPen(QPen(kk ? kc.wheelLine : QColor(255, 255, 255, 40), kk ? style::kWheelLine : 1));
    const double inset = d.width() * 0.12;
    p.drawLine(
        QPointF(d.left() + inset, d.center().y()), QPointF(d.right() - inset, d.center().y()));
    p.drawLine(
        QPointF(d.center().x(), d.top() + inset), QPointF(d.center().x(), d.bottom() - inset));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(d.center(), d.width() * 0.25, d.width() * 0.25);
    // Puck, with a line from the centre when moved.
    QPointF puck = puckFor(m_rgb);
    const double len = std::hypot(puck.x(), puck.y());
    if (len > 1.0)
        puck /= len;
    const QPointF at = d.center() + QPointF(puck.x(), -puck.y()) * (d.width() / 2 - 6);
    if (len > 1e-6) {
        p.setPen(QPen(QColor(255, 255, 255, 110), 1));
        p.drawLine(d.center(), at);
    }
    p.setPen(QPen(Qt::white, 1.5));
    p.drawEllipse(at, kk ? double(style::kWheelPuck) : 4.0, kk ? double(style::kWheelPuck) : 4.0);
    // Master strip: a groove with the neutral mark and a thumb.
    const QRectF s = stripRect();
    if (kk) {
        p.setPen(Qt::NoPen);
        p.setBrush(kc.groove);
        p.drawRoundedRect(QRectF(s.left(), s.center().y() - style::kWheelStrip / 2.0, s.width(), style::kWheelStrip),
            style::kWheelStrip / 2.0, style::kWheelStrip / 2.0);
    } else {
        p.setPen(QPen(pal.color(QPalette::Dark), 1));
        p.setBrush(pal.color(QPalette::Base).darker(120));
        p.drawRoundedRect(s, 3, 3);
    }
    p.setPen(QPen(QColor(255, 255, 255, 50), 1));
    p.drawLine(QPointF(s.center().x(), s.top() + 2), QPointF(s.center().x(), s.bottom() - 2));
    const double t = std::clamp(m_master / masterRange() * 0.5 + 0.5, 0.0, 1.0);
    const double x = s.left() + 3 + t * (s.width() - 6);
    p.setPen(Qt::NoPen);
    p.setBrush(m_master == 0.0 ? pal.color(QPalette::Mid) : pal.color(QPalette::Highlight));
    p.drawRoundedRect(QRectF(x - 3, s.top() + 1, 6, s.height() - 2), 2, 2);
}

void ColorWheel::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF pos = event->position();
    if (resetRect().contains(pos)) {
        emit resetRequested();
        return;
    }
    m_pressPos = pos;
    m_pressMaster = m_master;
    m_pressPuck = puckFor(m_rgb);
    m_pressRgb = m_rgb;
    if (stripRect().adjusted(-2, -4, 2, 4).contains(pos))
        m_drag = Drag::Strip;
    else if (discRect().contains(pos))
        m_drag = Drag::Puck;
    else
        return;
    emit dragStarted();
}

void ColorWheel::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    const bool fine = event->modifiers() & Qt::ShiftModifier;
    if (m_drag == Drag::Puck) {
        // Relative drag (the puck moves from where it was), like a trackball, at a fixed rate:
        // a full throw takes at least kPuckThrowPx of mouse travel whatever the wheel's size,
        // so a small wheel (a laptop screen) is not coarser than a big one.
        QPointF delta = (pos - m_pressPos) / puckThrowPx();
        delta.setY(-delta.y());
        if (fine)
            delta *= 0.25;
        QPointF v = m_pressPuck + delta;
        const double len = std::hypot(v.x(), v.y());
        if (len > 1.0)
            v /= len;
        // The puck shows only the colour (luma-neutral) part of the R, G, B values: a luma
        // part typed into the fields stays as it was.
        const std::array<double, 3> colour = deltaFor(v);
        const std::array<double, 3> pressColour = deltaFor(m_pressPuck);
        for (int c = 0; c < 3; ++c)
            m_rgb[size_t(c)] = colour[size_t(c)] + (m_pressRgb[size_t(c)] - pressColour[size_t(c)]);
        update();
        emit balanceChanged(m_rgb);
    } else if (m_drag == Drag::Strip) {
        double dx = pos.x() - m_pressPos.x();
        if (fine)
            dx *= 0.25;
        m_master = std::clamp(
            m_pressMaster + dx / masterThrowPx() * 2 * masterRange(), -masterRange() * 4,
            masterRange() * 4);
        update();
        emit masterChanged(m_master);
    }
}

void ColorWheel::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_drag == Drag::None)
        return;
    m_drag = Drag::None;
    emit dragFinished();
}

void ColorWheel::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (resetRect().contains(event->position()))
        return; // the press already reset
    if (stripRect().adjusted(-2, -4, 2, 4).contains(event->position()))
        emit masterResetRequested();
    else
        emit resetRequested();
}

// ---- PrimaryBars ----

PrimaryBars::PrimaryBars(Wheel wheel, QWidget* parent)
    : QWidget(parent)
    , m_wheel(wheel)
{
    setObjectName(QStringLiteral("PrimaryBars_") + wheelId(wheel));
    setToolTip(tr("%1: drag a bar up or down; double-click resets it").arg(wheelName(wheel)));
}

double PrimaryBars::range() const
{
    return m_wheel == Wheel::Gain ? 1.0 : 0.5;
}

void PrimaryBars::setValues(const std::array<double, 4>& deltas)
{
    if (deltas == m_values)
        return;
    m_values = deltas;
    update();
}

QRectF PrimaryBars::barRect(int channel) const
{
    // Order on screen: Y, R, G, B.
    const int slot = channel == 3 ? 0 : channel + 1;
    const double top = kTitleHeight + 2;
    const double h = std::max(10.0, height() - top - 4);
    const double total = std::min(double(width()) - 8, 4 * 26.0);
    const double w = total / 4;
    const double left = (width() - total) / 2;
    return QRectF(left + slot * w + 4, top, w - 8, h);
}

int PrimaryBars::channelAt(QPointF pos) const
{
    for (int c = 0; c < 4; ++c) {
        if (barRect(c).adjusted(-4, 0, 4, 0).contains(pos))
            return c;
    }
    return -1;
}

void PrimaryBars::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    p.setPen(pal.color(QPalette::WindowText));
    p.drawText(QRectF(0, 0, width(), kTitleHeight - 2), Qt::AlignHCenter | Qt::AlignVCenter,
        wheelName(m_wheel));
    for (int c = 0; c < 4; ++c) {
        const QRectF r = barRect(c);
        p.setPen(QPen(pal.color(QPalette::Dark), 1));
        p.setBrush(pal.color(QPalette::Base).darker(120));
        p.drawRoundedRect(r, 2, 2);
        const double mid = r.center().y();
        const double t = std::clamp(m_values[size_t(c)] / range(), -1.0, 1.0);
        const double y = mid - t * (r.height() / 2 - 2);
        QColor fill = channelColour(c);
        fill.setAlpha(170);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRect(QRectF(r.left() + 2, std::min(mid, y), r.width() - 4, std::abs(y - mid)));
        p.setPen(QPen(QColor(255, 255, 255, 60), 1));
        p.drawLine(QPointF(r.left(), mid), QPointF(r.right(), mid));
        p.setPen(QPen(channelColour(c), 2));
        p.drawLine(QPointF(r.left() + 1, y), QPointF(r.right() - 1, y));
    }
}

void PrimaryBars::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_drag = channelAt(event->position());
    if (m_drag < 0)
        return;
    m_pressY = event->position().y();
    m_pressValue = m_values[size_t(m_drag)];
    emit dragStarted();
}

void PrimaryBars::mouseMoveEvent(QMouseEvent* event)
{
    if (m_drag < 0)
        return;
    double dy = m_pressY - event->position().y();
    if (event->modifiers() & Qt::ShiftModifier)
        dy *= 0.25;
    const QRectF r = barRect(m_drag);
    const double v
        = std::clamp(m_pressValue + dy / (r.height() / 2) * range(), -range() * 2, range() * 2);
    m_values[size_t(m_drag)] = v;
    update();
    emit channelChanged(m_drag, v);
}

void PrimaryBars::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_drag < 0)
        return;
    m_drag = -1;
    emit dragFinished();
}

void PrimaryBars::mouseDoubleClickEvent(QMouseEvent* event)
{
    const int c = channelAt(event->position());
    if (c >= 0)
        emit channelResetRequested(c);
}

// ---- PrimariesPanel ----

PrimariesPanel::PrimariesPanel(ColorSession* session, QWidget* parent, Kind kind)
    : QWidget(parent)
    , m_session(session)
    , m_kind(kind)
{
    markVimForm(this);
    const bool log = kind == Kind::Log;
    const std::array<Wheel, 4> wheelSet = log ? std::array<Wheel, 4> {kLogWheels[0], kLogWheels[1], kLogWheels[2], kLogWheels[3]}
                                              : std::array<Wheel, 4> {kWheels[0], kWheels[1], kWheels[2], kWheels[3]};
    const QString prefix = log ? QStringLiteral("LogPalette_") : QStringLiteral("Primaries_");
    setObjectName(log ? QStringLiteral("LogPanel") : QStringLiteral("PrimariesPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(2);

    struct Spec
    {
        const QString* key;
        double min, max, step;
        int decimals;
    };
    // A label + value pair; pairs are spread over the row with stretches between them.
    auto makeField = [this, &prefix](const Spec& s, const QString& label, QHBoxLayout* row) {
        if (row->count() > 0)
            row->addStretch(1);
        auto* name = new QLabel(label, this);
        name->setObjectName(QStringLiteral("PrimariesLabel"));
        auto* f = new ValueField(this);
        f->setObjectName(prefix + *s.key);
        f->setRange(s.min, s.max);
        f->setStep(s.step);
        f->setDecimals(s.decimals);
        f->setToolTip(gk::label(*s.key));
        f->setFixedWidth(std::max(52, f->sizeHint().width()));
        row->addWidget(name);
        row->addSpacing(4);
        row->addWidget(f);
        bindField(f, *s.key);
        return f;
    };

    // ---- Top adjustment row + mode buttons ----
    auto* top = new QHBoxLayout;
    top->setSpacing(0);
    makeField({&gk::Temperature, -1, 1, 0.005, 3}, tr("Temp"), top);
    makeField({&gk::Tint, -1, 1, 0.005, 3}, tr("Tint"), top);
    makeField({&gk::Contrast, 0, 4, 0.005, 3}, tr("Contrast"), top);
    makeField({&gk::Pivot, 0, 1, 0.002, 3}, tr("Pivot"), top);
    makeField({&gk::MidtoneDetail, -100, 100, 0.5, 2}, tr("MD"), top);
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(4);
    if (!log) {
        m_autoButton = new QToolButton(this);
        m_autoButton->setObjectName(QStringLiteral("PrimariesAutoButton"));
        m_autoButton->setText(tr("Auto"));
        m_autoButton->setAutoRaise(true);
        m_autoButton->setToolTip(tr("Auto Color: balance the node's lift, gamma and gain from "
                                    "the picture (black and white points, colour cast)"));
        connect(m_autoButton, &QToolButton::clicked, this, &PrimariesPanel::autoColorRequested);
        topRow->addWidget(m_autoButton);
    }
    topRow->addLayout(top, 1);
    topRow->addSpacing(24);
    m_wheelsButton = modeButton(tr("Wheels"), this);
    m_wheelsButton->setObjectName(QStringLiteral("PrimariesWheelsButton"));
    m_barsButton = modeButton(tr("Bars"), this);
    m_barsButton->setObjectName(QStringLiteral("PrimariesBarsButton"));
    auto* modes = new QButtonGroup(this);
    modes->addButton(m_wheelsButton, 0);
    modes->addButton(m_barsButton, 1);
    m_wheelsButton->setChecked(true);
    topRow->addWidget(m_wheelsButton);
    topRow->addWidget(m_barsButton);
    layout->addLayout(topRow);

    // ---- Wheels / bars with their channel fields ----
    m_modes = new QStackedWidget(this);
    auto* wheelsPage = new QWidget;
    auto* barsPage = new QWidget;
    auto* wheelsRow = new QHBoxLayout(wheelsPage);
    auto* barsRow = new QHBoxLayout(barsPage);
    for (auto* row : {wheelsRow, barsRow}) {
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
    }
    for (const Wheel w : wheelSet) {
        const QString label = wheelName(w);
        auto* wheel = new ColorWheel(w, wheelsPage);
        m_wheels[size_t(w)] = wheel;
        auto* bars = new PrimaryBars(w, barsPage);
        m_bars[size_t(w)] = bars;
        wheelsRow->addWidget(wheel, 1);
        barsRow->addWidget(bars, 1);
        for (QObject* src : {static_cast<QObject*>(wheel), static_cast<QObject*>(bars)}) {
            auto start = [this, label] {
                if (m_session)
                    m_session->beginGesture(label);
            };
            auto finish = [this] {
                if (m_session)
                    m_session->endGesture();
            };
            if (auto* wh = qobject_cast<ColorWheel*>(src)) {
                connect(wh, &ColorWheel::dragStarted, this, start);
                connect(wh, &ColorWheel::dragFinished, this, finish);
            } else if (auto* b = qobject_cast<PrimaryBars*>(src)) {
                connect(b, &PrimaryBars::dragStarted, this, start);
                connect(b, &PrimaryBars::dragFinished, this, finish);
            }
        }
        connect(wheel, &ColorWheel::balanceChanged, this,
            [this, w, label](const std::array<double, 3>& d) {
                if (!m_session)
                    return;
                const double n = neutralOf(w);
                m_session->setParams(0,
                    {{gk::wheelKey(w, 0), n + d[0]}, {gk::wheelKey(w, 1), n + d[1]},
                        {gk::wheelKey(w, 2), n + d[2]}},
                    label);
            });
        connect(wheel, &ColorWheel::masterChanged, this, [this, w, label](double m) {
            if (m_session)
                m_session->setParam(0, gk::wheelKey(w, 3), neutralOf(w) + m, label);
        });
        connect(wheel, &ColorWheel::resetRequested, this, [this, w] { resetWheel(w); });
        connect(wheel, &ColorWheel::masterResetRequested, this, [this, w, label] {
            if (m_session)
                m_session->setParam(0, gk::wheelKey(w, 3), neutralOf(w), tr("Reset %1").arg(label));
        });
        connect(bars, &PrimaryBars::channelChanged, this, [this, w, label](int c, double d) {
            if (m_session)
                m_session->setParam(0, gk::wheelKey(w, c), neutralOf(w) + d, label);
        });
        connect(bars, &PrimaryBars::channelResetRequested, this, [this, w, label](int c) {
            if (m_session)
                m_session->setParam(0, gk::wheelKey(w, c), neutralOf(w), tr("Reset %1").arg(label));
        });
    }
    m_modes->addWidget(wheelsPage);
    m_modes->addWidget(barsPage);
    layout->addWidget(m_modes, 1);
    connect(modes, &QButtonGroup::idClicked, this, [this](int id) { setBarsMode(id == 1); });

    // Y R G B fields under each wheel (shared by both views).
    auto* fieldsRow = new QHBoxLayout;
    fieldsRow->setSpacing(6);
    for (const Wheel w : wheelSet) {
        auto* grid = new QHBoxLayout;
        grid->setSpacing(1);
        for (const int c : {3, 0, 1, 2}) {
            auto* f = new ValueField(this);
            const QString& key = gk::wheelKey(w, c);
            f->setObjectName(prefix + key);
            const auto [lo, hi] = gk::range(key);
            f->setRange(lo, hi);
            f->setStep(0.001);
            f->setDecimals(2);
            f->setToolTip(gk::label(key));
            QPalette pal = f->palette();
            pal.setColor(QPalette::Link, channelColour(c)); // ValueField draws its value in Link
            f->setPalette(pal);
            f->setMinimumWidth(10);
            grid->addWidget(f, 1);
            bindField(f, key);
        }
        fieldsRow->addLayout(grid, 1);
    }
    layout->addLayout(fieldsRow);

    // ---- Bottom adjustment row ----
    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(0);
    if (!log) {
        makeField({&gk::ColorBoost, -100, 100, 0.5, 2}, tr("Boost"), bottom);
        makeField({&gk::Shadows, -100, 100, 0.5, 2}, tr("Shad"), bottom);
        makeField({&gk::Highlights, -100, 100, 0.5, 2}, tr("Hi"), bottom);
    }
    makeField({&gk::Saturation, 0, 4, 0.005, 2}, tr("Sat"), bottom);
    makeField({&gk::Hue, -180, 180, 0.5, 1}, tr("Hue"), bottom);
    makeField({&gk::LumMix, 0, 1, 0.005, 2}, tr("Lum Mix"), bottom);
    if (log) {
        makeField({&gk::LogLowRange, 0, 1, 0.005, 2}, tr("Low Range"), bottom);
        makeField({&gk::LogHighRange, 0, 1, 0.005, 2}, tr("High Range"), bottom);
    } else {
        makeField({&gk::Mix, 0, 1, 0.005, 2}, tr("Mix"), bottom);
    }
    layout->addLayout(bottom);

    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &PrimariesPanel::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &PrimariesPanel::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &PrimariesPanel::refresh);
    }
    refresh();
}

void PrimariesPanel::bindField(ValueField* f, const QString& key)
{
    connect(f, &ValueField::scrubStarted, this, [this, key] {
        if (m_session)
            m_session->beginGesture(gk::label(key));
    });
    connect(f, &ValueField::valueScrubbed, this, [this, key](double v) {
        if (!m_refreshing && m_session)
            m_session->setParam(0, key, v, gk::label(key));
    });
    connect(f, &ValueField::scrubFinished, this, [this] {
        if (m_session)
            m_session->endGesture();
    });
    connect(f, &ValueField::valueCommitted, this, [this, key](double v) {
        if (!m_refreshing && m_session)
            m_session->setParam(0, key, v, gk::label(key));
    });
    m_fields.append({key, f});
}

void PrimariesPanel::resetWheel(Wheel w)
{
    if (!m_session)
        return;
    const double n = neutralOf(w);
    m_session->setParams(0,
        {{gk::wheelKey(w, 0), n}, {gk::wheelKey(w, 1), n}, {gk::wheelKey(w, 2), n},
            {gk::wheelKey(w, 3), n}},
        tr("Reset %1").arg(wheelName(w)));
}

void PrimariesPanel::resetAll()
{
    if (!m_session || !m_session->currentNode())
        return;
    QVector<std::pair<QString, double>> values;
    for (const auto& [key, f] : std::as_const(m_fields))
        values.append({key, gk::defaultValue(key)});
    m_session->setParams(0, values, tr("Reset Primaries"));
}

ValueField* PrimariesPanel::field(const QString& key) const
{
    for (const auto& [k, f] : m_fields) {
        if (k == key)
            return f;
    }
    return nullptr;
}

void PrimariesPanel::setBarsMode(bool bars)
{
    m_modes->setCurrentIndex(bars ? 1 : 0);
    (bars ? m_barsButton : m_wheelsButton)->setChecked(true);
}

bool PrimariesPanel::barsMode() const
{
    return m_modes->currentIndex() == 1;
}

void PrimariesPanel::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    const bool editable = m_session && m_session->hasGrade() && (!n || !n->isMixer());
    setEnabled(editable);
    const int64_t frame = m_session ? m_session->gradeFrame() : 0;
    auto value
        = [&](const QString& key) { return n ? n->value(key, frame) : gk::defaultValue(key); };
    for (const Wheel w : (m_kind == Kind::Log ? kLogWheels : kWheels)) {
        const double neutral = neutralOf(w);
        const std::array<double, 4> d
            = {value(gk::wheelKey(w, 0)) - neutral, value(gk::wheelKey(w, 1)) - neutral,
                value(gk::wheelKey(w, 2)) - neutral, value(gk::wheelKey(w, 3)) - neutral};
        m_wheels[size_t(w)]->setValues({d[0], d[1], d[2]}, d[3]);
        m_bars[size_t(w)]->setValues(d);
    }
    for (const auto& [key, f] : std::as_const(m_fields)) {
        if (!f->isScrubbing())
            f->setValue(value(key));
    }
    m_refreshing = false;
}

// ---- HdrPanel ----

HdrPanel::HdrPanel(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    markVimForm(this);
    setObjectName(QStringLiteral("HdrPanel"));
    static constexpr Wheel order[] = {Wheel::HdrBlack, Wheel::HdrGlobal, Wheel::HdrShadow,
        Wheel::HdrLight, Wheel::HdrHighlight, Wheel::HdrSpecular};
    struct Extra
    {
        const QString* sat;
        const QString* range;
        const QString* falloff;
    };
    const auto extraOf = [](Wheel w) -> Extra {
        switch (w) {
        case Wheel::HdrGlobal:
            return {&gk::HdrGlobalSat, nullptr, nullptr};
        case Wheel::HdrShadow:
            return {&gk::HdrShadowSat, &gk::HdrShadowRange, &gk::HdrShadowFalloff};
        case Wheel::HdrLight:
            return {&gk::HdrLightSat, &gk::HdrLightRange, &gk::HdrLightFalloff};
        case Wheel::HdrHighlight:
            return {&gk::HdrHighlightSat, &gk::HdrHighlightRange, &gk::HdrHighlightFalloff};
        case Wheel::HdrSpecular:
            return {&gk::HdrSpecularSat, nullptr, nullptr};
        default:
            return {nullptr, nullptr, nullptr};
        }
    };
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(2);
    auto* wheelsRow = new QHBoxLayout;
    wheelsRow->setSpacing(6);
    auto* fieldsRow = new QHBoxLayout;
    fieldsRow->setSpacing(6);
    for (const Wheel w : order) {
        const QString label = wheelName(w);
        auto* wheel = new ColorWheel(w, this);
        m_wheels[size_t(w)] = wheel;
        wheelsRow->addWidget(wheel, 1);
        connect(wheel, &ColorWheel::dragStarted, this, [this, label] {
            if (m_session)
                m_session->beginGesture(label);
        });
        connect(wheel, &ColorWheel::dragFinished, this, [this] {
            if (m_session)
                m_session->endGesture();
        });
        connect(wheel, &ColorWheel::balanceChanged, this,
            [this, w, label](const std::array<double, 3>& d) {
                if (m_session)
                    m_session->setParams(0,
                        {{gk::wheelKey(w, 0), d[0]}, {gk::wheelKey(w, 1), d[1]},
                            {gk::wheelKey(w, 2), d[2]}},
                        label);
            });
        connect(wheel, &ColorWheel::masterChanged, this, [this, w, label](double m) {
            if (m_session)
                m_session->setParam(0, gk::wheelKey(w, 3), m, label);
        });
        connect(wheel, &ColorWheel::resetRequested, this, [this, w] { resetWheel(w); });
        connect(wheel, &ColorWheel::masterResetRequested, this, [this, w, label] {
            if (m_session)
                m_session->setParam(0, gk::wheelKey(w, 3), 0.0, tr("Reset %1").arg(label));
        });

        auto* column = new QVBoxLayout;
        column->setSpacing(1);
        auto* ychan = new QHBoxLayout;
        ychan->setSpacing(1);
        for (const int c : {3, 0, 1, 2}) {
            auto* f = new ValueField(this);
            const QString& key = gk::wheelKey(w, c);
            f->setObjectName(QStringLiteral("HdrPalette_") + key);
            const auto [lo, hi] = gk::range(key);
            f->setRange(lo, hi);
            f->setStep(c == 3 ? 0.01 : 0.001);
            f->setDecimals(c == 3 ? 2 : 3);
            f->setToolTip(c == 3 ? (w == Wheel::HdrBlack ? tr("%1 level").arg(label)
                                                         : tr("%1 exposure (stops)").arg(label))
                                 : gk::label(key));
            QPalette pal = f->palette();
            pal.setColor(QPalette::Link, channelColour(c));
            f->setPalette(pal);
            f->setMinimumWidth(10);
            ychan->addWidget(f, 1);
            bindField(f, key);
        }
        column->addLayout(ychan);
        const Extra extra = extraOf(w);
        const auto addPair = [&](QHBoxLayout* row, const QString* key, const QString& text,
                                 double step, int decimals) {
            auto* name = new QLabel(text, this);
            name->setObjectName(QStringLiteral("PrimariesLabel"));
            auto* f = new ValueField(this);
            f->setObjectName(QStringLiteral("HdrPalette_") + *key);
            const auto [lo, hi] = gk::range(*key);
            f->setRange(lo, hi);
            f->setStep(step);
            f->setDecimals(decimals);
            f->setToolTip(gk::label(*key));
            f->setMinimumWidth(10);
            row->addWidget(name);
            row->addWidget(f, 1);
            bindField(f, *key);
        };
        auto* satRow = new QHBoxLayout;
        satRow->setSpacing(2);
        if (extra.sat)
            addPair(satRow, extra.sat, tr("Sat"), 0.005, 2);
        column->addLayout(satRow);
        auto* rangeRow = new QHBoxLayout;
        rangeRow->setSpacing(2);
        if (extra.range) {
            addPair(rangeRow, extra.range, tr("Range"), 0.05, 2);
            addPair(rangeRow, extra.falloff, tr("Fall"), 0.05, 2);
        }
        column->addLayout(rangeRow);
        fieldsRow->addLayout(column, 1);
    }
    layout->addLayout(wheelsRow, 1);
    layout->addLayout(fieldsRow);
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &HdrPanel::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &HdrPanel::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &HdrPanel::refresh);
    }
    refresh();
}

void HdrPanel::bindField(ValueField* f, const QString& key)
{
    connect(f, &ValueField::scrubStarted, this, [this, key] {
        if (m_session)
            m_session->beginGesture(gk::label(key));
    });
    connect(f, &ValueField::valueScrubbed, this, [this, key](double v) {
        if (!m_refreshing && m_session)
            m_session->setParam(0, key, v, gk::label(key));
    });
    connect(f, &ValueField::scrubFinished, this, [this] {
        if (m_session)
            m_session->endGesture();
    });
    connect(f, &ValueField::valueCommitted, this, [this, key](double v) {
        if (!m_refreshing && m_session)
            m_session->setParam(0, key, v, gk::label(key));
    });
    m_fields.append({key, f});
}

void HdrPanel::resetWheel(Wheel w)
{
    if (!m_session)
        return;
    m_session->setParams(0,
        {{gk::wheelKey(w, 0), 0.0}, {gk::wheelKey(w, 1), 0.0}, {gk::wheelKey(w, 2), 0.0},
            {gk::wheelKey(w, 3), 0.0}},
        tr("Reset %1").arg(wheelName(w)));
}

void HdrPanel::resetAll()
{
    if (!m_session || !m_session->currentNode())
        return;
    QVector<std::pair<QString, double>> values;
    for (const QString& key : gk::hdrKeys())
        values.append({key, gk::defaultValue(key)});
    m_session->setParams(0, values, tr("Reset HDR"));
}

ValueField* HdrPanel::field(const QString& key) const
{
    for (const auto& [k, f] : m_fields) {
        if (k == key)
            return f;
    }
    return nullptr;
}

void HdrPanel::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    setEnabled(m_session && m_session->hasGrade() && (!n || !n->isMixer()));
    const int64_t frame = m_session ? m_session->gradeFrame() : 0;
    const auto value
        = [&](const QString& key) { return n ? n->value(key, frame) : gk::defaultValue(key); };
    for (int i = int(Wheel::HdrGlobal); i < gk::kWheelCount; ++i) {
        const Wheel w = Wheel(i);
        m_wheels[size_t(w)]->setValues({value(gk::wheelKey(w, 0)), value(gk::wheelKey(w, 1)),
                                           value(gk::wheelKey(w, 2))},
            value(gk::wheelKey(w, 3)));
    }
    for (const auto& [key, f] : std::as_const(m_fields)) {
        if (!f->isScrubbing())
            f->setValue(value(key));
    }
    m_refreshing = false;
}

// ---- GradeCurveEditor ----


GradeCurveEditor::GradeCurveEditor(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("GradeCurveEditor"));
    setToolTip(tr("Click to add a point, drag to move it, double-click or right-click removes it"));
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, qOverload<>(&QWidget::update));
        connect(m_session, &ColorSession::selectedNodeChanged, this, qOverload<>(&QWidget::update));
    }
}

void GradeCurveEditor::setCurve(GradeCurve curve)
{
    m_curve = curve;
    update();
}

QRectF GradeCurveEditor::plotRect() const
{
    // Square-ish for the custom curves, wide for the HSL curves (like the palette Resolve
    // users know); the hue band below the HSL plots.
    const double band = isChannelCurve(m_curve) ? 0.0 : 10.0;
    QRectF r(6, 6, std::max(10.0, width() - 12.0), std::max(10.0, height() - 12.0 - band));
    if (isChannelCurve(m_curve) && r.width() > r.height() * 1.25)
        r.setWidth(r.height() * 1.25);
    return r;
}

CurvePoints GradeCurveEditor::points() const
{
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    return n ? n->curves[m_curve] : CurvePoints {};
}

QPointF GradeCurveEditor::toPlot(QPointF v) const
{
    const QRectF r = plotRect();
    return {r.left() + v.x() * r.width(), r.bottom() - v.y() * r.height()};
}

QPointF GradeCurveEditor::fromPlot(QPointF w) const
{
    const QRectF r = plotRect();
    return {std::clamp((w.x() - r.left()) / r.width(), 0.0, 1.0),
        std::clamp((r.bottom() - w.y()) / r.height(), 0.0, 1.0)};
}

int GradeCurveEditor::pointAt(QPointF w) const
{
    const CurvePoints pts = points();
    for (int i = 0; i < pts.size(); ++i) {
        const QPointF at = toPlot(QPointF(pts[i].x, pts[i].y));
        if (std::hypot(at.x() - w.x(), at.y() - w.y()) <= kPointRadius + 1)
            return i;
    }
    return -1;
}

void GradeCurveEditor::store(const CurvePoints& pts)
{
    if (!m_session)
        return;
    const bool rgb
        = m_curve == GradeCurve::Red || m_curve == GradeCurve::Green || m_curve == GradeCurve::Blue;
    if (m_linked && rgb) {
        m_session->setCurves(0,
            {{GradeCurve::Red, pts}, {GradeCurve::Green, pts}, {GradeCurve::Blue, pts}},
            tr("Curves"));
    } else {
        m_session->setCurve(0, m_curve, pts, tr("Curves"));
    }
}

void GradeCurveEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plotRect();
    p.fillRect(r, QColor(26, 26, 30));
    p.setPen(QPen(QColor(255, 255, 255, 28), 1));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()),
            QPointF(r.left() + r.width() * i / 4, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4),
            QPointF(r.right(), r.top() + r.height() * i / 4));
    }
    p.setPen(QPen(QColor(255, 255, 255, 50), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    // The neutral line.
    p.setPen(QPen(QColor(255, 255, 255, 40), 1, Qt::DashLine));
    if (isChannelCurve(m_curve))
        p.drawLine(r.bottomLeft(), r.topRight());
    else
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
    // HSL curves: a band along the bottom showing the x axis (hue, or a grey ramp).
    if (!isChannelCurve(m_curve)) {
        const QRectF band(r.left(), r.bottom() + 3, r.width(), 6);
        for (int i = 0; i < 64; ++i) {
            const double x = i / 64.0;
            QColor c = isHueCurve(m_curve)      ? QColor::fromHsvF(float(x), 0.75f, 0.9f)
                : m_curve == GradeCurve::LumSat ? QColor::fromRgbF(float(x), float(x), float(x))
                                                : QColor::fromHsvF(0.0f, float(x), 0.85f);
            p.fillRect(QRectF(band.left() + band.width() * x, band.top(), band.width() / 64 + 1,
                           band.height()),
                c);
        }
    }
    const GradeNode* node = m_session ? m_session->currentNode() : nullptr;
    auto curvePath = [&](GradeCurve c) {
        const CurvePoints pts = node ? node->curves[c] : CurvePoints {};
        QPainterPath path;
        for (int i = 0; i <= 160; ++i) {
            const double x = i / 160.0;
            const double y = grademath::evaluateCurve(pts, c, x);
            const QPointF at = toPlot(QPointF(x, std::clamp(y, 0.0, 1.0)));
            if (i == 0)
                path.moveTo(at);
            else
                path.lineTo(at);
        }
        return path;
    };
    auto colourOf = [](GradeCurve c) {
        switch (c) {
        case GradeCurve::Luma:
            return channelColour(3);
        case GradeCurve::Red:
            return channelColour(0);
        case GradeCurve::Green:
            return channelColour(1);
        case GradeCurve::Blue:
            return channelColour(2);
        default:
            return QColor(230, 200, 120);
        }
    };
    // Custom curves: the other channels faintly behind.
    if (isChannelCurve(m_curve)) {
        for (const GradeCurve c :
            {GradeCurve::Luma, GradeCurve::Red, GradeCurve::Green, GradeCurve::Blue}) {
            if (c == m_curve || !node || node->curves.isIdentity(c))
                continue;
            QColor faint = colourOf(c);
            faint.setAlpha(90);
            p.setPen(QPen(faint, 1));
            p.drawPath(curvePath(c));
        }
    }
    p.setPen(QPen(colourOf(m_curve), 1.6));
    p.drawPath(curvePath(m_curve));
    p.setBrush(QColor(30, 30, 34));
    const CurvePoints pts = points();
    for (const CurvePoint& pt : pts) {
        const QPointF at = toPlot(QPointF(pt.x, pt.y));
        p.drawEllipse(at, kPointRadius * 0.7, kPointRadius * 0.7);
        if (isHueCurve(m_curve)) {
            // Hue curves wrap: show the point at the other edge too.
            const QPointF wrap = toPlot(QPointF(pt.x < 0.5 ? pt.x + 1.0 : pt.x - 1.0, pt.y));
            if (r.adjusted(-kPointRadius, 0, kPointRadius, 0).contains(wrap))
                p.drawEllipse(wrap, kPointRadius * 0.5, kPointRadius * 0.5);
        }
    }
}

void GradeCurveEditor::mousePressEvent(QMouseEvent* event)
{
    if (!m_session || !m_session->hasGrade()
        || !plotRect()
                .adjusted(-kPointRadius, -kPointRadius, kPointRadius, kPointRadius)
                .contains(event->position()))
        return;
    const int hit = pointAt(event->position());
    if (event->button() == Qt::RightButton) {
        if (hit >= 0) {
            CurvePoints pts = points();
            pts.remove(hit);
            store(pts);
        }
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    m_session->beginGesture(tr("Curves"));
    CurvePoints pts = points();
    if (hit >= 0) {
        m_dragIndex = hit;
    } else {
        const QPointF v = fromPlot(event->position());
        m_dragIndex = grademath::addCurvePoint(pts, m_curve, v.x(), v.y());
        store(pts);
    }
}

void GradeCurveEditor::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragIndex < 0 || !m_session)
        return;
    CurvePoints pts = points();
    if (m_dragIndex >= pts.size())
        return;
    const QPointF v = fromPlot(event->position());
    // Channel and lum / sat curves keep their points in x order between the neighbours.
    double x = v.x();
    if (!isHueCurve(m_curve)) {
        const double lo = m_dragIndex > 0 ? pts[m_dragIndex - 1].x + 1e-3 : 0.0;
        const double hi = m_dragIndex + 1 < pts.size() ? pts[m_dragIndex + 1].x - 1e-3 : 1.0;
        x = std::clamp(x, lo, std::max(lo, hi));
    }
    const CurvePoint moved {x, v.y()};
    pts[m_dragIndex] = moved;
    std::sort(
        pts.begin(), pts.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    m_dragIndex = int(pts.indexOf(moved));
    store(pts);
}

void GradeCurveEditor::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_dragIndex < 0)
        return;
    m_dragIndex = -1;
    if (m_session)
        m_session->endGesture();
}

void GradeCurveEditor::mouseDoubleClickEvent(QMouseEvent* event)
{
    const int hit = pointAt(event->position());
    if (hit < 0)
        return;
    CurvePoints pts = points();
    pts.remove(hit);
    store(pts);
}

// ---- ColorCurvesPanel ----

ColorCurvesPanel::ColorCurvesPanel(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    markVimForm(this);
    setObjectName(QStringLiteral("ColorCurvesPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(2);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(1);
    m_modes = new QButtonGroup(this);
    const std::pair<GradeCurve, QString> modes[]
        = {{GradeCurve::Luma, tr("Custom")}, {GradeCurve::HueHue, tr("Hue vs Hue")},
            {GradeCurve::HueSat, tr("Hue vs Sat")}, {GradeCurve::HueLum, tr("Hue vs Lum")},
            {GradeCurve::LumSat, tr("Lum vs Sat")}, {GradeCurve::SatSat, tr("Sat vs Sat")},
            {GradeCurve::SatLum, tr("Sat vs Lum")}};
    for (const auto& [curve, text] : modes) {
        QToolButton* b = modeButton(text, this);
        b->setObjectName(QStringLiteral("CurveMode_") + QString::number(int(curve)));
        m_modes->addButton(b, int(curve));
        bar->addWidget(b);
    }
    m_modes->button(int(GradeCurve::Luma))->setChecked(true);
    bar->addStretch(1);
    m_pick = modeButton(tr("Pick"), this);
    m_pick->setObjectName(QStringLiteral("CurvePick"));
    m_pick->setToolTip(tr("Click the viewer to add the picked colour's (or luma's) points to the "
                          "shown curve"));
    connect(m_pick, &QToolButton::toggled, this, &ColorCurvesPanel::pickModeChanged);
    bar->addWidget(m_pick);
    layout->addLayout(bar);

    auto* body = new QHBoxLayout;
    body->setSpacing(4);
    m_editor = new GradeCurveEditor(session, this);
    m_channelRow = new QWidget(this);
    auto* channels = new QVBoxLayout(m_channelRow);
    channels->setContentsMargins(0, 0, 0, 0);
    channels->setSpacing(1);
    m_channels = new QButtonGroup(this);
    const std::pair<GradeCurve, QString> chans[] = {{GradeCurve::Luma, tr("Y")},
        {GradeCurve::Red, tr("R")}, {GradeCurve::Green, tr("G")}, {GradeCurve::Blue, tr("B")}};
    int c = 3;
    for (const auto& [curve, text] : chans) {
        QToolButton* b = modeButton(text, m_channelRow);
        b->setObjectName(QStringLiteral("CurveChannel_") + text);
        QPalette pal = b->palette();
        pal.setColor(QPalette::ButtonText, channelColour(c));
        b->setPalette(pal);
        c = (c + 1) % 4;
        m_channels->addButton(b, int(curve));
        channels->addWidget(b);
    }
    m_channels->button(int(GradeCurve::Luma))->setChecked(true);
    m_link = modeButton(tr("Link"), m_channelRow);
    m_link->setObjectName(QStringLiteral("CurveLink"));
    m_link->setToolTip(tr("Linked: editing R, G or B edits all three"));
    m_link->setChecked(true);
    channels->addSpacing(6);
    channels->addWidget(m_link);
    channels->addStretch(1);
    body->addWidget(m_channelRow);
    body->addWidget(m_editor, 1);
    layout->addLayout(body, 1);

    // Soft Clip (Custom curves): Low / Low Soft / High / High Soft.
    m_softRow = new QWidget(this);
    auto* soft = new QHBoxLayout(m_softRow);
    soft->setContentsMargins(0, 0, 0, 0);
    soft->setSpacing(4);
    auto* softTitle = new QLabel(tr("Soft Clip"), m_softRow);
    soft->addWidget(softTitle);
    const std::pair<const QString*, QString> softKeys[]
        = {{&gk::SoftClipLow, tr("Low")}, {&gk::SoftClipLowSoft, tr("L.Soft")},
            {&gk::SoftClipHigh, tr("High")}, {&gk::SoftClipHighSoft, tr("H.Soft")}};
    for (const auto& [keyPtr, text] : softKeys) {
        const QString key = *keyPtr;
        soft->addStretch(1);
        soft->addWidget(new QLabel(text, m_softRow));
        auto* f = new ValueField(m_softRow);
        f->setObjectName(QStringLiteral("SoftClip_") + key);
        f->setRange(0.0, 1.0);
        f->setStep(0.005);
        f->setDecimals(3);
        f->setToolTip(gk::label(key));
        f->setFixedWidth(std::max(52, f->sizeHint().width()));
        soft->addWidget(f);
        connect(f, &ValueField::scrubStarted, this, [this, key] {
            if (m_session)
                m_session->beginGesture(gk::label(key));
        });
        connect(f, &ValueField::valueScrubbed, this, [this, key](double v) {
            if (!m_refreshing && m_session)
                m_session->setParam(0, key, v, gk::label(key));
        });
        connect(f, &ValueField::scrubFinished, this, [this] {
            if (m_session)
                m_session->endGesture();
        });
        connect(f, &ValueField::valueCommitted, this, [this, key](double v) {
            if (!m_refreshing && m_session)
                m_session->setParam(0, key, v, gk::label(key));
        });
        m_softFields.append({key, f});
    }
    layout->addWidget(m_softRow);

    connect(m_modes, &QButtonGroup::idClicked, this, [this](int id) {
        GradeCurve c = GradeCurve(id);
        if (c == GradeCurve::Luma)
            c = GradeCurve(m_channels->checkedId());
        m_editor->setCurve(c);
        updateButtons();
    });
    connect(m_channels, &QButtonGroup::idClicked, this, [this](int id) {
        m_editor->setCurve(GradeCurve(id));
        updateButtons();
    });
    connect(m_link, &QToolButton::toggled, this, [this](bool on) { m_editor->setLinked(on); });
    if (m_session) {
        auto enable = [this] { setEnabled(m_session->hasGrade()); };
        connect(m_session, &ColorSession::clipChanged, this, enable);
        enable();
        connect(m_session, &ColorSession::gradeChanged, this, &ColorCurvesPanel::refreshSoftClip);
        connect(m_session, &ColorSession::selectedNodeChanged, this,
            &ColorCurvesPanel::refreshSoftClip);
        connect(m_session, &ColorSession::clipChanged, this, &ColorCurvesPanel::refreshSoftClip);
    }
    updateButtons();
    refreshSoftClip();
}

void ColorCurvesPanel::refreshSoftClip()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    const int64_t frame = m_session ? m_session->gradeFrame() : 0;
    for (const auto& [key, f] : std::as_const(m_softFields)) {
        if (!f->isScrubbing())
            f->setValue(n ? n->value(key, frame) : gk::defaultValue(key));
    }
    m_refreshing = false;
}

ValueField* ColorCurvesPanel::softClipField(const QString& key) const
{
    for (const auto& [k, f] : m_softFields) {
        if (k == key)
            return f;
    }
    return nullptr;
}

bool ColorCurvesPanel::pickActive() const
{
    return m_pick->isChecked();
}

void ColorCurvesPanel::pickColor(const float rgb[3])
{
    if (!m_session || !m_session->hasGrade())
        return;
    const GradeNode* n = m_session->currentNode();
    CurvePoints pts = n ? n->curves[m_editor->curve()] : CurvePoints {};
    grademath::addCurvePickPoints(pts, m_editor->curve(), rgb);
    m_session->setCurve(0, m_editor->curve(), pts, tr("Curve Pick"));
}

void ColorCurvesPanel::showCurve(GradeCurve curve)
{
    m_editor->setCurve(curve);
    if (isChannelCurve(curve)) {
        m_modes->button(int(GradeCurve::Luma))->setChecked(true);
        m_channels->button(int(curve))->setChecked(true);
    } else {
        m_modes->button(int(curve))->setChecked(true);
    }
    updateButtons();
}

void ColorCurvesPanel::resetCurve()
{
    if (!m_session || !m_session->currentNode())
        return;
    const GradeCurve c = m_editor->curve();
    const bool rgb = c == GradeCurve::Red || c == GradeCurve::Green || c == GradeCurve::Blue;
    if (m_editor->isLinked() && rgb) {
        m_session->setCurves(0,
            {{GradeCurve::Red, {}}, {GradeCurve::Green, {}}, {GradeCurve::Blue, {}}},
            tr("Reset Curve"));
    } else {
        m_session->setCurve(0, c, {}, tr("Reset Curve"));
    }
    if (isChannelCurve(c)) {
        m_session->setParams(0,
            {{gk::SoftClipLow, gk::defaultValue(gk::SoftClipLow)},
                {gk::SoftClipLowSoft, gk::defaultValue(gk::SoftClipLowSoft)},
                {gk::SoftClipHigh, gk::defaultValue(gk::SoftClipHigh)},
                {gk::SoftClipHighSoft, gk::defaultValue(gk::SoftClipHighSoft)}},
            tr("Reset Soft Clip"));
    }
}

void ColorCurvesPanel::updateButtons()
{
    m_channelRow->setVisible(isChannelCurve(m_editor->curve()));
    m_softRow->setVisible(isChannelCurve(m_editor->curve()));
}

} // namespace kk
