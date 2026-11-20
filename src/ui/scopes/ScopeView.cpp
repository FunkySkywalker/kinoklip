// SPDX-License-Identifier: GPL-3.0-or-later
// ScopeView / ScopesPanel — drawing the scope counts.
#include "ui/scopes/ScopeView.h"
#include "ui/common/UiFonts.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QSlider>
#include <QToolButton>
#include <QWidgetAction>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace kk {

namespace {

using namespace scopes;

/// Trace brightness of @p count pixels at one spot when @p typical is a "full" spot.
float glow(uint32_t count, double typical, double gain)
{
    if (count == 0)
        return 0.0f;
    return float(
        std::clamp(gain * (0.15 + 0.85 * (1.0 - std::exp(-3.0 * count / typical))), 0.0, 1.0));
}

/// Adds @p rgb x @p v to pixel (@p x, @p y) of @p img (additive mixing of the trace colours).
void addPixel(QImage& img, int x, int y, const float rgb[3], float v)
{
    QRgb* p = reinterpret_cast<QRgb*>(img.scanLine(y)) + x;
    const int r = std::min(255, qRed(*p) + int(rgb[0] * v * 255));
    const int g = std::min(255, qGreen(*p) + int(rgb[1] * v * 255));
    const int b = std::min(255, qBlue(*p) + int(rgb[2] * v * 255));
    *p = qRgb(r, g, b);
}

const float kLumaTrace[3] = {0.75f, 0.95f, 0.8f};
const float kChannel[3][3] = {{1.0f, 0.25f, 0.25f}, {0.3f, 1.0f, 0.3f}, {0.35f, 0.5f, 1.0f}};

/// Waveform planes into @p img starting at column @p x0 (one plane per colour, or all planes
/// overlaid when @p plane < 0).
void drawWaveform(QImage& img, const WaveformData& w, int x0, int plane, const float* colour,
    double rowsPerColumn, double gain)
{
    const double typical = std::max(1.0, rowsPerColumn / 24.0);
    const int first = plane < 0 ? 0 : plane;
    const int last = plane < 0 ? int(w.planes.size()) - 1 : plane;
    for (int p = first; p <= last; ++p) {
        const float* c = colour ? colour : (w.planes.size() == 1 ? kLumaTrace : kChannel[p]);
        for (int level = 0; level < w.levels; ++level) {
            const int y = w.levels - 1 - level;
            for (int x = 0; x < w.columns; ++x) {
                const float v = glow(w.at(p, x, level), typical, gain);
                if (v > 0)
                    addPixel(img, x0 + x, y, c, v);
            }
        }
    }
}

} // namespace

ScopeView::ScopeView(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 110);
}

void ScopeView::setType(ScopeType type)
{
    if (type == m_type)
        return;
    m_type = type;
    rebuild();
    update();
}

void ScopeView::setScale(ScopeScale scale)
{
    if (scale == m_scale)
        return;
    m_scale = scale;
    update();
}

void ScopeView::setGain(double gain)
{
    gain = std::clamp(gain, kMinScopeGain, kMaxScopeGain);
    if (gain == m_gain)
        return;
    m_gain = gain;
    rebuild();
    update();
}

void ScopeView::setImage(const QImage& image)
{
    m_input = scopeInput(image);
    rebuild();
    update();
}

