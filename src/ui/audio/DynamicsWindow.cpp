// SPDX-License-Identifier: GPL-3.0-or-later
// DynamicsWindow layout, edits and the transfer curve.
#include "ui/audio/DynamicsWindow.h"

#include "render/Dynamics.h"
#include "ui/audio/MeterModel.h"
#include "ui/audio/MixerController.h"
#include "ui/common/Metrics.h"
#include "ui/common/SchemeColors.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {
constexpr double kCurveFloorDb = -60.0;
}

// ---- TransferCurveView ----

TransferCurveView::TransferCurveView(QWidget* parent)
    : QWidget(parent)
{ }

void TransferCurveView::setSettings(const DynamicsSettings& settings)
{
    if (settings == m_settings)
        return;
    m_settings = settings;
    update();
}

void TransferCurveView::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    const double side = std::min(width(), height()) - 2.0;
    const QRectF r(1, 1, side, side);
    p.fillRect(r, sc.view);
    auto pt = [&](double inDb, double outDb) {
        return QPointF(r.left() + (inDb - kCurveFloorDb) / -kCurveFloorDb * r.width(),
            r.bottom()
                - (std::clamp(outDb, kCurveFloorDb, 0.0) - kCurveFloorDb) / -kCurveFloorDb
                    * r.height());
    };
    p.setPen(sc.grid);
    for (double db = -48.0; db < 0.0; db += 12.0) {
        p.drawLine(pt(db, kCurveFloorDb), pt(db, 0.0));
        p.drawLine(pt(kCurveFloorDb, db), pt(0.0, db));
    }
    p.setPen(QPen(sc.line, 1, Qt::DashLine));
    p.drawLine(pt(kCurveFloorDb, kCurveFloorDb), pt(0.0, 0.0)); // unity
    const DynamicsSettings& d = m_settings;
    QPainterPath path;
    for (int i = 0; i <= 120; ++i) {
        const double in = kCurveFloorDb + i * (-kCurveFloorDb / 120.0);
        double out = in;
        if (d.gateEnabled)
            out -= dsp::gateGainReductionDb(
                in, d.gateThresholdDb, d.gateRangeDb, d.gateRatio, d.gateMode);
        if (d.compressorEnabled)
            out = in - dsp::compressorGainReductionDb(in, d.thresholdDb, d.ratio, d.kneeDb)
                + d.makeupDb;
        if (d.limiterEnabled)
            out = std::min(out, d.limiterCeilingDb);
        if (i == 0)
            path.moveTo(pt(in, out));
        else
            path.lineTo(pt(in, out));
    }
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(d.gateEnabled || d.compressorEnabled || d.limiterEnabled ? sc.focus
                                                                          : sc.textDisabled,
        2));

    p.drawPath(path);
    p.setPen(QPen(sc.neutral, 1, Qt::DotLine));
    if (d.compressorEnabled)
        p.drawLine(pt(d.thresholdDb, kCurveFloorDb), pt(d.thresholdDb, 0.0));
    if (d.gateEnabled)
        p.drawLine(pt(d.gateThresholdDb, kCurveFloorDb), pt(d.gateThresholdDb, 0.0));
}

// ---- DynamicsWindow ----

