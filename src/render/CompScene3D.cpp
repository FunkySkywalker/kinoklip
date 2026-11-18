// SPDX-License-Identifier: GPL-3.0-or-later
// CompScene3D implementation (spec 130).
#include "render/CompScene3D.h"

#include "core/NodeRegistry.h"
#include "render/CompMath.h"
#include "render/TitleRenderer.h"

#include <QColor>
#include <QHash>
#include <QPainter>
#include <QPen>
#include <QSet>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace kk::render::scene3d {

namespace k = nodekeys;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxDepth = 64; ///< deepest Merge3D nesting followed
constexpr qint64 kBandSamples = 600000; ///< supersampled pixels a rasterizer band holds

double num(const ParamSet& p, const QString& key, int64_t frame, double fallback)
{
    return p.value(key, frame, fallback);
}

QVector3D colorOf(const ParamSet& p, const QString& key, const QColor& fallback)
{
    const QVariant v = p.staticValue(key, fallback);
    const QColor c = v.value<QColor>().isValid() ? v.value<QColor>() : fallback;
    return QVector3D(float(c.redF()), float(c.greenF()), float(c.blueF()));
}

// ---- Geometry helpers ----

void addGrid(Mesh& m, quint32 cols, quint32 rows)
{
    // The vertices of a (cols + 1) x (rows + 1) grid are the last ones added.
    const quint32 base = quint32(m.positions.size()) - (cols + 1) * (rows + 1);
    for (quint32 j = 0; j < rows; ++j) {
        for (quint32 i = 0; i < cols; ++i) {
            const quint32 a = base + j * (cols + 1) + i;
            const quint32 b = a + 1;
            const quint32 c = a + cols + 1;
            const quint32 d = c + 1;
            m.indices << a << b << d << a << d << c;
        }
    }
}

void addFace(Mesh& m, QVector3D normal, QVector3D u, QVector3D v, double size, int n)
{
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const double fu = double(i) / n, fv = double(j) / n;
            m.positions << normal * float(size / 2) + u * float((fu - 0.5) * size)
                    + v * float((fv - 0.5) * size);
            m.normals << normal;
            m.uvs << QVector2D(float(fu), float(fv));
        }
    }
    addGrid(m, quint32(n), quint32(n));
}

void addCap(Mesh& m, double y, double radius, int seg)
{
    // A fan: a centre vertex and a ring, facing up for y > 0 and down otherwise.
    const float ny = y > 0 ? 1.0f : -1.0f;
    const quint32 centre = quint32(m.positions.size());
    m.positions << QVector3D(0, float(y), 0);
    m.normals << QVector3D(0, ny, 0);
    m.uvs << QVector2D(0.5f, 0.5f);
    for (int i = 0; i <= seg; ++i) {
        const double a = 2 * kPi * i / seg;
        const float x = float(std::sin(a) * radius), z = float(std::cos(a) * radius);
        m.positions << QVector3D(x, float(y), z);
        m.normals << QVector3D(0, ny, 0);
        const double span = std::max(radius * 2, 1e-9);
        m.uvs << QVector2D(float(0.5 + x / span), float(0.5 - z / span));
    }
    for (int i = 0; i < seg; ++i)
        m.indices << centre << centre + 1 + quint32(i) << centre + 2 + quint32(i);
}

Mesh makeSphere(double size, int seg, int rings)
{
    Mesh m;
    const double r = size / 2;
    for (int j = 0; j <= rings; ++j) {
        const double theta = kPi * j / rings;
        for (int i = 0; i <= seg; ++i) {
            const double phi = 2 * kPi * i / seg;
            const QVector3D n(float(std::sin(theta) * std::sin(phi)), float(std::cos(theta)),
                float(std::sin(theta) * std::cos(phi)));
            m.positions << n * float(r);
            m.normals << n;
            m.uvs << QVector2D(float(double(i) / seg), float(1.0 - double(j) / rings));
        }
    }
    addGrid(m, quint32(seg), quint32(rings));
    return m;
}

/// A cylinder (@p cone false) or a cone (apex at the top) along Y, @p size across and high.
Mesh makeRound(double size, int seg, int rings, bool cone)
{
    Mesh m;
    const double r = size / 2, h = size;
    for (int j = 0; j <= rings; ++j) {
        const double t = double(j) / rings;
        const double y = (t - 0.5) * h;
        const double rad = cone ? r * (1 - t) : r;
        for (int i = 0; i <= seg; ++i) {
            const double phi = 2 * kPi * i / seg;
            const float s = float(std::sin(phi)), c = float(std::cos(phi));
            m.positions << QVector3D(float(rad) * s, float(y), float(rad) * c);
            m.normals << (cone ? QVector3D(s * float(h), float(r), c * float(h)).normalized()
                               : QVector3D(s, 0, c));
            m.uvs << QVector2D(float(double(i) / seg), float(t));
        }
    }
    addGrid(m, quint32(seg), quint32(rings));
    addCap(m, -h / 2, r, seg);
    if (!cone)
        addCap(m, h / 2, r, seg);
    return m;
}

