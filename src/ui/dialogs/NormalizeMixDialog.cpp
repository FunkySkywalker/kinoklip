// SPDX-License-Identifier: GPL-3.0-or-later
// NormalizeMixDialog implementation.
#include "ui/dialogs/NormalizeMixDialog.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "render/RenderTypes.h"
#include "ui/common/DialogSizing.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <limits>

namespace kk {

namespace {
constexpr int kPollMs = 50;
constexpr double kMinTarget = -60.0;
constexpr double kMaxTarget = 0.0;
} // namespace

NormalizeMixDialog::NormalizeMixDialog(
    Project* project, std::optional<MixTarget> channel, QWidget* parent)
    : QDialog(parent)
    , m_project(project)
    , m_channel(channel)
{
    setObjectName(QStringLiteral("NormalizeMixDialog"));
    setWindowTitle(tr("Normalize Audio Levels"));

    m_mode = new QComboBox;
    m_mode->setObjectName(QStringLiteral("NormalizeMixMode"));
    m_mode->addItem(tr("ITU-R BS.1770-4 (loudness)"), int(audionorm::Mode::Loudness));
    m_mode->addItem(tr("True Peak"), int(audionorm::Mode::TruePeak));
    m_mode->addItem(tr("Sample Peak Program"), int(audionorm::Mode::SamplePeak));
    m_mode->setToolTip(tr("What the target measures: the integrated loudness of the whole mix, "
                          "its highest true peak, or its highest sample"));
    m_standard = new QComboBox;
    m_standard->setObjectName(QStringLiteral("NormalizeMixStandard"));
    for (const loudness::Standard s : loudness::standards())
        m_standard->addItem(loudness::standardName(s), int(s));
    m_standard->setToolTip(tr("Loudness standard: choosing one sets the target; Custom keeps "
                              "your own number"));
    m_target = new QDoubleSpinBox;
    m_target->setObjectName(QStringLiteral("NormalizeMixTarget"));
    m_target->setRange(kMinTarget, kMaxTarget);
    m_target->setDecimals(1);
    m_target->setSingleStep(0.5);
    m_target->setToolTip(tr("The level the mix should reach"));

    auto* form = new QFormLayout;
    form->addRow(tr("Normalization Mode"), m_mode);
    form->addRow(tr("Standard"), m_standard);
    form->addRow(tr("Target Level"), m_target);

    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("NormalizeMixProgress"));
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progress->setVisible(false);
    const TimelineData& tl = m_project->timeline()->data();
    const QString what = m_channel ? mixerops::targetName(tl, *m_channel) : tr("Main mix");
    m_status = new QLabel(tr("%1: the whole mix of the In / Out range, or of the timeline.").arg(what));
    m_status->setObjectName(QStringLiteral("NormalizeMixStatus"));
    m_status->setWordWrap(true);
    m_status->setToolTip(tr("The fader of this channel is moved by one gain; one undo step"));

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Normalize"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &NormalizeMixDialog::start);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &NormalizeMixDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_status);
    layout->addWidget(m_progress);
    layout->addWidget(m_buttons);

    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this, &NormalizeMixDialog::pollProgress);
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_updating)
            updateUnit();
    });
    connect(m_standard, &QComboBox::currentIndexChanged, this, [this] {
        if (m_updating)
            return;
        const loudness::Standard s = standard();
        if (s != loudness::Standard::Custom) {
            m_updating = true;
            m_target->setValue(loudness::presetLufs(s));
            m_updating = false;
        }
    });
    connect(m_target, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (m_updating || mode() != audionorm::Mode::Loudness)
            return;
        if (standard() != loudness::Standard::Custom
            && std::abs(v - loudness::presetLufs(standard())) > 1e-9) {
            m_updating = true; // a typed number is a custom target
            m_standard->setCurrentIndex(m_standard->findData(int(loudness::Standard::Custom)));
            m_updating = false;
        }
    });
    updateUnit();
    dialogsizing::fit(this);
}

NormalizeMixDialog::~NormalizeMixDialog()
{
    if (m_shared)
        m_shared->cancel = true;
    if (m_future.isValid())
        m_future.waitForFinished();
}

