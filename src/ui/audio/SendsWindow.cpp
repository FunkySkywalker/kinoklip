// SPDX-License-Identifier: GPL-3.0-or-later
// SendsWindow layout and edits.
#include "ui/audio/SendsWindow.h"

#include "ui/audio/MixerController.h"
#include "ui/common/Metrics.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace kk {

namespace {

QDoubleSpinBox* spin(double min, double max, int decimals, const QString& suffix)
{
    auto* s = new QDoubleSpinBox;
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false); // a typed value commits on Enter / focus out: one step
    s->setAccelerated(true);
    return s;
}

} // namespace

SendsWindow::SendsWindow(MixerController* controller, const MixTarget& target, QWidget* parent)
    : QWidget(parent, Qt::Tool)
    , m_controller(controller)
    , m_target(target)
{
    setObjectName(QStringLiteral("SendsWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM);
    layout->setSpacing(metrics::kSpaceM);

    m_empty = new QLabel(tr("No sends yet. Choose a bus below and press Add."));
    m_empty->setToolTip(tr("A send copies this channel's signal into another bus"));
    layout->addWidget(m_empty);
    m_grid = new QGridLayout;
    m_grid->setHorizontalSpacing(metrics::kSpaceM);
    m_grid->setVerticalSpacing(metrics::kSpaceS);
    layout->addLayout(m_grid);
    layout->addStretch(1);

    auto* addRow = new QHBoxLayout;
    m_addBus = new QComboBox;
    m_addBus->setObjectName(QStringLiteral("SendAddBus"));
    m_addBus->setToolTip(tr("Buses this channel can send to; a bus that would make a loop is "
                            "not listed"));
    m_add = new QPushButton(tr("Add Send"));
    m_add->setObjectName(QStringLiteral("SendAdd"));
    m_add->setToolTip(tr("Add a send to the chosen bus"));
    addRow->addWidget(m_addBus, 1);
    addRow->addWidget(m_add);
    layout->addLayout(addRow);
    connect(m_add, &QPushButton::clicked, this, [this] {
        if (m_addBus->currentIndex() >= 0)
            addSendTo(m_addBus->currentData().toInt());
    });

    connect(m_controller, &MixerController::mixChanged, this, &SendsWindow::refresh);
    refresh();
}

bool SendsWindow::addSendTo(int busId)
{
    bool added = false;
    m_controller->edit(tr("Add Send"), [&](TimelineData& d) {
        added = mixerops::addSend(d, m_target, busId);
        return added;
    });
    return added;
}

void SendsWindow::editSend(
    int busId, const QString& label, const std::function<void(SendData&)>& change)
{
    m_controller->edit(label, [&](TimelineData& d) {
        const ChannelStrip* s = mixerops::strip(d, m_target);
        if (!s)
            return false;
        for (const SendData& e : s->sends) {
            if (e.busId == busId) {
                SendData next = e;
                change(next);
                return mixerops::updateSend(d, m_target, next);
            }
        }
        return false;
    });
}

void SendsWindow::rebuildRows(const ChannelStrip& strip)
{
    for (const Row& r : m_rows) {
        for (QWidget* w : {r.on, r.level, r.mode, r.pan, r.remove}) {
            m_grid->removeWidget(w);
            w->deleteLater();
        }
    }
    m_rows.clear();
    // Header
    const TimelineData& d = m_controller->data();
    int rowIndex = 0;
    for (const SendData& send : strip.sends) {
        const int busId = send.busId;
        const BusData* bus = mixerops::bus(d, busId);
        Row row;
        row.busId = busId;
        auto* on = new QCheckBox(bus ? busDisplayName(*bus) : QString::number(busId));
        on->setObjectName(QStringLiteral("SendOn"));
        on->setToolTip(tr("Switch this send on or off"));
        auto* level = spin(kSendMinDb, kSendMaxDb, 1, tr(" dB"));
        level->setObjectName(QStringLiteral("SendLevel"));
        level->setSingleStep(0.5);
        level->setToolTip(tr("Send level in dB"));
        auto* mode = new QComboBox;
        mode->setObjectName(QStringLiteral("SendMode"));
        mode->addItem(tr("Post"), false);
        mode->addItem(tr("Pre"), true);
        mode->setToolTip(tr("Post: the signal after the fader and pan. Pre: the signal before "
                            "them, so the fader does not change the send"));
        auto* pan = spin(-100.0, 100.0, 0, tr(" %"));
        pan->setObjectName(QStringLiteral("SendPan"));
        pan->setToolTip(tr("Send pan: -100 % left, 0 centre, +100 % right"));
        auto* remove = new QPushButton(tr("Remove"));
        remove->setObjectName(QStringLiteral("SendRemove"));
        remove->setToolTip(tr("Remove this send"));
        m_grid->addWidget(on, rowIndex, 0);
        m_grid->addWidget(level, rowIndex, 1);
        m_grid->addWidget(mode, rowIndex, 2);
        m_grid->addWidget(pan, rowIndex, 3);
        m_grid->addWidget(remove, rowIndex, 4);
        row.on = on;
        row.level = level;
        row.mode = mode;
        row.pan = pan;
        row.remove = remove;
        m_rows.append(row);
        ++rowIndex;

        connect(on, &QCheckBox::toggled, this, [this, busId](bool v) {
            editSend(busId, v ? tr("Enable Send") : tr("Disable Send"),
                [v](SendData& s) { s.enabled = v; });
        });
        connect(level, &QDoubleSpinBox::valueChanged, this, [this, busId](double v) {
            editSend(busId, tr("Send Level"), [v](SendData& s) { s.levelDb = v; });
        });
        connect(mode, &QComboBox::activated, this, [this, busId, mode](int index) {
            const bool pre = mode->itemData(index).toBool();
            editSend(busId, pre ? tr("Send Pre Fader") : tr("Send Post Fader"),
                [pre](SendData& s) { s.preFader = pre; });
        });
        connect(pan, &QDoubleSpinBox::valueChanged, this, [this, busId](double v) {
            editSend(busId, tr("Send Pan"), [v](SendData& s) { s.pan = v / 100.0; });
        });
        connect(remove, &QPushButton::clicked, this, [this, busId] {
            m_controller->edit(tr("Remove Send"), [&](TimelineData& data) {
                return mixerops::removeSend(data, m_target, busId);
            });
        });
    }
}

void SendsWindow::refresh()
{
    const TimelineData& d = m_controller->data();
    const ChannelStrip* s = mixerops::strip(d, m_target);
    if (!s) {
        close(); // the track / bus is gone
        return;
    }
    setWindowTitle(tr("Sends - %1").arg(mixerops::targetName(d, m_target)));
    bool same = s->sends.size() == m_rows.size();
    for (int i = 0; same && i < m_rows.size(); ++i)
        same = s->sends[i].busId == m_rows[i].busId;
    if (!same)
        rebuildRows(*s);
    m_empty->setVisible(s->sends.isEmpty());
    for (int i = 0; i < m_rows.size(); ++i) {
        const SendData& send = s->sends[i];
        const Row& r = m_rows[i];
        auto* on = static_cast<QCheckBox*>(r.on);
        auto* level = static_cast<QDoubleSpinBox*>(r.level);
        auto* mode = static_cast<QComboBox*>(r.mode);
        auto* pan = static_cast<QDoubleSpinBox*>(r.pan);
        const QSignalBlocker b1(on), b2(level), b3(mode), b4(pan);
        on->setChecked(send.enabled);
        // A field being typed in keeps its text (the mix changes during playback too).
        if (!level->hasFocus() || level->value() == send.levelDb)
            level->setValue(send.levelDb);
        mode->setCurrentIndex(mode->findData(send.preFader));
        if (!pan->hasFocus() || pan->value() == send.pan * 100.0)
            pan->setValue(send.pan * 100.0);
    }
    // The buses a send can still be added to.
    const QSignalBlocker blockAdd(m_addBus);
    const QVariant current = m_addBus->currentData();
    m_addBus->clear();
    for (const BusData& b : d.mixer.buses) {
        const bool exists = std::any_of(s->sends.begin(), s->sends.end(),
            [&](const SendData& e) { return e.busId == b.id; });
        if (!exists && mixerops::sendAllowed(d, m_target, b.id))
            m_addBus->addItem(busDisplayName(b), b.id);
    }
    const int again = m_addBus->findData(current);
    if (again >= 0)
        m_addBus->setCurrentIndex(again);
    m_add->setEnabled(m_addBus->count() > 0 && s->sends.size() < kMaxSends);
}

} // namespace kk
