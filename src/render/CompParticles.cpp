// SPDX-License-Identifier: GPL-3.0-or-later
// CompParticles implementation (spec 129).
#include "render/CompParticles.h"

#include "core/NodeRegistry.h"

#include <QColor>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace kk::render::particles {

namespace k = nodekeys;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr qsizetype kMaxParticles = 200000; ///< safety stop for a runaway Number x Lifespan
constexpr int64_t kMaxScan = 20000; ///< the longest life looked back over, in frames

// ---- Random numbers: SplitMix64, so every platform draws the same sequence ----

uint64_t mix64(uint64_t z)
{
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct Rng
{
    uint64_t s;
    Rng(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
        : s(mix64(mix64(mix64(a) ^ b) + c) ^ d)
    { }
    uint64_t next()
    {
        s += 0x9E3779B97F4A7C15ULL;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double uni() { return double(next() >> 11) * (1.0 / 9007199254740992.0); } ///< [0, 1)
    double sym() { return uni() * 2.0 - 1.0; } ///< [-1, 1)
};

// ---- Value noise ----

/// A lattice value in [-1, 1].
float lattice(int ix, int iy, int iz, int channel, uint32_t seed)
{
    uint64_t h = mix64(uint64_t(uint32_t(ix)) | (uint64_t(uint32_t(iy)) << 32));
    h = mix64(h ^ (uint64_t(uint32_t(iz)) << 8) ^ (uint64_t(channel) << 56) ^ (uint64_t(seed) << 20));
    return float(double(h >> 40) / 8388608.0 - 1.0);
}

float smooth(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

/// Smooth 3D value noise in [-1, 1] (x, y: space; z: time).
float noise3(float x, float y, float z, int channel, uint32_t seed)
{
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = int(fx), iy = int(fy), iz = int(fz);
    const float tx = smooth(x - fx), ty = smooth(y - fy), tz = smooth(z - fz);
    float c[2][2][2];
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
                c[dz][dy][dx] = lattice(ix + dx, iy + dy, iz + dz, channel, seed);
    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    float layer[2];
    for (int dz = 0; dz < 2; ++dz) {
        const float a = lerp(c[dz][0][0], c[dz][0][1], tx);
        const float b = lerp(c[dz][1][0], c[dz][1][1], tx);
        layer[dz] = lerp(a, b, ty);
    }
    return lerp(layer[0], layer[1], tz);
}

// ---- Forces, by frame ----

struct ForceFrame
{
    bool active = false;
    bool turbulence = false;
    float ax = 0, ay = 0; ///< gravity: speed change per frame, width units
    float strength = 0; ///< turbulence
    float density = 1;
    float evolution = 0;
    uint32_t seed = 0;
};

ForceFrame forceAt(const CompNode& n, int64_t frame)
{
    ForceFrame f;
    f.active = n.params.staticValue(k::PActive, true).toBool();
    if (n.type == nodetypes::PGravity) {
        const double a = n.params.value(k::Angle, frame, 270.0) * kPi / 180.0;
        const double s = n.params.value(k::PStrength, frame, 0.0005);
        f.ax = float(s * std::cos(a));
        f.ay = float(s * std::sin(a));
    } else {
        f.turbulence = true;
        f.strength = float(n.params.value(k::PStrength, frame, 0.002));
        f.density = float(std::max(0.01, n.params.value(k::PDensity, frame, 4.0)));
        f.evolution = float(n.params.value(k::PEvolution, frame, 0.05));
        f.seed = uint32_t(std::lround(n.params.value(k::Seed, frame, 0.0)));
    }
    return f;
}

// ---- Emitter values, at a birth frame ----

struct Birth
{
    int count = 0;
    double life = 1, lifeVar = 0;
    double velocity = 0, velocityVar = 0, angle = 90, angleVar = 0;
    double rotation = 0, rotationVar = 0, spin = 0, spinVar = 0;
    double size = 0.03, sizeEnd = 0.03, sizeVar = 0;
    double opacity = 1, opacityEnd = 0;
    double cx = 0.5, cy = 0.5, width = 0, height = 0;
};

} // namespace

bool isStreamNode(const QString& type)
{
    return type == nodetypes::PEmitter || type == nodetypes::PGravity
        || type == nodetypes::PTurbulence || type == nodetypes::PMerge;
}

namespace {

void collect(const Composition& comp, quint64 id, QVector<quint64> forcesNearRender,
    QVector<Path>& out, int depth)
{
    const CompNode* n = comp.node(id);
    if (!n || depth > comp.nodes.size() + 1)
        return;
    const auto follow = [&](const QString& port, const QVector<quint64>& forces) {
        const auto it = n->inputs.constFind(port);
        if (it != n->inputs.constEnd() && it->isConnected() && it->outputIndex == 0)
            collect(comp, it->nodeId, forces, out, depth + 1);
    };
    if (n->type == nodetypes::PEmitter) {
        if (!n->enabled)
            return;
        Path p;
        p.emitter = id;
        for (qsizetype i = forcesNearRender.size() - 1; i >= 0; --i)
            p.forces.append(forcesNearRender[i]);
        out.append(p);
    } else if (n->type == nodetypes::PMerge) {
        follow(nodeports::Input, forcesNearRender);
        if (n->enabled)
            follow(nodeports::Input2, forcesNearRender);
    } else if (n->type == nodetypes::PGravity || n->type == nodetypes::PTurbulence) {
        if (n->enabled)
            forcesNearRender.append(id);
        follow(nodeports::Input, forcesNearRender);
    }
}

} // namespace

QVector<Path> paths(const Composition& comp, quint64 stream)
{
    QVector<Path> out;
    collect(comp, stream, {}, out, 0);
    return out;
}

namespace {

/// The largest value a keyframed parameter takes (its base and every key).
double maxOf(const CompNode& n, const QString& key, double fallback)
{
    const auto it = n.params.values.constFind(key);
    if (it == n.params.values.constEnd())
        return fallback;
    double m = it->base;
    for (const Keyframe& kf : it->keys)
        m = std::max(m, kf.value);
    return m;
}

Birth birthAt(const CompNode& n, int64_t t, Rng& rng)
{
    const auto v = [&](const QString& key, double def) { return n.params.value(key, t, def); };
    Birth b;
    const double number = v(k::PNumber, 5) + v(k::PNumberVar, 0) * rng.sym();
    const double whole = std::floor(std::max(0.0, number));
    b.count = int(whole + (rng.uni() < std::max(0.0, number) - whole ? 1 : 0));
    b.life = v(k::PLife, 50);
    b.lifeVar = v(k::PLifeVar, 0);
    b.velocity = v(k::PVelocity, 0.01);
    b.velocityVar = v(k::PVelocityVar, 0);
    b.angle = v(k::Angle, 90);
    b.angleVar = v(k::PAngleVar, 15);
    b.rotation = v(k::PRotation, 0);
    b.rotationVar = v(k::PRotationVar, 0);
    b.spin = v(k::PSpin, 0);
    b.spinVar = v(k::PSpinVar, 0);
    b.size = v(k::PSize, 0.03);
    b.sizeEnd = v(k::PSizeEnd, 0.03);
    b.sizeVar = v(k::PSizeVar, 0);
    b.opacity = v(k::POpacity, 1);
    b.opacityEnd = v(k::POpacityEnd, 0);
    b.cx = v(k::CenterX, 0.5);
    b.cy = v(k::CenterY, 0.5);
    b.width = v(k::Width, 0.2);
    b.height = v(k::Height, 0.2);
    return b;
}

QColor colorOf(const CompNode& n, const QString& key, QColor def)
{
    const QColor c = n.params.staticValue(key, def).value<QColor>();
    return c.isValid() ? c : def;
}

} // namespace

QVector<Particle> simulate(
    const Composition& comp, const QVector<Path>& paths, int64_t frame, double aspect)
{
    QVector<Particle> out;
    // The frames that can still hold a particle: back from this frame by the longest life.
    int64_t lowest = frame;
    for (const Path& p : paths) {
        const CompNode* e = comp.node(p.emitter);
        if (!e)
            continue;
        const double longest = maxOf(*e, k::PLife, 50) + maxOf(*e, k::PLifeVar, 0);
        const int64_t start = int64_t(std::lround(e->params.value(k::PStart, frame, 0.0)));
        lowest = std::min(lowest,
            std::max<int64_t>(start, frame - std::min<int64_t>(kMaxScan, int64_t(std::ceil(longest)))));
    }
    // Forces, by frame, for every frame a particle can have lived through.
    QHash<quint64, QVector<ForceFrame>> forceFrames;
    const auto forcesOf = [&](quint64 id) -> const QVector<ForceFrame>& {
        auto it = forceFrames.find(id);
        if (it == forceFrames.end()) {
            QVector<ForceFrame> v;
            if (const CompNode* n = comp.node(id)) {
                v.reserve(qsizetype(frame - lowest + 1));
                for (int64_t f = lowest; f <= frame; ++f)
                    v.append(forceAt(*n, f));
            }
            it = forceFrames.insert(id, v);
        }
        return *it;
    };

    for (const Path& path : paths) {
        const CompNode* e = comp.node(path.emitter);
        if (!e || !e->enabled)
            continue;
        const int64_t start = int64_t(std::lround(e->params.value(k::PStart, frame, 0.0)));
        const int64_t end = int64_t(std::lround(e->params.value(k::PEnd, frame, 100000.0)));
        const QString regionId = e->params.staticValue(k::PRegion, particleregions::Point).toString();
        const QString styleId = e->params.staticValue(k::PStyle, particlestyles::Blob).toString();
        const Style style = styleId == particlestyles::Point ? Style::Point
            : styleId == particlestyles::Bitmap             ? Style::Bitmap
                                                            : Style::Blob;
        const QColor c0 = colorOf(*e, k::Color, QColor(255, 255, 255));
        const QColor c1 = colorOf(*e, k::PColorEnd, QColor(255, 255, 255));
        const uint64_t seed = uint64_t(std::lround(e->params.value(k::Seed, 0, 0.0)));
        QVector<const QVector<ForceFrame>*> forces;
        for (quint64 f : path.forces)
            forces.append(&forcesOf(f));
        const int64_t first = std::max(start, lowest);
        const int64_t last = std::min(end, frame);
        for (int64_t t = first; t <= last; ++t) {
            Rng rng(seed, e->id, uint64_t(t), 0x70617274ULL);
            const Birth b = birthAt(*e, t, rng);
            for (int i = 0; i < b.count; ++i) {
                if (out.size() >= kMaxParticles)
                    return out;
                const double life = std::max(1.0, b.life + b.lifeVar * rng.sym());
                const double age = double(frame - t);
                // Draw every random number of the particle in a fixed order, alive or not, so the
                // others do not move when it dies.
                double px = b.cx, py = b.cy * aspect;
                if (regionId == particleregions::Line) {
                    px += (rng.uni() - 0.5) * b.width;
                } else if (regionId == particleregions::Rectangle) {
                    px += (rng.uni() - 0.5) * b.width;
                    py += (rng.uni() - 0.5) * b.height;
                } else if (regionId == particleregions::Circle) {
                    const double r = b.width * 0.5 * std::sqrt(rng.uni());
                    const double a = rng.uni() * 2.0 * kPi;
                    px += r * std::cos(a);
                    py += r * std::sin(a);
                }
                const double speed = b.velocity + b.velocityVar * rng.sym();
                const double dir = (b.angle + b.angleVar * rng.sym()) * kPi / 180.0;
                const double rot0 = b.rotation + b.rotationVar * rng.sym();
                const double spin = b.spin + b.spinVar * rng.sym();
                const double sizeScale = std::max(0.0, 1.0 + b.sizeVar * rng.sym());
                if (age >= life)
                    continue;
                double vx = speed * std::cos(dir), vy = speed * std::sin(dir);
                const int steps = int(age);
                for (int s = 1; s <= steps; ++s) {
                    const int64_t f = t + s;
                    for (const QVector<ForceFrame>* force : forces) {
                        const ForceFrame& ff = (*force)[qsizetype(f - lowest)];
                        if (!ff.active)
                            continue;
                        if (!ff.turbulence) {
                            vx += ff.ax;
                            vy += ff.ay;
                        } else {
                            const float nx = float(px) * ff.density;
                            const float ny = float(py) * ff.density;
                            const float nz = float(f) * ff.evolution;
                            vx += ff.strength * noise3(nx, ny, nz, 0, ff.seed);
                            vy += ff.strength * noise3(nx, ny, nz, 1, ff.seed);
                        }
                    }
                    px += vx;
                    py += vy;
                }
                const double u = age / life;
                const auto lerp = [u](double a, double z) { return a + (z - a) * u; };
                Particle p;
                p.x = float(px);
                p.y = float(py);
                p.size = float(std::max(0.0, lerp(b.size, b.sizeEnd) * sizeScale));
                p.rotation = float(rot0 + spin * age);
                p.r = float(lerp(c0.redF(), c1.redF()));
                p.g = float(lerp(c0.greenF(), c1.greenF()));
                p.b = float(lerp(c0.blueF(), c1.blueF()));
                p.opacity = float(std::clamp(lerp(b.opacity, b.opacityEnd), 0.0, 1.0));
                p.emitter = e->id;
                p.style = style;
                out.append(p);
            }
        }
    }
    return out;
}

QVector<Particle> streamAt(const Composition& comp, quint64 stream, int64_t frame, double aspect)
{
    return simulate(comp, paths(comp, stream), frame, aspect);
}

namespace {

/// The visible part of a sprite picture (premultiplied), or a null image.
QImage cropped(const QImage& in)
{
    if (in.isNull())
        return {};
    const QImage image = in.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int x0 = image.width(), y0 = image.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(row[x]) == 0)
                continue;
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
    }
    if (x1 < 0)
        return {};
    return image.copy(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

} // namespace

QImage draw(const QVector<Particle>& particles, QSize size, const SpriteFn& sprite)
{
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    if (particles.isEmpty() || size.isEmpty())
        return out;
    const int w = size.width(), h = size.height();
    std::vector<float> acc(size_t(w) * size_t(h) * 4, 0.0f); // premultiplied r g b a
    QHash<quint64, QImage> sprites;
    for (const Particle& p : particles) {
        const double radius = double(p.size) * w * 0.5;
        if (radius < 0.05 || p.opacity <= 0.0f)
            continue;
        const double cx = double(p.x) * w;
        const double cy = double(h) - double(p.y) * w;
        QImage pic;
        if (p.style == Style::Bitmap) {
            auto it = sprites.find(p.emitter);
            if (it == sprites.end())
                it = sprites.insert(p.emitter, sprite ? cropped(sprite(p.emitter)) : QImage());
            pic = *it;
            if (pic.isNull())
                continue;
        }
        double reach = radius + 1.0;
        double sw = 1, sh = 1, halfW = 0, halfH = 0, cs = 1, sn = 0;
        if (!pic.isNull()) {
            sw = pic.width();
            sh = pic.height();
            halfW = radius;
            halfH = radius * sh / sw;
            reach = std::hypot(halfW, halfH) + 1.0;
            const double a = double(p.rotation) * kPi / 180.0;
            cs = std::cos(a);
            sn = std::sin(a);
        }
        const int xa = std::max(0, int(std::floor(cx - reach))),
                  xb = std::min(w - 1, int(std::ceil(cx + reach)));
        const int ya = std::max(0, int(std::floor(cy - reach))),
                  yb = std::min(h - 1, int(std::ceil(cy + reach)));
        for (int y = ya; y <= yb; ++y) {
            for (int x = xa; x <= xb; ++x) {
                const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                float sr, sg, sb, sa;
                if (pic.isNull()) {
                    const double d = std::sqrt(dx * dx + dy * dy);
                    double cov;
                    if (p.style == Style::Point) {
                        cov = std::clamp(radius - d + 0.5, 0.0, 1.0);
                    } else {
                        const double t = std::clamp(1.0 - d / radius, 0.0, 1.0);
                        cov = t * t * (3.0 - 2.0 * t);
                    }
                    if (cov <= 0.0)
                        continue;
                    sa = float(cov) * p.opacity;
                    sr = p.r * sa;
                    sg = p.g * sa;
                    sb = p.b * sa;
                } else {
                    const double a = dx * cs - dy * sn;
                    const double b = dx * sn + dy * cs;
                    const double fx = (a / (2.0 * halfW) + 0.5) * sw - 0.5;
                    const double fy = (b / (2.0 * halfH) + 0.5) * sh - 0.5;
                    if (fx < -1 || fy < -1 || fx > sw || fy > sh)
                        continue;
                    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
                    const float tx = float(fx - ix), ty = float(fy - iy);
                    float c[4] = {0, 0, 0, 0};
                    for (int j = 0; j < 2; ++j) {
                        for (int i = 0; i < 2; ++i) {
                            const int qx = ix + i, qy = iy + j;
                            if (qx < 0 || qy < 0 || qx >= pic.width() || qy >= pic.height())
                                continue;
                            const QRgb q
                                = reinterpret_cast<const QRgb*>(pic.constScanLine(qy))[qx];
                            const float wt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
                            c[0] += wt * qRed(q) / 255.0f;
                            c[1] += wt * qGreen(q) / 255.0f;
                            c[2] += wt * qBlue(q) / 255.0f;
                            c[3] += wt * qAlpha(q) / 255.0f;
                        }
                    }
                    sa = c[3] * p.opacity;
                    if (sa <= 0.0f)
                        continue;
                    sr = c[0] * p.r * p.opacity;
                    sg = c[1] * p.g * p.opacity;
                    sb = c[2] * p.b * p.opacity;
                }
                float* d = &acc[(size_t(y) * size_t(w) + size_t(x)) * 4];
                const float inv = 1.0f - sa;
                d[0] = sr + d[0] * inv;
                d[1] = sg + d[1] * inv;
                d[2] = sb + d[2] * inv;
                d[3] = sa + d[3] * inv;
            }
        }
    }
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const float* d = &acc[(size_t(y) * size_t(w) + size_t(x)) * 4];
            const auto q = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            const int a = q(d[3]);
            row[x] = qRgba(std::min(q(d[0]), a), std::min(q(d[1]), a), std::min(q(d[2]), a), a);
        }
    }
    return out;
}

QVector<quint64> emittersOf(const Composition& comp, const CompNode& render)
{
    QVector<quint64> out;
    const auto it = render.inputs.constFind(nodeports::Input);
    if (it == render.inputs.constEnd() || !it->isConnected())
        return out;
    for (const Path& p : paths(comp, it->nodeId)) {
        if (!out.contains(p.emitter))
            out.append(p.emitter);
    }
    return out;
}

QImage renderNode(const Composition& comp, const CompNode& render, int64_t frame, QSize size,
    const SpriteFn& sprite)
{
    QImage blank(size, QImage::Format_ARGB32_Premultiplied);
    blank.fill(Qt::transparent);
    const auto it = render.inputs.constFind(nodeports::Input);
    if (size.isEmpty() || it == render.inputs.constEnd() || !it->isConnected()
        || it->outputIndex != 0)
        return blank;
    const double aspect = double(size.height()) / double(size.width());
    return draw(streamAt(comp, it->nodeId, frame, aspect), size, sprite);
}

} // namespace kk::render::particles
