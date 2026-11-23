// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/DpxWriter.h"

#include <QSaveFile>

#include <cstring>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace kk::render::dpx {

namespace {

struct CtxDeleter
{
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct FrameDeleter
{
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct PacketDeleter
{
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};

} // namespace

bool available()
{
    return avcodec_find_encoder_by_name("dpx") != nullptr;
}

bool write(const QImage& image, const QString& file, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    if (image.isNull())
        return fail(QStringLiteral("no picture"));
    const AVCodec* codec = avcodec_find_encoder_by_name("dpx");
    if (!codec)
        return fail(QStringLiteral("this FFmpeg build has no DPX encoder"));

    std::unique_ptr<AVCodecContext, CtxDeleter> ctx(avcodec_alloc_context3(codec));
    ctx->width = image.width();
    ctx->height = image.height();
    ctx->pix_fmt = AV_PIX_FMT_RGB24;
    ctx->time_base = AVRational {1, 25};
    ctx->thread_count = 1;
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0)
        return fail(QStringLiteral("cannot open the DPX encoder"));

    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = image.width();
    frame->height = image.height();
    if (av_frame_get_buffer(frame.get(), 0) < 0)
        return fail(QStringLiteral("out of memory"));
    const QImage src = image.convertToFormat(QImage::Format_RGB888);
    for (int y = 0; y < src.height(); ++y)
        std::memcpy(frame->data[0] + qsizetype(y) * frame->linesize[0], src.constScanLine(y),
            size_t(src.width()) * 3);

    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (avcodec_send_frame(ctx.get(), frame.get()) < 0 || avcodec_send_frame(ctx.get(), nullptr) < 0)
        return fail(QStringLiteral("the DPX encoder refused the picture"));
    if (avcodec_receive_packet(ctx.get(), packet.get()) < 0)
        return fail(QStringLiteral("the DPX encoder produced nothing"));
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)
        || out.write(reinterpret_cast<const char*>(packet->data), packet->size) != packet->size
        || !out.commit())
        return fail(QStringLiteral("cannot write %1").arg(file));
    return true;
}

} // namespace kk::render::dpx
