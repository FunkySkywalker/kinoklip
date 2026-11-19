// SPDX-License-Identifier: GPL-3.0-or-later
// LoudnessPanel layout, readouts and the loudness gauge.
#include "ui/audio/LoudnessPanel.h"

#include "core/MixerOps.h"
#include "playback/PlaybackEngine.h"
#include "ui/audio/LevelMeter.h"
#include "ui/audio/MeterModel.h"
#include "ui/audio/MixerController.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelInfo.h"
#include "ui/common/SchemeColors.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace kk {

namespace {
constexpr double kGaugeFloor = -50.0; ///< LUFS at the bottom of the gauge
constexpr double kOnTargetLu = 1.0; ///< +-LU counted as "on target"
constexpr double kNone = -199.0; ///< readings at or below this show "-inf"

constexpr const char* kTargetSetting = "audio/loudnessTarget";

QString lufsText(double v, const char* unit)
{
    if (v <= kNone)
        return QStringLiteral("-inf %1").arg(QLatin1String(unit));
    return QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QLatin1String(unit));
}
} // namespace

// ---- LoudnessGauge ----

LoudnessGauge::LoudnessGauge(MeterModel* meters, QWidget* parent)
    : QWidget(parent)
    , m_meters(meters)
{
    connect(m_meters, &MeterModel::changed, this, qOverload<>(&QWidget::update));
    setToolTip(tr("Momentary (M) and short-term (S) loudness; the line marks the target"));
}

void LoudnessGauge::setTargetLufs(double lufs)
{
    m_target = lufs;
    update();
}

void LoudnessGauge::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    const MeterFrame& f = m_meters->lastFrame();
    const QRect area(0, 14, width(), height() - 16);
    auto yOf = [&](double lufs) {
        const double t = std::clamp((lufs - kGaugeFloor) / -kGaugeFloor, 0.0, 1.0);
        return area.bottom() - int(t * area.height());
    };
    const double values[2] = {f.momentaryLufs, f.shortTermLufs};
    const QString names[2] = {tr("M"), tr("S")};
    const int barWidth = (area.width() - 8) / 2;
    for (int i = 0; i < 2; ++i) {
        const QRect bar(4 + i * (barWidth + 4), area.top(), barWidth - 4, area.height());
        p.fillRect(bar, mixColors(sc.view, sc.shadow, 0.35));
        const double v = values[i];
        if (v > kGaugeFloor) {
            const QColor c = v > m_target + kOnTargetLu ? sc.negative
                : v >= m_target - kOnTargetLu           ? sc.positive
                                                        : sc.neutral;
            const int top = yOf(v);
            p.fillRect(QRect(bar.left(), top, bar.width(), bar.bottom() - top + 1), c);
        }
        p.setPen(sc.textDim);
        p.drawText(QRect(bar.left(), 0, bar.width(), 14), Qt::AlignCenter, names[i]);
    }
    p.setPen(QPen(sc.text, 1, Qt::DashLine));
    const int ty = yOf(m_target);
    p.drawLine(0, ty, width(), ty);
}

// ---- LoudnessHistoryGraph ----

LoudnessHistoryGraph::LoudnessHistoryGraph(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("LoudnessHistory"));
    setToolTip(tr("Short-term loudness over the last minutes; the dashed line is the target"));
}

void LoudnessHistoryGraph::addSample(double lufs)
{
    m_samples.append(float(lufs <= kNone ? kGaugeFloor : std::max(lufs, kGaugeFloor)));
    if (m_samples.size() > kMaxPoints)
        m_samples.remove(0, m_samples.size() - kMaxPoints);
    update();
}

void LoudnessHistoryGraph::clear()
{
    m_samples.clear();
    update();
}

void LoudnessHistoryGraph::setTargetLufs(double lufs)
{
    m_target = lufs;
    update();
}

