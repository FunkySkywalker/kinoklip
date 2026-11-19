// SPDX-License-Identifier: GPL-3.0-or-later
// MixerStrip layout and gestures.
#include "ui/audio/MixerStrip.h"

#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "core/VoiceIsolation.h"
#include "ui/audio/DynamicsWindow.h"
#include "ui/audio/EqWindow.h"
#include "ui/audio/SendsWindow.h"
#include "ui/audio/FaderWidget.h"
#include "ui/audio/LevelMeter.h"
#include "ui/audio/MixerController.h"
#include "ui/audio/VoiceOverController.h"
#include "ui/audio/VoiceOverMenu.h"
#include "ui/common/DragMime.h"
#include "ui/common/Metrics.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TextFit.h"
#include "ui/dialogs/NormalizeMixDialog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSpinBox>
#include <QToolButton>
#include <QWidgetAction>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr AutomationMode kModes[] = {AutomationMode::Off, AutomationMode::Read,
    AutomationMode::Touch, AutomationMode::Latch, AutomationMode::Write, AutomationMode::Trim};

QString modeLabel(AutomationMode m)
{
    switch (m) {
    case AutomationMode::Off:
        return QCoreApplication::translate("kk::MixerStrip", "Off");
    case AutomationMode::Read:
        return QCoreApplication::translate("kk::MixerStrip", "Read");
    case AutomationMode::Touch:
        return QCoreApplication::translate("kk::MixerStrip", "Touch");
    case AutomationMode::Write:
        return QCoreApplication::translate("kk::MixerStrip", "Write");
    case AutomationMode::Latch:
        return QCoreApplication::translate("kk::MixerStrip", "Latch");
    case AutomationMode::Trim:
        return QCoreApplication::translate("kk::MixerStrip", "Trim");
    }
    return {};
}

/// The input level of an armed track (dBFS scale from -48, red above -6, a clip mark).
class InputLevelBar final : public QWidget
{
public:
    explicit InputLevelBar(const VoiceOverController* vo, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_vo(vo)
    {
        setFixedHeight(6);
        setToolTip(QCoreApplication::translate("kk::MixerStrip", "Level of the microphone"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const SchemeColors& sc = schemeColors();
        QPainter p(this);
        p.fillRect(rect(), palette().window().color().darker(150));
        const float peak = m_vo ? m_vo->peak() : 0.0f;
        const double db = peak > 0.0f ? 20.0 * std::log10(double(peak)) : -96.0;
        const double t = std::clamp((db + 48.0) / 48.0, 0.0, 1.0);
        p.fillRect(QRect(0, 0, int(t * width()), height()),
            db > -6.0        ? sc.negative
                : db > -18.0 ? sc.neutral
                             : sc.positive);
        if (m_vo && m_vo->clipped())
            p.fillRect(QRect(width() - 4, 0, 4, height()), sc.negative);
    }

private:
    const VoiceOverController* m_vo;
};

QToolButton* toggleButton(const QString& text, const QString& tip)
{
    auto* b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setAutoRaise(false);
    b->setFocusPolicy(Qt::NoFocus);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

QString dbText(double db)
{
    if (db <= kFaderMinDb)
        return QCoreApplication::translate("kk::MixerStrip", "-inf");
    return QString::number(db, 'f', 1);
}

} // namespace

MixerStrip::MixerStrip(
    MixerController* controller, MeterModel* meters, const MixTarget& target, QWidget* parent)
    : QWidget(parent)
    , m_controller(controller)
    , m_meters(meters)
    , m_target(target)
{
    setObjectName(target.isBus() ? QStringLiteral("BusStrip") : QStringLiteral("TrackStrip"));
    setFixedWidth(kWidth);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceXs, metrics::kSpaceS, metrics::kSpaceXs, metrics::kSpaceS);
    layout->setSpacing(metrics::kSpaceXs);
    const bool bus = target.isBus();

    m_input = new QLabel;
    m_input->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_input);

    m_eq = toggleButton(tr("EQ"), tr("Equalizer: lit when on; click to edit"));
    m_dynamics
        = toggleButton(tr("Dyn"), tr("Dynamics (compressor, limiter): lit when on; click to edit"));
    // The buttons show the state; a click opens the editor (the state does not toggle).
    for (QToolButton* b : {m_eq, m_dynamics})
        layout->addWidget(b);
    connect(m_eq, &QToolButton::clicked, this, [this] {
        refresh(m_frame); // the click toggled the button: show the real state again
        openEq();
    });
    connect(m_dynamics, &QToolButton::clicked, this, [this] {
        refresh(m_frame);
        openDynamics();
    });

    // Sends (spec 137) and Phase Invert: lit when on; Sends opens its window, Ø inverts.
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(metrics::kSpaceXs);
        m_sends = toggleButton(tr("Sends"),
            tr("Sends: copies of this channel's signal into buses, with a level, a pan and Pre / "
               "Post fader; lit when it has an enabled send. Click to edit"));
        m_invert = toggleButton(QStringLiteral("\u00D8"),
            tr("Phase Invert: flips the polarity of the signal before the effects (both "
               "channels; right-click to choose Left or Right)"));
        m_invert->setContextMenuPolicy(Qt::CustomContextMenu);
        m_invert->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        row->addWidget(m_sends, 1);
        row->addWidget(m_invert);
        layout->addLayout(row);
        connect(m_sends, &QToolButton::clicked, this, [this] {
            refresh(m_frame);
            openSends();
        });
        connect(m_invert, &QToolButton::clicked, this, [this] {
            const ChannelStrip* s = mixerops::strip(m_controller->data(), m_target);
            const bool both = s && s->invertLeft && s->invertRight;
            setInvert(!both, !both);
        });
        connect(m_invert, &QToolButton::customContextMenuRequested, this, [this](QPoint pos) {
            const ChannelStrip* s = mixerops::strip(m_controller->data(), m_target);
            if (!s)
                return;
            QMenu menu(this);
            QAction* l = menu.addAction(tr("Invert Left"));
            l->setCheckable(true);
            l->setChecked(s->invertLeft);
            QAction* r = menu.addAction(tr("Invert Right"));
            r->setCheckable(true);
            r->setChecked(s->invertRight);
            const ChannelStrip before = *s;
            QAction* chosen = menu.exec(m_invert->mapToGlobal(pos));
            if (chosen == l)
                setInvert(!before.invertLeft, before.invertRight);
            else if (chosen == r)
                setInvert(before.invertLeft, !before.invertRight);
        });
    }

