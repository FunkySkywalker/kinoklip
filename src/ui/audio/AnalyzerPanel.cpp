// SPDX-License-Identifier: GPL-3.0-or-later
// AnalyzerPanel — spectrum, correlation meter, goniometer and monitor controls.
// Reference: x42 meters (GPL-2.0-or-later) for the log-frequency spectrum, the correlation bar
// and the rotated goniometer; the code is our own.
#include "ui/audio/AnalyzerPanel.h"

#include "core/MixerOps.h"
#include "playback/MonitorControls.h"
#include "playback/PlaybackEngine.h"
#include "ui/audio/MeterModel.h"
#include "ui/audio/MixerController.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelInfo.h"
#include "ui/common/SchemeColors.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr const char* kHoldKey = "audio/spectrum/peakHold";
constexpr const char* kSlopeKey = "audio/spectrum/slope";
constexpr const char* kSmoothingKey = "audio/spectrum/smoothing";
} // namespace

// ---- SpectrumView ----

SpectrumView::SpectrumView(const SpectrumDisplay* display, QWidget* parent)
    : QWidget(parent)
    , m_display(display)
{
    setObjectName(QStringLiteral("SpectrumView"));
    setToolTip(tr("Spectrum: low to high frequency from left to right (20 Hz to 20 kHz), level "
                  "from -90 to 0 dB. The thin line is the peak hold."));
}

void SpectrumView::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), mixColors(sc.view, sc.shadow, 0.35));
    const QRectF area = QRectF(rect()).adjusted(26, 4, -4, -14);
    auto xOf = [&](double hz) {
        return area.left() + area.width() * std::log(hz / dsp::kSpectrumMinHz)
            / std::log(dsp::kSpectrumMaxHz / dsp::kSpectrumMinHz);
    };
    auto yOf = [&](double db) {
        const double t = std::clamp((db - kFloorDb) / -kFloorDb, 0.0, 1.0);
        return area.bottom() - t * area.height();
    };
    p.setPen(QPen(sc.textDim, 1, Qt::DotLine));
    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() - 2.0));
    p.setFont(small);
    for (const double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        p.drawLine(QPointF(xOf(hz), area.top()), QPointF(xOf(hz), area.bottom()));
    }
    for (double db = -80.0; db < 0.0; db += 20.0) {
        p.drawLine(QPointF(area.left(), yOf(db)), QPointF(area.right(), yOf(db)));
        p.drawText(QRectF(0, yOf(db) - 6, 24, 12), Qt::AlignRight | Qt::AlignVCenter,
            QString::number(int(db)));
    }
    p.setPen(sc.textDim);
    for (const auto& [hz, text] : {std::pair {100.0, "100"}, {1000.0, "1k"}, {10000.0, "10k"}})
        p.drawText(QRectF(xOf(hz) - 16, area.bottom() + 1, 32, 12), Qt::AlignCenter, QLatin1String(text));
    // The bars: a filled curve, with the peak hold as a line over it.
    QPolygonF fill;
    fill << QPointF(area.left(), area.bottom());
    for (int b = 0; b < dsp::kSpectrumBands; ++b)
        fill << QPointF(xOf(dsp::spectrumBandHz(b)), yOf(double(m_display->barDb(b))));
    fill << QPointF(area.right(), area.bottom());
    QColor body = sc.positive;
    body.setAlpha(150);
    p.setPen(Qt::NoPen);
    p.setBrush(body);
    p.drawPolygon(fill);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(sc.positive, 1.2));
    p.drawPolyline(fill.mid(1, dsp::kSpectrumBands));
    if (m_display->peakHold()) {
        QPolygonF peak;
        for (int b = 0; b < dsp::kSpectrumBands; ++b)
            peak << QPointF(xOf(dsp::spectrumBandHz(b)), yOf(double(m_display->peakDb(b))));
        p.setPen(QPen(sc.neutral, 1.0));
        p.drawPolyline(peak);
    }
}

// ---- CorrelationBar ----

CorrelationBar::CorrelationBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("CorrelationBar"));
    setToolTip(tr("Phase correlation of left and right: +1 is the same sound on both sides "
                  "(mono), 0 is wide stereo, -1 is out of phase (it disappears in mono)."));
}

void CorrelationBar::setValue(double value)
{
    m_value = std::clamp(value, -1.0, 1.0);
    update();
}

