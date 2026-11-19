// SPDX-License-Identifier: GPL-3.0-or-later
// KinoklipStyle tests: sizes come from the style tokens, pill switches draw the dot where the
// state says, every standard control draws without a crash in a dark and a light palette.
#include "app/KinoklipStyle.h"
#include "app/PageBar.h"
#include "ui/audio/FaderWidget.h"
#include "ui/common/PanelFrame.h"
#include "ui/common/StyleTokens.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOptionSlider>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace kk;
namespace tk = kk::style;

namespace {
QPalette darkPalette()
{
    QPalette p;
    p.setColor(QPalette::Window, QColor(40, 38, 46));
    p.setColor(QPalette::WindowText, QColor(214, 213, 218));
    p.setColor(QPalette::Base, QColor(31, 30, 35));
    p.setColor(QPalette::Button, QColor(52, 50, 58));
    p.setColor(QPalette::ButtonText, QColor(214, 213, 218));
    p.setColor(QPalette::Text, QColor(214, 213, 218));
    p.setColor(QPalette::Highlight, QColor(190, 56, 52));
    p.setColor(QPalette::HighlightedText, Qt::white);
    return p;
}
QPalette lightPalette()
{
    QPalette p;
    p.setColor(QPalette::Window, QColor(239, 240, 241));
    p.setColor(QPalette::WindowText, QColor(35, 38, 41));
    p.setColor(QPalette::Base, Qt::white);
    p.setColor(QPalette::Button, QColor(252, 252, 252));
    p.setColor(QPalette::ButtonText, QColor(35, 38, 41));
    p.setColor(QPalette::Text, QColor(35, 38, 41));
    p.setColor(QPalette::Highlight, QColor(61, 174, 233));
    p.setColor(QPalette::HighlightedText, Qt::white);
    return p;
}
} // namespace

class TestKinoklipStyle : public QObject
{
    Q_OBJECT
private:
    QStyle* previous_ = nullptr;
private slots:
    void initTestCase()
    {
        QApplication::setStyle(new KinoklipStyle);
    }

    void name_and_fusion_base()
    {
        QCOMPARE(QApplication::style()->objectName(), KinoklipStyle::key());
    }

    void metrics_come_from_tokens()
    {
        QStyle* s = QApplication::style();
        QCOMPARE(s->pixelMetric(QStyle::PM_ScrollBarExtent), tk::kScrollBar);
        QCOMPARE(s->pixelMetric(QStyle::PM_SliderLength), tk::kHandleHit);
        QCOMPARE(s->pixelMetric(QStyle::PM_IndicatorWidth), tk::kIndicator);
        QPushButton button(QStringLiteral("Apply"));
        QVERIFY(button.sizeHint().height() >= tk::kControlHeight);
        QComboBox combo;
        combo.addItem(QStringLiteral("One"));
        QVERIFY(combo.sizeHint().height() >= tk::kControlHeight);
        QSpinBox spin;
        QVERIFY(spin.sizeHint().height() >= tk::kControlHeight);
        QLineEdit edit;
        QVERIFY(edit.sizeHint().height() >= tk::kControlHeight);
        QCheckBox toggle(QStringLiteral("x"));
        tk::setToggle(&toggle);
        QStyleOptionButton opt;
        opt.initFrom(&toggle);
        QCOMPARE(s->pixelMetric(QStyle::PM_IndicatorWidth, &opt, &toggle), tk::kToggleWidth);
    }

    void active_and_chrome_sizes_follow_the_style()
    {
        QVERIFY(tk::kinoklipActive());
        PanelFrame panel(QStringLiteral("Inspector"));
        QCOMPARE(panel.findChild<QWidget*>(QStringLiteral("PanelTitleBar"))->maximumHeight(), tk::kPanelTitleHeight);
        PageBar bar;
        QAction page(QStringLiteral("Edit"));
        page.setCheckable(true);
        bar.addPageAction(&page);
        QCOMPARE(bar.maximumHeight(), tk::kPageBarHeight);
        // Another style: the bar and the title bar go back to the old sizes.
        QApplication::setStyle(QStringLiteral("Fusion"));
        QVERIFY(!tk::kinoklipActive());
        QVERIFY(panel.findChild<QWidget*>(QStringLiteral("PanelTitleBar"))->maximumHeight() != tk::kPanelTitleHeight);
        QVERIFY(bar.maximumHeight() != tk::kPageBarHeight);
        QApplication::setStyle(new KinoklipStyle);
        QCOMPARE(bar.maximumHeight(), tk::kPageBarHeight);
    }