    if (!bus) {
        // Voice Isolation (spec 141): one click keeps only the voice of this track; the
        // right-click menu sets the Amount.
        m_voice = toggleButton(tr("Voice Isolation"),
            tr("Voice Isolation (Optional AI): keeps the voice of this track and takes out noise, "
               "music and room sound. Runs on this computer with the built-in RNNoise model; "
               "nothing is downloaded. Right-click to set the Amount."));
        m_voice->setObjectName(QStringLiteral("VoiceIsolationButton"));
        m_voice->setContextMenuPolicy(Qt::CustomContextMenu);
        layout->addWidget(m_voice);
        connect(m_voice, &QToolButton::clicked, this, [this](bool on) {
            m_controller->edit(on ? tr("Voice Isolation On") : tr("Voice Isolation Off"),
                [&](TimelineData& d) { return mixerops::setTrackVoiceIsolation(d, m_target.id, on); });
            refresh(m_frame);
        });
        connect(m_voice, &QToolButton::customContextMenuRequested, this, [this](QPoint pos) {
            QMenu menu(this);
            auto* box = new QSpinBox;
            box->setRange(0, 100);
            box->setSuffix(QStringLiteral(" %"));
            box->setValue(int(voiceisolation::amount(mixerops::audioTrack(m_controller->data(), m_target.id)
                    ? mixerops::audioTrack(m_controller->data(), m_target.id)->mix.effects
                    : QVector<EffectInstance> {})));
            box->setToolTip(tr("Amount: how strongly everything that is not a voice is taken "
                               "away. 0 leaves the sound as it is."));
            auto* row = new QWidget;
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(8, 2, 8, 2);
            rowLayout->addWidget(new QLabel(tr("Amount")));
            rowLayout->addWidget(box);
            auto* action = new QWidgetAction(&menu);
            action->setDefaultWidget(row);
            menu.addAction(action);
            connect(box, &QSpinBox::editingFinished, &menu, &QMenu::close);
            menu.exec(m_voice->mapToGlobal(pos));
            const double value = box->value();
            m_controller->edit(tr("Voice Isolation Amount"), [&](TimelineData& d) {
                return mixerops::setTrackVoiceIsolationAmount(d, m_target.id, value);
            });
            refresh(m_frame);
        });
    }