void LoudnessHistoryGraph::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), mixColors(sc.view, sc.shadow, 0.35));
    const QRectF area = QRectF(rect()).adjusted(2, 2, -2, -2);
    auto yOf = [&](double lufs) {
        const double t = std::clamp((lufs - kGaugeFloor) / -kGaugeFloor, 0.0, 1.0);
        return area.bottom() - t * area.height();
    };
    // Grid every 10 LU.
    p.setPen(QPen(sc.textDim, 1, Qt::DotLine));
    for (double v = kGaugeFloor + 10.0; v < 0.0; v += 10.0)
        p.drawLine(QPointF(area.left(), yOf(v)), QPointF(area.right(), yOf(v)));
    // The newest sample sits at the right edge; the points are evenly spaced.
    const double step = area.width() / double(kMaxPoints - 1);
    QPolygonF line;
    const int n = int(m_samples.size());
    for (int i = 0; i < n; ++i)
        line << QPointF(area.right() - double(n - 1 - i) * step, yOf(double(m_samples[i])));
    if (n >= 2) {
        const double last = double(m_samples.last());
        const QColor c = last > m_target + kOnTargetLu ? sc.negative
            : last >= m_target - kOnTargetLu           ? sc.positive
                                                       : sc.neutral;
        p.setPen(QPen(c, 1.5));
        p.drawPolyline(line);
    }
    p.setPen(QPen(sc.text, 1, Qt::DashLine));
    p.drawLine(QPointF(area.left(), yOf(m_target)), QPointF(area.right(), yOf(m_target)));
}

// ---- LoudnessPanel ----

