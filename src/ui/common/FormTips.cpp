// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/FormTips.h"

#include <QFormLayout>
#include <QLabel>
#include <QLayout>
#include <QString>
#include <QWidget>

namespace kk::formtips {

namespace {

QString tipOf(QLayoutItem* item)
{
    if (!item)
        return {};
    if (QWidget* widget = item->widget()) {
        if (!widget->toolTip().isEmpty())
            return widget->toolTip();
        for (const QWidget* child : widget->findChildren<QWidget*>())
            if (!child->toolTip().isEmpty())
                return child->toolTip();
        return {};
    }
    if (QLayout* layout = item->layout()) {
        for (int i = 0; i < layout->count(); ++i) {
            const QString tip = tipOf(layout->itemAt(i));
            if (!tip.isEmpty())
                return tip;
        }
    }
    return {};
}

} // namespace

void mirrorToLabels(QWidget* root)
{
    for (QFormLayout* form : root->findChildren<QFormLayout*>()) {
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem* labelItem = form->itemAt(row, QFormLayout::LabelRole);
            QWidget* label = labelItem ? labelItem->widget() : nullptr;
            if (!label || !label->toolTip().isEmpty())
                continue;
            const QString tip = tipOf(form->itemAt(row, QFormLayout::FieldRole));
            if (!tip.isEmpty())
                label->setToolTip(tip);
        }
    }
}

} // namespace kk::formtips