    if (!bus) {
        // Effects section: slots of the track's effects, then the "+" button (spec 133).
        auto* fx = new QWidget;
        fx->setObjectName(QStringLiteral("StripEffects"));
        auto* fxLayout = new QVBoxLayout(fx);
        fxLayout->setContentsMargins(0, 0, 0, 0);
        fxLayout->setSpacing(1);
        auto* slotBox = new QWidget;
        m_slotLayout = new QVBoxLayout(slotBox);
        m_slotLayout->setContentsMargins(0, 0, 0, 0);
        m_slotLayout->setSpacing(1);
        fxLayout->addWidget(slotBox);
        m_addEffect = new QToolButton;
        m_addEffect->setObjectName(QStringLiteral("FxAdd"));
        m_addEffect->setText(tr("+ FX"));
        m_addEffect->setToolTip(tr("Add an audio effect to this track: it processes the whole "
                                   "track after its clips and before the track's EQ, dynamics, "
                                   "pan and fader. You can also drag an effect here from the "
                                   "Effects Library."));
        m_addEffect->setPopupMode(QToolButton::InstantPopup);
        m_addEffect->setFocusPolicy(Qt::NoFocus);
        m_addEffect->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* menu = new QMenu(m_addEffect);
        m_addEffect->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this] {
            emit selectRequested(m_target);
            fillEffectMenu();
        });
        fxLayout->addWidget(m_addEffect);
        layout->addWidget(fx);
        setAcceptDrops(true);
    }

    m_bus = new QToolButton;
    m_bus->setPopupMode(QToolButton::InstantPopup);
    m_bus->setFocusPolicy(Qt::NoFocus);
    m_bus->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (!bus) {
        m_bus->setToolTip(tr("Bus assignment"));
        auto* menu = new QMenu(m_bus);
        m_bus->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, &MixerStrip::fillBusMenu);
    } else {
        m_bus->setCheckable(true);
        m_bus->setText(tr("Output"));
        m_bus->setToolTip(
            tr("Make this bus the output (heard in playback and rendered by export)"));
        connect(m_bus, &QToolButton::clicked, this, [this] {
            m_controller->edit(tr("Set Output Bus"),
                [this](TimelineData& d) { return mixerops::setOutputBus(d, int(m_target.id)); });
            refresh(m_frame);
        });
    }
    layout->addWidget(m_bus);

    {
        m_pan = new PanKnob;
        m_pan->setFixedHeight(40);
        layout->addWidget(m_pan, 0, Qt::AlignHCenter);
        connect(m_pan, &PanKnob::pressed, this,
            [this] { m_controller->pressControl(m_target, MixerController::Control::Pan); });
        connect(m_pan, &PanKnob::moved, this, [this](double v) {
            m_controller->moveControl(m_target, MixerController::Control::Pan, v);
        });
        connect(m_pan, &PanKnob::released, this,
            [this] { m_controller->releaseControl(m_target, MixerController::Control::Pan); });
        connect(m_pan, &PanKnob::valueRequested, this, [this](double v) {
            m_controller->setControl(m_target, MixerController::Control::Pan, v);
        });
    }
    if (!bus) {
        m_automation = new QComboBox;
        m_automation->setToolTip(tr("Automation mode of this track: Off ignores the curves, Read plays them, "
                                    "Touch records while a control is held, Latch keeps the last value after "
                                    "the release, Write records all the time, Trim adds your change to the "
                                    "existing curve"));
        m_automation->setFocusPolicy(Qt::NoFocus);
        for (AutomationMode m : kModes)
            m_automation->addItem(modeLabel(m), int(m));
        layout->addWidget(m_automation);
        connect(m_automation, &QComboBox::activated, this, [this](int index) {
            const auto mode = AutomationMode(m_automation->itemData(index).toInt());
            m_controller->edit(tr("Automation Mode"),
                [&](TimelineData& d) { return mixerops::setAutomationMode(d, m_target.id, mode); });
        });
    }

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(metrics::kSpaceXs);
    if (!bus) {
        m_arm = toggleButton(tr("R"), tr("Record arm (voice-over)"));
        m_arm->setContextMenuPolicy(Qt::CustomContextMenu);
        buttons->addWidget(m_arm);
        if (VoiceOverController* vo = m_controller->voiceOver()) {
            connect(m_arm, &QToolButton::clicked, this, [this, vo] {
                vo->toggleArm(m_target.id);
                m_arm->setChecked(vo->isArmed(m_target.id)); // a refused arm stays off
            });
            connect(m_arm, &QToolButton::customContextMenuRequested, this, [this, vo](QPoint pos) {
                QMenu menu(this);
                fillVoiceOverInputMenu(menu, vo, m_target.id);
                menu.exec(m_arm->mapToGlobal(pos));
            });
            connect(vo, &VoiceOverController::armedChanged, this, [this] { refresh(m_frame); });
            connect(vo, &VoiceOverController::inputChanged, this, [this] { refresh(m_frame); });
            m_inputLevel = new InputLevelBar(vo);
            m_inputLevel->hide();
            connect(vo, &VoiceOverController::levelChanged, m_inputLevel,
                qOverload<>(&QWidget::update));
        } else {
            m_arm->setEnabled(false);
        }
    }
    m_mute = toggleButton(tr("M"), tr("Mute"));
    buttons->addWidget(m_mute);
    if (!bus) {
        m_solo = toggleButton(tr("S"), tr("Solo"));
        buttons->addWidget(m_solo);
        connect(m_solo, &QToolButton::clicked, this,
            [this](bool on) { m_controller->setTrackSolo(m_target.id, on); });
        connect(m_mute, &QToolButton::clicked, this,
            [this](bool on) { m_controller->setTrackMuted(m_target.id, on); });
    } else {
        connect(m_mute, &QToolButton::clicked, this, [this](bool on) {
            m_controller->edit(on ? tr("Mute Bus") : tr("Unmute Bus"), [&](TimelineData& d) {
                BusData* b = mixerops::bus(d, int(m_target.id));
                if (!b || b->strip.muted == on)
                    return false;
                b->strip.muted = on;
                return true;
            });
        });
    }
    layout->addLayout(buttons);
    if (m_inputLevel)
        layout->addWidget(m_inputLevel);

    auto* faderRow = new QHBoxLayout;
    faderRow->setSpacing(metrics::kSpaceXs);
    m_fader = new FaderWidget;
    m_meter = new LevelMeter(
        meters, bus ? LevelMeter::Source::Bus : LevelMeter::Source::Track, target.id);
    faderRow->addWidget(m_fader, 1);
    faderRow->addWidget(m_meter);
    layout->addLayout(faderRow, 1);
    connect(m_fader, &FaderWidget::pressed, this,
        [this] { m_controller->pressControl(m_target, MixerController::Control::Fader); });
    connect(m_fader, &FaderWidget::moved, this, [this](double db) {
        m_controller->moveControl(m_target, MixerController::Control::Fader, db);
        m_value->setText(dbText(db));
    });
    connect(m_fader, &FaderWidget::released, this,
        [this] { m_controller->releaseControl(m_target, MixerController::Control::Fader); });
    connect(m_fader, &FaderWidget::valueRequested, this, [this](double db) {
        m_controller->setControl(m_target, MixerController::Control::Fader, db);
    });

    m_value = new QLabel;
    m_value->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_value);

    m_name = new QLabel;
    m_name->setAlignment(Qt::AlignCenter);
    m_name->setAutoFillBackground(true);
    m_name->setContextMenuPolicy(Qt::CustomContextMenu);
    m_name->installEventFilter(this); // a double-click renames
    connect(m_name, &QLabel::customContextMenuRequested, this,
        [this](const QPoint& pos) { showNameMenu(m_name->mapToGlobal(pos)); });
    layout->addWidget(m_name);

    refresh(0);
}

