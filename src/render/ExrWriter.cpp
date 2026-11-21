// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/ExrWriter.h"

#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace kk::render::exr {

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

float srgbToLinear(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

/// IEEE half float to float.
float halfToFloat(quint16 h)
{
    const int sign = (h >> 15) & 1;
    const int exponent = (h >> 10) & 0x1f;
    const int mantissa = h & 0x3ff;
    float v;
    if (exponent == 0)
        v = std::ldexp(float(mantissa), -24);
    else if (exponent == 31)
        v = mantissa ? std::nanf("") : std::numeric_limits<float>::infinity();
    else
        v = std::ldexp(float(mantissa | 0x400), exponent - 25);
    return sign ? -v : v;
}

quint32 le32(const QByteArray& b, int pos)
{
    return quint32(uchar(b[pos])) | (quint32(uchar(b[pos + 1])) << 8)
        | (quint32(uchar(b[pos + 2])) << 16) | (quint32(uchar(b[pos + 3])) << 24);
}

} // namespace

bool available()
{
    return avcodec_find_encoder_by_name("exr") != nullptr;
}

bool write(const QImage& image, const QString& file, const Options& options, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    if (image.isNull())
        return fail(QStringLiteral("no picture"));
    const AVCodec* codec = avcodec_find_encoder_by_name("exr");
    if (!codec)
        return fail(QStringLiteral("this FFmpeg build has no OpenEXR encoder"));

    std::unique_ptr<AVCodecContext, CtxDeleter> ctx(avcodec_alloc_context3(codec));
    ctx->width = image.width();
    ctx->height = image.height();
    ctx->pix_fmt = AV_PIX_FMT_GBRAPF32LE;
    ctx->time_base = AVRational {1, 25};
    ctx->thread_count = 1;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "format", options.depth == kFloat ? "float" : "half", 0);
    const QByteArray compression
        = (options.compression == kNone || options.compression == kRle || options.compression == kZip1)
        ? options.compression.toLatin1()
        : QByteArray("zip16");
    av_dict_set(&opts, "compression", compression.constData(), 0);
    const int opened = avcodec_open2(ctx.get(), codec, &opts);
    av_dict_free(&opts);
    if (opened < 0)
        return fail(QStringLiteral("cannot open the OpenEXR encoder"));

    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_GBRAPF32LE;
    frame->width = image.width();
    frame->height = image.height();
    if (av_frame_get_buffer(frame.get(), 0) < 0)
        return fail(QStringLiteral("out of memory"));

    // Planes: G, B, R, A. The file holds premultiplied colour: unpremultiply the 8-bit value for
    // the colour space conversion, then multiply by alpha again.
    const QImage src = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    float lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = options.linear ? srgbToLinear(float(i) / 255.f) : float(i) / 255.f;
    for (int y = 0; y < src.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        auto* g = reinterpret_cast<float*>(frame->data[0] + qsizetype(y) * frame->linesize[0]);
        auto* b = reinterpret_cast<float*>(frame->data[1] + qsizetype(y) * frame->linesize[1]);
        auto* r = reinterpret_cast<float*>(frame->data[2] + qsizetype(y) * frame->linesize[2]);
        auto* a = reinterpret_cast<float*>(frame->data[3] + qsizetype(y) * frame->linesize[3]);
        for (int x = 0; x < src.width(); ++x) {
            const QRgb p = line[x];
            const int alpha = qAlpha(p);
            if (alpha == 0) {
                r[x] = g[x] = b[x] = a[x] = 0.f;
                continue;
            }
            const float af = float(alpha) / 255.f;
            auto channel = [&](int premult) {
                const int straight = std::min(255, (premult * 255 + alpha / 2) / alpha);
                return lut[straight] * af;
            };
            r[x] = channel(qRed(p));
            g[x] = channel(qGreen(p));
            b[x] = channel(qBlue(p));
            a[x] = af;
        }
    }

    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (avcodec_send_frame(ctx.get(), frame.get()) < 0 || avcodec_send_frame(ctx.get(), nullptr) < 0)
        return fail(QStringLiteral("the OpenEXR encoder refused the picture"));
    if (avcodec_receive_packet(ctx.get(), packet.get()) < 0)
        return fail(QStringLiteral("the OpenEXR encoder produced nothing"));
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)
        || out.write(reinterpret_cast<const char*>(packet->data), packet->size) != packet->size
        || !out.commit())
        return fail(QStringLiteral("cannot write %1").arg(file));
    return true;
}

