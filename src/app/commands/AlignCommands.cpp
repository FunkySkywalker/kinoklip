// SPDX-License-Identifier: GPL-3.0-or-later
// Command: clip.align (spec section 153): Auto Align Clips from the timeline clip menu, for the
// command line and the MCP server. ui/timeline/AutoAlign does the work (the same code the menu
// runs); this file reads the arguments and builds the JSON result. The analysis (decoding the
// audio) runs in the calling thread and reports through the progress callback.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/timeline/AutoAlign.h"
#include "ui/timeline/TimelineEditUtil.h"

namespace kk::commands {

using timelineedit::runEdit;

namespace {

CommandResult runAlign(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    QString error;
    autoalign::Request request;
    request.clips = parseIds(args.strings(QStringLiteral("clip")), &error);
    if (!error.isEmpty())
        return fail("bad_value", error);
    if (args.has(QStringLiteral("reference")))
        request.reference = quint64(args.integer(QStringLiteral("reference")));
    request.method = args.string(QStringLiteral("method"), QStringLiteral("waveform"))
            == QLatin1String("timecode")
        ? autoalign::Method::Timecode
        : autoalign::Method::Waveform;
    request.muteReference = args.boolean(QStringLiteral("mute-reference"));

    QString code;
    const auto job = autoalign::prepare(
        project.timeline()->data(), *project.mediaPool(), request, &error, &code);
    if (!job)
        return fail(code.toUtf8().constData(), error);
    if (ctx.progress)
        ctx.progress(0.0, QStringLiteral("analysing the audio"));
    autoalign::Result result = autoalign::analyse(*job, nullptr, [&](int done, int total) {
        if (ctx.progress && total > 0)
            ctx.progress(double(done) / total, QStringLiteral("analysing the audio"));
    });
    runEdit(&project, QStringLiteral("Auto Align Clips"),
        [&](TimelineData& data) { return autoalign::apply(data, *job, result); });

    const int aligned = result.count(autoalign::Status::Aligned);
    const int unchanged = result.count(autoalign::Status::Unchanged);
    if (aligned + unchanged == 0) {
        const bool unreliable = result.count(autoalign::Status::Unreliable) > 0;
        return fail(unreliable ? "unreliable" : "cannot_align",
            QStringLiteral("no clip was moved\n") + autoalign::summary(result));
    }
    return CommandResult::success(autoalign::toJson(*job, result), autoalign::summary(result));
}

} // namespace

void registerAlignCommands(CommandRegistry& registry)
{
    registry.add({{QStringLiteral("clip.align"),
                      QStringLiteral(
                          "Auto Align Clips: move clips that are already on the timeline so "
                          "their sound (--method waveform) or start timecode (--method timecode) "
                          "matches a reference clip, for example an external audio recording "
                          "against the camera clip. The reference never moves; the others move "
                          "with the clips linked to them. A clip with no clear match (result "
                          "status \"unreliable\"), one that would overwrite other clips, sit on "
                          "a locked track or start before the timeline is left where it is and "
                          "reported. One undo step."),
                      {{QStringLiteral("clip"), ParamType::StringList,
                           QStringLiteral("the clips to line up with the reference (repeat the "
                                          "option, or separate ids by commas)"),
                           true, {}, {}},
                          {QStringLiteral("reference"), ParamType::Integer,
                              QStringLiteral("id of the reference clip, which does not move "
                                             "(default: the --clip on the lowest video track, "
                                             "else the first one)"),
                              false, {}, {}},
                          {QStringLiteral("method"), ParamType::String,
                              QStringLiteral("match the sound or the start timecode"), false,
                              QStringLiteral("waveform"),
                              {QStringLiteral("waveform"), QStringLiteral("timecode")}},
                          {QStringLiteral("mute-reference"), ParamType::Boolean,
                              QStringLiteral("silence the reference's own audio after aligning "
                                             "(the camera sound)"),
                              false, {}, {}}},
                      ProjectUse::Modify, true},
        runAlign});
}

} // namespace kk::commands