void MixerStrip::refresh(int64_t frame)
{
    m_frame = frame;
    const TimelineData& d = m_controller->data();
    const SchemeColors& sc = schemeColors();
    const ChannelStrip* s = mixerops::strip(d, m_target);
    if (!s)
        return;
    QPalette pal = m_name->palette();
    const QColor plate = m_target.isBus() ? sc.focus : sc.audioTrack;
    pal.setColor(QPalette::Window, plate);
    pal.setColor(QPalette::WindowText, sc.textOn(plate));
    m_name->setPalette(pal);

    if (m_slotLayout)
        rebuildSlots(d);
    m_eq->setChecked(s->eq.enabled);
    m_invert->setChecked(s->invertLeft || s->invertRight);
    if (m_voice) {
        const TrackData* t = mixerops::audioTrack(d, m_target.id);
        m_voice->setChecked(t && voiceisolation::isOn(t->mix.effects));
    }
    m_sends->setChecked(std::any_of(s->sends.begin(), s->sends.end(),
        [](const SendData& d) { return d.enabled; }));
    m_dynamics->setChecked(s->dynamics.gateEnabled || s->dynamics.compressorEnabled || s->dynamics.limiterEnabled);
    if (m_target.isBus()) {
        const BusData* b = mixerops::bus(d, int(m_target.id));
        // The strip has a fixed width (as in Resolve): long names are elided, in full as the tip.
        if (b->aux) {
            textfit::setElidedText(m_input, tr("Aux"),
                tr("Aux bus: its input comes from sends only (tracks cannot be routed to it)"));
        } else {
            textfit::setElidedText(
                m_input, tr("Sum"), tr("Input: the tracks routed to this bus, and sends to it"));
        }
        textfit::setElidedText(m_name,
            QStringLiteral("%1\n%2").arg(busDisplayName(*b), b->mono ? tr("Mono") : tr("Stereo")),
            tr("Double-click to rename; right-click for the channel format and delete"));
        m_bus->setChecked(d.mixer.outputBus == b->id);
        m_mute->setChecked(s->muted);
    } else {
        const TrackData* t = mixerops::audioTrack(d, m_target.id);
        const QString name = mixerops::targetName(d, m_target);
        textfit::setElidedText(m_input, name, tr("Input: the track's clips"));
        textfit::setElidedText(m_name,
            QStringLiteral("%1\n%2").arg(name, t->mix.mono ? tr("Mono") : tr("Stereo")),
            tr("Double-click to rename; right-click for the channel format"));
        QStringList buses;
        for (int id : t->mix.buses) {
            if (const BusData* b = mixerops::bus(d, id))
                buses << busDisplayName(*b);
        }
        textfit::setElidedText(m_bus,
            buses.isEmpty()         ? tr("No Bus")
                : buses.size() == 1 ? buses.front()
                                    : tr("%n Buses", nullptr, int(buses.size())),
            tr("Bus assignment: %1")
                .arg(buses.isEmpty() ? tr("none") : buses.join(QStringLiteral(", "))));
        m_mute->setChecked(t->muted);
        m_solo->setChecked(t->solo);
        if (const VoiceOverController* vo = m_controller->voiceOver()) {
            const bool armed = vo->isArmed(t->id);
            m_arm->setChecked(armed);
            m_arm->setToolTip(voiceOverArmTooltip(vo, t->id));
            if (m_inputLevel)
                m_inputLevel->setVisible(armed);
        }
        m_automation->setCurrentIndex(m_automation->findData(int(t->mix.automation)));
    }
    using C = MixerController::Control;
    if (!m_fader->isDragging()) {
        const double db = m_controller->controlValue(m_target, C::Fader, double(frame));
        m_fader->setValue(db);
        m_value->setText(dbText(db));
    }
    if (m_pan)
        m_pan->setValue(m_controller->controlValue(m_target, C::Pan, double(frame)));
}