LoudnessPanel::LoudnessPanel(
    AppContext& ctx, MixerController* controller, MeterModel* meters, QWidget* parent)
    : PanelFrame(tr("Meters"), parent)
    , m_ctx(ctx)
    , m_controller(controller)
    , m_meters(meters)
{
    setPanelInfo(this, tr("Meters"),
        tr("Output and bus meters, and the loudness of the output bus (momentary, short-term, "
           "integrated, loudness range, true peak) against a loudness standard, with a history "
           "graph."));
    setObjectName(QStringLiteral("LoudnessPanel"));
    if (ctx.actions && ctx.actions->contains(QStringLiteral("audio.reset_loudness")))
        addTitleBarAction(ctx.actions->action(QStringLiteral("audio.reset_loudness")));

    auto* body = new QWidget;
    auto* layout = new QHBoxLayout(body);
    layout->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceS, metrics::kSpaceM, metrics::kSpaceS);
    layout->setSpacing(metrics::kSpaceL);

    auto* outColumn = new QVBoxLayout;
    m_output = new LevelMeter(meters, LevelMeter::Source::Output);
    m_output->setScaleVisible(true);
    m_output->setMinimumWidth(48);
    outColumn->addWidget(m_output, 1);
    auto* outLabel = new QLabel(tr("Output"));
    outLabel->setAlignment(Qt::AlignCenter);
    outColumn->addWidget(outLabel);
    layout->addLayout(outColumn);

    m_busRow = new QHBoxLayout;
    m_busRow->setSpacing(metrics::kSpaceS);
    layout->addLayout(m_busRow);

    m_gauge = new LoudnessGauge(meters);
    layout->addWidget(m_gauge);

    // Standard, target and Start / Pause above the readouts and the history graph.
    auto* right = new QVBoxLayout;
    auto* options = new QHBoxLayout;
    m_standard = new QComboBox;
    m_standard->setObjectName(QStringLiteral("LoudnessStandard"));
    for (const loudness::Standard s : loudness::standards())
        m_standard->addItem(loudness::standardName(s), int(s));
    m_standard->setToolTip(tr("Loudness standard to measure against: EBU R128 -23 LUFS, ATSC "
                              "A/85 -24 LKFS, YouTube / streaming -14 LUFS, podcast -16 LUFS, "
                              "or your own target"));
    m_targetSpin = new QDoubleSpinBox;
    m_targetSpin->setObjectName(QStringLiteral("LoudnessTarget"));
    m_targetSpin->setRange(loudness::kMinTargetLufs, loudness::kMaxTargetLufs);
    m_targetSpin->setDecimals(1);
    m_targetSpin->setSingleStep(0.5);
    m_targetSpin->setSuffix(tr(" LUFS"));
    m_targetSpin->setToolTip(tr("Target loudness (editable with Custom)"));
    m_startPause = new QToolButton;
    m_startPause->setObjectName(QStringLiteral("LoudnessStartPause"));
    m_startPause->setCheckable(true);
    m_startPause->setToolButtonStyle(Qt::ToolButtonTextOnly);
    options->addWidget(m_standard);
    options->addWidget(m_targetSpin);
    options->addWidget(m_startPause);
    options->addStretch(1);
    right->addLayout(options);

    auto* lower = new QHBoxLayout;
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    m_momentary = new QLabel;
    m_shortTerm = new QLabel;
    m_integrated = new QLabel;
    m_range = new QLabel;
    m_truePeak = new QLabel;
    m_targetLabel = new QLabel;
    m_targetLabel->setObjectName(QStringLiteral("LoudnessTargetReadout"));
    m_momentary->setToolTip(tr("Loudness of the last 0.4 seconds"));
    m_shortTerm->setToolTip(tr("Loudness of the last 3 seconds"));
    m_integrated->setToolTip(tr("Loudness of everything measured since Reset (gated, BS.1770)"));
    m_range->setToolTip(tr("Loudness range (LRA): how much the loudness varies, in LU"));
    m_truePeak->setToolTip(tr("Highest true peak since Reset"));
    m_targetLabel->setToolTip(tr("The target, and whether the integrated loudness is over or "
                                 "under it"));
    form->addRow(tr("Momentary"), m_momentary);
    form->addRow(tr("Short term"), m_shortTerm);
    form->addRow(tr("Integrated"), m_integrated);
    form->addRow(tr("Loudness range"), m_range);
    form->addRow(tr("True peak max"), m_truePeak);
    form->addRow(tr("Target"), m_targetLabel);
    lower->addLayout(form);
    m_history = new LoudnessHistoryGraph;
    lower->addWidget(m_history, 1);
    right->addLayout(lower, 1);
    layout->addLayout(right, 1);
    setContentWidget(body);

    const QSettings settings;
    m_targetSetting = loudness::targetFromString(
        settings.value(QLatin1String(kTargetSetting)).toString(), m_targetSetting);
    updateTargetUi();
    connect(m_standard, &QComboBox::currentIndexChanged, this, [this] {
        if (m_updatingUi)
            return;
        loudness::Target t = m_targetSetting;
        t.standard = loudness::Standard(m_standard->currentData().toInt());
        if (t.standard == loudness::Standard::Custom) { // starts from what was shown
            t.customLufs = m_targetSpin->value();
        }
        setTarget(t);
    });
    connect(m_targetSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (m_updatingUi || m_targetSetting.standard != loudness::Standard::Custom)
            return;
        loudness::Target t = m_targetSetting;
        t.customLufs = v;
        setTarget(t);
    });
    connect(m_startPause, &QToolButton::clicked, this, [this](bool paused) { setMeasuring(!paused); });
    if (ctx.actions && ctx.actions->contains(QStringLiteral("audio.reset_loudness"))) {
        connect(ctx.actions->action(QStringLiteral("audio.reset_loudness")), &QAction::triggered,
            this, [this] { m_history->clear(); });
    }

    connect(m_meters, &MeterModel::changed, this, &LoudnessPanel::updateReadouts);
    connect(m_controller, &MixerController::mixChanged, this, &LoudnessPanel::syncBuses);
    syncBuses();
    updateReadouts();
}

