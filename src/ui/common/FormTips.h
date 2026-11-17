// SPDX-License-Identifier: GPL-3.0-or-later
// FormTips — every option of a settings dialog explains itself on hover, and so does its
// label (the owner's rule for Preferences and Project Settings).
#pragma once

class QWidget;

namespace kk::formtips {

/// For every QFormLayout below @p root: a row's label gets the tooltip of its field (the
/// field itself, else the first widget inside a field row that has one) unless the label
/// already has its own. Labels of rows without a tooltip stay as they are.
void mirrorToLabels(QWidget* root);

} // namespace kk::formtips