    void tab_buttons_draw_an_underline_when_checked()
    {
        QToolButton b;
        b.setText(QStringLiteral("Primaries"));
        b.setCheckable(true);
        b.setProperty("kkTab", true);
        b.setChecked(true);
        b.resize(90, 24);
        const QImage on = b.grab().toImage();
        const QColor mark = tk::colors(b.palette()).mark;
        const QColor px = on.pixelColor(45, 23);
        QVERIFY2(qAbs(px.red() - mark.red()) < 8 && qAbs(px.blue() - mark.blue()) < 8, qPrintable(px.name()));
        b.setChecked(false);
        QVERIFY(b.grab().toImage().pixelColor(45, 23) != px);
    }

    void filled_slider_groove_reaches_the_handle()
    {
        QSlider s(Qt::Horizontal);
        s.setRange(0, 100);
        s.setValue(50);
        s.resize(200, 20);
        const QImage plain = s.grab().toImage();
        s.setProperty("kkFilled", true);
        const QImage filled = s.grab().toImage();
        QVERIFY(plain != filled);
        const QColor mark = tk::colors(s.palette()).mark;
        bool found = false;
        for (int y = 0; y < filled.height() && !found; ++y) {
            const QColor px = filled.pixelColor(40, y);
            found = qAbs(px.red() - mark.red()) < 8 && qAbs(px.green() - mark.green()) < 8
                && qAbs(px.blue() - mark.blue()) < 8;
        }
        QVERIFY(found);
        found = false; // right of the handle the groove stays plain
        for (int y = 0; y < filled.height(); ++y) {
            const QColor px = filled.pixelColor(170, y);
            found = found || (qAbs(px.red() - mark.red()) < 8 && qAbs(px.blue() - mark.blue()) < 8);
        }
        QVERIFY(!found);
    }

    void custom_painted_controls_take_their_look_from_the_style()
    {
        FaderWidget fader;
        fader.resize(60, 200);
        const QImage kk = fader.grab().toImage();
        QApplication::setStyle(QStringLiteral("Fusion"));
        const QImage other = fader.grab().toImage();
        QApplication::setStyle(new KinoklipStyle);
        QVERIFY(kk != other);
        QCOMPARE(fader.grab().toImage(), kk); // switching back restores the Kinoklip drawing
    }

    void scrollbar_has_no_arrows()
    {
        QScrollBar bar(Qt::Vertical);
        bar.setRange(0, 100);
        bar.setPageStep(10);
        bar.resize(tk::kScrollBar, 200);
        QStyleOptionSlider opt;
        opt.initFrom(&bar);
        opt.orientation = Qt::Vertical;
        opt.minimum = 0; opt.maximum = 100; opt.pageStep = 10; opt.sliderPosition = 0;
        QStyle* s = QApplication::style();
        QVERIFY(s->subControlRect(QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarAddLine, &bar).isEmpty());
        const QRect slider = s->subControlRect(QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarSlider, &bar);
        QCOMPARE(slider.top(), 0);
        QVERIFY(slider.height() >= tk::kScrollMinSlider);
    }

    void toggle_draws_dot_by_state()
    {
        QApplication::setPalette(darkPalette());
        QCheckBox box;
        tk::setToggle(&box);
        box.resize(tk::kToggleWidth + 8, tk::kToggleHeight + 8);
        const tk::Colors c = tk::colors(box.palette());
        box.setChecked(true);
        QImage on = box.grab().toImage();
        box.setChecked(false);
        QImage off = box.grab().toImage();
        const int y = on.height() / 2;
        const auto count = [&](const QImage& img, int x0, int x1) {
            int n = 0;
            for (int x = x0; x < x1; ++x)
                n += qAbs(img.pixelColor(x, y).red() - c.accent.red()) < 6 && qAbs(img.pixelColor(x, y).green() - c.accent.green()) < 6;
            return n;
        };
        QVERIFY(count(on, on.width() / 2, on.width()) > 3);   // dot on the right = on
        QCOMPARE(count(off, 0, off.width()), 0);              // off: no accent at all
    }