EqWindow* MixerStrip::openEq()
{
    if (!m_eqWindow) {
        m_eqWindow = new EqWindow(m_controller, m_target, window());
        m_eqWindow->resize(620, 460);
        // Shortcuts are window-scoped: Space, J / K / L, undo work in the tool window too.
        m_eqWindow->addActions(window()->actions());
    }
    m_eqWindow->show();
    m_eqWindow->raise();
    m_eqWindow->activateWindow();
    return m_eqWindow;
}

DynamicsWindow* MixerStrip::openDynamics()
{
    if (!m_dynamicsWindow) {
        m_dynamicsWindow = new DynamicsWindow(m_controller, m_meters, m_target, window());
        m_dynamicsWindow->addActions(window()->actions()); // window-scoped shortcuts
    }
    m_dynamicsWindow->show();
    m_dynamicsWindow->raise();
    m_dynamicsWindow->activateWindow();
    return m_dynamicsWindow;
}

SendsWindow* MixerStrip::openSends()
{
    if (!m_sendsWindow) {
        m_sendsWindow = new SendsWindow(m_controller, m_target, window());
        m_sendsWindow->addActions(window()->actions());
    }
    m_sendsWindow->show();
    m_sendsWindow->raise();
    m_sendsWindow->activateWindow();
    return m_sendsWindow;
}

