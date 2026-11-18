// SPDX-License-Identifier: GPL-3.0-or-later
// CompScene3D — the 3D nodes of the Compositing page (Image Plane 3D, Shape3D, Text3D, Merge3D,
// Camera3D, the four lights and Renderer3D; spec 130): the scene a node graph describes, the
// matrix maths, and a small software rasterizer that draws it. Pure functions of the composition
// and the frame. The GPU and the CPU renderer both call renderNode(), so they draw the same
// pixels (like the particles, spec 129).
//
// Conventions: right-handed, X right, Y up, a camera looks along its own minus Z axis, one unit is
// the width of an Image Plane 3D of Size 1. A node's matrix is T * Rz * Ry * Rx * S (the X turn is
// applied first). Angles are degrees, counter-clockwise looking down an axis towards the origin.
#pragma once

#include "core/Composition.h"

#include <QImage>
#include <QMatrix4x4>
#include <QSize>
#include <QVector>
#include <QVector2D>
#include <QVector3D>

#include <cstdint>
#include <functional>

namespace kk::render::scene3d {

/// Z of the default camera, so that an Image Plane 3D of Size 1 at the centre fills the frame
/// exactly: 0.5 / tan(angle of view / 2) for a 50 mm lens on a 36 mm film back.
constexpr double kDefaultCameraZ = 25.0 / 18.0;
/// Horizontal angle of view of a 50 mm lens on a 36 mm film back, degrees.
constexpr double kDefaultAngleOfView = 39.597752;
/// Width of the film back the focal length refers to, millimetres.
constexpr double kFilmWidth = 36.0;

/// Surface of a mesh.
struct Material
{
    QVector3D color {1, 1, 1};
    float opacity = 1;
    float specular = 0;
    float shininess = 32;
};

/// A triangle mesh in its own space, placed in the scene by @c model.
struct Mesh
{
    QVector<QVector3D> positions;
    QVector<QVector3D> normals;
    QVector<QVector2D> uvs; ///< u to the right, v up
    QVector<quint32> indices; ///< three per triangle
    QMatrix4x4 model; ///< mesh space -> scene
    Material material;
    quint64 textureNode = 0; ///< the node whose Texture input is the picture (0 = none)
    QImage image; ///< a picture made by the node itself (Text3D), used when textureNode is 0
    bool textured = false;
};

enum class LightType : quint8 { Ambient, Directional, Point, Spot };

/// A light. It shines along its own minus Z axis (Directional and Spot); Point and Spot sit at the
/// translation of @c model.
struct Light
{
    LightType type = LightType::Ambient;
    QVector3D color {1, 1, 1};
    float intensity = 1;
    QMatrix4x4 model;
    float coneAngle = 30; ///< Spot: half angle, degrees
    float penumbra = 0.2f; ///< Spot: soft part of the cone, 0..1
};

/// A camera: @c model places it in the scene (scale ignored).
struct Camera
{
    QMatrix4x4 model;
    double angleOfView = kDefaultAngleOfView; ///< horizontal, degrees
    double nearClip = 0.05;
    double farClip = 1000;
};

struct Scene
{
    QVector<Mesh> meshes;
    QVector<Light> lights;
    QVector<Camera> cameras; ///< in the order found; the Renderer3D uses the first
};

/// How a Renderer3D draws.
struct RenderOptions
{
    bool lighting = true; ///< false, or no light in the scene: every surface shows its own colour
    bool blinnPhong = false; ///< Lambert otherwise
    int samples = 2; ///< per pixel side (1, 2 or 4)
};

/// The picture of a node's Texture input (premultiplied ARGB32; any size; null = none).
using TextureFn = std::function<QImage(quint64 node)>;

// ---- Matrices ----

/// T * Rz * Ry * Rx * S from the Translate / Rotate / Scale parameters at @p frame. @p withScale
/// false ignores the scale (cameras and lights).
[[nodiscard]] QMatrix4x4 transformMatrix(const ParamSet& p, int64_t frame, bool withScale = true);

/// Horizontal angle of view in degrees of a camera node's parameters (View By picks the angle or
/// the focal length).
[[nodiscard]] double angleOfViewOf(const ParamSet& p, int64_t frame);

/// Camera of node @p n at @p frame.
[[nodiscard]] Camera cameraOf(const CompNode& n, int64_t frame, const QMatrix4x4& parent);

/// Scene -> camera space (the inverse of the camera's placement, scale removed).
[[nodiscard]] QMatrix4x4 viewMatrix(const Camera& camera);

/// Camera space -> clip space for a picture of @p aspect = width / height.
[[nodiscard]] QMatrix4x4 projectionMatrix(const Camera& camera, double aspect);

/// The camera a Renderer3D without a Camera3D uses: at (0, 0, kDefaultCameraZ) looking at the origin.
[[nodiscard]] Camera defaultCamera();

// ---- Geometry ----

/// A plane of @p width x @p height centred on the origin in the XY plane, facing +Z.
[[nodiscard]] Mesh makePlane(double width, double height, int nx = 1, int ny = 1);

/// Shape3D's mesh: shape id (shapes3d::), @p size, subdivisions. Plane: size x size; cube: side
/// size; sphere: diameter size; cylinder and cone: diameter size, height size, along Y.
[[nodiscard]] Mesh makeShape(const QString& shape, double size, int subdivisions, int subdivisionsHeight);

// ---- Scene ----

/// The scene node @p sceneNode and everything under it describes at @p frame. @p size is the
/// picture the Renderer3D draws (the plane of an Image Plane 3D takes its aspect); @p frameSize and
/// @p scale are the composition's (Text3D lays text out as the Text node does).
[[nodiscard]] Scene buildScene(const Composition& comp, quint64 sceneNode, int64_t frame,
    QSize size, QSizeF frameSize, double scale);

/// The nodes (Image Plane 3D, Shape3D) whose Texture input the scene under @p sceneNode reads.
[[nodiscard]] QVector<quint64> textureNodes(const Composition& comp, quint64 sceneNode);

/// True for the node types that output a 3D scene.
[[nodiscard]] bool isSceneNode(const QString& type);

// ---- Rendering ----

/// @p scene seen by @p camera, drawn over transparent black: premultiplied ARGB32 of @p size.
[[nodiscard]] QImage rasterize(const Scene& scene, const Camera& camera, QSize size,
    const RenderOptions& options, const TextureFn& texture);

/// RenderOptions of a Renderer3D node's parameters.
[[nodiscard]] RenderOptions optionsOf(const CompNode& renderer);

/// The picture of Renderer3D node @p renderer at @p frame: its scene drawn at @p size through the
/// first camera in the scene. Transparent when nothing is connected.
[[nodiscard]] QImage renderNode(const Composition& comp, const CompNode& renderer, int64_t frame,
    QSize size, QSizeF frameSize, double scale, const TextureFn& texture);

// ---- The 3D viewer ----

/// The viewer's orbit camera: it looks at @c target from @c distance, turned @c yaw about Y and
/// tilted up by @c pitch (degrees).
struct Orbit
{
    double yaw = 30;
    double pitch = 20;
    double distance = 3;
    QVector3D target {0, 0, 0};