    void draws_every_control_in_both_palettes_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("dark") << true;
        QTest::newRow("light") << false;
    }
    void draws_every_control_in_both_palettes()
    {
        QFETCH(bool, dark);
        QApplication::setPalette(dark ? darkPalette() : lightPalette());
        QWidget w;
        auto* lay = new QVBoxLayout(&w);
        lay->addWidget(new QPushButton(QStringLiteral("Button")));
        auto* flat = new QPushButton(QStringLiteral("On"));
        flat->setCheckable(true);
        flat->setChecked(true);
        lay->addWidget(flat);
        auto* tool = new QToolButton;
        tool->setText(QStringLiteral("T"));
        tool->setCheckable(true);
        tool->setChecked(true);
        lay->addWidget(tool);
        lay->addWidget(new QCheckBox(QStringLiteral("Check")));
        auto* tri = new QCheckBox(QStringLiteral("Tri"));
        tri->setTristate(true);
        tri->setCheckState(Qt::PartiallyChecked);
        lay->addWidget(tri);
        auto* sw = new QCheckBox(QStringLiteral("Switch"));
        tk::setToggle(sw);
        sw->setChecked(true);
        lay->addWidget(sw);
        auto* radio = new QRadioButton(QStringLiteral("Radio"));
        radio->setChecked(true);
        lay->addWidget(radio);
        auto* slider = new QSlider(Qt::Horizontal);
        lay->addWidget(slider);
        auto* vslider = new QSlider(Qt::Vertical);
        vslider->setFixedHeight(60);
        lay->addWidget(vslider);
        lay->addWidget(new QSpinBox);
        lay->addWidget(new QDoubleSpinBox);
        lay->addWidget(new QLineEdit(QStringLiteral("text")));
        auto* combo = new QComboBox;
        combo->addItems({QStringLiteral("A"), QStringLiteral("B")});
        lay->addWidget(combo);
        auto* editable = new QComboBox;
        editable->setEditable(true);
        lay->addWidget(editable);
        auto* tabs = new QTabWidget;
        tabs->addTab(new QWidget, QStringLiteral("One"));
        tabs->addTab(new QWidget, QStringLiteral("Two"));
        lay->addWidget(tabs);
        auto* bar = new QProgressBar;
        bar->setValue(40);
        lay->addWidget(bar);
        auto* busy = new QProgressBar;
        busy->setRange(0, 0);
        lay->addWidget(busy);
        auto* group = new QGroupBox(QStringLiteral("Group"));
        group->setCheckable(true);
        group->setLayout(new QVBoxLayout);
        group->layout()->addWidget(new QLabel(QStringLiteral("inside")));
        lay->addWidget(group);
        auto* tree = new QTreeWidget;
        auto* top = new QTreeWidgetItem(tree, {QStringLiteral("a")});
        new QTreeWidgetItem(top, {QStringLiteral("b")});
        top->setExpanded(true);
        lay->addWidget(tree);
        auto* table = new QTableWidget(3, 3);
        lay->addWidget(table);
        auto* list = new QListWidget;
        list->addItems({QStringLiteral("x"), QStringLiteral("y")});
        lay->addWidget(list);
        w.resize(420, 1100);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(!w.grab().isNull());
        QMenu menu;
        menu.addAction(QStringLiteral("Plain"));
        auto* checked = menu.addAction(QStringLiteral("Checked\tCtrl+K"));
        checked->setCheckable(true);
        checked->setChecked(true);
        menu.addSeparator();
        menu.addMenu(QStringLiteral("Sub"))->addAction(QStringLiteral("In"));
        menu.addAction(QStringLiteral("Off"))->setEnabled(false);
        QVERIFY(!menu.grab().isNull());
        QMenuBar mb;
        mb.addMenu(QStringLiteral("File"));
        mb.resize(200, 24);
        QVERIFY(!mb.grab().isNull());
        combo->showPopup();
        combo->hidePopup();
    }
};

QTEST_MAIN(TestKinoklipStyle)
#include "test_kinoklipstyle.moc"
