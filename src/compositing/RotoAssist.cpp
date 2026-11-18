// SPDX-License-Identifier: GPL-3.0-or-later
// RotoAssist — see RotoAssist.h. The idea (snap a roto point to the strongest nearby gradient)
// is the "Roto Assist" of Fusion's polygon tool as described in its public manual; the code is
// our own (Sobel operator on the luminance).
#include "compositing/RotoAssist.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace kk::roto {

std::optional<QPointF> snapToEdge(const QImage& image, QPointF p, double radius, double minStrength)
{
    if (image.isNull() || radius < 1.0)
        return std::nullopt;
    const int r = int(std::ceil(radius));
    const int x0 = std::max(1, int(std::floor(p.x())) - r), x1 = std::min(image.width() - 2, int(std::floor(p.x())) + r);
    const int y0 = std::max(1, int(std::floor(p.y())) - r), y1 = std::min(image.height() - 2, int(std::floor(p.y())) + r);
    if (x1 < x0 || y1 < y0)
        return std::nullopt;
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    const auto lum = [&](int x, int y) {
        const QRgb c = reinterpret_cast<const QRgb*>(rgb.constScanLine(y))[x];
        return (0.299 * qRed(c) + 0.587 * qGreen(c) + 0.114 * qBlue(c)) / 255.0;
    };
    double best = -1;
    QPointF bestPoint = p;
    double bestMag = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const double d = std::hypot(x + 0.5 - p.x(), y + 0.5 - p.y());
            if (d > radius)
                continue;
            const double gx = (lum(x + 1, y - 1) + 2 * lum(x + 1, y) + lum(x + 1, y + 1))
                - (lum(x - 1, y - 1) + 2 * lum(x - 1, y) + lum(x - 1, y + 1));
            const double gy = (lum(x - 1, y + 1) + 2 * lum(x, y + 1) + lum(x + 1, y + 1))
                - (lum(x - 1, y - 1) + 2 * lum(x, y - 1) + lum(x + 1, y - 1));
            const double mag = std::hypot(gx, gy) / 4.0;
            const double score = mag / (1.0 + (d / radius) * (d / radius));
            if (score > best) {
                best = score;
                bestMag = mag;
                bestPoint = QPointF(x + 0.5, y + 0.5);
            }
        }
    }
    if (best < 0 || bestMag < minStrength)
        return std::nullopt;
    return bestPoint;
}

} // namespace kk::roto