void CorrelationBar::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    const QRectF bar = QRectF(rect()).adjusted(4, 4, -4, -12);
    p.fillRect(bar, mixColors(sc.view, sc.shadow, 0.35));
    const double mid = bar.center().x();
    const double x = mid + m_value * bar.width() / 2.0;
    const QColor c = m_value < -0.2 ? sc.negative : m_value < 0.0 ? sc.neutral : sc.positive;
    p.fillRect(QRectF(std::min(mid, x), bar.top(), std::abs(x - mid), bar.height()), c);
    p.setPen(QPen(sc.text, 1));
    p.drawLine(QPointF(mid, bar.top()), QPointF(mid, bar.bottom()));
    p.setPen(sc.textDim);
    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() - 2.0));
    p.setFont(small);
    p.drawText(QRectF(bar.left(), bar.bottom(), 24, 12), Qt::AlignLeft, QStringLiteral("-1"));
    p.drawText(QRectF(mid - 12, bar.bottom(), 24, 12), Qt::AlignCenter, QStringLiteral("0"));
    p.drawText(QRectF(bar.right() - 24, bar.bottom(), 24, 12), Qt::AlignRight, QStringLiteral("+1"));
}

// ---- GoniometerView ----

GoniometerView::GoniometerView(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("GoniometerView"));
    setToolTip(tr("Stereo picture: mono sound is a vertical line, left only or right only a "
                  "diagonal, out of phase a horizontal line."));
}

void GoniometerView::setPoints(const float* xy, int count)
{
    m_points.resize(count);
    for (int i = 0; i < count; ++i)
        m_points[i] = QPointF(std::clamp(double(xy[2 * i]), -1.0, 1.0),
            std::clamp(double(xy[2 * i + 1]), -1.0, 1.0));
    update();
}

void GoniometerView::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), mixColors(sc.view, sc.shadow, 0.35));
    const double side = std::min(width(), height()) - 8.0;
    const QPointF c = rect().center();
    p.setPen(QPen(sc.textDim, 1, Qt::DotLine));
    p.drawLine(QPointF(c.x(), c.y() - side / 2), QPointF(c.x(), c.y() + side / 2));
    p.drawLine(QPointF(c.x() - side / 2, c.y()), QPointF(c.x() + side / 2, c.y()));
    p.drawEllipse(c, side / 2, side / 2);
    p.setPen(QPen(sc.positive, 2.0, Qt::SolidLine, Qt::RoundCap));
    for (const QPointF& pt : std::as_const(m_points))
        p.drawPoint(QPointF(c.x() + pt.x() * side / 2, c.y() - pt.y() * side / 2));
}

// ---- AnalyzerPanel ----