// ---- Scene building ----

struct BuildCtx
{
    const Composition& comp;
    int64_t frame;
    QSize size;
    QSizeF frameSize;
    double scale;
    Scene scene;
};

void collect(BuildCtx& cx, quint64 id, const QMatrix4x4& parent, int depth);

void collectInputs(BuildCtx& cx, const CompNode& n, const QMatrix4x4& matrix, int depth)
{
    for (const QString& port : {nodeports::Scene1, nodeports::Scene2, nodeports::Scene3,
             nodeports::Scene4}) {
        const auto it = n.inputs.constFind(port);
        if (it != n.inputs.constEnd() && it->isConnected() && it->outputIndex == 0)
            collect(cx, it->nodeId, matrix, depth + 1);
    }
}

Material materialOf(const ParamSet& p, int64_t frame, bool withColor)
{
    Material m;
    if (withColor)
        m.color = colorOf(p, k::Color, QColor(Qt::white));
    m.opacity = float(std::clamp(num(p, k::Opacity, frame, 1.0), 0.0, 1.0));
    m.specular = float(std::clamp(num(p, k::Specular, frame, 0.0), 0.0, 1.0));
    m.shininess = float(std::clamp(num(p, k::Shininess, frame, 32.0), 1.0, 256.0));
    return m;
}

void collect(BuildCtx& cx, quint64 id, const QMatrix4x4& parent, int depth)
{
    const CompNode* n = cx.comp.node(id);
    if (!n || depth > kMaxDepth)
        return;
    const int64_t f = cx.frame;
    if (!n->enabled) {
        // A disabled node passes its first scene through (nothing for a node that has none).
        if (n->type == nodetypes::Merge3D) {
            const auto it = n->inputs.constFind(nodeports::Scene1);
            if (it != n->inputs.constEnd() && it->isConnected())
                collect(cx, it->nodeId, parent, depth + 1);
        }
        return;
    }
    if (n->type == nodetypes::Merge3D) {
        collectInputs(cx, *n, parent * transformMatrix(n->params, f), depth);
    } else if (n->type == nodetypes::ImagePlane3D) {
        if (!n->inputs.value(nodeports::Texture).isConnected())
            return; // no picture: nothing to show
        const double w = std::max(1e-6, num(n->params, k::Size, f, 1.0));
        const double aspect = cx.size.width() > 0 ? double(cx.size.height()) / cx.size.width() : 1.0;
        Mesh m = makePlane(w, w * aspect);
        m.model = parent * transformMatrix(n->params, f);
        m.material = materialOf(n->params, f, false);
        m.textureNode = n->id;
        m.textured = true;
        cx.scene.meshes << m;
    } else if (n->type == nodetypes::Shape3D) {
        Mesh m = makeShape(n->params.staticValue(k::Shape, shapes3d::Cube).toString(),
            std::max(1e-6, num(n->params, k::Size, f, 1.0)),
            int(std::lround(num(n->params, k::Subdivisions, f, 16))),
            int(std::lround(num(n->params, k::SubdivisionsHeight, f, 16))));
        m.model = parent * transformMatrix(n->params, f);
        m.material = materialOf(n->params, f, true);
        if (n->inputs.value(nodeports::Texture).isConnected()) {
            m.textureNode = n->id;
            m.textured = true;
        }
        cx.scene.meshes << m;
    } else if (n->type == nodetypes::Text3D) {
        QPointF offset;
        const TitleStyle style = compmath::textStyle(
            n->params, f, cx.size, cx.scale, cx.frameSize, &offset, nullptr);
        if (style.text.isEmpty())
            return;
        const TitleImage t = renderTitle(style);
        if (t.image.isNull())
            return;
        QImage canvas(cx.size, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        {
            QPainter p(&canvas);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(t.rect.translated(offset), t.image);
        }
        const double aspect = cx.size.width() > 0 ? double(cx.size.height()) / cx.size.width() : 1.0;
        Mesh m = makePlane(1.0, aspect);
        m.model = parent * transformMatrix(n->params, f);
        m.material = materialOf(n->params, f, false);
        m.image = canvas;
        m.textured = true;
        cx.scene.meshes << m;
    } else if (n->type == nodetypes::Camera3D) {
        cx.scene.cameras << cameraOf(*n, f, parent);
    } else if (n->type == nodetypes::AmbientLight || n->type == nodetypes::DirectionalLight
        || n->type == nodetypes::PointLight || n->type == nodetypes::SpotLight) {
        Light l;
        l.type = n->type == nodetypes::AmbientLight ? LightType::Ambient
            : n->type == nodetypes::DirectionalLight ? LightType::Directional
            : n->type == nodetypes::PointLight ? LightType::Point
                                                : LightType::Spot;
        l.color = colorOf(n->params, k::LightColor, QColor(Qt::white));
        l.intensity = float(std::max(0.0, num(n->params, k::Intensity, f,
            l.type == LightType::Ambient ? 0.3 : 1.0)));
        l.coneAngle = float(std::clamp(num(n->params, k::ConeAngle, f, 30.0), 1.0, 89.0));
        l.penumbra = float(std::clamp(num(n->params, k::Penumbra, f, 0.2), 0.0, 1.0));
        l.model = parent * transformMatrix(n->params, f, false);
        cx.scene.lights << l;
    }
}

// ---- Rasterizer ----

struct Vtx
{
    QVector4D clip;
    QVector3D world;
    QVector3D normal;
    QVector2D uv;
};

struct Tri
{
    Vtx v[3];
    int mesh = 0;
    float depth = 0; ///< distance along the view axis of the nearest corner (sort key)
};

Vtx mixVtx(const Vtx& a, const Vtx& b, float t)
{
    Vtx r;
    r.clip = a.clip + (b.clip - a.clip) * t;
    r.world = a.world + (b.world - a.world) * t;
    r.normal = a.normal + (b.normal - a.normal) * t;
    r.uv = a.uv + (b.uv - a.uv) * t;
    return r;
}

/// One triangle clipped to the near plane (z >= -w): 0, 1 or 2 triangles into @p out.
void clipNear(const Tri& in, QVector<Tri>& out)
{
    std::array<Vtx, 4> poly;
    int count = 0;
    for (int i = 0; i < 3; ++i) {
        const Vtx& a = in.v[i];
        const Vtx& b = in.v[(i + 1) % 3];
        const float da = a.clip.z() + a.clip.w();
        const float db = b.clip.z() + b.clip.w();
        if (da >= 0 && count < 4)
            poly[size_t(count++)] = a;
        if ((da >= 0) != (db >= 0) && count < 4)
            poly[size_t(count++)] = mixVtx(a, b, da / (da - db));
    }
    for (int i = 1; i + 1 < count; ++i) {
        Tri t = in;
        t.v[0] = poly[0];
        t.v[1] = poly[size_t(i)];
        t.v[2] = poly[size_t(i + 1)];
        out << t;
    }
}

/// A texture: premultiplied ARGB32 sampled bilinearly, clamped at the edges.
struct Texture
{
    QImage image;
    int w = 0, h = 0;

    void sample(float u, float v, float rgba[4]) const
    {
        const float x = u * float(w) - 0.5f;
        const float y = (1.0f - v) * float(h) - 0.5f;
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const float fx = x - float(x0), fy = y - float(y0);
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
        for (int j = 0; j < 2; ++j) {
            const int yy = std::clamp(y0 + j, 0, h - 1);
            const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(yy));
            for (int i = 0; i < 2; ++i) {
                const int xx = std::clamp(x0 + i, 0, w - 1);
                const float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                const QRgb c = row[xx];
                rgba[0] += wgt * float(qRed(c));
                rgba[1] += wgt * float(qGreen(c));
                rgba[2] += wgt * float(qBlue(c));
                rgba[3] += wgt * float(qAlpha(c));
            }
        }
        for (int c = 0; c < 4; ++c)
            rgba[c] *= 1.0f / 255.0f;
    }
};

