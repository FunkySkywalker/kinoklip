// SPDX-License-Identifier: GPL-3.0-or-later
// Every option of Preferences (all four registered pages) and of Project Settings explains
// itself on hover: each input widget has a tooltip, and so has the label of its form row.
//
// Rule for what counts as an input: combo boxes, check boxes, radio buttons, push and tool
// buttons, spin boxes, sliders, line edits and key sequence fields that were not hidden
// with hide(). Disabled widgets count (their tooltip still shows). Widgets inside another
// input (the line edit of a spin box or combo box, a tab bar's scroll arrows) are parts of
// it and are skipped.
#include "app/SettingsTransferDialog.h"
#include "app/Theme.h"
#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/AiModelDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/dialogs/ProjectSettingsDialog.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSlider>
#include <QStandardPaths>
#include <QTabBar>
#include <QTest>

using namespace kk;

namespace {

bool isInput(const QWidget* w)
{
    return qobject_cast<const QAbstractButton*>(w) || qobject_cast<const QComboBox*>(w)
        || qobject_cast<const QAbstractSpinBox*>(w) || qobject_cast<const QSlider*>(w)
        || qobject_cast<const QLineEdit*>(w) || qobject_cast<const QKeySequenceEdit*>(w);
}

bool isPartOfAnotherInput(const QWidget* w, const QWidget* root)
{
    for (const QWidget* p = w->parentWidget(); p && p != root; p = p->parentWidget())
        if (qobject_cast<const QAbstractSpinBox*>(p) || qobject_cast<const QComboBox*>(p)
            || qobject_cast<const QTabBar*>(p) || qobject_cast<const QKeySequenceEdit*>(p))
            return true;
    return false;
}

/// Names of the inputs and form labels of @p dialog that have no tooltip; counts the inputs.
QStringList missingTips(QDialog& dialog, int& inputs)
{
    QStringList missing;
    for (QWidget* w : dialog.findChildren<QWidget*>()) {
        if (!isInput(w) || w->isHidden() || isPartOfAnotherInput(w, &dialog))
            continue;
        ++inputs;
        if (w->toolTip().isEmpty())
            missing << QStringLiteral("%1 \"%2\" (%3)")
                           .arg(QString::fromLatin1(w->metaObject()->className()),
                               w->objectName(), w->parentWidget() ? w->parentWidget()->objectName() : QString());
    }
    for (QFormLayout* form : dialog.findChildren<QFormLayout*>()) {
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem* item = form->itemAt(row, QFormLayout::LabelRole);
            auto* label = item ? qobject_cast<QLabel*>(item->widget()) : nullptr;
            if (label && !label->text().isEmpty() && label->toolTip().isEmpty())
                missing << QStringLiteral("label \"%1\"").arg(label->text());
        }
    }
    return missing;
}

/// Steps through every section of the dialog's list so each one is built and current once.
void walkSections(QDialog& dialog, const char* listName)
{
    auto* list = dialog.findChild<QListWidget*>(QLatin1String(listName));
    QVERIFY(list);
    for (int row = 0; row < list->count(); ++row)
        list->setCurrentRow(row);
}

class TestTooltips : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void preferencesEveryOptionHasATooltip()
    {
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        theme::registerActions(actions);
        SettingsPageRegistry pages;
        registerStandardSettingsPages(pages, &actions);
        theme::registerSettingsPage(pages, actions);
        registerSettingsTransferPage(pages);
        registerAiModelsPage(pages);
        PreferencesDialog dialog(&pages);
        int inputs = 0;
        // The System and User tabs show different sections; walk both.
        for (const char* group : {"system", "user"}) {
            auto* tabs = dialog.findChild<QTabBar*>(QStringLiteral("PreferencesGroups"));
            QVERIFY(tabs);
            tabs->setCurrentIndex(QByteArray(group) == "system" ? 0 : 1);
            walkSections(dialog, "PreferencesSections");
        }
        const QStringList missing = missingTips(dialog, inputs);
        QVERIFY2(inputs > 30, qPrintable(QString::number(inputs)));
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QLatin1Char('\n'))));
    }

    void projectSettingsEveryOptionHasATooltip()
    {
        Project project;
        ProjectSettingsDialog dialog(&project);
        walkSections(dialog, "ProjectSettingsSections");
        int inputs = 0;
        const QStringList missing = missingTips(dialog, inputs);
        QVERIFY2(inputs > 15, qPrintable(QString::number(inputs)));
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QLatin1Char('\n'))));
    }
};

} // namespace

QTEST_MAIN(TestTooltips)
#include "test_tooltips.moc"