AnalyzerPanel::AnalyzerPanel(
    AppContext& ctx, MixerController* controller, MeterModel* meters, QWidget* parent)
    : PanelFrame(tr("Analyzer"), parent)
    , m_ctx(ctx)
    , m_controller(controller)
    , m_meters(meters)
{
    setPanelInfo(this, tr("Analyzer"),
        tr("The spectrum of the main mix or one track, the phase correlation and the stereo "
           "picture, and the monitor controls (Dim, Mono, Mute) for what you hear."));
    setObjectName(QStringLiteral("AnalyzerPanel"));
    m_clock.start();

    const QSettings settings;
    m_display.setPeakHold(settings.value(QLatin1String(kHoldKey), true).toBool());
    m_display.setSlope(SpectrumSlope(std::clamp(settings.value(QLatin1String(kSlopeKey), 0).toInt(), 0, 2)));
    m_display.setSmoothing(
        SpectrumSmoothing(std::clamp(settings.value(QLatin1String(kSmoothingKey), 1).toInt(), 0, 2)));
    m_display.clear();

    auto* body = new QWidget;
    auto* layout = new QHBoxLayout(body);
    layout->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceS, metrics::kSpaceM, metrics::kSpaceS);
    layout->setSpacing(metrics::kSpaceL);

    // Left: spectrum with its options.
    auto* left = new QVBoxLayout;
    auto* options = new QHBoxLayout;
    m_source = new QComboBox;
    m_source->setObjectName(QStringLiteral("SpectrumSource"));
    m_source->setToolTip(tr("What the spectrum shows: the main mix, or one track or bus on its "
                            "own (while the timeline plays at normal speed)."));
    m_hold = new QCheckBox(tr("Peak hold"));
    m_hold->setObjectName(QStringLiteral("SpectrumHold"));
    m_hold->setChecked(m_display.peakHold());
    m_hold->setToolTip(tr("Keep a thin line at the highest level of each frequency for a moment "
                          "before it falls."));
    m_slope = new QComboBox;
    m_slope->setObjectName(QStringLiteral("SpectrumSlope"));
    m_slope->addItem(tr("Flat"), int(SpectrumSlope::Flat));
    m_slope->addItem(tr("3 dB / octave"), int(SpectrumSlope::Pink3));
    m_slope->addItem(tr("4.5 dB / octave"), int(SpectrumSlope::Mix45));
    m_slope->setCurrentIndex(int(m_display.slope()));
    m_slope->setToolTip(tr("Tilts the display up towards the high frequencies, so music looks "
                           "level instead of sloping down: 3 dB per octave makes pink noise "
                           "flat, 4.5 dB per octave suits a full mix."));
    m_smoothing = new QComboBox;
    m_smoothing->setObjectName(QStringLiteral("SpectrumSmoothing"));
    m_smoothing->addItem(tr("No smoothing"), int(SpectrumSmoothing::Off));
    m_smoothing->addItem(tr("Fast"), int(SpectrumSmoothing::Fast));
    m_smoothing->addItem(tr("Slow"), int(SpectrumSmoothing::Slow));
    m_smoothing->setCurrentIndex(int(m_display.smoothing()));
    m_smoothing->setToolTip(tr("How much the bars lag behind the sound: slow is calmer to read, "
                               "none shows every change."));
    options->addWidget(m_source);
    options->addWidget(m_hold);
    options->addWidget(m_slope);
    options->addWidget(m_smoothing);
    options->addStretch(1);
    left->addLayout(options);
    m_view = new SpectrumView(&m_display);
    left->addWidget(m_view, 1);
    layout->addLayout(left, 3);

    // Middle: correlation and goniometer.
    auto* middle = new QVBoxLayout;
    m_gonio = new GoniometerView;
    middle->addWidget(m_gonio, 1);
    m_correlation = new CorrelationBar;
    middle->addWidget(m_correlation);
    m_correlationLabel = new QLabel(tr("Correlation 0.00"));
    m_correlationLabel->setAlignment(Qt::AlignCenter);
    m_correlationLabel->setToolTip(m_correlation->toolTip());
    middle->addWidget(m_correlationLabel);
    layout->addLayout(middle, 1);

    // Right: monitor controls.
    auto* monitor = new QVBoxLayout;
    auto* monitorTitle = new QLabel(tr("Monitor"));
    monitorTitle->setAlignment(Qt::AlignCenter);
    monitorTitle->setToolTip(tr("What you hear. These change the sound of the speakers only; "
                                "the meters, the render and the export stay the same."));
    monitor->addWidget(monitorTitle);
    MonitorControls& mc = MonitorControls::shared();
    m_dim = new QToolButton;
    m_dim->setObjectName(QStringLiteral("MonitorDim"));
    m_dim->setText(tr("Dim"));
    m_dim->setCheckable(true);
    m_dim->setChecked(mc.dim());
    m_dim->setToolTip(tr("Lower what you hear by the dB amount below, to talk or check at a low "
                         "level. Not part of the render."));
    m_dimDb = new QDoubleSpinBox;
    m_dimDb->setObjectName(QStringLiteral("MonitorDimDb"));
    m_dimDb->setRange(MonitorControls::kMinDimDb, MonitorControls::kMaxDimDb);
    m_dimDb->setDecimals(1);
    m_dimDb->setSingleStep(1.0);
    m_dimDb->setSuffix(tr(" dB"));
    m_dimDb->setValue(mc.dimDb());
    m_dimDb->setToolTip(tr("How far Dim lowers the sound, -20 dB by default."));
    m_mono = new QToolButton;
    m_mono->setObjectName(QStringLiteral("MonitorMono"));
    m_mono->setText(tr("Mono"));
    m_mono->setCheckable(true);
    m_mono->setChecked(mc.mono());
    m_mono->setToolTip(tr("Hear left and right added together, to check how the mix sounds on "
                          "a mono speaker. Not part of the render."));
    m_mute = new QToolButton;
    m_mute->setObjectName(QStringLiteral("MonitorMute"));
    m_mute->setText(tr("Mute"));
    m_mute->setCheckable(true);
    m_mute->setChecked(mc.mute());
    m_mute->setToolTip(tr("Silence the speakers. The meters, the render and the export are not "
                          "affected, and Mute is off again the next time Kinoklip starts."));
    auto* dimRow = new QHBoxLayout;
    dimRow->addWidget(m_dim);
    dimRow->addWidget(m_dimDb);
    monitor->addLayout(dimRow);
    auto* row2 = new QHBoxLayout;
    row2->addWidget(m_mono);
    row2->addWidget(m_mute);
    monitor->addLayout(row2);
    monitor->addStretch(1);
    layout->addLayout(monitor, 1);
    setContentWidget(body);

    connect(m_hold, &QCheckBox::toggled, this, [this](bool on) {
        m_display.setPeakHold(on);
        saveDisplaySettings();
        m_view->update();
    });
    connect(m_slope, &QComboBox::currentIndexChanged, this, [this] {
        m_display.setSlope(SpectrumSlope(m_slope->currentData().toInt()));
        saveDisplaySettings();
    });
    connect(m_smoothing, &QComboBox::currentIndexChanged, this, [this] {
        m_display.setSmoothing(SpectrumSmoothing(m_smoothing->currentData().toInt()));
        saveDisplaySettings();
    });
    connect(m_source, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_updating)
            applySource();
    });
    auto saveMonitor = [] { MonitorControls::shared().save(); };
    connect(m_dim, &QToolButton::toggled, this, [saveMonitor](bool on) {
        MonitorControls::shared().setDim(on);
        saveMonitor();
    });
    connect(m_dimDb, &QDoubleSpinBox::valueChanged, this, [saveMonitor](double db) {
        MonitorControls::shared().setDimDb(db);
        saveMonitor();
    });
    connect(m_mono, &QToolButton::toggled, this, [saveMonitor](bool on) {
        MonitorControls::shared().setMono(on);
        saveMonitor();
    });
    connect(m_mute, &QToolButton::toggled, this,
        [](bool on) { MonitorControls::shared().setMute(on); });

    connect(m_meters, &MeterModel::changed, this, &AnalyzerPanel::onMeters);
    connect(m_controller, &MixerController::mixChanged, this, &AnalyzerPanel::syncSources);
    syncSources();
}

