// SPDX-License-Identifier: GPL-3.0-or-later
// Files with several audio streams (spec 126): the probe counts them, an edit brings every
// stream in as its own linked audio clip on consecutive tracks, each clip decodes its own
// stream, projects keep the stream (and old ones load with the primary), a one-stream file is
// unchanged, and the mixer (Deliver render, meters, Audio page) mixes all of them.
#include "../media/MediaTestUtil.h"
#include "TimelineTestUtils.h"
#include "core/MediaPool.h"
#include "core/Serialization.h"
#include "media/AudioDecoder.h"
#include "media/MediaInfo.h"
#include "render/AudioMixer.h"
#include "ui/dialogs/ClipAttributesDialog.h"
#include "ui/timeline/TimelineController.h"

#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <numbers>
#include <vector>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {

constexpr double kTones[3] = {440.0, 880.0, 1760.0};

/// Power of @p hz in the stereo block (left channel), Goertzel.
double power(const std::vector<float>& block, double hz)
{
    const size_t frames = block.size() / 2;
    double s1 = 0, s2 = 0;
    const double k = 2.0 * std::cos(2.0 * std::numbers::pi * hz / 48000.0);
    for (size_t i = 0; i < frames; ++i) {
        const double s = double(block[i * 2]) + k * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return (s1 * s1 + s2 * s2 - k * s1 * s2) / double(frames) / double(frames) * 4.0;
}

/// Index of the tone of kTones that dominates @p block, or -1 when none rises above the others.
int dominantTone(const std::vector<float>& block)
{
    double p[3];
    for (int i = 0; i < 3; ++i)
        p[i] = power(block, kTones[i]);
    const int best = int(std::max_element(p, p + 3) - p);
    for (int i = 0; i < 3; ++i) {
        if (i != best && p[i] * 100.0 > p[best])
            return -1;
    }
    return p[best] > 0.01 ? best : -1;
}

std::vector<float> decode(const QString& path, int track, int64_t from = 4800, int frames = 9600)
{
    media::AudioDecoder decoder;
    QString error;
    if (!decoder.open(path, &error, track))
        qWarning("open: %s", qPrintable(error));
    std::vector<float> block(size_t(frames) * 2);
    decoder.read(from, block.data(), frames);
    return block;
}

} // namespace

class TestMultiAudio : public QObject
{
    Q_OBJECT

    QString m_skip;

    static QString multi() { return fixture("multi_audio.mkv"); }
    static QString single() { return fixture("one_audio.mkv"); }

    /// Imports @p path into the fixture's media pool and returns its id.
    static QString import(TimelineFixture& f, const QString& path)
    {
        const ImportResult r = f.project.importMedia({path});
        return r.importedIds.value(0);
    }

    /// Mixer path map of the media pool.
    static std::shared_ptr<MediaPathMap> pathsOf(const Project& project)
    {
        auto map = std::make_shared<MediaPathMap>();
        for (const MediaItem& item : project.mediaPool()->items())
            map->insert(item.id, item.path);
        return map;
    }

    static std::vector<float> mixed(const Project& project, const TimelineData& tl)
    {
        AudioMixer mixer;
        mixer.setMediaPaths(pathsOf(project));
        std::vector<float> out(2 * 9600);
        mixer.mix(tl, 4800, 9600, out.data());
        return out;
    }

private slots:
    void initTestCase()
    {
        initTimelineTestCase("test_multiaudio");
        if (!ensureFixtures(&m_skip))
            qWarning("%s", qPrintable(m_skip));
    }