void NormalizeMixDialog::updateUnit()
{
    m_updating = true;
    const bool loud = mode() == audionorm::Mode::Loudness;
    m_standard->setEnabled(loud);
    switch (mode()) {
    case audionorm::Mode::SamplePeak:
        m_target->setSuffix(tr(" dBFS"));
        break;
    case audionorm::Mode::TruePeak:
        m_target->setSuffix(tr(" dBTP"));
        break;
    case audionorm::Mode::Loudness:
        m_target->setSuffix(tr(" LUFS"));
        break;
    }
    if (loud) {
        const loudness::Standard s = standard();
        m_target->setValue(loudness::presetLufs(s == loudness::Standard::Custom
                ? loudness::Standard::EbuR128
                : s));
    } else {
        m_target->setValue(audionorm::defaultTarget(mode()));
    }
    m_updating = false;
}

void NormalizeMixDialog::setMode(audionorm::Mode mode)
{
    m_mode->setCurrentIndex(m_mode->findData(int(mode)));
}

audionorm::Mode NormalizeMixDialog::mode() const
{
    return audionorm::Mode(m_mode->currentData().toInt());
}

void NormalizeMixDialog::setStandard(loudness::Standard standard)
{
    m_standard->setCurrentIndex(m_standard->findData(int(standard)));
}

loudness::Standard NormalizeMixDialog::standard() const
{
    return loudness::Standard(m_standard->currentData().toInt());
}

void NormalizeMixDialog::setTarget(double level)
{
    m_target->setValue(level);
}

double NormalizeMixDialog::target() const
{
    return m_target->value();
}

QString NormalizeMixDialog::statusText() const
{
    return m_status->text();
}

bool NormalizeMixDialog::isRunning() const
{
    return m_future.isValid() && !m_future.isFinished();
}

void NormalizeMixDialog::setControlsEnabled(bool enabled)
{
    for (QWidget* w : {static_cast<QWidget*>(m_mode), static_cast<QWidget*>(m_standard),
             static_cast<QWidget*>(m_target)})
        w->setEnabled(enabled);
    if (enabled && mode() != audionorm::Mode::Loudness)
        m_standard->setEnabled(false);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(enabled);
}

void NormalizeMixDialog::start()
{
    if (isRunning())
        return;
    setControlsEnabled(false);
    m_progress->setValue(0);
    m_progress->setVisible(true);
    m_status->setText(tr("Measuring…"));
    m_shared = std::make_shared<Shared>();
    const auto [first, end] = audionorm::mixRange(m_project->timeline()->data());
    // The worker owns a snapshot of the timeline and the media table; nothing else.
    m_future = QtConcurrent::run([timeline = m_project->timeline()->snapshot(),
                                     media = makeMediaPathMap(m_project->mediaPool()->state()),
                                     channel = m_channel, first = first, end = end,
                                     shared = m_shared] {
        return audionorm::measureMix(*timeline, media, channel, first, end, &shared->cancel,
            [&](double f) { shared->permille = int(1000.0 * f); });
    });
    m_poll->start();
}

void NormalizeMixDialog::pollProgress()
{
    if (m_shared)
        m_progress->setValue(m_shared->permille);
    if (m_future.isValid() && m_future.isFinished()) {
        m_poll->stop();
        finishMeasurement();
    }
}

void NormalizeMixDialog::finishMeasurement()
{
    m_result = m_future.result();
    m_future = {};
    if (m_shared && m_shared->cancel)
        return; // reject() already closed the dialog
    m_progress->setVisible(false);
    const double level = audionorm::levelOf(m_result, mode());
    const double change = m_result.valid ? audionorm::faderChange(level, target())
                                         : std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(change)) {
        m_change = std::numeric_limits<double>::quiet_NaN();
        m_status->setText(tr("Nothing to normalize: there is no sound in this range."));
        setControlsEnabled(true);
        return;
    }
    const MixTarget channel = m_channel ? *m_channel
                                        : MixTarget::bus(m_project->timeline()->data().mixer.outputBus);
    m_project->beginEdit(tr("Normalize Audio Levels"));
    mixerops::offsetFader(m_project->timeline()->mutableData(), channel, change);
    m_project->endEdit();
    m_change = change;
    QDialog::accept();
}

void NormalizeMixDialog::reject()
{
    if (m_future.isValid()) {
        m_shared->cancel = true;
        m_poll->stop();
        m_future.waitForFinished(); // at most one 100 ms block of mixing
        m_future = {};
    }
    QDialog::reject();
}

bool normalizeMix(Project* project, std::optional<MixTarget> channel, QWidget* parent)
{
    NormalizeMixDialog dialog(project, channel, parent);
    return dialog.exec() == QDialog::Accepted;
}

} // namespace kk