    friend bool operator==(const Orbit&, const Orbit&) = default;
};

/// Drag by (@p dx, @p dy) pixels turns the view: dragging right turns the scene to the right.
[[nodiscard]] Orbit orbited(Orbit o, double dx, double dy);
/// Middle-drag by (@p dx, @p dy) pixels of a picture @p viewportWidth wide moves the target so the
/// scene follows the mouse.
[[nodiscard]] Orbit panned(Orbit o, double dx, double dy, double viewportWidth);
/// Wheel: @p steps notches (positive moves in) change the distance by 10 % each.
[[nodiscard]] Orbit zoomed(Orbit o, double steps);
/// The camera of an orbit.
[[nodiscard]] Camera orbitCamera(const Orbit& o);

/// The viewer's orbit camera, shared by the viewers and read by the renderers (thread safe).
[[nodiscard]] Orbit viewerOrbit();
void setViewerOrbit(const Orbit& o);

/// What a viewer shows for a 3D node: the scene under @p sceneNode through the orbit camera
/// @p orbit on a dark background with a ground grid; lit only when the scene has lights.
[[nodiscard]] QImage viewerNode(const Composition& comp, quint64 sceneNode, int64_t frame,
    QSize size, QSizeF frameSize, double scale, const Orbit& orbit, const TextureFn& texture);

} // namespace kk::render::scene3d
