// SPDX-License-Identifier: GPL-3.0-or-later
// CompWarpMath implementation. Displace and Vector Distort are standard displacement-map warps;
// the grid warp is the Catmull-Rom displacement field of the Warper effect (GlMeshEffects.cpp).
#include "render/CompWarpMath.h"

#include "core/NodeRegistry.h"
#include "render/CompMath.h"

#include <QPointF>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::render::compmath {

namespace {

namespace k = nodekeys;

double num(const ParamSet& p, const QString& key, int64_t frame, double fallback)
{
    return p.valueAtTime(key, double(frame), fallback);
}

int channelIndex(const QString& id)
{
    return std::clamp(channelsources::index(id), 0, 4);
}

} // namespace

float mapChannel(const float c[4], int ch)
{
    if (ch == MapAlpha)
        return c[3];
    float s[3] = {0, 0, 0};
    if (c[3] > 0) {
        for (int i = 0; i < 3; ++i)
            s[i] = c[i] / c[3];
    }
    if (ch == MapLuma)
        return 0.2126f * s[0] + 0.7152f * s[1] + 0.0722f * s[2];
    return s[std::clamp(ch, 0, 2)];
}

void sampleBilinear(const FImage& image, double x, double y, int edges, float out[4])
{
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = float(x - x0), fy = float(y - y0);
    float acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int ix = edgeIndex(x0 + i, image.w, edges);
            const int iy = edgeIndex(y0 + j, image.h, edges);
            if (ix < 0 || iy < 0)
                continue;
            const float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const float* p = image.at(ix, iy);
            for (int c = 0; c < 4; ++c)
                acc[c] += w * p[c];
        }
    }
    for (int c = 0; c < 4; ++c)
        out[c] = acc[c];
}

// ---- Displace ----

DisplaceParams displaceParams(const ParamSet& p, int64_t frame)
{
    DisplaceParams d;
    d.radial = p.staticValue(k::DisplaceType, displacetypes::XY).toString() == displacetypes::Radial;
    d.xChannel = channelIndex(p.staticValue(k::DisplaceXChannel, channelsources::Red).toString());
    d.yChannel = channelIndex(p.staticValue(k::DisplaceYChannel, channelsources::Green).toString());
    d.xRefraction = float(num(p, k::DisplaceXRefraction, frame, 20));
    d.yRefraction = float(num(p, k::DisplaceYRefraction, frame, 20));
    d.offset = float(num(p, k::DisplaceOffset, frame, 0.5));
    d.centre[0] = float(num(p, k::CenterX, frame, 0.5));
    d.centre[1] = float(num(p, k::CenterY, frame, 0.5));
    d.lightPower = float(num(p, k::DisplaceLightPower, frame, 0));
    d.lightAngle = float(num(p, k::DisplaceLightAngle, frame, 0));
    d.edges = edgemodes::index(p.staticValue(k::Edges).toString());
    return d;
}

void displaceAt(const DisplaceParams& d, const FImage& map, int x, int y, double scale,
    double src[2], float* shade)
{
    const float* m = map.at(x, y);
    double dx, dy; // movement of the content, pixels, y up
    if (d.radial) {
        const double cx = d.centre[0] * map.w, cy = (1.0 - d.centre[1]) * map.h;
        double rx = x + 0.5 - cx, ry = y + 0.5 - cy;
        const double len = std::hypot(rx, ry);
        if (len > 1e-6) {
            rx /= len;
            ry /= len;
        } else {
            rx = ry = 0;
        }
        const double amount = (mapChannel(m, d.xChannel) - d.offset) * d.xRefraction * scale;
        dx = rx * amount;
        dy = -ry * amount;
    } else {
        dx = (mapChannel(m, d.xChannel) - d.offset) * d.xRefraction * scale;
        dy = (mapChannel(m, d.yChannel) - d.offset) * d.yRefraction * scale;
    }
    src[0] = x - dx;
    src[1] = y + dy;
    *shade = 1.0f;
    if (d.lightPower != 0.0f) {
        const float vl = mapChannel(map.clamped(x - 1, y), d.xChannel);
        const float vr = mapChannel(map.clamped(x + 1, y), d.xChannel);
        const float vu = mapChannel(map.clamped(x, y - 1), d.xChannel);
        const float vd = mapChannel(map.clamped(x, y + 1), d.xChannel);
        const float a = d.lightAngle * float(std::numbers::pi / 180.0);
        const float slope = (vr - vl) * std::cos(a) + (vu - vd) * std::sin(a);
        *shade = std::max(0.0f, 1.0f + d.lightPower * 4.0f * slope);
    }
}

FImage displace(const FImage& in, const FImage& map, const DisplaceParams& d, double scale)
{
    FImage out(in.w, in.h);
    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            double src[2];
            float shade;
            displaceAt(d, map, x, y, scale, src, &shade);
            float c[4];
            sampleBilinear(in, src[0], src[1], d.edges, c);
            float* o = out.at(x, y);
            for (int i = 0; i < 3; ++i)
                o[i] = std::min(c[i] * shade, c[3]);
            o[3] = c[3];
        }
    }
    return out;
}

// ---- Vector Distort ----