struct LightWorld
{
    Light light;
    QVector3D position; ///< Point, Spot
    QVector3D direction; ///< Directional, Spot: the way the light travels (unit)
    float cosOuter = 0, cosInner = 0;
};

struct ScreenVtx
{
    double x, y, z, iw;
    QVector3D world, normal; ///< divided by w
    QVector2D uv; ///< divided by w
};

struct MeshCtx
{
    const Mesh* mesh = nullptr;
    Texture texture;
    bool hasTexture = false;
};

struct Frame
{
    int width = 0, height = 0; ///< supersampled size
    QVector3D eye;
    const QVector<LightWorld>* lights = nullptr;
    bool lit = false;
    bool blinn = false;
};

float lightFactor(const Frame& fr, const Material& mat, const QVector3D& P, QVector3D N,
    QVector3D rgbOut[2])
{
    // rgbOut[0] receives the diffuse + ambient light, rgbOut[1] the specular highlight.
    const QVector3D V = (fr.eye - P).normalized();
    if (QVector3D::dotProduct(N, V) < 0)
        N = -N;
    QVector3D diffuse(0, 0, 0), specular(0, 0, 0);
    for (const LightWorld& lw : *fr.lights) {
        const Light& l = lw.light;
        const QVector3D lc = l.color * l.intensity;
        if (l.type == LightType::Ambient) {
            diffuse += lc;
            continue;
        }
        QVector3D L;
        float atten = 1;
        if (l.type == LightType::Directional) {
            L = -lw.direction;
        } else {
            L = lw.position - P;
            if (L.lengthSquared() < 1e-12f)
                continue;
            L.normalize();
            if (l.type == LightType::Spot) {
                const float cosA = QVector3D::dotProduct(-L, lw.direction);
                const float t = std::clamp(
                    (cosA - lw.cosOuter) / std::max(lw.cosInner - lw.cosOuter, 1e-6f), 0.0f, 1.0f);
                atten = t * t * (3 - 2 * t);
            }
        }
        const float nl = QVector3D::dotProduct(N, L);
        if (nl <= 0 || atten <= 0)
            continue;
        diffuse += lc * (nl * atten);
        if (fr.blinn && mat.specular > 0) {
            const QVector3D H = (L + V).normalized();
            const float nh = std::max(0.0f, QVector3D::dotProduct(N, H));
            specular += lc * (std::pow(nh, mat.shininess) * mat.specular * atten);
        }
    }
    rgbOut[0] = diffuse;
    rgbOut[1] = specular;
    return 1;
}

