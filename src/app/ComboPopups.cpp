// SPDX-License-Identifier: GPL-3.0-or-later
// ComboPopups implementation: an application filter that widens combo box lists to their entries.
#include "app/ComboPopups.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QPointer>
#include <QScreen>
#include <QScrollBar>
#include <QWidget>

#include <algorithm>

namespace kk::combopopups {

namespace {

/// Room left of the text by the item delegates' own margins (a check mark or icon column and
/// the text padding are part of sizeHintForColumn(); this is the rest of the row).
constexpr int kRowPadding = 6;

/// The view inside a combo list window (the container is Qt's private QComboBoxPrivateContainer).
QAbstractItemView* listView(const QWidget* window)
{
    if (!window || !window->isWindow() || window->windowType() != Qt::Popup
        || qstrcmp(window->metaObject()->className(), "QComboBoxPrivateContainer") != 0)
        return nullptr;
    return window->findChild<QAbstractItemView*>();
}

class ComboFilter : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Show && watched->isWidgetType()) {
            auto* w = static_cast<QWidget*>(watched);
            if (listView(w)) {
                const int needed = neededWidth(w);
                if (needed > w->width()) {
                    QRect g = w->geometry();
                    g.setWidth(needed);
                    // Keep the list on its screen: shift left rather than run off the right edge.
                    if (const QScreen* screen = w->screen()) {
                        const QRect avail = screen->availableGeometry();
                        if (g.right() > avail.right())
                            g.moveRight(avail.right());
                        g.moveLeft(std::max(g.left(), avail.left()));
                    }
                    w->setGeometry(g);
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

QPointer<ComboFilter> g_filter;

} // namespace

int neededWidth(const QWidget* listWindow)
{
    const QAbstractItemView* view = listView(listWindow);
    if (!view)
        return 0;
    int width = view->sizeHintForColumn(0) + kRowPadding + 2 * view->frameWidth();
    if (const QScrollBar* bar = view->verticalScrollBar(); bar && bar->maximum() > bar->minimum())
        width += bar->sizeHint().width();
    // The container's own frame and margins around the view.
    width += listWindow->width() - view->width();
    return width;
}

void install(QApplication& app)
{
    if (g_filter)
        return;
    g_filter = new ComboFilter(&app);
    app.installEventFilter(g_filter);
}

} // namespace kk::combopopups