QVector<float> Info::pixel(int x, int y) const
{
    if (x < 0 || y < 0 || x >= width || y >= height)
        return {};
    const qsizetype i = (qsizetype(y) * width + x) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

Info read(const QString& file)
{
    Info info;
    // The header attributes "compression" and "channels", read from the bytes.
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return info;
    const QByteArray head = f.read(4096);
    f.close();
    if (head.size() < 16 || le32(head, 0) != 20000630u)
        return info;
    int pos = 8;
    while (pos < head.size() && head[pos] != 0) {
        const int nameEnd = head.indexOf('\0', pos);
        if (nameEnd < 0)
            break;
        const int typeEnd = head.indexOf('\0', nameEnd + 1);
        if (typeEnd < 0 || typeEnd + 5 > head.size())
            break;
        const QByteArray name = head.mid(pos, nameEnd - pos);
        const qint32 size = qint32(le32(head, typeEnd + 1));
        const int data = typeEnd + 5;
        if (size < 0 || data + size > head.size())
            break;
        if (name == "compression" && size >= 1) {
            switch (uchar(head[data])) {
            case 0: info.compression = kNone; break;
            case 1: info.compression = kRle; break;
            case 2: info.compression = kZip1; break;
            case 3: info.compression = kZip16; break;
            default: info.compression = QStringLiteral("other"); break;
            }
        } else if (name == "channels") {
            // Each channel: name\0, int pixel type (1 half, 2 float), 12 more bytes.
            int c = data;
            while (c < data + size && head[c] != 0) {
                const int e = head.indexOf('\0', c);
                if (e < 0 || e + 5 > head.size())
                    break;
                info.bitsPerChannel = le32(head, e + 1) == 1 ? 16 : 32;
                c = e + 1 + 16;
            }
        }
        pos = data + size;
    }

    // The pixels, through FFmpeg's decoder.
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, file.toUtf8().constData(), nullptr, nullptr) < 0)
        return info;
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> fmt(
        raw, [](AVFormatContext* c) { avformat_close_input(&c); });
    if (avformat_find_stream_info(fmt.get(), nullptr) < 0 || fmt->nb_streams < 1)
        return info;
    AVCodecParameters* par = fmt->streams[0]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec)
        return info;
    std::unique_ptr<AVCodecContext, CtxDeleter> ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), par);
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0)
        return info;
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (av_read_frame(fmt.get(), packet.get()) < 0 || avcodec_send_packet(ctx.get(), packet.get()) < 0)
        return info;
    avcodec_send_packet(ctx.get(), nullptr);
    if (avcodec_receive_frame(ctx.get(), frame.get()) < 0)
        return info;
    info.width = frame->width;
    info.height = frame->height;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
    if (!desc || !(desc->flags & AV_PIX_FMT_FLAG_PLANAR) || !(desc->flags & AV_PIX_FMT_FLAG_FLOAT)
        || (desc->comp[0].depth != 32 && desc->comp[0].depth != 16))
        return info; // the decoder gives gbrp(a)f32 (float files) or gbrp(a)f16 (half files)
    const bool half = desc->comp[0].depth == 16;
    const bool alpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    info.hasAlpha = alpha;
    info.rgba.resize(qsizetype(info.width) * info.height * 4);
    auto sample = [half](const uint8_t* line, int x) {
        return half ? halfToFloat(reinterpret_cast<const quint16*>(line)[x])
                    : reinterpret_cast<const float*>(line)[x];
    };
    for (int y = 0; y < info.height; ++y) {
        const uint8_t* g = frame->data[0] + qsizetype(y) * frame->linesize[0];
        const uint8_t* b = frame->data[1] + qsizetype(y) * frame->linesize[1];
        const uint8_t* r = frame->data[2] + qsizetype(y) * frame->linesize[2];
        const uint8_t* a = alpha ? frame->data[3] + qsizetype(y) * frame->linesize[3] : nullptr;
        for (int x = 0; x < info.width; ++x) {
            const qsizetype i = (qsizetype(y) * info.width + x) * 4;
            info.rgba[i] = sample(r, x);
            info.rgba[i + 1] = sample(g, x);
            info.rgba[i + 2] = sample(b, x);
            info.rgba[i + 3] = a ? sample(a, x) : 1.f;
        }
    }
    info.ok = true;
    return info;
}

} // namespace kk::render::exr