/// Draws the triangles of one band (supersampled rows [y0, y1)) into @p color / @p depth.
void drawBand(const Frame& fr, const QVector<Tri>& tris, const QVector<MeshCtx>& meshes, int y0,
    int y1, std::vector<float>& color, std::vector<float>& depth)
{
    const int W = fr.width;
    std::fill(color.begin(), color.end(), 0.0f);
    std::fill(depth.begin(), depth.end(), 1e30f);
    for (const Tri& t : tris) {
        const MeshCtx& mc = meshes[t.mesh];
        ScreenVtx s[3];
        for (int i = 0; i < 3; ++i) {
            const Vtx& v = t.v[i];
            const double iw = 1.0 / double(v.clip.w());
            s[i].x = (double(v.clip.x()) * iw * 0.5 + 0.5) * W;
            s[i].y = (1.0 - (double(v.clip.y()) * iw * 0.5 + 0.5)) * fr.height;
            s[i].z = double(v.clip.z()) * iw;
            s[i].iw = iw;
            s[i].world = v.world * float(iw);
            s[i].normal = v.normal * float(iw);
            s[i].uv = v.uv * float(iw);
        }
        const double minY = std::min({s[0].y, s[1].y, s[2].y});
        const double maxY = std::max({s[0].y, s[1].y, s[2].y});
        const double minX = std::min({s[0].x, s[1].x, s[2].x});
        const double maxX = std::max({s[0].x, s[1].x, s[2].x});
        const int ya = std::max(y0, int(std::floor(minY - 0.5)));
        const int yb = std::min(y1 - 1, int(std::ceil(maxY - 0.5)));
        const int xa = std::max(0, int(std::floor(minX - 0.5)));
        const int xb = std::min(W - 1, int(std::ceil(maxX - 0.5)));
        if (ya > yb || xa > xb)
            continue;
        const auto edge = [](const ScreenVtx& a, const ScreenVtx& b, double px, double py) {
            return (px - a.x) * (b.y - a.y) - (py - a.y) * (b.x - a.x);
        };
        double area = edge(s[0], s[1], s[2].x, s[2].y);
        if (std::abs(area) < 1e-12)
            continue;
        const ScreenVtx* a = &s[0];
        const ScreenVtx* b = &s[1];
        const ScreenVtx* c = &s[2];
        if (area < 0) {
            std::swap(b, c);
            area = -area;
        }
        // Inclusive edges: the top and left ones only (no pixel is drawn twice).
        const auto owns = [](const ScreenVtx& p, const ScreenVtx& q) {
            const double dy = q.y - p.y, dx = q.x - p.x;
            return dy > 0 || (dy == 0 && dx < 0);
        };
        const bool own0 = owns(*b, *c), own1 = owns(*c, *a), own2 = owns(*a, *b);
        const float alphaBase = mc.mesh->material.opacity;
        for (int y = ya; y <= yb; ++y) {
            for (int x = xa; x <= xb; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double e0 = edge(*b, *c, px, py);
                const double e1 = edge(*c, *a, px, py);
                const double e2 = edge(*a, *b, px, py);
                if (e0 < 0 || e1 < 0 || e2 < 0 || (e0 == 0 && !own0) || (e1 == 0 && !own1)
                    || (e2 == 0 && !own2))
                    continue;
                const double l0 = e0 / area, l1 = e1 / area, l2 = e2 / area;
                const double z = l0 * a->z + l1 * b->z + l2 * c->z;
                if (z > 1.0)
                    continue; // beyond the far plane
                const size_t di = size_t(y - y0) * size_t(W) + size_t(x);
                if (z >= double(depth[di]))
                    continue;
                const double iw = l0 * a->iw + l1 * b->iw + l2 * c->iw;
                const float inv = float(1.0 / iw);
                const QVector3D P = (a->world * float(l0) + b->world * float(l1) + c->world * float(l2)) * inv;
                float tex[4] = {1, 1, 1, 1};
                if (mc.hasTexture) {
                    const QVector2D uv = (a->uv * float(l0) + b->uv * float(l1) + c->uv * float(l2)) * inv;
                    mc.texture.sample(uv.x(), uv.y(), tex);
                }
                const Material& mat = mc.mesh->material;
                const float alpha = tex[3] * alphaBase;
                if (alpha <= 0)
                    continue;
                float rgb[3] = {tex[0] * mat.color.x() * alphaBase, tex[1] * mat.color.y() * alphaBase,
                    tex[2] * mat.color.z() * alphaBase};
                if (fr.lit) {
                    const QVector3D N = ((a->normal * float(l0) + b->normal * float(l1)
                                             + c->normal * float(l2))
                        * inv).normalized();
                    QVector3D lum[2];
                    lightFactor(fr, mat, P, N, lum);
                    rgb[0] = rgb[0] * lum[0].x() + lum[1].x() * alpha;
                    rgb[1] = rgb[1] * lum[0].y() + lum[1].y() * alpha;
                    rgb[2] = rgb[2] * lum[0].z() + lum[1].z() * alpha;
                }
                float* dst = &color[di * 4];
                const float keep = 1.0f - alpha;
                dst[0] = rgb[0] + dst[0] * keep;
                dst[1] = rgb[1] + dst[1] * keep;
                dst[2] = rgb[2] + dst[2] * keep;
                dst[3] = alpha + dst[3] * keep;
                if (alpha >= 0.999f)
                    depth[di] = float(z);
            }
        }
    }
}

