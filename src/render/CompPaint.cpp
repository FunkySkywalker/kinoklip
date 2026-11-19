// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/CompPaint.h"

#include <QColor>
#include <QPoint>
#include <QRect>

#include <algorithm>
#include <cmath>
#include <vector>

namespace kk::render::paint {

namespace {

using kk::paint::Point;
using kk::paint::Stroke;
using kk::paint::Tool;

/// One brush stamp along a stroke, in pixels (y down).
struct Stamp
{
    float x = 0;
    float y = 0;
    float radius = 0;
    float dx = 0; ///< the move from the previous stamp (Smear)
    float dy = 0;
};

struct Rgba
{
    float c[4] = {0, 0, 0, 0};
};

Rgba unpack(QRgb v)
{
    return {{qRed(v) / 255.0f, qGreen(v) / 255.0f, qBlue(v) / 255.0f, qAlpha(v) / 255.0f}};
}

QRgb pack(const Rgba& p)
{
    const auto b = [](float v) { return int(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const int a = b(p.c[3]);
    return qRgba(std::min(b(p.c[0]), a), std::min(b(p.c[1]), a), std::min(b(p.c[2]), a), a);
}

/// Pixel (x, y) of @p img (origin @p origin in the full picture); outside: transparent, or the
/// nearest edge pixel with @p clamp.
Rgba pixelAt(const QImage& img, QPoint origin, int x, int y, bool clamp)
{
    x -= origin.x();
    y -= origin.y();
    if (clamp) {
        x = std::clamp(x, 0, img.width() - 1);
        y = std::clamp(y, 0, img.height() - 1);
    } else if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) {
        return {};
    }
    return unpack(reinterpret_cast<const QRgb*>(img.constScanLine(y))[x]);
}

/// Bilinear sample at (x, y); pixel centres lie on whole numbers, so a whole position is exact.
Rgba sample(const QImage& img, QPoint origin, double x, double y, bool clamp)
{
    const double fx = std::floor(x), fy = std::floor(y);
    const float tx = float(x - fx), ty = float(y - fy);
    const int ix = int(fx), iy = int(fy);
    if (tx == 0.0f && ty == 0.0f)
        return pixelAt(img, origin, ix, iy, clamp);
    const Rgba a = pixelAt(img, origin, ix, iy, clamp), b = pixelAt(img, origin, ix + 1, iy, clamp),
               c = pixelAt(img, origin, ix, iy + 1, clamp),
               d = pixelAt(img, origin, ix + 1, iy + 1, clamp);
    Rgba out;
    for (int i = 0; i < 4; ++i) {
        const float top = a.c[i] + (b.c[i] - a.c[i]) * tx, bottom = c.c[i] + (d.c[i] - c.c[i]) * tx;
        out.c[i] = top + (bottom - top) * ty;
    }
    return out;
}

/// The stamps of @p s: along the polyline in steps of a tenth of the brush width, at least half a
/// pixel apart; pressure scales the radius.
std::vector<Stamp> stampsOf(const Stroke& s, QSize size)
{
    std::vector<Stamp> out;
    const double w = size.width(), h = size.height();
    const double base = s.size * w * 0.5;
    const auto radius = [&](const Point& p) { return float(std::max(0.05, double(p.pressure)) * base); };
    if (s.points.isEmpty())
        return out;
    const auto at = [&](const Point& p) { return QPointF(p.x * w, (1.0 - p.y) * h); };
    QPointF last = at(s.points.first());
    out.push_back({float(last.x()), float(last.y()), radius(s.points.first()), 0, 0});
    for (qsizetype i = 1; i < s.points.size(); ++i) {
        const QPointF a = at(s.points[i - 1]), b = at(s.points[i]);
        const float ra = radius(s.points[i - 1]), rb = radius(s.points[i]);
        const double len = std::hypot(b.x() - a.x(), b.y() - a.y());
        if (len <= 0.0 && ra == rb)
            continue;
        const double step = std::max(0.5, 0.1 * 2.0 * std::min(ra, rb));
        const int n = std::max(1, int(std::ceil(len / step)));
        for (int k = 1; k <= n; ++k) {
            const double t = double(k) / n;
            const QPointF p = a + (b - a) * t;
            out.push_back({float(p.x()), float(p.y()), ra + (rb - ra) * float(t),
                float(p.x() - last.x()), float(p.y() - last.y())});
            last = p;
        }
    }
    return out;
}

QRect stampRect(const Stamp& st, int margin, QSize size)
{
    const int r = int(std::ceil(st.radius)) + 1 + margin;
    return QRect(int(std::floor(st.x)) - r, int(std::floor(st.y)) - r, 2 * r + 1, 2 * r + 1)
        .intersected(QRect(QPoint(0, 0), size));
}

void applyColorOrEraseOrClone(const Stroke& s, const std::vector<Stamp>& stamps, QImage& res,
    const QImage& orig, QSize size)
{
    QRect box;
    for (const Stamp& st : stamps)
        box |= stampRect(st, 0, size);
    if (box.isEmpty())
        return;
    // One even layer: the strongest brush strength over the pixel, then applied once.
    std::vector<float> cover(size_t(box.width()) * size_t(box.height()), 0.0f);
    for (const Stamp& st : stamps) {
        const QRect r = stampRect(st, 0, size);
        for (int y = r.top(); y <= r.bottom(); ++y) {
            for (int x = r.left(); x <= r.right(); ++x) {
                const float d = std::hypot(float(x) - st.x, float(y) - st.y);
                const float wgt = brushWeight(d, st.radius, float(s.softness));
                float& c = cover[size_t(y - box.top()) * size_t(box.width()) + size_t(x - box.left())];
                c = std::max(c, wgt);
            }
        }
    }
    QImage snapshot;
    double offX = 0, offY = 0;
    if (s.tool == Tool::Clone) {
        snapshot = res.copy(); // the picture before this stroke is the source
        offX = s.cloneX * size.width();
        offY = -s.cloneY * size.height(); // y up in the stroke, down in pixels
    }
    const float ca = float(s.color.alphaF());
    const float cr = float(s.color.redF()) * ca, cg = float(s.color.greenF()) * ca,
                cb = float(s.color.blueF()) * ca;
    for (int y = box.top(); y <= box.bottom(); ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(res.scanLine(y));
        for (int x = box.left(); x <= box.right(); ++x) {
            const float a = cover[size_t(y - box.top()) * size_t(box.width()) + size_t(x - box.left())]
                * float(s.opacity);
            if (a <= 0.0f)
                continue;
            Rgba dst = unpack(row[x]);
            if (s.tool == Tool::Stroke) {
                const float src[4] = {cr, cg, cb, ca};
                const float keep = 1.0f - ca * a;
                for (int i = 0; i < 4; ++i)
                    dst.c[i] = src[i] * a + dst.c[i] * keep;
            } else {
                const Rgba src = s.tool == Tool::Eraser
                    ? unpack(reinterpret_cast<const QRgb*>(orig.constScanLine(y))[x])
                    : sample(snapshot, QPoint(0, 0), x + offX, y + offY, false);
                for (int i = 0; i < 4; ++i)
                    dst.c[i] += (src.c[i] - dst.c[i]) * a;
            }
            row[x] = pack(dst);
        }
    }
}

void applySmear(const Stroke& s, const std::vector<Stamp>& stamps, QImage& res, QSize size)
{
    for (size_t i = 1; i < stamps.size(); ++i) {
        const Stamp& st = stamps[i];
        if (st.dx == 0.0f && st.dy == 0.0f)
            continue;
        const int margin = int(std::ceil(std::max(std::abs(st.dx), std::abs(st.dy)))) + 2;
        const QRect read = stampRect(st, margin, size);
        const QRect write = stampRect(st, 0, size);
        const QImage tmp = res.copy(read);
        for (int y = write.top(); y <= write.bottom(); ++y) {
            QRgb* row = reinterpret_cast<QRgb*>(res.scanLine(y));
            for (int x = write.left(); x <= write.right(); ++x) {
                const float wgt
                    = brushWeight(std::hypot(float(x) - st.x, float(y) - st.y), st.radius,
                          float(s.softness))
                    * float(s.opacity);
                if (wgt <= 0.0f)
                    continue;
                // The pixel takes what was behind it on the way the brush came from.
                const Rgba src = sample(tmp, read.topLeft(), x - double(st.dx), y - double(st.dy), true);
                Rgba dst = unpack(row[x]);
                for (int k = 0; k < 4; ++k)
                    dst.c[k] += (src.c[k] - dst.c[k]) * wgt;
                row[x] = pack(dst);
            }
        }
    }
}

} // namespace

float brushWeight(float distance, float radius, float softness)
{
    const float feather = std::max(1.0f, radius * std::clamp(softness, 0.0f, 1.0f));
    const float t = std::clamp((radius - distance) / feather, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

QImage drawStrokes(const QVector<Stroke>& strokes, int64_t frame, QSize size, const QImage& input)
{
    bool any = false;
    for (const Stroke& s : strokes)
        any = any || (s.visibleAt(frame) && !s.points.isEmpty() && s.opacity > 0.0);
    if (!any || size.isEmpty())
        return {};
    QImage orig = input.isNull() ? QImage(size, QImage::Format_ARGB32_Premultiplied)
                                 : input.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (input.isNull())
        orig.fill(Qt::transparent);
    else if (orig.size() != size)
        orig = orig.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage res = orig.copy();
    for (const Stroke& s : strokes) {
        if (!s.visibleAt(frame) || s.points.isEmpty() || s.opacity <= 0.0)
            continue;
        const std::vector<Stamp> stamps = stampsOf(s, size);
        if (s.tool == Tool::Smear)
            applySmear(s, stamps, res, size);
        else
            applyColorOrEraseOrClone(s, stamps, res, orig, size);
    }
    return res;
}

QImage renderNode(const CompNode& node, int64_t frame, QSize size, const QImage& input)
{
    return drawStrokes(kk::paint::strokesOf(node.params), frame, size, input);
}

} // namespace kk::render::paint