    void probeCountsTheAudioStreams()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        const media::MediaInfo info = media::probe(multi());
        QVERIFY(info.valid);
        QCOMPARE(info.audioTrackCount(), 3);
        QCOMPARE(info.audioStreams.size(), 3);
        QCOMPARE(info.audioStreams[0].title, QStringLiteral("Game"));
        QCOMPARE(info.audioStreams[2].title, QStringLiteral("Discord"));
        QCOMPARE(info.audioStreams[1].channels, 1);
        QCOMPARE(media::probe(single()).audioTrackCount(), 1);
        QCOMPARE(media::probe(fixture("sine.mp3")).audioTrackCount(), 1);
    }

    void eachStreamDecodesItsOwnTone()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        for (int t = 0; t < 3; ++t)
            QCOMPARE(dominantTone(decode(multi(), t)), t);
        QCOMPARE(dominantTone(decode(multi(), -1)), 0); // primary = the default stream
    }

    void insertMakesLinkedClipsOnConsecutiveTracks()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        TimelineFixture f;
        TimelineController c(f.ctx);
        const QString id = import(f, multi());
        QVERIFY(!id.isEmpty());
        QCOMPARE(f.project.mediaPool()->item(id)->info.audioTrackCount(), 3);
        const size_t tracksBefore = f.view().audioTracks.size();
        QVERIFY(!c.insertSource(id, 0, 0, SourceEditMode::Insert).isEmpty());

        const TimelineData& d = f.view();
        QVERIFY(d.audioTracks.size() >= 3);
        QVERIFY(d.audioTracks.size() >= tracksBefore);
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(1));
        const ClipData& video = d.videoTracks[0].clips[0];
        QVERIFY(video.linkGroup != 0);
        QCOMPARE(video.audioStream, -1);
        for (int t = 0; t < 3; ++t) {
            QCOMPARE(d.audioTracks[size_t(t)].clips.size(), size_t(1));
            const ClipData& a = d.audioTracks[size_t(t)].clips[0];
            QCOMPARE(a.audioStream, t);
            QCOMPARE(a.linkGroup, video.linkGroup);
            QCOMPARE(a.start, video.start);
            QCOMPARE(a.duration, video.duration);
        }
        // They move and delete together (linked selection of one pulls in the others).
        const TimelineData& before = f.view();
        QVector<quint64> group;
        for (const auto* list : {&before.videoTracks, &before.audioTracks})
            for (const TrackData& tr : *list)
                for (const ClipData& cl : tr.clips)
                    if (cl.linkGroup == video.linkGroup)
                        group.append(cl.id);
        QCOMPARE(group.size(), 4);

        // The mixer plays each clip's stream: all three tones are in the mix.
        const std::vector<float> mix = mixed(f.project, d);
        for (int t = 0; t < 3; ++t)
            QVERIFY2(power(mix, kTones[t]) > 0.01, qPrintable(QString::number(kTones[t])));
        // ... and one track alone plays only its own tone.
        TimelineData only = d;
        only.audioTracks[0].clips.clear();
        only.audioTracks[2].clips.clear();
        QCOMPARE(dominantTone(mixed(f.project, only)), 1);
    }

    void clipAttributesChooseTheTracks()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        TimelineFixture f;
        TimelineController c(f.ctx);
        const QString id = import(f, multi());
        const MediaItem* item = f.project.mediaPool()->item(id);
        ClipAttributesDialog dialog(*item, 1);
        QCOMPARE(dialog.audioTrackBoxes().size(), 3);
        QVERIFY(dialog.audioTrackBoxes()[0]->isChecked());
        QCOMPARE(dialog.result().audioTracks, quint64(0)); // all = the default
        dialog.audioTrackBoxes()[0]->setChecked(false);
        dialog.audioTrackBoxes()[2]->setChecked(false);
        const ClipAttributes a = dialog.result();
        QCOMPARE(a.audioTracks, quint64(2)); // only track 2 (bit 1)
        // The last ticked track cannot be unticked.
        dialog.audioTrackBoxes()[1]->setChecked(false);
        QVERIFY(dialog.audioTrackBoxes()[1]->isChecked());

        f.project.beginEdit(QStringLiteral("Clip Attributes"));
        QVERIFY(f.project.mediaPool()->setItemAttributes(id, a));
        f.project.endEdit();
        const auto clips = c.makeMediaClips(id, 0, 0, 0, 0);
        int audio = 0;
        for (const auto& p : clips) {
            if (p.track.type == TrackType::Audio) {
                ++audio;
                QCOMPARE(p.clip.audioStream, 1);
                QCOMPARE(p.track.index, 0);
            }
        }
        QCOMPARE(audio, 1);
        // Round trip of the attribute.
        QCOMPARE(serialization::attributesFromJson(serialization::attributesToJson(a)), a);
        QVERIFY(!serialization::attributesToJson(ClipAttributes {}).contains(QStringLiteral("audioTracks")));
    }

    void projectRoundTripAndOldProjects()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        TimelineFixture f;
        TimelineController c(f.ctx);
        const QString id = import(f, multi());
        QVERIFY(!c.insertSource(id, 0, 0, SourceEditMode::Insert).isEmpty());
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("multi.kkproj"));
        QString error;
        QVERIFY2(f.project.save(path, &error), qPrintable(error));

        Project loaded;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        const TimelineData& d = loaded.timeline()->data();
        for (int t = 0; t < 3; ++t)
            QCOMPARE(d.audioTracks[size_t(t)].clips[0].audioStream, t);
        QCOMPARE(loaded.mediaPool()->item(id)->info.audioTrackCount(), 3);
        QCOMPARE(loaded.mediaPool()->item(id)->info.audioStreams[1].title, QStringLiteral("Mic"));

        // An old project has neither the stream of a clip nor the stream list: it loads with
        // the primary stream (-1) and one audio track.
        QFile in(path);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const QByteArray text = in.readAll();
        in.close();
        QVERIFY(text.contains("\"audioStream\""));
        QByteArray old = text;
        old.replace("\"audioStream\"", "\"unknownKey\"");
        old.replace("\"audioStreams\"", "\"unknownKeys\"");
        const QString oldPath = dir.filePath(QStringLiteral("old.kkproj"));
        QFile out(oldPath);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(old);
        out.close();
        Project legacy;
        QVERIFY2(legacy.load(oldPath, &error), qPrintable(error));
        const TimelineData& ld = legacy.timeline()->data();
        for (int t = 0; t < 3; ++t)
            QCOMPARE(ld.audioTracks[size_t(t)].clips[0].audioStream, -1);
        QCOMPARE(legacy.mediaPool()->item(id)->info.audioTrackCount(), 1);
        // Played back it is the stream it always used (the primary one).
        TimelineData first = ld;
        first.audioTracks[1].clips.clear();
        first.audioTracks[2].clips.clear();
        QCOMPARE(dominantTone(mixed(legacy, first)), 0);
    }

    void oneStreamFileIsUnchanged()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        TimelineFixture f;
        TimelineController c(f.ctx);
        const QString id = import(f, single());
        QCOMPARE(f.project.mediaPool()->item(id)->info.audioTrackCount(), 1);
        const size_t tracks = f.view().audioTracks.size();
        QVERIFY(!c.insertSource(id, 0, 0, SourceEditMode::Insert).isEmpty());
        const TimelineData& d = f.view();
        QCOMPARE(d.audioTracks.size(), tracks); // no track was added
        QCOMPARE(d.audioTracks[0].clips.size(), size_t(1));
        QCOMPARE(d.audioTracks[0].clips[0].audioStream, -1);
        QCOMPARE(d.audioTracks[0].clips[0].linkGroup, d.videoTracks[0].clips[0].linkGroup);
        // Nothing about audio streams is written for it.
        const QJsonObject clip = serialization::clipToJson(d.audioTracks[0].clips[0]);
        QVERIFY(!clip.contains(QStringLiteral("audioStream")));
        QVERIFY(!serialization::mediaInfoToJson(f.project.mediaPool()->item(id)->info)
                     .contains(QStringLiteral("audioStreams")));
        QCOMPARE(dominantTone(mixed(f.project, d)), 0);
    }
};

QTEST_MAIN(TestMultiAudio)
#include "test_multiaudio.moc"
