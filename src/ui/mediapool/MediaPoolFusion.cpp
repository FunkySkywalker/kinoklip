// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel, Fusion part: New Fusion Composition and New Fusion Clip (from the selected
// media files). Fusion clips made from timeline clips are in TimelineCompound.cpp.
#include "core/MediaPool.h"
#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QAction>
#include <QMenu>

namespace kk {

namespace {

const char* const kNewFusionComposition = "mediapool.new_fusion_composition";
const char* const kNewFusionClip = "mediapool.new_fusion_clip";

} // namespace

QStringList MediaPoolPanel::fusionSourceIds(const QStringList& ids) const
{
    QStringList out;
    for (const QString& id : ids) {
        const MediaItem* item = m_ctx.project->mediaPool()->item(id);
        if (item && item->kind == MediaItemKind::File
            && (item->info.hasVideo || item->info.hasAudio))
            out.append(id);
    }
    return out;
}

quint64 MediaPoolPanel::createFusionClip(const QStringList& ids)
{
    const QStringList usable = fusionSourceIds(ids);
    const QString bin = currentBin();
    const quint64 uid = m_ctx.project->createFusionComposition({}, bin, usable);
    if (uid)
        showImportedItems({m_ctx.project->timelineItemId(uid)}, bin);
    return uid;
}

void MediaPoolPanel::addFusionEntries(QMenu& menu, const QStringList& ids)
{
    QAction* blank = menu.addAction(icon(QStringLiteral("fusion_clip")),
        tr("New Fusion Composition"), this, [this] { createFusionClip({}); });
    blank->setObjectName(QLatin1String(kNewFusionComposition));
    if (!fusionSourceIds(ids).isEmpty()) {
        QAction* fromMedia = menu.addAction(icon(QStringLiteral("fusion_clip")),
            tr("New Fusion Clip"), this, [this, ids] { createFusionClip(ids); });
        fromMedia->setObjectName(QLatin1String(kNewFusionClip));
    }
}

void MediaPoolPanel::connectFusionActions()
{
    if (!m_ctx.actions)
        return;
    auto on = [this](const char* id, const std::function<void()>& run) {
        if (!m_ctx.actions->contains(QLatin1String(id)))
            return;
        connect(m_ctx.actions->action(QLatin1String(id)), &QAction::triggered, this, [this, run] {
            if (isVisible())
                run();
        });
    };
    on(kNewFusionComposition, [this] { createFusionClip({}); });
    on(kNewFusionClip, [this] { createFusionClip(selectedMediaIds()); });
}

} // namespace kk