void ScopeView::rebuild()
{
    m_trace = QImage();
    if (m_input.isNull())
        return;
    const double rows = m_input.height();
    constexpr int kColumns = 256;
    switch (m_type) {
    case ScopeType::WaveformLuma:
    case ScopeType::WaveformRgb: {
        const bool rgb = m_type == ScopeType::WaveformRgb;
        const WaveformData w = waveform(m_input, rgb, kColumns, 256);
        m_trace = QImage(w.columns, w.levels, QImage::Format_RGB32);
        m_trace.fill(Qt::black);
        drawWaveform(m_trace, w, 0, -1, nullptr, rows * m_input.width() / kColumns, m_gain);
        break;
    }
    case ScopeType::Parade: {
        const WaveformData w = waveform(m_input, true, kColumns / 2, 256);
        m_trace = QImage(w.columns * 3, w.levels, QImage::Format_RGB32);
        m_trace.fill(Qt::black);
        for (int p = 0; p < 3; ++p)
            drawWaveform(
                m_trace, w, p * w.columns, p, kChannel[p], rows * m_input.width() / w.columns, m_gain);
        break;
    }
    case ScopeType::Vectorscope: {
        const VectorscopeData v = vectorscope(m_input, 256);
        m_trace = QImage(v.size, v.size, QImage::Format_RGB32);
        m_trace.fill(Qt::black);
        const double typical = std::max(1.0, double(m_input.width()) * m_input.height() / 2000.0);
        for (int y = 0; y < v.size; ++y) {
            for (int x = 0; x < v.size; ++x) {
                const float g = glow(v.at(x, y), typical, m_gain);
                if (g > 0)
                    addPixel(m_trace, x, y, kLumaTrace, g);
            }
        }
        break;
    }
    case ScopeType::CieChromaticity: {
        const CieData c = cieChromaticity(m_input, 256);
        m_trace = QImage(c.size, c.size, QImage::Format_RGB32);
        m_trace.fill(Qt::black);
        const double typical = std::max(1.0, double(m_input.width()) * m_input.height() / 2000.0);
        for (int y = 0; y < c.size; ++y) {
            for (int x = 0; x < c.size; ++x) {
                const float g = glow(c.at(x, y), typical, m_gain);
                if (g > 0)
                    addPixel(m_trace, x, y, kLumaTrace, g);
            }
        }
        break;
    }
    case ScopeType::Histogram: {
        const HistogramData h = histogram(m_input, 256);
        constexpr int kHeight = 128;
        m_trace = QImage(h.bins, kHeight, QImage::Format_RGB32);
        m_trace.fill(Qt::black);
        uint32_t peak = 1;
        for (int c = 0; c < 3; ++c)
            for (const uint32_t n : h.channels[size_t(c)])
                peak = std::max(peak, n);
        for (int c = 0; c < 3; ++c) {
            for (int bin = 0; bin < h.bins; ++bin) {
                // Square-root scale: small counts stay visible next to a spike.
                const double f = std::sqrt(double(h.channels[size_t(c)][size_t(bin)]) / peak);
                const int top = kHeight - int(std::lround(f * kHeight));
                for (int y = std::max(0, top); y < kHeight; ++y)
                    addPixel(m_trace, bin, y, kChannel[c], float(std::min(1.0, 0.55 * m_gain)));
            }
        }
        break;
    }
    }
}

