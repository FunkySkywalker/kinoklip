// SPDX-License-Identifier: GPL-3.0-or-later
// Page panel layout (docs/behavior-spec.md section 156, issue #17): toggling a panel never makes a
// page wider than its window, hiding a panel gives its width to the neighbours, showing it
// restores its width, and dragging a splitter handle afterwards still resizes the panels.
#include "app/MainWindow.h"
#include "app/pages/AudioPage.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Page.h"
#include "ui/common/StandardActions.h"

#include <QAction>
#include <QApplication>
#include <QMouseEvent>
#include <QSplitter>
#include <QTest>

#include <memory>

using namespace kk;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    bool showPage(const char* action, int width)
    {
        window->resize(width, 1000);
        window->show();
        if (!QTest::qWaitForWindowExposed(window.get()))
            return false;
        actions.action(QLatin1String(action))->trigger();
        QCoreApplication::processEvents();
        return true;
    }
};

void drag(QWidget* w, QPoint from, QPoint to)
{
    auto send = [w](QEvent::Type type, QPoint p, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent e(
            type, QPointF(p), w->mapToGlobal(QPointF(p)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(w, &e);
    };
    send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 5; ++i)
        send(QEvent::MouseMove, from + (to - from) * i / 5, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

/// Empty when the page fits its window and every visible child of every splitter lies inside it.
QString problems(Page* page, QWidget* window)
{
    QString out;
    if (page->width() > window->width())
        out += QStringLiteral("page %1 wider than window %2; ").arg(page->width()).arg(window->width());
    if (page->minimumSizeHint().width() > window->width())
        out += QStringLiteral("page min width %1 > window %2; ")
                   .arg(page->minimumSizeHint().width())
                   .arg(window->width());
    for (QSplitter* s : page->findChildren<QSplitter*>()) {
        if (!s->isVisible())
            continue;
        int used = 0;
        for (int i = 0; i < s->count(); ++i)
            if (!s->widget(i)->isHidden())
                used += s->orientation() == Qt::Horizontal ? s->widget(i)->width()
                                                           : s->widget(i)->height();
        const int total = s->orientation() == Qt::Horizontal ? s->width() : s->height();
        if (used > total)
            out += QStringLiteral("splitter(%1 kids, %2 min) children %3 > %4; ")
                       .arg(s->count())
                       .arg(s->minimumSizeHint().width())
                       .arg(used)
                       .arg(total);
    }
    return out;
}

QSplitter* outerSplitter(Page* page)
{
    for (QSplitter* s : page->findChildren<QSplitter*>())
        if (s->orientation() == Qt::Horizontal && s->count() == 3 && s->widget(1)->findChild<QSplitter*>())
            return s;
    return nullptr;
}

} // namespace

class TestPagePanels : public QObject
{
    Q_OBJECT
private slots:
    void audioPageToggleKeepsLayoutInsideWindow()
    {
        Fixture f;
        QVERIFY(f.showPage("view.page_audio", 1800));
        AudioPage* page = f.window->audioPage();
        QCOMPARE(problems(page, f.window.get()), QString());
        for (PanelId id : {PanelId::Effects, PanelId::Inspector, PanelId::Mixer}) {
            for (int round = 0; round < 2; ++round) {
                const bool was = page->isPanelVisible(id);
                page->setPanelVisible(id, !was);
                QCoreApplication::processEvents();
                QCOMPARE(problems(page, f.window.get()), QString());
                page->setPanelVisible(id, was);
                QCoreApplication::processEvents();
                QCOMPARE(problems(page, f.window.get()), QString());
            }
        }
        page->setPanelVisible(PanelId::Effects, true);
        page->setPanelVisible(PanelId::Inspector, true);
        QCoreApplication::processEvents();
        QCOMPARE(problems(page, f.window.get()), QString());
    }

    void audioPageHandlesDragAfterToggle()
    {
        Fixture f;
        QVERIFY(f.showPage("view.page_audio", 1800));
        AudioPage* page = f.window->audioPage();
        page->setPanelVisible(PanelId::Effects, true);
        page->setPanelVisible(PanelId::Inspector, true);
        QCoreApplication::processEvents();
        for (QSplitter* s : page->findChildren<QSplitter*>()) {
            if (!s->isVisible() || s->orientation() != Qt::Horizontal)
                continue;
            for (int h = 1; h < s->count(); ++h) {
                if (s->widget(h - 1)->isHidden() || s->widget(h)->isHidden())
                    continue;
                QSplitterHandle* handle = s->handle(h);
                const int before = s->widget(h - 1)->width();
                const int dx = s->widget(h)->width() > before ? 25 : -25;
                const QPoint c = handle->rect().center();
                drag(handle, c, c + QPoint(dx, 0));
                QCoreApplication::processEvents();
                QVERIFY2(s->widget(h - 1)->width() != before,
                    qPrintable(QStringLiteral("handle %1 of a %2-panel splitter did not move")
                                   .arg(h)
                                   .arg(s->count())));
                QCOMPARE(problems(page, f.window.get()), QString());
            }
        }
    }

    void panelWidthIsRestoredAndFreedWidthGoesToNeighbours()
    {
        Fixture f;
        QVERIFY(f.showPage("view.page_audio", 1800));
        AudioPage* page = f.window->audioPage();
        page->setPanelVisible(PanelId::Inspector, true);
        QCoreApplication::processEvents();
        QSplitter* outer = outerSplitter(page);
        QVERIFY(outer);
        const int centreShown = outer->widget(1)->width();
        const int inspectorShown = outer->widget(2)->width();
        const QPoint c = outer->handle(2)->rect().center();
        drag(outer->handle(2), c, c + QPoint(-60, 0));
        QCoreApplication::processEvents();
        const int resized = outer->widget(2)->width();
        QVERIFY(resized > inspectorShown + 30);
        page->setPanelVisible(PanelId::Inspector, false);
        QCoreApplication::processEvents();
        QVERIFY(outer->widget(1)->width() > centreShown);
        page->setPanelVisible(PanelId::Inspector, true);
        QCoreApplication::processEvents();
        QVERIFY(qAbs(outer->widget(2)->width() - resized) <= 2);
    }
};

QTEST_MAIN(TestPagePanels)
#include "test_pagepanels.moc"
