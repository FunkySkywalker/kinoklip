// SPDX-License-Identifier: GPL-3.0-or-later
// AutoAlignUi — Clip > Auto Align Clips and the timeline clip menu entry: runs AutoAlign on the
// selected clips behind a progress dialog and reports in the status bar.
#pragma once

#include "ui/timeline/AutoAlign.h"

class QWidget;

namespace kk {

struct AppContext;

/// Aligns the clips selected in the timeline of @p ctx to the reference (the selected clip on
/// the lowest video track). One undo step; a clip with no clear match stays where it is and the
/// status bar of @p window says so.
void runAutoAlign(AppContext& ctx, QWidget* window, autoalign::Method method);

} // namespace kk