void MixerStrip::setInvert(bool left, bool right)
{
    m_controller->edit(tr("Phase Invert"), [&](TimelineData& d) {
        return mixerops::setStripInvert(d, m_target, left, right);
    });
}

void MixerStrip::fillBusMenu()
{
    QMenu* menu = m_bus->menu();
    menu->clear();
    const TimelineData& d = m_controller->data();
    const TrackData* t = mixerops::audioTrack(d, m_target.id);
    if (!t)
        return;
    for (const BusData& b : d.mixer.buses) {
        QAction* a = menu->addAction(busDisplayName(b));
        a->setCheckable(true);
        if (b.aux) { // send-only input: use the Sends window
            a->setEnabled(false);
            a->setToolTip(tr("Aux bus: tracks reach it with a send, not a route"));
        }
        a->setChecked(t->mix.buses.contains(b.id));
        const int id = b.id;
        connect(a, &QAction::triggered, this, [this, id](bool on) {
            m_controller->edit(
                on ? tr("Assign to Bus") : tr("Remove from Bus"), [&](TimelineData& data) {
                    return mixerops::setTrackRoute(data, m_target.id, id, on);
                });
        });
    }
}

void MixerStrip::setSelected(bool selected)
{
    if (m_selected == selected)
        return;
    m_selected = selected;
    update();
}

void MixerStrip::mousePressEvent(QMouseEvent* event)
{
    emit selectRequested(m_target);
    QWidget::mousePressEvent(event);
}

void MixerStrip::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (!m_selected)
        return;
    QPainter p(this);
    p.setPen(QPen(schemeColors().focus, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(1, 1, -1, -1));
}

quint64 MixerStrip::addEffect(const QString& type)
{
    if (m_target.isBus())
        return 0;
    quint64 added = 0;
    m_controller->edit(tr("Add Audio Effect"), [&](TimelineData& d) {
        added = mixerops::addTrackEffect(d, m_target.id, type);
        return added != 0;
    });
    if (added)
        emit selectRequested(m_target);
    return added;
}

void MixerStrip::fillEffectMenu()
{
    QMenu* menu = m_addEffect->menu();
    menu->clear();
    const EffectRegistry& registry = EffectRegistry::instance();
    const QVector<const EffectDescriptor*> effects
        = registry.byCategory(EffectCategory::AudioEffect);
    auto addEntry = [&](QMenu* into, const EffectDescriptor* d) {
        const QString id = d->id;
        QAction* a = into->addAction(d->name);
        a->setToolTip(d->description);
        connect(a, &QAction::triggered, this, [this, id] { addEffect(id); });
    };
    for (const QString& group : registry.groups(EffectCategory::AudioEffect)) {
        QMenu* sub = menu->addMenu(EffectRegistry::groupName(group));
        for (const EffectDescriptor* d : effects) {
            if (d->group == group)
                addEntry(sub, d);
        }
    }
    for (const EffectDescriptor* d : effects) {
        if (d->group.isEmpty())
            addEntry(menu, d);
    }
}