void ScopeView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(14, 14, 16));
    const QRectF area = QRectF(rect()).adjusted(28, 8, -8, -8);
    if (area.width() < 10 || area.height() < 10)
        return;
    QRectF traceRect = area;
    if (m_type == ScopeType::Vectorscope || m_type == ScopeType::CieChromaticity) {
        const double side = std::min(area.width(), area.height());
        traceRect = QRectF(area.center().x() - side / 2, area.center().y() - side / 2, side, side);
    }
    if (!m_trace.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        p.drawImage(traceRect, m_trace);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    // Graticule.
    p.setRenderHint(QPainter::Antialiasing);
    const QColor line(255, 255, 255, 40);
    const QColor text(200, 200, 200, 150);
    const QFont f = uifonts::font(uifonts::Role::Tiny, font());
    p.setFont(f);
    if (m_type == ScopeType::CieChromaticity) {
        // Axes every 0.1, the spectral locus with its purple line, the Rec. 709, DCI-P3 and
        // Rec. 2020 triangles and the D65 white point.
        auto toWidget = [&](QPointF xy) {
            return QPointF(traceRect.left() + xy.x() / kCieMax * traceRect.width(),
                traceRect.bottom() - xy.y() / kCieMax * traceRect.height());
        };
        p.setPen(QPen(line, 1));
        p.drawRect(traceRect);
        for (int i = 1; i * 0.1 < kCieMax; ++i) {
            const double v = i * 0.1;
            p.drawLine(toWidget({v, 0}), toWidget({v, kCieMax}));
            p.drawLine(toWidget({0, v}), toWidget({kCieMax, v}));
            p.setPen(text);
            p.drawText(toWidget({v, 0}) + QPointF(-8, 12), QString::number(v, 'f', 1));
            p.drawText(toWidget({0, v}) + QPointF(-24, 4), QString::number(v, 'f', 1));
            p.setPen(QPen(line, 1));
        }
        QPainterPath locus;
        const std::vector<QPointF>& points = cieSpectralLocus();
        for (size_t i = 0; i < points.size(); ++i) {
            if (i == 0)
                locus.moveTo(toWidget(points[i]));
            else
                locus.lineTo(toWidget(points[i]));
        }
        locus.closeSubpath();
        p.setPen(QPen(QColor(230, 230, 230, 150), 1.2));
        p.drawPath(locus);
        const QColor gamutColours[kGamutCount] = {
            QColor(255, 255, 255, 130), QColor(230, 180, 120, 110), QColor(120, 190, 230, 110)};
        for (int g = 0; g < kGamutCount; ++g) {
            const std::array<QPointF, 3> pr = gamutPrimaries(g);
            QPainterPath tri;
            tri.moveTo(toWidget(pr[0]));
            tri.lineTo(toWidget(pr[1]));
            tri.lineTo(toWidget(pr[2]));
            tri.closeSubpath();
            p.setPen(QPen(gamutColours[g], 1, g == 0 ? Qt::SolidLine : Qt::DashLine));
            p.drawPath(tri);
        }
        const QPointF white = toWidget({kD65x, kD65y});
        p.setPen(QPen(QColor(255, 255, 255, 200), 1));
        p.drawEllipse(white, 3.0, 3.0);
        p.setPen(text);
        p.drawText(white + QPointF(6, -4), QStringLiteral("D65"));
        return;
    }
    if (m_type == ScopeType::Vectorscope) {
        p.setPen(QPen(line, 1));
        const QPointF c = traceRect.center();
        const double r = traceRect.width() / 2;
        p.drawEllipse(c, r, r);
        p.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
        p.drawLine(QPointF(c.x(), c.y() - r), QPointF(c.x(), c.y() + r));
        // 75 % colour bar targets.
        const std::pair<const char*, std::array<double, 3>> targets[]
            = {{"R", {0.75, 0, 0}}, {"Mg", {0.75, 0, 0.75}}, {"B", {0, 0, 0.75}},
                {"Cy", {0, 0.75, 0.75}}, {"G", {0, 0.75, 0}}, {"Yl", {0.75, 0.75, 0}}};
        for (const auto& [name, rgb] : targets) {
            const QPointF cell = vectorscopeCell(chroma(rgb[0], rgb[1], rgb[2]), 256);
            const QPointF at(traceRect.left() + cell.x() / 255.0 * traceRect.width(),
                traceRect.top() + cell.y() / 255.0 * traceRect.height());
            p.setPen(QPen(QColor(255, 255, 255, 90), 1));
            p.drawRect(QRectF(at - QPointF(5, 5), QSizeF(10, 10)));
            p.setPen(text);
            p.drawText(at + QPointF(7, -6), QLatin1String(name));
        }
        // Skin tone line (about 123 degrees counter-clockwise from +Cb).
        const double a = 123.0 * std::numbers::pi / 180.0;
        p.setPen(QPen(QColor(230, 180, 120, 70), 1));
        p.drawLine(c, c + QPointF(std::cos(a) * r, -std::sin(a) * r));
        return;
    }
    // Levels: 0, 25, 50, 75 and 100 % (the waveforms' and the histogram's x).
    p.setPen(QPen(line, 1));
    const bool histogram = m_type == ScopeType::Histogram;
    for (int i = 0; i <= 4; ++i) {
        const double t = i / 4.0;
        if (histogram) {
            const double x = traceRect.left() + t * traceRect.width();
            p.drawLine(QPointF(x, traceRect.top()), QPointF(x, traceRect.bottom()));
        } else {
            const double y = traceRect.bottom() - t * traceRect.height();
            p.drawLine(QPointF(traceRect.left(), y), QPointF(traceRect.right(), y));
            p.setPen(text);
            p.drawText(QRectF(0, y - 8, 24, 16), Qt::AlignRight | Qt::AlignVCenter,
                scaleLabel(m_scale, i));
            p.setPen(QPen(line, 1));
        }
    }
    if (m_type == ScopeType::Parade) {
        for (int i = 1; i < 3; ++i) {
            const double x = traceRect.left() + traceRect.width() * i / 3.0;
            p.drawLine(QPointF(x, traceRect.top()), QPointF(x, traceRect.bottom()));
        }
    }
}