void LoudnessPanel::syncBuses()
{
    const TimelineData& d = m_controller->data();
    QVector<int> ids;
    for (const BusData& b : d.mixer.buses)
        ids.append(b.id);
    if (ids != m_busIds) {
        for (LevelMeter* m : std::as_const(m_busMeters))
            m->deleteLater();
        for (QLabel* l : std::as_const(m_busLabels))
            l->deleteLater();
        m_busMeters.clear();
        m_busLabels.clear();
        while (QLayoutItem* item = m_busRow->takeAt(0)) {
            if (QLayout* sub = item->layout())
                delete sub;
            else
                delete item;
        }
        for (int id : ids) {
            auto* column = new QVBoxLayout;
            auto* meter = new LevelMeter(m_meters, LevelMeter::Source::Bus, quint64(id));
            auto* label = new QLabel;
            label->setAlignment(Qt::AlignCenter);
            column->addWidget(meter, 1, Qt::AlignHCenter);
            column->addWidget(label);
            m_busRow->addLayout(column);
            m_busMeters.append(meter);
            m_busLabels.append(label);
        }
        m_busIds = ids;
    }
    for (int i = 0; i < m_busIds.size(); ++i) {
        const BusData* b = mixerops::bus(d, m_busIds[i]);
        m_busLabels[i]->setText(b ? busDisplayName(*b) : QString());
        m_busLabels[i]->setEnabled(b && b->id == d.mixer.outputBus);
    }
    m_output->setToolTip(tr("Output bus: %1")
                             .arg(mixerops::bus(d, d.mixer.outputBus)
                                     ? busDisplayName(*mixerops::bus(d, d.mixer.outputBus))
                                     : QString()));
}

void LoudnessPanel::updateReadouts()
{
    const MeterFrame& f = m_meters->lastFrame();
    m_momentary->setText(lufsText(f.momentaryLufs, "LUFS"));
    m_shortTerm->setText(lufsText(f.shortTermLufs, "LUFS"));
    m_integrated->setText(lufsText(f.integratedLufs, "LUFS"));
    m_range->setText(QStringLiteral("%1 LU").arg(f.loudnessRange, 0, 'f', 1));
    m_truePeak->setText(lufsText(f.truePeakMaxDb, "dBTP"));
    const double target = m_targetSetting.lufs();
    const QString verdict = loudness::deviationText(f.integratedLufs, target);
    m_targetLabel->setText(verdict.isEmpty() ? lufsText(target, "LUFS")
                                             : QStringLiteral("%1: %2").arg(
                                                   lufsText(target, "LUFS"), verdict));
    // One history point every kSampleMs while sound is measured.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_measuring && f.sample >= 0 && now - m_lastSampleMs >= LoudnessHistoryGraph::kSampleMs) {
        m_lastSampleMs = now;
        m_history->addSample(f.shortTermLufs);
    }
}

void LoudnessPanel::updateTargetUi()
{
    m_updatingUi = true;
    const int index = m_standard->findData(int(m_targetSetting.standard));
    m_standard->setCurrentIndex(std::max(0, index));
    const bool custom = m_targetSetting.standard == loudness::Standard::Custom;
    m_targetSpin->setValue(m_targetSetting.lufs());
    m_targetSpin->setEnabled(custom);
    m_startPause->setChecked(!m_measuring);
    m_startPause->setText(m_measuring ? tr("Pause") : tr("Start"));
    m_startPause->setToolTip(m_measuring ? tr("Pause the loudness measurement; the readings stay")
                                         : tr("Continue the loudness measurement"));
    m_gauge->setTargetLufs(m_targetSetting.lufs());
    m_history->setTargetLufs(m_targetSetting.lufs());
    m_updatingUi = false;
}

void LoudnessPanel::storeTarget() const
{
    QSettings settings;
    settings.setValue(QLatin1String(kTargetSetting), loudness::targetToString(m_targetSetting));
}

void LoudnessPanel::setTarget(const loudness::Target& target)
{
    m_targetSetting = loudness::clamped(target);
    storeTarget();
    updateTargetUi();
    updateReadouts();
}

void LoudnessPanel::setMeasuring(bool on)
{
    m_measuring = on;
    if (m_ctx.playback)
        m_ctx.playback->setLoudnessPaused(!on);
    updateTargetUi();
}

QString LoudnessPanel::targetText() const
{
    return m_targetLabel->text();
}

QString LoudnessPanel::readoutText() const
{
    return QStringList {m_momentary->text(), m_shortTerm->text(), m_integrated->text(),
        m_range->text(), m_truePeak->text()}
        .join(QStringLiteral(" | "));
}

} // namespace kk
