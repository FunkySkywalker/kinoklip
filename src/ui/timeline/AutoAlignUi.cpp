// SPDX-License-Identifier: GPL-3.0-or-later
// AutoAlignUi — see AutoAlignUi.h.
#include "ui/timeline/AutoAlignUi.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMainWindow>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStatusBar>

#include <chrono>
#include <future>
#include <memory>

namespace kk {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::AutoAlignUi", text);
}

constexpr int kPollMs = 30;
constexpr int kStatusMs = 10000;

void say(QWidget* window, const QString& text)
{
    if (auto* main = qobject_cast<QMainWindow*>(window))
        main->statusBar()->showMessage(text, kStatusMs);
    else
        QMessageBox::information(window, tr("Auto Align Clips"), text);
}

} // namespace

void runAutoAlign(AppContext& ctx, QWidget* window, autoalign::Method method)
{
    if (!ctx.project || !ctx.selection)
        return;
    autoalign::Request request;
    request.clips = ctx.selection->clips();
    request.method = method;
    if (request.clips.size() < 2) {
        say(window, tr("Auto Align Clips: select two or more clips on different tracks."));
        return;
    }
    QString error;
    const auto job = autoalign::prepare(
        ctx.project->timeline()->data(), *ctx.project->mediaPool(), request, &error);
    if (!job) {
        say(window, tr("Auto Align Clips: %1").arg(error));
        return;
    }

    autoalign::Result result;
    if (method == autoalign::Method::Waveform) {
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        auto done = std::make_shared<std::atomic<int>>(0);
        const int total = 1 + int(job->others.size());
        QProgressDialog dialog(
            tr("Analysing the audio…"), tr("Cancel"), 0, total, window);
        dialog.setWindowTitle(tr("Auto Align Clips"));
        dialog.setWindowModality(Qt::WindowModal);
        dialog.setMinimumDuration(300);
        dialog.setValue(0);
        const autoalign::Job copy = *job;
        std::future<autoalign::Result> future
            = std::async(std::launch::async, [copy, cancel, done] {
                  return autoalign::analyse(copy, cancel.get(),
                      [done](int finished, int) { done->store(finished); });
              });
        while (future.wait_for(std::chrono::milliseconds(kPollMs)) != std::future_status::ready) {
            dialog.setValue(done->load());
            QApplication::processEvents(QEventLoop::AllEvents, kPollMs);
            if (dialog.wasCanceled())
                cancel->store(true);
        }
        result = future.get();
        dialog.reset();
        if (result.cancelled)
            return;
    } else {
        result = autoalign::analyse(*job);
    }

    timelineedit::runEdit(ctx.project, tr("Auto Align Clips"),
        [&](TimelineData& data) { return autoalign::apply(data, *job, result); });

    const int aligned = result.count(autoalign::Status::Aligned);
    const int unchanged = result.count(autoalign::Status::Unchanged);
    const int problems = int(result.entries.size()) - aligned - unchanged;
    QString text;
    if (aligned + unchanged == 0)
        text = tr("Auto Align Clips: nothing was moved.");
    else if (aligned == 0)
        text = tr("Auto Align Clips: the clips are already in sync.");
    else
        text = tr("Auto Align Clips: %1 clip(s) moved.").arg(aligned);
    if (problems > 0) {
        for (const autoalign::Entry& e : std::as_const(result.entries)) {
            if (e.status != autoalign::Status::Aligned && e.status != autoalign::Status::Unchanged) {
                text += QLatin1Char(' ')
                    + tr("%1: not moved, %2.")
                          .arg(e.name.isEmpty() ? QString::number(e.clip) : e.name, e.reason);
            }
        }
    }
    say(window, text);
}

} // namespace kk