void MixerStrip::rebuildSlots(const TimelineData& d)
{
    const TrackData* t = mixerops::audioTrack(d, m_target.id);
    QVector<quint64> ids;
    QVector<bool> on;
    if (t) {
        for (const EffectInstance& fx : t->mix.effects) {
            ids.append(fx.id);
            on.append(fx.enabled);
        }
    }
    if (ids == m_slotIds && on == m_slotOn)
        return;
    m_slotIds = ids;
    m_slotOn = on;
    while (QLayoutItem* item = m_slotLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->setProperty("kkRetired", true); // waits for deleteLater()
            w->deleteLater();
        }
        delete item;
    }
    if (!t)
        return;
    const EffectRegistry& registry = EffectRegistry::instance();
    for (const EffectInstance& fx : t->mix.effects) {
        const EffectDescriptor* desc = registry.find(fx.type);
        const QString name = desc ? desc->name : fx.type;
        const quint64 id = fx.id;
        auto* row = new QWidget;
        row->setObjectName(QStringLiteral("FxSlot"));
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(1);
        auto* bypass = new QToolButton;
        bypass->setObjectName(QStringLiteral("FxSlotBypass"));
        bypass->setText(QStringLiteral("●"));
        bypass->setCheckable(true);
        bypass->setChecked(fx.enabled);
        bypass->setAutoRaise(true);
        bypass->setFocusPolicy(Qt::NoFocus);
        bypass->setFixedWidth(18);
        bypass->setToolTip(tr("Lit: the effect is on. Click to bypass %1, click again to switch "
                              "it on.")
                .arg(name));
        auto* label = new QLabel;
        label->setObjectName(QStringLiteral("FxSlotName"));
        label->setProperty("kkEffectId", qulonglong(id));
        textfit::setElidedText(label, name,
            tr("%1: double-click to open its controls in the Inspector").arg(name));
        if (!fx.enabled) { // bypassed: italic (a disabled label would take no double-click)
            QFont f = label->font();
            f.setItalic(true);
            label->setFont(f);
        }
        label->installEventFilter(this);
        auto* remove = new QToolButton;
        remove->setObjectName(QStringLiteral("FxSlotRemove"));
        remove->setText(QStringLiteral("×"));
        remove->setAutoRaise(true);
        remove->setFocusPolicy(Qt::NoFocus);
        remove->setFixedWidth(18);
        remove->setToolTip(tr("Remove %1 from this track").arg(name));
        h->addWidget(bypass);
        h->addWidget(label, 1);
        h->addWidget(remove);
        m_slotLayout->addWidget(row);
        connect(bypass, &QToolButton::clicked, this, [this, id](bool on) {
            emit selectRequested(m_target);
            m_controller->edit(on ? tr("Enable Audio Effect") : tr("Bypass Audio Effect"),
                [&](TimelineData& data) {
                    return mixerops::setTrackEffectEnabled(data, m_target.id, id, on);
                });
        });
        connect(remove, &QToolButton::clicked, this, [this, id] {
            emit selectRequested(m_target);
            m_controller->edit(tr("Remove Audio Effect"), [&](TimelineData& data) {
                return mixerops::removeTrackEffect(data, m_target.id, id);
            });
        });
    }
}

void MixerStrip::dragEnterEvent(QDragEnterEvent* event)
{
    if (m_target.isBus() || !event->mimeData()->hasFormat(QLatin1String(mime::Effect)))
        return;
    const QString id = QString::fromUtf8(event->mimeData()->data(QLatin1String(mime::Effect)));
    const EffectDescriptor* d = EffectRegistry::instance().find(id);
    if (d && d->category == EffectCategory::AudioEffect)
        event->acceptProposedAction();
}

void MixerStrip::dragMoveEvent(QDragMoveEvent* event)
{
    event->acceptProposedAction();
}

void MixerStrip::dropEvent(QDropEvent* event)
{
    const QString id = QString::fromUtf8(event->mimeData()->data(QLatin1String(mime::Effect)));
    if (addEffect(id))
        event->acceptProposedAction();
}

