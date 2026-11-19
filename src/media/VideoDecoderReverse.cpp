// SPDX-License-Identifier: GPL-3.0-or-later
// VideoDecoderReverse — the reverse cache of VideoDecoder (spec 143). Frames can only be decoded
// forward from a keyframe, so a request that moves backwards used to seek and decode a whole GOP
// for every single frame. When the requests walk backwards the decoder instead decodes one
// stretch [keyframe .. target] forward once, keeps its last frames (as VideoFrames, in whatever
// form the decoder hands them out, hardware frames downloaded) and answers the following requests
// from them. When the stretch is used up, the one before it is decoded the same way; the playback
// engine's decode-ahead (prefetchReverse()) does that in small slices before it is needed.
#include "core/MemoryBudget.h"
#include "media/MediaLog.h"
#include "media/VideoDecoderImpl.h"

#include <QElapsedTimer>

extern "C" {
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <functional>
#include <limits>

namespace kk::media {

using namespace detail;

namespace {

/// A request this far (or less) behind the last one still belongs to the same backward walk
/// (a J shuttle at 8x steps 8 frames, a scrub drag a few more).
constexpr int64_t kWalkStepUs = 2'000'000;
/// A request this far (or less) below the cache continues it; further away it starts a new one.
constexpr int64_t kGapUs = 500'000;
/// Memory of one stretch. The cache holds up to two stretches and the one being decoded below it
/// is a third: about 290 MB for 1080p, never fewer than kMinFrames per stretch.
constexpr int64_t kStretchBytes = 96LL << 20;
constexpr int64_t kMinStretchBytes = 32LL << 20;
constexpr size_t kMinFrames = 24;
constexpr size_t kMaxFrames = 120;

/// KINOKLIP_REVERSE_CACHE=off: the old behaviour (a seek for every backward frame), for
/// measurements and for finding out whether a problem comes from the cache.
bool reverseEnabled()
{
    static const bool on = qEnvironmentVariable("KINOKLIP_REVERSE_CACHE") != QLatin1String("off");
    return on;
}

} // namespace

size_t VideoDecoder::Impl::reverseFrameLimit() const
{
    // The size of a decoded picture in the stream's own format (NV12 / P010 downloads have the
    // same size as their planar relatives); 1.5 bytes a pixel when FFmpeg does not know it.
    int64_t frameBytes = av_image_get_buffer_size(
        AVPixelFormat(stream->codecpar->format), info.width, info.height, 32);
    if (frameBytes <= 0) {
        frameBytes = int64_t(info.width) * info.height * 3 / 2;
    }
    frameBytes = std::max<int64_t>(1, frameBytes);
    const int64_t budget = membudget::scaledBytes(kStretchBytes, kMinStretchBytes);
    return std::clamp<size_t>(size_t(budget / frameBytes), kMinFrames, kMaxFrames);
}

bool VideoDecoder::Impl::noteRequest(int64_t ts)
{
    if (lastRequestTs != AV_NOPTS_VALUE) {
        if (ts < lastRequestTs) {
            const int64_t step = av_rescale_q(kWalkStepUs, kMicrosecondsTb, stream->time_base);
            backSteps = lastRequestTs - ts <= step ? backSteps + 1 : 0;
        } else if (ts > lastRequestTs) {
            backSteps = 0;
        }
    }
    lastRequestTs = ts;
    return backSteps >= 1;
}

std::shared_ptr<const VideoFrame> VideoDecoder::Impl::reverseAt(int64_t ts) const
{
    if (revSkipNonReference && !skipNonReference) {
        return nullptr; // decoded in catch-up mode (frames missing), not the frames asked for
    }
    auto it = std::upper_bound(rev.begin(), rev.end(), ts,
        [](int64_t value, const CachedFrame& f) { return value < f.ts; });
    if (it == rev.begin()) {
        return nullptr;
    }
    --it;
    return ts < it->endTs ? it->video : nullptr;
}

void VideoDecoder::Impl::dropReverse()
{
    std::vector<CachedFrame>().swap(rev);
    std::vector<CachedFrame>().swap(revExt);
    revExtTarget = AV_NOPTS_VALUE;
    resumeTs = AV_NOPTS_VALUE;
    collecting = nullptr;
}

void VideoDecoder::Impl::collectCurrent(int64_t endTs)
{
    if (!collecting || !cur) {
        return;
    }
    std::shared_ptr<const VideoFrame> video = currentVideoFrame();
    if (video && video->dmabuf) {
        // A cache of GPU surfaces would starve the decoder's pool: keep the system memory copy.
        video = video->dmabuf->download();
    }
    if (!video) {
        collecting = nullptr; // a hardware failure: position() falls back, the stretch is dropped
        return;
    }
    collecting->push_back({cur.pts, endTs, std::move(video)});
    if (collecting->size() > collectCap) {
        collecting->erase(collecting->begin());
    }
}

VideoDecoder::Impl::Extend VideoDecoder::Impl::extendReverse(
    int64_t target, size_t cap, int budgetMs)
{
    if (target < firstFrameTs) {
        return Extend::Failed;
    }
    if (revExtTarget != target) {
        revExt.clear();
        revExtTarget = target;
        resumeTs = AV_NOPTS_VALUE;
        revExtSkipNonReference = false;
    }
    revExtSkipNonReference = revExtSkipNonReference || skipNonReference;
    QElapsedTimer timer;
    timer.start();
    const std::function<bool()> stop = [&] { return timer.elapsed() >= budgetMs; };
    const std::function<bool()>* const savedStop = stopCheck;
    stopCheck = budgetMs > 0 ? &stop : nullptr;
    stopped = false;
    collecting = &revExt;
    collectCap = cap;
    collectFromTs = target - int64_t(cap) * frameStepTs;

    const bool ok = position(target, Accuracy::Exact);
    const bool paused = stopped;
    stopCheck = savedStop;
    stopped = false;
    if (paused) {
        collecting = nullptr;
        resumeTs = target; // the next slice goes on from here instead of seeking again
        return Extend::Paused;
    }
    if (ok && collecting && cur) {
        const int64_t endTs = next ? next.pts : (eof ? std::numeric_limits<int64_t>::max() : -1);
        if (endTs >= 0) {
            collectCurrent(endTs);
        }
    }
    const bool complete = ok && collecting == &revExt && !revExt.empty()
        && revExt.back().ts == cur.pts && (rev.empty() || revExt.back().endTs >= rev.front().ts);
    collecting = nullptr;
    resumeTs = AV_NOPTS_VALUE;
    revExtTarget = AV_NOPTS_VALUE;
    if (!complete) {
        revExt.clear();
        return Extend::Failed;
    }
    qCDebug(lcMedia) << "reverse cache" << static_cast<const void*>(this) << ": stretch of" << revExt.size() << "frames ending at"
                     << tsToUs(target) << "us" << (budgetMs > 0 ? "(ahead)" : "(requested)")
                     << timer.elapsed() << "ms";
    revExt.insert(revExt.end(), std::make_move_iterator(rev.begin()),
        std::make_move_iterator(rev.end()));
    revSkipNonReference = (revSkipNonReference && !rev.empty()) || revExtSkipNonReference;
    rev = std::move(revExt);
    revExt.clear();
    // Frames above the playhead are the first to go; the ones at and below it are never dropped.
    const size_t keep = std::max(cap, 2 * reverseFrameLimit());
    while (rev.size() > keep && rev.back().ts > lastRequestTs) {
        rev.pop_back();
    }
    return Extend::Done;
}

std::shared_ptr<const VideoFrame> VideoDecoder::Impl::reverseFrame(int64_t ts)
{
    if (!reverseEnabled() || stopCheck) {
        return nullptr;
    }
    if (auto hit = reverseAt(ts)) {
        noteRequest(ts);
        return hit;
    }
    if (!noteRequest(ts)) {
        if (!rev.empty() || !revExt.empty()) {
            dropReverse(); // moved forward or jumped: the walk is over
        }
        return nullptr;
    }
    if (isCurrent(ts)) {
        return nullptr;
    }
    if (!rev.empty() && revSkipNonReference && !skipNonReference) {
        dropReverse();
    }
    const size_t limit = reverseFrameLimit();
    const int64_t gap = av_rescale_q(kGapUs, kMicrosecondsTb, stream->time_base);
    if (!rev.empty() && (ts >= rev.front().ts || rev.front().ts - ts > gap)) {
        dropReverse();
    }
    const Extend result = rev.empty() ? extendReverse(ts, limit, 0)
                                      : extendReverse(rev.front().ts - 1, limit, 0);
    if (result != Extend::Done) {
        dropReverse();
        backSteps = 0;
        return nullptr;
    }
    return reverseAt(ts);
}

void VideoDecoder::prefetchReverse(int budgetMs)
{
    Impl& d = *m_impl;
    if (!d.isOpen || d.isStill || d.frameList || !reverseEnabled() || d.stopCheck
        || d.backSteps < 1 || d.rev.empty() || d.lastRequestTs == AV_NOPTS_VALUE
        || (d.revSkipNonReference && !d.skipNonReference)) {
        return;
    }
    const size_t limit = d.reverseFrameLimit();
    const auto playhead = std::upper_bound(d.rev.begin(), d.rev.end(), d.lastRequestTs,
        [](int64_t value, const CachedFrame& f) { return value < f.ts; });
    const size_t below = size_t(playhead - d.rev.begin());
    if (d.revExtTarget == AV_NOPTS_VALUE && below >= limit) {
        return; // plenty left below the playhead
    }
    const int64_t target = d.rev.front().ts - 1;
    if (target < d.firstFrameTs) {
        return; // the start of the file
    }
    if (d.extendReverse(target, limit, std::max(1, budgetMs)) == Impl::Extend::Failed) {
        d.backSteps = 0;
        d.dropReverse();
    }
}

void VideoDecoder::releaseReverseCache()
{
    m_impl->dropReverse();
    m_impl->backSteps = 0;
}

int VideoDecoder::reverseCachedFrames() const
{
    return int(m_impl->rev.size());
}

} // namespace kk::media
