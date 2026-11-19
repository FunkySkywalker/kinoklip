// SPDX-License-Identifier: GPL-3.0-or-later
// MixerController — undo-step handling and automation recording of the mixer widgets.
#include "ui/audio/MixerController.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/AppContext.h"

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
/// A playhead step longer than this (frames) or backwards is a jump (loop, seek): the
/// recording does not erase the keys between the old and the new position.
constexpr int64_t kMaxRecordStepFrames = 50;
} // namespace

MixerController::MixerController(AppContext& ctx, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
{
    connect(m_ctx.project->timeline(), &Timeline::playheadChanged, this,
        &MixerController::onPlayheadChanged);
    connect(m_ctx.project->timeline(), &Timeline::changed, this, &MixerController::mixChanged);
    if (m_ctx.playback) {
        connect(
            m_ctx.playback, &PlaybackEngine::stateChanged, this, &MixerController::onPlaybackState);
    }
}

VoiceOverController* MixerController::voiceOver() const
{
    return m_ctx.voiceOver;
}

MixerController::~MixerController()
{
    if (m_gesture)
        m_ctx.project->endEdit();
    stopRecordings();
}

Project* MixerController::project() const
{
    return m_ctx.project;
}

const TimelineData& MixerController::data() const
{
    return m_ctx.project->timeline()->data();
}

QString MixerController::automationKey(Control control)
{
    return control == Control::Pan ? mixkeys::kPan : mixkeys::kVolumeDb;
}

bool MixerController::playing() const
{
    if (m_playingProbe)
        return m_playingProbe();
    return m_ctx.playback && m_ctx.playback->isPlaying();
}

void MixerController::edit(const QString& label, const std::function<bool(TimelineData&)>& change)
{
    m_ctx.project->beginEdit(label);
    if (!change(m_ctx.project->timeline()->mutableData())) {
        m_ctx.project->cancelEdit();
        return;
    }
    m_ctx.project->endEdit();
}

void MixerController::beginGesture(const QString& label)
{
    if (m_gesture)
        endGesture();
    m_ctx.project->beginEdit(label);
    m_gesture = true;
}

void MixerController::updateGesture(const std::function<bool(TimelineData&)>& change)
{
    if (!m_gesture)
        return;
    if (change(m_ctx.project->timeline()->mutableData()))
        m_ctx.project->timeline()->notifyChanged();
}

void MixerController::endGesture()
{
    if (!m_gesture)
        return;
    m_gesture = false;
    m_ctx.project->endEdit();
}

double MixerController::controlValue(const MixTarget& target, Control control, double frame) const
{
    const TimelineData& d = data();
    if (target.isBus()) {
        const BusData* b = mixerops::bus(d, int(target.id));
        return !b ? 0.0 : control == Control::Fader ? b->strip.volumeDb : b->strip.pan;
    }
    const TrackData* t = mixerops::audioTrack(d, target.id);
    if (!t)
        return 0.0;
    return control == Control::Fader ? mixerops::faderDbAt(*t, frame) : mixerops::panAt(*t, frame);
}

bool MixerController::applyStatic(
    TimelineData& d, const MixTarget& target, Control control, double value)
{
    ChannelStrip* s = mixerops::strip(d, target);
    if (!s)
        return false;
    if (!target.isBus()) {
        const TrackData* t = mixerops::audioTrack(d, target.id);
        if (mixerops::readsAutomation(t->mix, automationKey(control))) {
            // The curve plays: the control edits the key at the playhead.
            const int64_t frame = m_ctx.project->timeline()->playhead();
            return mixerops::writeAutomation(
                d, target.id, automationKey(control), frame, value, frame);
        }
    }
    double& field = control == Control::Pan ? s->pan : s->volumeDb;
    const double clamped = control == Control::Pan ? std::clamp(value, -1.0, 1.0)
                                                   : std::clamp(value, kFaderMinDb, kFaderMaxDb);
    if (field == clamped)
        return false;
    field = clamped;
    return true;
}

MixerController::Recording* MixerController::recording(quint64 trackId, const QString& key)
{
    for (Recording& r : m_recordings) {
        if (r.trackId == trackId && r.key == key)
            return &r;
    }
    return nullptr;
}

bool MixerController::isRecording(quint64 trackId, const QString& key) const
{
    return const_cast<MixerController*>(this)->recording(trackId, key) != nullptr;
}

void MixerController::writeRecording(Recording& r, int64_t frame)
{
    const bool jump = frame < r.lastFrame || frame - r.lastFrame > kMaxRecordStepFrames;
    const int64_t eraseFrom = jump ? frame : r.lastFrame;
    TimelineData& d = m_ctx.project->timeline()->mutableData();
    const bool changed = r.mode == AutomationMode::Trim
        ? mixerops::trimAutomation(
              d, r.trackId, r.key, r.original, eraseFrom, frame, r.value - r.startValue)
        : mixerops::writeAutomation(d, r.trackId, r.key, frame, r.value, eraseFrom);
    if (changed)
        m_ctx.project->timeline()->notifyChanged();
    r.lastFrame = frame;
}

bool MixerController::pressParam(quint64 trackId, const QString& key)
{
    const TrackData* t = mixerops::audioTrack(data(), trackId);
    const AutomationMode mode = t ? t->mix.automation : AutomationMode::Off;
    const bool records = t && playing() && mode != AutomationMode::Off
        && mode != AutomationMode::Read;
    if (!records)
        return false;
    if (Recording* r = recording(trackId, key)) {
        r->touched = true; // a pass already running for this control (Write, Latch)
        return true;
    }
    if (!m_recordEditOpen) {
        m_ctx.project->beginEdit(tr("Write Automation"));
        m_recordEditOpen = true;
    }
    const int64_t frame = m_ctx.project->timeline()->playhead();
    Recording r;
    r.trackId = trackId;
    r.key = key;
    r.mode = mode;
    r.value = mixerops::automationValueAt(*t, key, double(frame));
    r.lastFrame = frame;
    r.touched = true;
    if (mode == AutomationMode::Trim) {
        const auto it = t->mix.automationKeys.values.constFind(key);
        r.original = it != t->mix.automationKeys.values.cend()
            ? *it
            : AnimatedValue(mixerops::staticAutomationValue(*t, key));
        r.startValue = r.value;
        // The trim starts here: a key one frame before the press keeps the curve as it was
        // before it (the first moved value is written at the press frame itself).
        const int64_t before = std::max<int64_t>(0, frame - 1);
        if (mixerops::trimAutomation(m_ctx.project->timeline()->mutableData(), trackId, key,
                r.original, before, before, 0.0))
            m_ctx.project->timeline()->notifyChanged();
    }
    m_recordings.append(r);
    return true;
}

void MixerController::moveParam(quint64 trackId, const QString& key, double value)
{
    if (Recording* r = recording(trackId, key)) {
        r->value = value;
        writeRecording(*r, m_ctx.project->timeline()->playhead());
    }
}

void MixerController::releaseParam(quint64 trackId, const QString& key)
{
    Recording* r = recording(trackId, key);
    if (!r)
        return;
    if (r->mode == AutomationMode::Write || r->mode == AutomationMode::Latch) {
        r->touched = false; // the last value keeps being written until stop
        return;
    }
    m_recordings.erase(m_recordings.begin() + (r - m_recordings.data()));
    if (m_recordings.isEmpty() && m_recordEditOpen) {
        m_recordEditOpen = false;
        m_ctx.project->endEdit();
    }
}

void MixerController::pressControl(const MixTarget& target, Control control)
{
    if (!target.isBus() && pressParam(target.id, automationKey(control)))
        return;
    beginGesture(control == Control::Pan ? tr("Pan") : tr("Fader"));
}

void MixerController::moveControl(const MixTarget& target, Control control, double value)
{
    if (!target.isBus() && isRecording(target.id, automationKey(control))) {
        moveParam(target.id, automationKey(control), value);
        return;
    }
    updateGesture([&](TimelineData& d) { return applyStatic(d, target, control, value); });
}

void MixerController::releaseControl(const MixTarget& target, Control control)
{
    if (!target.isBus() && isRecording(target.id, automationKey(control))) {
        releaseParam(target.id, automationKey(control));
        return;
    }
    endGesture();
}

void MixerController::setControl(const MixTarget& target, Control control, double value)
{
    edit(control == Control::Pan ? tr("Pan") : tr("Fader"),
        [&](TimelineData& d) { return applyStatic(d, target, control, value); });
}

void MixerController::setTrackMuted(quint64 trackId, bool muted)
{
    // Playing in Touch / Latch / Write / Trim: the click writes a Mute step at the playhead.
    if (const TrackData* t = mixerops::audioTrack(data(), trackId);
        t && playing() && t->mix.automation != AutomationMode::Off
        && t->mix.automation != AutomationMode::Read) {
        if (!m_recordEditOpen) {
            m_ctx.project->beginEdit(tr("Write Automation"));
            m_recordEditOpen = true;
        }
        const int64_t frame = m_ctx.project->timeline()->playhead();
        if (mixerops::writeAutomation(m_ctx.project->timeline()->mutableData(), trackId,
                mixkeys::kMute, frame, muted ? 1.0 : 0.0, frame))
            m_ctx.project->timeline()->notifyChanged();
        return;
    }
    edit(muted ? tr("Mute Track") : tr("Unmute Track"), [&](TimelineData& d) {
        TrackData* t = mixerops::audioTrack(d, trackId);
        if (!t || t->muted == muted)
            return false;
        t->muted = muted;
        return true;
    });
}

void MixerController::setTrackSolo(quint64 trackId, bool solo)
{
    edit(solo ? tr("Solo Track") : tr("Unsolo Track"), [&](TimelineData& d) {
        TrackData* t = mixerops::audioTrack(d, trackId);
        if (!t || t->solo == solo)
            return false;
        t->solo = solo;
        return true;
    });
}

void MixerController::onPlayheadChanged(int64_t frame)
{
    if (m_recordings.isEmpty())
        return;
    if (!playing()) {
        stopRecordings();
        return;
    }
    for (Recording& r : m_recordings)
        writeRecording(r, frame);
}

void MixerController::onPlaybackState()
{
    if (playing())
        startWritePasses();
    else
        stopRecordings();
}

void MixerController::startWritePasses()
{
    if (!m_writePasses)
        return;
    const int64_t frame = m_ctx.project->timeline()->playhead();
    for (const TrackData& t : data().audioTracks) {
        if (t.mix.automation != AutomationMode::Write)
            continue;
        for (Control c : {Control::Fader, Control::Pan}) {
            if (recording(t.id, automationKey(c)))
                continue;
            if (!m_recordEditOpen) {
                m_ctx.project->beginEdit(tr("Write Automation"));
                m_recordEditOpen = true;
            }
            Recording r;
            r.trackId = t.id;
            r.key = automationKey(c);
            r.mode = AutomationMode::Write;
            r.value = controlValue(MixTarget::track(t.id), c, double(frame));
            r.lastFrame = frame;
            m_recordings.append(r);
            writeRecording(m_recordings.back(), frame);
        }
    }
}

void MixerController::stopRecordings()
{
    m_recordings.clear();
    if (m_recordEditOpen) {
        m_recordEditOpen = false;
        m_ctx.project->endEdit();
    }
}

} // namespace kk