bool MixerStrip::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonDblClick && watched != m_name) {
        const QVariant id = watched->property("kkEffectId");
        if (id.isValid() && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            emit selectRequested(m_target);
            emit effectOpenRequested(m_target, id.toULongLong());
            return true;
        }
    }
    if (watched == m_name && event->type() == QEvent::MouseButtonPress)
        emit selectRequested(m_target);
    if (watched == m_name && event->type() == QEvent::MouseButtonDblClick
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        askRename();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void MixerStrip::askRename()
{
    const TimelineData& d = m_controller->data();
    const bool bus = m_target.isBus();
    const QString current = bus ? busDisplayName(*mixerops::bus(d, int(m_target.id)))
                                : mixerops::targetName(d, m_target);
    if (m_namePrompt) {
        if (const auto name = m_namePrompt(current))
            renameTarget(*name);
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, bus ? tr("Rename Bus") : tr("Rename Track"),
        tr("Name:"), QLineEdit::Normal, current, &ok);
    if (ok)
        renameTarget(name);
}

void MixerStrip::renameTarget(const QString& name)
{
    const bool bus = m_target.isBus();
    const QString trimmed = name.trimmed();
    m_controller->edit(bus ? tr("Rename Bus") : tr("Rename Track"), [&](TimelineData& data) {
        if (bus)
            return mixerops::renameBus(data, int(m_target.id), trimmed);
        TrackData* t = mixerops::audioTrack(data, m_target.id);
        if (!t || t->name == trimmed)
            return false;
        t->name = trimmed;
        return true;
    });
}

void MixerStrip::showNameMenu(const QPoint& globalPos)
{
    const TimelineData& d = m_controller->data();
    QMenu menu;
    const bool bus = m_target.isBus();
    const bool mono = bus ? mixerops::bus(d, int(m_target.id))->mono
                          : mixerops::audioTrack(d, m_target.id)->mix.mono;
    QAction* monoAction = menu.addAction(tr("Mono"));
    monoAction->setCheckable(true);
    monoAction->setChecked(mono);
    QAction* stereoAction = menu.addAction(tr("Stereo"));
    stereoAction->setCheckable(true);
    stereoAction->setChecked(!mono);
    QAction* rename = nullptr;
    QAction* remove = nullptr;
    QAction* aux = nullptr;
    if (!bus) {
        menu.addSeparator();
        rename = menu.addAction(tr("Rename Track…"));
    } else {
        menu.addSeparator();
        rename = menu.addAction(tr("Rename Bus…"));
        remove = menu.addAction(tr("Delete Bus"));
        remove->setEnabled(m_target.id != quint64(kMainBusId));
        const BusData* thisBus = mixerops::bus(d, int(m_target.id));
        aux = menu.addAction(tr("Aux Bus (send only)"));
        aux->setCheckable(true);
        aux->setChecked(thisBus && thisBus->aux);
        aux->setEnabled(m_target.id != quint64(kMainBusId)
            && int(m_target.id) != d.mixer.outputBus);
        aux->setToolTip(tr("An aux bus takes its input from sends only: tracks cannot be "
                           "routed to it. The main bus and the output bus cannot be aux buses"));
    }
    menu.addSeparator();
    QAction* normalize = menu.addAction(tr("Normalize Audio Levels…"));
    normalize->setToolTip(tr("Measure the whole mix of this channel and move its fader by one "
                             "gain so it reaches a loudness or peak target"));
    QAction* chosen = menu.exec(globalPos);
    if (!chosen)
        return;
    if (chosen == normalize) {
        // The output bus is the main mix (what is heard and rendered).
        const bool mainMix = bus && int(m_target.id) == d.mixer.outputBus;
        normalizeMix(m_controller->project(),
            mainMix ? std::nullopt : std::optional<MixTarget>(m_target), this);
        return;
    }
    if (chosen == monoAction || chosen == stereoAction) {
        const bool wantMono = chosen == monoAction;
        m_controller->edit(tr("Channel Format"), [&](TimelineData& data) {
            return bus ? mixerops::setBusMono(data, int(m_target.id), wantMono)
                       : mixerops::setTrackMono(data, m_target.id, wantMono);
        });
    } else if (chosen == rename) {
        askRename();
    } else if (aux && chosen == aux) {
        const bool on = aux->isChecked();
        m_controller->edit(on ? tr("Make Aux Bus") : tr("Make Normal Bus"),
            [&](TimelineData& data) { return mixerops::setBusAux(data, int(m_target.id), on); });
    } else if (chosen == remove) {
        m_controller->edit(tr("Delete Bus"),
            [&](TimelineData& data) { return mixerops::removeBus(data, int(m_target.id)); });
    }
}

} // namespace kk