quint8 to8(float v)
{
    return quint8(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

} // namespace

// ---- Matrices ----

QMatrix4x4 transformMatrix(const ParamSet& p, int64_t frame, bool withScale)
{
    QMatrix4x4 m;
    m.translate(float(num(p, k::TranslateX, frame, 0)), float(num(p, k::TranslateY, frame, 0)),
        float(num(p, k::TranslateZ, frame, 0)));
    m.rotate(float(num(p, k::RotateZ, frame, 0)), 0, 0, 1);
    m.rotate(float(num(p, k::RotateY, frame, 0)), 0, 1, 0);
    m.rotate(float(num(p, k::RotateX, frame, 0)), 1, 0, 0);
    if (withScale) {
        const double s = num(p, k::ScaleUniform, frame, 1);
        m.scale(float(s * num(p, k::ScaleX, frame, 1)), float(s * num(p, k::ScaleY, frame, 1)),
            float(s * num(p, k::ScaleZ, frame, 1)));
    }
    return m;
}

double angleOfViewOf(const ParamSet& p, int64_t frame)
{
    if (p.staticValue(k::ViewBy, cameraviews::AngleOfView).toString() == cameraviews::FocalLength) {
        const double f = std::max(0.01, num(p, k::FocalLength, frame, 50.0));
        return 2.0 * std::atan(kFilmWidth / 2.0 / f) * 180.0 / kPi;
    }
    return std::clamp(num(p, k::AngleOfView, frame, kDefaultAngleOfView), 1.0, 170.0);
}

Camera cameraOf(const CompNode& n, int64_t frame, const QMatrix4x4& parent)
{
    Camera c;
    c.model = parent * transformMatrix(n.params, frame, false);
    c.angleOfView = angleOfViewOf(n.params, frame);
    c.nearClip = std::max(1e-5, num(n.params, k::NearClip, frame, 0.05));
    c.farClip = std::max(c.nearClip * 1.001, num(n.params, k::FarClip, frame, 1000));
    return c;
}

QMatrix4x4 viewMatrix(const Camera& camera)
{
    // The camera's axes without any scale it inherited: the inverse of a rotation and a move.
    QVector3D x = camera.model.column(0).toVector3D().normalized();
    QVector3D y = camera.model.column(1).toVector3D().normalized();
    QVector3D z = camera.model.column(2).toVector3D().normalized();
    const QVector3D t = camera.model.column(3).toVector3D();
    QMatrix4x4 v(x.x(), x.y(), x.z(), -QVector3D::dotProduct(x, t), y.x(), y.y(), y.z(),
        -QVector3D::dotProduct(y, t), z.x(), z.y(), z.z(), -QVector3D::dotProduct(z, t), 0, 0, 0, 1);
    return v;
}

QMatrix4x4 projectionMatrix(const Camera& camera, double aspect)
{
    const double f = 1.0 / std::tan(camera.angleOfView * kPi / 360.0);
    const double n = camera.nearClip, fa = camera.farClip;
    return QMatrix4x4(float(f), 0, 0, 0, 0, float(f * aspect), 0, 0, 0, 0, float((fa + n) / (n - fa)),
        float(2 * fa * n / (n - fa)), 0, 0, -1, 0);
}

Camera defaultCamera()
{
    Camera c;
    c.model.translate(0, 0, float(kDefaultCameraZ));
    return c;
}

// ---- Geometry ----

Mesh makePlane(double width, double height, int nx, int ny)
{
    Mesh m;
    nx = std::max(1, nx);
    ny = std::max(1, ny);
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            const double fu = double(i) / nx, fv = double(j) / ny;
            m.positions << QVector3D(float((fu - 0.5) * width), float((fv - 0.5) * height), 0);
            m.normals << QVector3D(0, 0, 1);
            m.uvs << QVector2D(float(fu), float(fv));
        }
    }
    addGrid(m, quint32(nx), quint32(ny));
    return m;
}