ScopesPanel::ScopesPanel(const QString& settingsKey, QWidget* parent)
    : QWidget(parent)
    , m_settingsKey(settingsKey)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    m_types = new QComboBox(this);
    m_types->setObjectName(QStringLiteral("ScopeType"));
    const std::pair<ScopeType, const char*> names[] = {
        {ScopeType::WaveformLuma, QT_TR_NOOP("Waveform")},
        {ScopeType::WaveformRgb, QT_TR_NOOP("Waveform RGB")},
        {ScopeType::Parade, QT_TR_NOOP("Parade")},
        {ScopeType::Vectorscope, QT_TR_NOOP("Vectorscope")},
        {ScopeType::Histogram, QT_TR_NOOP("Histogram")},
        {ScopeType::CieChromaticity, QT_TR_NOOP("CIE Chromaticity")},
    };
    for (const auto& [type, name] : names)
        m_types->addItem(tr(name), QString::fromLatin1(scopeTypeId(type)));
    m_view = new ScopeView(this);
    // Scope settings (Scale and Gain) in a popup behind a button beside the type menu.
    m_settings = new QToolButton(this);
    m_settings->setObjectName(QStringLiteral("ScopeSettingsButton"));
    m_settings->setText(tr("Settings"));
    m_settings->setAutoRaise(true);
    m_settings->setPopupMode(QToolButton::InstantPopup);
    m_settings->setToolTip(tr("Scope settings: scale and gain"));
    auto* menu = new QMenu(m_settings);
    auto* box = new QWidget(menu);
    auto* form = new QFormLayout(box);
    form->setContentsMargins(8, 6, 8, 6);
    m_scaleBox = new QComboBox(box);
    m_scaleBox->setObjectName(QStringLiteral("ScopeScaleBox"));
    const std::pair<ScopeScale, const char*> scales[] = {{ScopeScale::TenBit, QT_TR_NOOP("0 - 1023")},
        {ScopeScale::EightBit, QT_TR_NOOP("0 - 255")}, {ScopeScale::Percent, QT_TR_NOOP("Percent")},
        {ScopeScale::Float, QT_TR_NOOP("0.0 - 1.0")}};
    for (const auto& [scale, name] : scales)
        m_scaleBox->addItem(tr(name), QString::fromLatin1(scopeScaleId(scale)));
    m_gainSlider = new QSlider(Qt::Horizontal, box);
    m_gainSlider->setObjectName(QStringLiteral("ScopeGainSlider"));
    m_gainSlider->setRange(-100, 100); // gain = 4 ^ (value / 100): 0.25 .. 4, 1 at the middle
    m_gainSlider->setValue(0);
    m_gainSlider->setToolTip(tr("Gain: trace brightness"));
    form->addRow(tr("Scale"), m_scaleBox);
    form->addRow(tr("Gain"), m_gainSlider);
    auto* boxAction = new QWidgetAction(menu);
    boxAction->setDefaultWidget(box);
    menu->addAction(boxAction);
    m_settings->setMenu(menu);
    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->addWidget(m_types);
    top->addWidget(m_settings);
    top->addStretch(1);
    layout->addLayout(top);
    layout->addWidget(m_view, 1);
    connect(m_scaleBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString id = m_scaleBox->itemData(index).toString();
        m_view->setScale(scopeScaleFromId(id));
        if (!m_settingsKey.isEmpty())
            QSettings().setValue(m_settingsKey + QStringLiteral("/scale"), id);
    });
    connect(m_gainSlider, &QSlider::valueChanged, this, [this](int value) {
        m_view->setGain(std::pow(4.0, value / 100.0));
        if (!m_settingsKey.isEmpty())
            QSettings().setValue(m_settingsKey + QStringLiteral("/gain"), value);
    });
    if (!m_settingsKey.isEmpty()) {
        const QString id = QSettings().value(m_settingsKey).toString();
        setType(scopeTypeFromId(id));
        const int scaleIndex = m_scaleBox->findData(
            QSettings().value(m_settingsKey + QStringLiteral("/scale")).toString());
        if (scaleIndex >= 0)
            m_scaleBox->setCurrentIndex(scaleIndex);
        m_gainSlider->setValue(std::clamp(
            QSettings().value(m_settingsKey + QStringLiteral("/gain"), 0).toInt(), -100, 100));
    }
    connect(m_types, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString id = m_types->itemData(index).toString();
        m_view->setType(scopeTypeFromId(id));
        if (!m_settingsKey.isEmpty())
            QSettings().setValue(m_settingsKey, id);
    });
}

void ScopesPanel::setType(ScopeType type)
{
    const int index = m_types->findData(QString::fromLatin1(scopeTypeId(type)));
    if (index >= 0)
        m_types->setCurrentIndex(index);
    m_view->setType(type);
}

} // namespace kk
