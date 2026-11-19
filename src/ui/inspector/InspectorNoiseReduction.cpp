// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorNoiseReduction — the AI label, the Learn / Clear Profile buttons and the status line
// under the parameters of the Noise Reduction audio effect (spec 132). Part of InspectorPanel.
// Learn reads the clip's own sound over the timeline's In / Out range (or the first second of
// the clip) and keeps the noise profile in the effect, in one undo step.
#include "core/AudioEffectKeys.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "render/NoiseReduction.h"
#include "ui/common/AppContext.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamAccess.h"
#include "ui/inspector/ParamGroupBox.h"

#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <memory>

namespace kk {

void InspectorPanel::buildNoiseReductionControls(
    ParamGroupBox* group, const EffectInstance& fx, quint64 clipId)
{
    if (fx.type != QLatin1String(afxid::NoiseReduction))
        return;
    const quint64 effectId = fx.id;
    auto* badge = new QLabel(tr("Optional AI: the RNNoise model is built into Kinoklip and runs "
                                "on this computer; nothing is downloaded or uploaded. The "
                                "spectral gate needs no model."));
    badge->setObjectName(QStringLiteral("AiLabel"));
    badge->setWordWrap(true);
    group->addWidgetRow(tr("AI"), badge);

    auto* buttons = new QWidget;
    auto* layout = new QHBoxLayout(buttons);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* learn = new QPushButton(tr("Learn"));
    learn->setObjectName(QStringLiteral("NrLearn"));
    learn->setToolTip(tr("Learn the noise from the stretch between the timeline's In and Out "
                         "marks that lies on this clip (mark a pause with only the noise in "
                         "it); without marks the first second of the clip is used. Switches "
                         "the mode to Manual Learn."));
    auto* clear = new QPushButton(tr("Clear Profile"));
    clear->setObjectName(QStringLiteral("NrClear"));
    clear->setToolTip(tr("Forget the learned noise profile."));
    layout->addWidget(learn);
    layout->addWidget(clear);
    layout->addStretch(1);
    group->addWidgetRow(QString(), buttons);
    auto* status = new QLabel;
    status->setObjectName(QStringLiteral("NrStatus"));
    status->setWordWrap(true);
    group->addWidgetRow(QString(), status);
    auto message = std::make_shared<QString>(); // the outcome of the last Learn
    auto busy = std::make_shared<bool>(false); // a Learn is reading the sound on a worker thread

    connect(learn, &QPushButton::clicked, this, [this, clipId, effectId, message, busy, learn] {
        if (*busy)
            return;
        const ClipData* c = clip(clipId);
        if (!c || !m_ctx.project || !m_ctx.project->mediaPool())
            return;
        const MediaItem* item = m_ctx.project->mediaPool()->item(c->mediaId);
        if (!item) {
            *message = tr("This clip has no sound file to learn from.");
            refreshValues();
            return;
        }
        const TimelineData& d = m_ctx.project->timeline()->data();
        int64_t from = c->start;
        int64_t to = c->start + std::min<int64_t>(c->duration, std::max<int64_t>(1, std::llround(d.fps.toDouble())));
        if (d.markIn >= 0 && d.markOut > d.markIn) {
            const int64_t lo = std::max(c->start, d.markIn);
            const int64_t hi = std::min(c->end(), d.markOut);
            if (hi > lo) {
                from = lo;
                to = hi;
            }
        }
        const int64_t a = c->sourceTimeUsAt(from, d.fps);
        const int64_t b = c->sourceTimeUsAt(to, d.fps);
        // The sound is read and analysed on a worker thread (up to 10 s of decoding): the
        // button stays disabled and the status line says so until the result comes back.
        struct Result
        {
            dsp::noise::Profile profile;
            QString error;
        };
        *busy = true;
        *message = tr("Learning the noise ...");
        learn->setEnabled(false);
        refreshValues();
        auto* watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcher<Result>::finished, this,
            [this, watcher, clipId, effectId, message, busy, a, b] {
                const Result r = watcher->result();
                watcher->deleteLater();
                *busy = false;
                if (!r.profile.valid()) {
                    *message = tr("Could not learn a profile: %1. Mark a longer stretch (at "
                                  "least a tenth of a second) that holds only the noise.")
                                   .arg(r.error);
                    refreshValues();
                    return;
                }
                const QString text = dsp::noise::encodeProfile(r.profile);
                *message = tr("Learned from %1 s of sound.")
                               .arg(double(std::abs(b - a)) / 1e6, 0, 'f', 1);
                if (!clip(clipId)) {
                    refreshValues();
                    return;
                }
                editClip(tr("Learn Noise Profile"), clipId, [effectId, text](ClipData& cd) {
                    if (EffectInstance* e = paramaccess::effect(cd, effectId)) {
                        e->params.setStatic(QLatin1String(afxkeys::NrProfile), text);
                        e->params.setStatic(
                            QLatin1String(afxkeys::NrMode), QLatin1String(afxchoice::NrLearn));
                    }
                });
                refreshValues();
            });
        watcher->setFuture(QtConcurrent::run([path = item->path, a, b, attributes = c->attributes,
                                                 stream = c->audioStream]() -> Result {
            Result r;
            r.profile = dsp::noise::learnProfileFromFile(path, std::max<int64_t>(0, std::min(a, b)),
                std::max(a, b), attributes, stream, &r.error);
            return r;
        }));
    });
    connect(clear, &QPushButton::clicked, this, [this, clipId, effectId, message] {
        message->clear();
        editClip(tr("Clear Noise Profile"), clipId, [effectId](ClipData& cd) {
            if (EffectInstance* e = paramaccess::effect(cd, effectId))
                e->params.setStatic(QLatin1String(afxkeys::NrProfile), QString());
        });
    });

    group->addHeaderRefresh([this, effectId, clear, learn, status, message, busy](const ClipData& cd) {
        const EffectInstance* e = paramaccess::effect(cd, effectId);
        const bool has = e
            && dsp::noise::decodeProfile(
                   e->params.staticValue(QLatin1String(afxkeys::NrProfile)).toString())
                   .valid();
        const bool learnMode = e
            && e->params.staticValue(QLatin1String(afxkeys::NrMode)).toString()
                == QLatin1String(afxchoice::NrLearn);
        clear->setEnabled(has && !*busy);
        learn->setEnabled(!*busy);
        QString text = has ? tr("A noise profile is learned.")
                           : tr("No noise profile yet.");
        if (learnMode && !has)
            text += tr(" Manual Learn works like Auto Speech's spectral gate until you press Learn.");
        if (!message->isEmpty())
            text += QLatin1Char(' ') + *message;
        if (status->text() != text)
            status->setText(text);
    });
}

} // namespace kk