VectorDistortParams vectorDistortParams(const ParamSet& p, int64_t frame)
{
    VectorDistortParams v;
    v.range = float(num(p, k::VectorRange, frame, 32));
    v.multiplier = float(num(p, k::VectorMultiplier, frame, 1));
    v.steps = std::clamp(int(num(p, k::VectorSteps, frame, 1)), 1, 8);
    v.edges = edgemodes::index(p.staticValue(k::Edges).toString());
    return v;
}

void vectorAt(const VectorDistortParams& v, const FImage& map, double x, double y, double scale,
    double out[2])
{
    float c[4];
    sampleBilinear(map, x, y, edgemodes::index(edgemodes::Duplicate), c);
    out[0] = out[1] = 0;
    if (c[3] <= 0)
        return;
    const double f = double(v.range) * v.multiplier * scale * 2.0;
    out[0] = (c[0] / c[3] - 0.5) * f;
    out[1] = (c[1] / c[3] - 0.5) * f;
}

FImage vectorDistort(const FImage& in, const FImage& map, const VectorDistortParams& v, double scale)
{
    FImage out(in.w, in.h);
    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            double px = x, py = y; // pixel-centre coordinates (the centre of pixel x is x)
            for (int i = 0; i < v.steps; ++i) {
                double vec[2];
                vectorAt(v, map, px, py, scale, vec);
                px -= vec[0] / v.steps;
                py += vec[1] / v.steps;
            }
            sampleBilinear(in, px, py, v.edges, out.at(x, y));
        }
    }
    return out;
}

// ---- Grid Warp ----

bool GridWarpParams::isIdentity() const
{
    return std::all_of(d.begin(), d.end(), [](float v) { return std::abs(v) < 1e-7f; });
}

GridWarpParams gridWarpParams(
    const ParamSet& p, int64_t frame, double scale, int width, int height)
{
    GridWarpParams g;
    g.cols = std::clamp(int(num(p, k::Columns, frame, 4)), 2, k::kGridMaxSide);
    g.rows = std::clamp(int(num(p, k::Rows, frame, 4)), 2, k::kGridMaxSide);
    g.d.resize(size_t(g.cols) * size_t(g.rows) * 2);
    for (int r = 0; r < g.rows; ++r) {
        for (int c = 0; c < g.cols; ++c) {
            const size_t i = (size_t(r) * size_t(g.cols) + size_t(c)) * 2;
            g.d[i] = float(num(p, k::gridKey(r, c, 0), frame, 0) * scale / std::max(width, 1));
            g.d[i + 1]
                = float(num(p, k::gridKey(r, c, 1), frame, 0) * scale / std::max(height, 1));
        }
    }
    return g;
}

namespace {

void catmullRom(double t, double w[4])
{
    const double t2 = t * t, t3 = t2 * t;
    w[0] = -0.5 * t3 + t2 - 0.5 * t;
    w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
    w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    w[3] = 0.5 * t3 - 0.5 * t2;
}

void gridField(const GridWarpParams& g, const double t[2], double out[2])
{
    const double top[2] = {double(g.cols - 1), double(g.rows - 1)};
    double gp[2];
    for (int a = 0; a < 2; ++a)
        gp[a] = std::clamp(t[a] * top[a], 0.0, top[a]);
    const int ix = int(std::floor(gp[0])), iy = int(std::floor(gp[1]));
    double wx[4], wy[4];
    catmullRom(gp[0] - ix, wx);
    catmullRom(gp[1] - iy, wy);
    out[0] = out[1] = 0;
    for (int j = -1; j <= 2; ++j) {
        double row[2] = {0, 0};
        for (int c = -1; c <= 2; ++c) {
            const int cc = std::clamp(ix + c, 0, g.cols - 1), rr = std::clamp(iy + j, 0, g.rows - 1);
            const size_t i = (size_t(rr) * size_t(g.cols) + size_t(cc)) * 2;
            row[0] += wx[c + 1] * g.d[i];
            row[1] += wx[c + 1] * g.d[i + 1];
        }
        out[0] += wy[j + 1] * row[0];
        out[1] += wy[j + 1] * row[1];
    }
}

} // namespace

void gridWarpSource(const GridWarpParams& g, const double t[2], double out[2])
{
    double s[2] = {t[0], t[1]};
    for (int n = 0; n < 5; ++n) {
        double f[2];
        gridField(g, s, f);
        s[0] = t[0] - f[0];
        s[1] = t[1] - f[1];
    }
    out[0] = s[0];
    out[1] = s[1];
}

FImage gridWarp(const FImage& in, const GridWarpParams& g)
{
    if (g.cols < 2 || g.rows < 2 || g.isIdentity())
        return in;
    FImage out(in.w, in.h);
    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            const double t[2] = {(x + 0.5) / in.w, (y + 0.5) / in.h};
            double s[2];
            gridWarpSource(g, t, s);
            if (s[0] < 0 || s[1] < 0 || s[0] > 1 || s[1] > 1)
                continue;
            sampleBilinear(in, s[0] * in.w - 0.5, s[1] * in.h - 0.5,
                edgemodes::index(edgemodes::Duplicate), out.at(x, y));
        }
    }
    return out;
}

} // namespace kk::render::compmath
