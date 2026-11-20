// SPDX-License-Identifier: GPL-3.0-or-later
// Whole-window responsiveness with a large project open (about 1,500 clips on 8 + 8 tracks,
// tests/timeline/LargeProject.h): time to the first exposed window, switching through every
// page (first visit and revisit), selecting clips one after another (Inspector and viewer
// follow), moving the playhead as a scrub does, and zooming the Edit timeline.
// Every number is printed (`ctest -V -R test_interactionperf`); the asserts are generous
// ceilings that only catch gross regressions on a loaded machine.
#include "../timeline/LargeProject.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/timeline/TimelineWidget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

#include <algorithm>
#include <vector>

using namespace kk;

namespace {

double medianOf(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

/// Lets queued work (layout, deferred updates, zero timers) finish and paints now, as the
/// event loop does before the next input event.
void settle(QWidget& w)
{
    QCoreApplication::processEvents();
    w.repaint();
    QCoreApplication::processEvents();
}

} // namespace

class TestInteractionPerf : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_interactionperf"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void interactions()
    {
        QElapsedTimer total;
        total.start();
        Project project;
        test::buildLargeProject(project);
        SelectionModel selection;
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        PlaybackEngine::Options options;
        options.noAudio = true;
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.playback = &engine;
        MainWindowOptions windowOptions;
        windowOptions.autosave = false;

        QElapsedTimer t;
        t.start();
        auto* window = new MainWindow(ctx, windowOptions);
        const double construct = t.nsecsElapsed() / 1e6;
        window->resize(1600, 900);
        window->setCurrentPage(QStringLiteral("edit"));
        t.restart();
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        settle(*window);
        const double firstWindow = t.nsecsElapsed() / 1e6;
        QTest::qWait(300);
        qInfo("startup: MainWindow constructed %.1f ms, shown and first paint %.1f ms", construct,
            firstWindow);

        // Page switches.
        const QStringList pages {QStringLiteral("media"), QStringLiteral("cut"),
            QStringLiteral("edit"), QStringLiteral("compositing"), QStringLiteral("color"),
            QStringLiteral("audio"), QStringLiteral("deliver")};
        for (int round = 0; round < 2; ++round) {
            QStringList line;
            double sum = 0;
            for (const QString& p : pages) {
                t.restart();
                window->setCurrentPage(p);
                settle(*window);
                const double ms = t.nsecsElapsed() / 1e6;
                sum += ms;
                line << QStringLiteral("%1 %2").arg(p).arg(ms, 0, 'f', 1);
            }
            qInfo("pages %s: %s ms (sum %.1f)", round == 0 ? "first visit" : "revisit",
                qPrintable(line.join(QStringLiteral(", "))), sum);
            if (round == 1)
                QVERIFY2(sum < 5000, "page switching is far too slow");
        }

        // Select clips one after another (Inspector rebuild, viewer, keyframes panel).
        window->setCurrentPage(QStringLiteral("edit"));
        settle(*window);
        const TimelineData& tl = project.timeline()->data();
        QVector<quint64> ids;
        for (const auto& track : tl.videoTracks)
            for (const auto& c : track.clips)
                ids.push_back(c.id);
        QVERIFY(ids.size() > 100);
        std::vector<double> sel;
        for (int i = 0; i < 40; ++i) {
            t.restart();
            selection.selectClip(ids[(i * 37) % ids.size()]);
            settle(*window);
            sel.push_back(t.nsecsElapsed() / 1e6);
        }
        const double selMedian = medianOf(sel);
        qInfo("select clip (Edit page): median %.1f ms, max %.1f ms", selMedian,
            *std::max_element(sel.begin(), sel.end()));
        QVERIFY2(selMedian < 500, "selecting a clip is far too slow");

        // Playhead scrub: the whole window follows the playhead.
        std::vector<double> scrub;
        for (int i = 0; i < 100; ++i) {
            t.restart();
            project.timeline()->setPlayhead((i * 97) % 90000);
            settle(*window);
            scrub.push_back(t.nsecsElapsed() / 1e6);
        }
        const double scrubMedian = medianOf(scrub);
        qInfo("playhead move: median %.1f ms, max %.1f ms", scrubMedian,
            *std::max_element(scrub.begin(), scrub.end()));
        QVERIFY2(scrubMedian < 300, "playhead moves are far too slow");

        // Timeline zoom.
        auto* widget = window->findChild<TimelineWidget*>();
        QVERIFY(widget);
        std::vector<double> zoom;
        double ppf = std::max(0.05, widget->pixelsPerFrame());
        for (int i = 0; i < 30; ++i) {
            t.restart();
            widget->setPixelsPerFrame(ppf * (i % 2 ? 0.8 : 1.25), widget->width() / 2.0);
            ppf = widget->pixelsPerFrame();
            settle(*window);
            zoom.push_back(t.nsecsElapsed() / 1e6);
        }
        qInfo("timeline zoom step: median %.1f ms, max %.1f ms", medianOf(zoom),
            *std::max_element(zoom.begin(), zoom.end()));
        QVERIFY2(medianOf(zoom) < 500, "timeline zoom is far too slow");

        delete window;
        qInfo("test total %.1f s", total.elapsed() / 1000.0);
    }
};

QTEST_MAIN(TestInteractionPerf)
#include "test_interactionperf.moc"