DynamicsWindow::DynamicsWindow(
    MixerController* controller, MeterModel* meters, const MixTarget& target, QWidget* parent)
    : QWidget(parent, Qt::Tool)
    , m_controller(controller)
    , m_meters(meters)
    , m_target(target)
{
    setObjectName(QStringLiteral("DynamicsWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM);
    layout->setSpacing(metrics::kSpaceL);

    auto* left = new QVBoxLayout;
    m_curve = new TransferCurveView;
    m_curve->setMinimumSize(180, 180);
    left->addWidget(m_curve, 1);
    m_reduction = new QLabel;
    left->addWidget(m_reduction);
    layout->addLayout(left, 1);

    auto* form = new QFormLayout;
    m_gate = new QCheckBox(tr("Gate / Expander"));
    m_gate->setObjectName(QStringLiteral("DynamicsGate"));
    m_gate->setToolTip(tr("Switch the gate on. It makes the sound quieter while the key is "
                          "under the threshold: silence between words, noise between notes."));
    form->addRow(m_gate);
    m_gateMode = new QComboBox;
    m_gateMode->setObjectName(QStringLiteral("DynamicsGateMode"));
    m_gateMode->addItem(tr("Gate"), int(GateMode::Gate));
    m_gateMode->addItem(tr("Expander"), int(GateMode::Expander));
    m_gateMode->setToolTip(tr("Gate drops the sound by the Range at once when the key is under "
                              "the threshold. Expander lowers it gradually, by the Ratio, the "
                              "further the key is under the threshold."));
    form->addRow(tr("Mode"), m_gateMode);
    addField(form, tr("Gate threshold"), kGateMinThresholdDb, 0.0, 1, tr(" dB"),
        &DynamicsSettings::gateThresholdDb,
        tr("The gate opens when the key level goes above this."), "DynamicsGateThreshold");
    addField(form, tr("Range"), 0.0, kGateMaxRangeDb, 1, tr(" dB"), &DynamicsSettings::gateRangeDb,
        tr("How much quieter the sound gets while the gate is closed."), "DynamicsGateRange");
    m_gateRatio = addField(form, tr("Expander ratio"), 1.0, kGateMaxRatio, 1, tr(":1"),
        &DynamicsSettings::gateRatio,
        tr("Expander only: for every dB the key is under the threshold the sound loses "
           "Ratio - 1 more dB, up to the Range."),
        "DynamicsGateRatio");
    addField(form, tr("Gate attack"), kDynMinTimeMs, kDynMaxAttackMs, 1, tr(" ms"),
        &DynamicsSettings::gateAttackMs, tr("How fast the gate opens when the key rises."),
        "DynamicsGateAttack");
    addField(form, tr("Hold"), 0.0, kGateMaxHoldMs, 0, tr(" ms"), &DynamicsSettings::gateHoldMs,
        tr("How long the gate stays open after the key fell under the threshold."),
        "DynamicsGateHold");
    addField(form, tr("Gate release"), kDynMinTimeMs, kDynMaxReleaseMs, 0, tr(" ms"),
        &DynamicsSettings::gateReleaseMs, tr("How fast the gate closes after the hold."),
        "DynamicsGateRelease");
    addField(form, tr("Hysteresis"), 0.0, kGateMaxHysteresisDb, 1, tr(" dB"),
        &DynamicsSettings::gateHysteresisDb,
        tr("The gate closes only when the key is this far under the threshold, so it does not "
           "flutter on a level that hovers around it."),
        "DynamicsGateHysteresis");
    m_key = new QComboBox;
    m_key->setObjectName(QStringLiteral("DynamicsKey"));
    m_key->setToolTip(tr("What the gate and the compressor listen to. Self is this strip. Pick "
                         "another track or bus to duck by it, for example the music by the "
                         "voice. Sources that would make a loop are greyed out."));
    form->addRow(tr("Key"), m_key);
    addField(form, tr("Key low cut"), kKeyMinHz, kKeyMaxHz, 0, tr(" Hz"),
        &DynamicsSettings::keyLowCutHz,
        tr("Sounds under this frequency are not heard by the key (20 Hz is off): a rumble does "
           "not open the gate or push the compressor."),
        "DynamicsKeyLowCut");
    addField(form, tr("Key high cut"), kKeyMinHz, kKeyMaxHz, 0, tr(" Hz"),
        &DynamicsSettings::keyHighCutHz,
        tr("Sounds over this frequency are not heard by the key (20000 Hz is off)."),
        "DynamicsKeyHighCut");
    m_compressor = new QCheckBox(tr("Compressor"));
    m_compressor->setObjectName(QStringLiteral("DynamicsCompressor"));
    m_compressor->setToolTip(tr("Switch the compressor on. It lowers loud parts."));
    form->addRow(m_compressor);
    addField(form, tr("Threshold"), kDynMinThresholdDb, 0.0, 1, tr(" dB"),
        &DynamicsSettings::thresholdDb, tr("The compressor works on sound louder than this."),
        "DynamicsThreshold");
    addField(form, tr("Ratio"), 1.0, kDynMaxRatio, 1, tr(":1"), &DynamicsSettings::ratio,
        tr("For every Ratio dB over the threshold the output rises 1 dB."), "DynamicsRatio");
    addField(form, tr("Attack"), kDynMinTimeMs, kDynMaxAttackMs, 1, tr(" ms"),
        &DynamicsSettings::attackMs, tr("How fast the compressor lowers the sound."),
        "DynamicsAttack");
    addField(form, tr("Release"), kDynMinTimeMs, kDynMaxReleaseMs, 0, tr(" ms"),
        &DynamicsSettings::releaseMs, tr("How fast the compressor lets go."), "DynamicsRelease");
    addField(form, tr("Knee"), 0.0, kDynMaxKneeDb, 1, tr(" dB"), &DynamicsSettings::kneeDb,
        tr("0 starts the compression at once at the threshold; more starts it softly before it."),
        "DynamicsKnee");
    addField(form, tr("Make-up"), 0.0, kDynMaxMakeupDb, 1, tr(" dB"), &DynamicsSettings::makeupDb,
        tr("Gain added after the compressor to make up for the level it took off."),
        "DynamicsMakeup");
    m_limiter = new QCheckBox(tr("Limiter"));
    m_limiter->setObjectName(QStringLiteral("DynamicsLimiter"));
    m_limiter->setToolTip(tr("Switch the limiter on. Nothing goes over the ceiling."));
    form->addRow(m_limiter);
    addField(form, tr("Ceiling"), kLimiterMinCeilingDb, 0.0, 1, tr(" dB"),
        &DynamicsSettings::limiterCeilingDb, tr("The highest level the limiter lets through."),
        "DynamicsCeiling");
    addField(form, tr("Limiter release"), kDynMinTimeMs, kDynMaxReleaseMs, 0, tr(" ms"),
        &DynamicsSettings::limiterReleaseMs, tr("How fast the limiter lets go."),
        "DynamicsLimiterRelease");
    layout->addLayout(form);

    connect(m_gate, &QCheckBox::toggled, this, [this](bool on) {
        editDynamics(on ? tr("Enable Gate") : tr("Bypass Gate"),
            [on](DynamicsSettings& d) { d.gateEnabled = on; });
    });
    connect(m_gateMode, &QComboBox::activated, this, [this](int index) {
        const GateMode mode = GateMode(m_gateMode->itemData(index).toInt());
        editDynamics(tr("Gate Mode"), [mode](DynamicsSettings& d) { d.gateMode = mode; });
    });
    connect(m_key, &QComboBox::activated, this, [this](int index) {
        const KeyKind kind = KeyKind(m_key->itemData(index, Qt::UserRole).toInt());
        const quint64 id = m_key->itemData(index, Qt::UserRole + 1).toULongLong();
        m_controller->edit(tr("Key Source"), [this, kind, id](TimelineData& d) {
            return mixerops::setKeySource(d, m_target, kind, id);
        });
    });
    connect(m_compressor, &QCheckBox::toggled, this, [this](bool on) {
        editDynamics(on ? tr("Enable Compressor") : tr("Bypass Compressor"),
            [on](DynamicsSettings& d) { d.compressorEnabled = on; });
    });
    connect(m_limiter, &QCheckBox::toggled, this, [this](bool on) {
        editDynamics(on ? tr("Enable Limiter") : tr("Bypass Limiter"),
            [on](DynamicsSettings& d) { d.limiterEnabled = on; });
    });
    connect(m_controller, &MixerController::mixChanged, this, &DynamicsWindow::refresh);
    if (m_meters) {
        connect(m_meters, &MeterModel::changed, this, [this] {
            const MeterReading r
                = m_target.isBus() ? m_meters->bus(int(m_target.id)) : m_meters->track(m_target.id);
            m_reduction->setText(tr("Gain reduction: %1 dB").arg(-r.gainReductionDb, 0, 'f', 1));
        });
    }
    m_reduction->setText(tr("Gain reduction: %1 dB").arg(0.0, 0, 'f', 1));
    refresh();
}

QDoubleSpinBox* DynamicsWindow::addField(QFormLayout* form, const QString& label, double min,
    double max, int decimals, const QString& suffix, double DynamicsSettings::*field,
    const QString& tooltip, const char* objectName)
{
    auto* box = new QDoubleSpinBox;
    box->setObjectName(QLatin1String(objectName));
    box->setToolTip(tooltip);
    box->setRange(min, max);
    box->setDecimals(decimals);
    box->setSuffix(suffix);
    box->setKeyboardTracking(false);
    box->setAccelerated(true);
    form->addRow(label, box);
    if (QWidget* labelWidget = form->labelForField(box))
        labelWidget->setToolTip(tooltip);
    m_fields.append({box, field});
    connect(box, &QDoubleSpinBox::valueChanged, this, [this, label, field](double v) {
        editDynamics(label, [field, v](DynamicsSettings& d) { d.*field = v; });
    });
    return box;
}

void DynamicsWindow::editDynamics(
    const QString& label, const std::function<void(DynamicsSettings&)>& change)
{
    m_controller->edit(label, [&](TimelineData& d) {
        ChannelStrip* s = mixerops::strip(d, m_target);
        if (!s)
            return false;
        const DynamicsSettings before = s->dynamics;
        change(s->dynamics);
        mixerops::clampStrip(*s);
        return !(s->dynamics == before);
    });
}

void DynamicsWindow::refresh()
{
    const TimelineData& d = m_controller->data();
    const ChannelStrip* s = mixerops::strip(d, m_target);
    if (!s) {
        close();
        return;
    }
    setWindowTitle(tr("Dynamics - %1").arg(mixerops::targetName(d, m_target)));
    const QSignalBlocker b1(m_compressor), b2(m_limiter), b3(m_gate), b4(m_gateMode);
    m_gate->setChecked(s->dynamics.gateEnabled);
    m_gateMode->setCurrentIndex(m_gateMode->findData(int(s->dynamics.gateMode)));
    m_gateRatio->setEnabled(s->dynamics.gateMode == GateMode::Expander);
    fillKeyList(d);
    m_compressor->setChecked(s->dynamics.compressorEnabled);
    m_limiter->setChecked(s->dynamics.limiterEnabled);
    for (const auto& [box, field] : std::as_const(m_fields)) {
        const QSignalBlocker block(box);
        const double v = s->dynamics.*field;
        if (!box->hasFocus() || box->value() == v)
            box->setValue(v);
    }
    m_curve->setSettings(s->dynamics);
}

void DynamicsWindow::fillKeyList(const TimelineData& data)
{
    const ChannelStrip* strip = mixerops::strip(data, m_target);
    if (!strip)
        return;
    const QSignalBlocker block(m_key);
    m_key->clear();
    auto add = [&](const QString& text, KeyKind kind, quint64 id) {
        m_key->addItem(text);
        const int row = m_key->count() - 1;
        m_key->setItemData(row, int(kind), Qt::UserRole);
        m_key->setItemData(row, id, Qt::UserRole + 1);
        if (auto* model = qobject_cast<QStandardItemModel*>(m_key->model())) {
            if (!mixerops::keySourceAllowed(data, m_target, kind, id))
                model->item(row)->setEnabled(false);
        }
        return row;
    };
    int current = add(tr("Self"), KeyKind::Self, 0);
    const DynamicsSettings& dyn = strip->dynamics;
    for (const TrackData& t : data.audioTracks) {
        if (m_target.isBus() || t.id != m_target.id) {
            const int row = add(
                tr("Track %1").arg(mixerops::targetName(data, MixTarget::track(t.id))),
                KeyKind::Track, t.id);
            if (dyn.keyKind == KeyKind::Track && dyn.keyId == t.id)
                current = row;
        }
    }
    for (const BusData& b : data.mixer.buses) {
        if (!m_target.isBus() || quint64(b.id) != m_target.id) {
            const int row = add(tr("Bus %1").arg(busDisplayName(b)), KeyKind::Bus, quint64(b.id));
            if (dyn.keyKind == KeyKind::Bus && dyn.keyId == quint64(b.id))
                current = row;
        }
    }
    m_key->setCurrentIndex(current);
}

} // namespace kk