void AnalyzerPanel::saveDisplaySettings() const
{
    QSettings settings;
    settings.setValue(QLatin1String(kHoldKey), m_display.peakHold());
    settings.setValue(QLatin1String(kSlopeKey), int(m_display.slope()));
    settings.setValue(QLatin1String(kSmoothingKey), int(m_display.smoothing()));
}

void AnalyzerPanel::syncSources()
{
    // Entries: the main mix, then every audio track and bus (data: kind * 1e12 + id).
    const TimelineData& d = m_controller->data();
    QVector<QPair<QString, qint64>> wanted;
    wanted.append({tr("Main mix"), 0});
    for (const TrackData& t : d.audioTracks)
        wanted.append({t.name.isEmpty() ? tr("Track") : t.name, qint64(1'000'000'000'000LL) + qint64(t.id)});
    for (const BusData& b : d.mixer.buses)
        wanted.append({busDisplayName(b), qint64(2'000'000'000'000LL) + qint64(b.id)});
    bool same = m_source->count() == wanted.size();
    for (int i = 0; same && i < wanted.size(); ++i)
        same = m_source->itemText(i) == wanted[i].first && m_source->itemData(i).toLongLong() == wanted[i].second;
    if (same)
        return;
    const qint64 keep = m_source->currentData().toLongLong();
    m_updating = true;
    m_source->clear();
    for (const auto& w : wanted)
        m_source->addItem(w.first, w.second);
    const int at = m_source->findData(keep);
    m_source->setCurrentIndex(std::max(0, at));
    m_updating = false;
    applySource();
}

void AnalyzerPanel::applySource()
{
    if (!m_ctx.playback)
        return;
    const qint64 v = m_source->currentData().toLongLong();
    if (v == 0) {
        m_ctx.playback->setSpectrumSource(nullptr);
    } else if (v >= 2'000'000'000'000LL) {
        const MixTarget t = MixTarget::bus(int(v - 2'000'000'000'000LL));
        m_ctx.playback->setSpectrumSource(&t);
    } else {
        const MixTarget t = MixTarget::track(quint64(v - 1'000'000'000'000LL));
        m_ctx.playback->setSpectrumSource(&t);
    }
}

void AnalyzerPanel::onMeters()
{
    const MeterFrame& f = m_meters->lastFrame();
    const qint64 now = m_clock.elapsed();
    const double dt = double(std::clamp<qint64>(now - m_lastMs, 1, 500));
    m_lastMs = now;
    if (f.sample >= 0 && f.hasAnalysis) {
        m_display.update(f.spectrumDb.data(), dt);
        m_correlation->setValue(double(f.correlation));
        m_correlationLabel->setText(tr("Correlation %1").arg(double(f.correlation), 0, 'f', 2));
        m_gonio->setPoints(f.gonio.data(), dsp::kGoniometerPoints);
    } else {
        m_display.decay(dt);
        m_correlation->setValue(0.0);
        m_correlationLabel->setText(tr("Correlation %1").arg(0.0, 0, 'f', 2));
        float none[2 * dsp::kGoniometerPoints] = {};
        m_gonio->setPoints(none, dsp::kGoniometerPoints);
    }
    m_view->update();
}

} // namespace kk