Mesh makeShape(const QString& shape, double size, int subdivisions, int subdivisionsHeight)
{
    const int n = std::clamp(subdivisions, 1, 128);
    const int h = std::clamp(subdivisionsHeight, 1, 128);
    if (shape == shapes3d::Plane)
        return makePlane(size, size, n, n);
    if (shape == shapes3d::Sphere)
        return makeSphere(size, std::max(3, n), std::max(2, h));
    if (shape == shapes3d::Cylinder)
        return makeRound(size, std::max(3, n), h, false);
    if (shape == shapes3d::Cone)
        return makeRound(size, std::max(3, n), h, true);
    Mesh m; // cube
    addFace(m, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, size, n);
    addFace(m, {0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, size, n);
    addFace(m, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, size, n);
    addFace(m, {-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, size, n);
    addFace(m, {0, 1, 0}, {1, 0, 0}, {0, 0, -1}, size, n);
    addFace(m, {0, -1, 0}, {1, 0, 0}, {0, 0, 1}, size, n);
    return m;
}

// ---- Scene ----

bool isSceneNode(const QString& type)
{
    return type == nodetypes::ImagePlane3D || type == nodetypes::Shape3D
        || type == nodetypes::Text3D || type == nodetypes::Merge3D || type == nodetypes::Camera3D
        || type == nodetypes::AmbientLight || type == nodetypes::DirectionalLight
        || type == nodetypes::PointLight || type == nodetypes::SpotLight;
}

Scene buildScene(const Composition& comp, quint64 sceneNode, int64_t frame, QSize size,
    QSizeF frameSize, double scale)
{
    BuildCtx cx {comp, frame, size, frameSize, scale, {}};
    collect(cx, sceneNode, QMatrix4x4(), 0);
    return std::move(cx.scene);
}

QVector<quint64> textureNodes(const Composition& comp, quint64 sceneNode)
{
    QVector<quint64> out;
    QVector<quint64> todo {sceneNode};
    QSet<quint64> seen;
    while (!todo.isEmpty()) {
        const quint64 id = todo.takeLast();
        const CompNode* n = comp.node(id);
        if (!n || seen.contains(id))
            continue;
        seen.insert(id);
        if ((n->type == nodetypes::ImagePlane3D || n->type == nodetypes::Shape3D)
            && n->inputs.value(nodeports::Texture).isConnected())
            out << id;
        for (const QString& port : {nodeports::Scene1, nodeports::Scene2, nodeports::Scene3,
                 nodeports::Scene4}) {
            const NodeLink l = n->inputs.value(port);
            if (l.isConnected())
                todo << l.nodeId;
        }
    }
    return out;
}

// ---- Rendering ----

QImage rasterize(const Scene& scene, const Camera& camera, QSize size, const RenderOptions& options,
    const TextureFn& textureFn)
{
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    if (size.isEmpty() || scene.meshes.isEmpty())
        return out;
    int S = options.samples >= 4 ? 4 : options.samples >= 2 ? 2 : 1;
    // Huge pictures at a high sample count are drawn with fewer samples instead of exhausting memory.
    while (S > 1 && qint64(size.width()) * size.height() * S * S > 40000000)
        S /= 2;
    Frame fr;
    fr.width = size.width() * S;
    fr.height = size.height() * S;
    const QMatrix4x4 view = viewMatrix(camera);
    const QMatrix4x4 proj = projectionMatrix(camera, double(size.width()) / size.height());
    const QMatrix4x4 viewProj = proj * view;
    fr.eye = camera.model.column(3).toVector3D();
    QVector<LightWorld> lights;
    for (const Light& l : scene.lights) {
        LightWorld w;
        w.light = l;
        w.position = l.model.column(3).toVector3D();
        w.direction = (-l.model.column(2).toVector3D()).normalized();
        w.cosOuter = float(std::cos(double(l.coneAngle) * kPi / 180.0));
        w.cosInner = float(std::cos(double(l.coneAngle) * (1.0 - double(l.penumbra)) * kPi / 180.0));
        lights << w;
    }
    fr.lights = &lights;
    fr.lit = options.lighting && !lights.isEmpty();
    fr.blinn = options.blinnPhong;

    // Meshes: textures fetched once, vertices transformed once.
    QVector<MeshCtx> meshes(scene.meshes.size());
    QHash<quint64, QImage> fetched;
    QVector<Tri> tris;
    for (int mi = 0; mi < scene.meshes.size(); ++mi) {
        const Mesh& m = scene.meshes[mi];
        MeshCtx& mc = meshes[mi];
        mc.mesh = &m;
        if (m.textured) {
            QImage img = m.image;
            if (m.textureNode) {
                const auto it = fetched.constFind(m.textureNode);
                if (it != fetched.constEnd())
                    img = *it;
                else {
                    img = textureFn ? textureFn(m.textureNode) : QImage();
                    fetched.insert(m.textureNode, img);
                }
            }
            if (!img.isNull()) {
                if (img.format() != QImage::Format_ARGB32_Premultiplied)
                    img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                mc.texture.image = img;
                mc.texture.w = img.width();
                mc.texture.h = img.height();
                mc.hasTexture = true;
            } else if (m.textureNode) {
                continue; // its picture is missing: the object is not drawn
            }
        }
        const QMatrix4x4 mvp = viewProj * m.model;
        const QMatrix4x4 nm = m.model.inverted().transposed();
        QVector<Vtx> vs(m.positions.size());
        for (qsizetype i = 0; i < vs.size(); ++i) {
            vs[i].clip = mvp.map(QVector4D(m.positions[i], 1));
            vs[i].world = m.model.map(m.positions[i]);
            vs[i].normal = nm.mapVector(m.normals[i]).normalized();
            vs[i].uv = m.uvs[i];
        }
        for (qsizetype i = 0; i + 2 < m.indices.size(); i += 3) {
            Tri t;
            t.mesh = mi;
            t.v[0] = vs[m.indices[i]];
            t.v[1] = vs[m.indices[i + 1]];
            t.v[2] = vs[m.indices[i + 2]];
            // Wholly outside one side of the view volume: dropped now.
            bool out2 = false;
            for (int plane = 0; plane < 4 && !out2; ++plane) {
                bool all = true;
                for (int v = 0; v < 3 && all; ++v) {
                    const QVector4D& c = t.v[v].clip;
                    const float comp = plane < 2 ? c.x() : c.y();
                    all = plane % 2 == 0 ? comp < -c.w() : comp > c.w();
                }
                out2 = all;
            }
            if (out2)
                continue;
            const size_t before = size_t(tris.size());
            clipNear(t, tris);
            for (size_t q = before; q < size_t(tris.size()); ++q) {
                Tri& c = tris[qsizetype(q)];
                c.depth = std::min({c.v[0].clip.w(), c.v[1].clip.w(), c.v[2].clip.w()});
            }
        }
    }
    // Far first, so translucent pixels blend over what is behind; the depth buffer orders the rest.
    std::stable_sort(tris.begin(), tris.end(),
        [](const Tri& a, const Tri& b) { return a.depth > b.depth; });

    // Horizontal bands of the (supersampled) picture, drawn in parallel.
    const int bandRows = std::max(S, int(kBandSamples / std::max(1, fr.width)) / S * S);
    QVector<int> bands;
    for (int y = 0; y < fr.height; y += bandRows)
        bands << y;
    QtConcurrent::blockingMap(bands, [&](int y0) {
        const int y1 = std::min(fr.height, y0 + bandRows);
        std::vector<float> color(size_t(y1 - y0) * size_t(fr.width) * 4);
        std::vector<float> depth(size_t(y1 - y0) * size_t(fr.width));
        drawBand(fr, tris, meshes, y0, y1, color, depth);
        const float norm = 1.0f / float(S * S);
        for (int oy = y0 / S; oy < y1 / S; ++oy) {
            auto* dst = reinterpret_cast<QRgb*>(out.scanLine(oy));
            for (int ox = 0; ox < size.width(); ++ox) {
                float acc[4] = {0, 0, 0, 0};
                for (int sy = 0; sy < S; ++sy) {
                    for (int sx = 0; sx < S; ++sx) {
                        const float* px
                            = &color[(size_t(oy * S + sy - y0) * size_t(fr.width) + size_t(ox * S + sx)) * 4];
                        for (int c = 0; c < 4; ++c)
                            acc[c] += px[c];
                    }
                }
                const quint8 a = to8(acc[3] * norm);
                dst[ox] = qRgba(std::min(to8(acc[0] * norm), a), std::min(to8(acc[1] * norm), a),
                    std::min(to8(acc[2] * norm), a), a);
            }
        }
    });
    return out;
}

RenderOptions optionsOf(const CompNode& renderer)
{
    RenderOptions o;
    o.lighting = renderer.params.staticValue(k::Lighting, true).toBool();
    o.blinnPhong
        = renderer.params.staticValue(k::Shading, shadings3d::Lambert).toString() == shadings3d::BlinnPhong;
    o.samples = renderer.params.staticValue(k::AntiAlias, QStringLiteral("2")).toString().toInt();
    return o;
}

QImage renderNode(const Composition& comp, const CompNode& renderer, int64_t frame, QSize size,
    QSizeF frameSize, double scale, const TextureFn& texture)
{
    QImage blank(size, QImage::Format_ARGB32_Premultiplied);
    blank.fill(Qt::transparent);
    const NodeLink link = renderer.inputs.value(nodeports::Input);
    if (!link.isConnected() || size.isEmpty())
        return blank;
    const Scene scene = buildScene(comp, link.nodeId, frame, size, frameSize, scale);
    const Camera camera = scene.cameras.isEmpty() ? defaultCamera() : scene.cameras.first();
    return rasterize(scene, camera, size, optionsOf(renderer), texture);
}

// ---- The 3D viewer ----

namespace {
std::mutex g_orbitLock;
Orbit g_orbit;
} // namespace

Orbit orbited(Orbit o, double dx, double dy)
{
    o.yaw -= dx * 0.5;
    o.pitch = std::clamp(o.pitch + dy * 0.5, -89.0, 89.0);
    return o;
}

Orbit panned(Orbit o, double dx, double dy, double viewportWidth)
{
    const Camera cam = orbitCamera(o);
    const double perPixel
        = 2.0 * o.distance * std::tan(cam.angleOfView * kPi / 360.0) / std::max(1.0, viewportWidth);
    const QVector3D right = cam.model.column(0).toVector3D().normalized();
    const QVector3D up = cam.model.column(1).toVector3D().normalized();
    o.target += float(perPixel) * (-float(dx) * right + float(dy) * up);
    return o;
}

Orbit zoomed(Orbit o, double steps)
{
    o.distance = std::clamp(o.distance * std::pow(0.9, steps), 0.05, 5000.0);
    return o;
}

Camera orbitCamera(const Orbit& o)
{
    const double y = o.yaw * kPi / 180.0, p = o.pitch * kPi / 180.0;
    const QVector3D eye = o.target
        + float(o.distance) * QVector3D(float(std::cos(p) * std::sin(y)), float(std::sin(p)),
              float(std::cos(p) * std::cos(y)));
    QMatrix4x4 view;
    view.lookAt(eye, o.target, QVector3D(0, 1, 0));
    Camera c;
    c.model = view.inverted();
    c.nearClip = 0.02;
    c.farClip = 5000;
    return c;
}

Orbit viewerOrbit()
{
    std::lock_guard<std::mutex> lock(g_orbitLock);
    return g_orbit;
}

void setViewerOrbit(const Orbit& o)
{
    std::lock_guard<std::mutex> lock(g_orbitLock);
    g_orbit = o;
}

QImage viewerNode(const Composition& comp, quint64 sceneNode, int64_t frame, QSize size,
    QSizeF frameSize, double scale, const Orbit& orbit, const TextureFn& texture)
{
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    out.fill(QColor(36, 36, 40));
    if (size.isEmpty())
        return out;
    const Camera camera = orbitCamera(orbit);
    const QMatrix4x4 viewProj
        = projectionMatrix(camera, double(size.width()) / size.height()) * viewMatrix(camera);
    {
        // The ground grid: lines every unit on the XZ plane, the X and Z axes coloured.
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing);
        const auto line = [&](QVector3D a, QVector3D b, const QColor& color) {
            QVector4D ca = viewProj.map(QVector4D(a, 1)), cb = viewProj.map(QVector4D(b, 1));
            const float da = ca.z() + ca.w(), db = cb.z() + cb.w();
            if (da < 0 && db < 0)
                return;
            if (da < 0)
                ca = ca + (cb - ca) * (da / (da - db));
            else if (db < 0)
                cb = cb + (ca - cb) * (db / (db - da));
            const auto toPx = [&](const QVector4D& c) {
                return QPointF((c.x() / c.w() * 0.5 + 0.5) * size.width(),
                    (1.0 - (c.y() / c.w() * 0.5 + 0.5)) * size.height());
            };
            p.setPen(QPen(color, 1));
            p.drawLine(toPx(ca), toPx(cb));
        };
        constexpr int kHalf = 10;
        for (int i = -kHalf; i <= kHalf; ++i) {
            const QColor grid(86, 86, 94);
            line({float(i), 0, -kHalf}, {float(i), 0, kHalf}, i == 0 ? QColor(70, 100, 190) : grid);
            line({-kHalf, 0, float(i)}, {kHalf, 0, float(i)}, i == 0 ? QColor(190, 80, 80) : grid);
        }
    }
    const Scene scene = buildScene(comp, sceneNode, frame, size, frameSize, scale);
    RenderOptions options;
    options.lighting = true;
    options.samples = 2;
    const QImage drawn = rasterize(scene, camera, size, options, texture);
    QPainter p(&out);
    p.drawImage(0, 0, drawn);
    return out;
}

} // namespace kk::render::scene3d
