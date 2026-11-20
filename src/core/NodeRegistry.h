// SPDX-License-Identifier: GPL-3.0-or-later
// NodeRegistry — the catalogue of Compositing page node types (docs/compositing-page.md):
// ports, parameters (reusing ParamDescriptor) and viewer controls. Descriptors only; the
// pixel code is in src/render (GlComposition, CpuComposition), keyed by descriptor id.
#pragma once

#include "core/EffectRegistry.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace kk {

/// What a port carries. Image: a premultiplied RGBA picture. Mask: a matte (its alpha is
/// used; mask nodes output white with alpha = coverage).
enum class PortKind : quint8 {
    Image,
    Mask,
    Particles, ///< a stream of particles (pEmitter, pGravity, ...): connects only to particle inputs
    Scene3D ///< a 3D scene (Image Plane 3D, Merge3D, Camera3D, ...): connects only to 3D inputs (spec 130)
};

/// One input port of a node type.
struct NodePort
{
    QString id; ///< key in CompNode::inputs ("background", "foreground", "mask", "input")
    QString label; ///< translated
    PortKind kind = PortKind::Image;
    /// The node needs it to produce anything useful (drawn highlighted). Evaluation never
    /// fails: an unconnected port reads as a transparent picture / an empty mask... except a
    /// "mask" port, which reads as "everywhere" when unconnected.
    bool required = false;
    /// The picture is only analysed (the Tracker's tracking source), never drawn into the
    /// node's output: evaluation of the node does not pull it.
    bool analysisOnly = false;
};

/// Node library sections (Nodes panel, Select Node list).
enum class NodeCategory : quint8 {
    InputOutput, ///< MediaIn, MediaOut
    Composite, ///< Merge, Dissolve
    Transform, ///< Transform, Crop, Resize, Letterbox
    Generator, ///< Background, Text, Fast Noise
    Mask, ///< Rectangle, Ellipse, Polygon
    Color, ///< Color Corrector, Channel Booleans
    Matte, ///< Luma Keyer, Delta Keyer, Ultra Keyer, Matte Control
    Filter, ///< Soft Glow, Light Wrap
    Warp, ///< Grid Warp, Displace, Vector Distort
    Time, ///< Time Speed, Time Stretcher
    Particles, ///< pEmitter, pGravity, pTurbulence, pMerge, pRender
    ThreeD, ///< Image Plane 3D, Shape3D, Text3D, Merge3D, Camera3D, lights, Renderer3D
    Paint, ///< Paint
    Effect ///< every EffectRegistry video effect ("fx:<id>")
};

/// On-viewer controls the Compositing page viewers draw for the selected node.
enum class ViewerControls : quint8 {
    None,
    /// Centre / size / angle handles: nodekeys::CenterX, CenterY, Size, Angle (Transform,
    /// Merge (the foreground), Text (centre only), Polygon (whole shape)).
    Transform,
    /// A rotated box: CenterX, CenterY, Width, Height, Angle (Rectangle and Ellipse masks).
    Rect,
    /// The editable point path CompNode::path plus the Transform handles (Polygon mask).
    Polygon,
    /// The tracked point (CenterX, CenterY) with its pattern and search boxes (PatternSize,
    /// SearchSize): Tracker.
    Tracker,
    /// The four corner points of a region (nodekeys::planarCorner()) joined by a quad, each
    /// corner draggable, the whole quad movable: Planar Tracker.
    Planar,
    /// The control grid of Grid Warp: every grid point draggable (nodekeys::gridKey()).
    Grid,
    /// Bezier mask: the points with tangent handles, plus the whole shape's centre, size and
    /// angle (CompNode::path).
    Bezier,
    /// Text: the centre and angle of the block, and, with the Path layout, the path's points
    /// with tangent handles (CompNode::path).
    TextPath,
    /// Paint (spec 131): the viewer takes drags as brush strokes (Alt-click sets the Clone source).
    Paint
};

/// Describes one node type.
///
/// Coordinate conventions of every built-in node (like a Resolve user knows them from the
/// node compositor): positions are normalised frame coordinates, x 0..1 from left to right
/// and y 0..1 from BOTTOM to TOP (0.5, 0.5 = the centre of the frame); sizes are factors
/// (1 = unchanged); widths and heights of shapes are fractions of the frame width / height;
/// soft edges are fractions of the frame width; angles are degrees counter-clockwise. Effect
/// nodes ("fx:") keep their Edit page units (pixel sizes are timeline pixels).
struct NodeDescriptor
{
    QString id; ///< stable type id stored in CompNode::type ("merge", "fx:gaussian_blur")
    QString name; ///< translated display name ("Merge", "Gaussian Blur")
    QString shortName; ///< 2-4 letters for small tiles and the toolbar ("Mrg", "Blur")
    /// Base of new node names (name without spaces, untranslated: "Merge" -> "Merge1").
    QString nameBase;
    NodeCategory category = NodeCategory::Effect;
    QString description; ///< one-line tooltip
    /// Effect nodes: the effects library folder (effectgroups::); empty otherwise.
    QString group;
    /// Effect nodes: the EffectRegistry id ("gaussian_blur"); empty otherwise.
    QString effectId;
    QVector<NodePort> inputs; ///< in display order
    /// Port whose image a disabled node passes through, and that "add after the selected
    /// node" connects to; empty for nodes without an image input (generators, MediaIn).
    QString mainInput;
    int outputs = 1; ///< number of outputs (0 for MediaOut)
    PortKind outputKind = PortKind::Image; ///< Mask for the mask nodes
    QVector<ParamDescriptor> params;
    ViewerControls controls = ViewerControls::None;

    /// Port with id @p id, or nullptr.
    [[nodiscard]] const NodePort* input(const QString& id) const;
    /// Parameter descriptor with key @p key, or nullptr.
    [[nodiscard]] const ParamDescriptor* param(const QString& key) const;
    /// True for nodes that make a picture without an image input (Background, Text, noise,
    /// masks, MediaIn): adding one after a selected node merges it over that node.
    [[nodiscard]] bool isSource() const
    {
        return mainInput.isEmpty() || outputKind == PortKind::Mask;
    }
};

/// Well-known node type ids.
namespace nodetypes {
inline const QString MediaIn = QStringLiteral("media_in");
inline const QString MediaOut = QStringLiteral("media_out");
inline const QString Group = QStringLiteral("group"); ///< Group / Macro (spec 120)
inline const QString Merge = QStringLiteral("merge");
inline const QString Dissolve = QStringLiteral("dissolve");
inline const QString Transform = QStringLiteral("transform");
inline const QString Crop = QStringLiteral("crop");
inline const QString Resize = QStringLiteral("resize");
inline const QString Letterbox = QStringLiteral("letterbox");
inline const QString Tracker = QStringLiteral("tracker");
inline const QString PlanarTracker = QStringLiteral("planar_tracker");
inline const QString Background = QStringLiteral("background");
inline const QString Text = QStringLiteral("text");
inline const QString FastNoise = QStringLiteral("fast_noise");
inline const QString RectangleMask = QStringLiteral("rectangle_mask");
inline const QString EllipseMask = QStringLiteral("ellipse_mask");
inline const QString PolygonMask = QStringLiteral("polygon_mask");
inline const QString BSplineMask = QStringLiteral("bspline_mask");
inline const QString BezierMask = QStringLiteral("bezier_mask");
inline const QString LumaKeyer = QStringLiteral("luma_keyer");
inline const QString DeltaKeyer = QStringLiteral("delta_keyer");
inline const QString CornerPositioner = QStringLiteral("corner_positioner");
inline const QString SoftGlow = QStringLiteral("soft_glow");
inline const QString LightWrap = QStringLiteral("light_wrap");
inline const QString GridWarp = QStringLiteral("grid_warp");
inline const QString Displace = QStringLiteral("displace");
inline const QString VectorDistort = QStringLiteral("vector_distort");
inline const QString UltraKeyer = QStringLiteral("ultra_keyer");
inline const QString MatteControl = QStringLiteral("matte_control");
inline const QString ColorCorrector = QStringLiteral("color_corrector");
inline const QString ChannelBooleans = QStringLiteral("channel_booleans");
// Spec 123.
inline const QString BrightnessContrast = QStringLiteral("brightness_contrast");
inline const QString AlphaMultiply = QStringLiteral("alpha_multiply");
inline const QString AlphaDivide = QStringLiteral("alpha_divide");
inline const QString ChannelMapper = QStringLiteral("channel_mapper");
inline const QString ColorCurves = QStringLiteral("color_curves");
inline const QString HueCurves = QStringLiteral("hue_curves");
inline const QString CustomTool = QStringLiteral("custom_tool");
inline const QString VectorMotionBlur = QStringLiteral("vector_motion_blur");
// Spec 128.
inline const QString Loader = QStringLiteral("loader");
inline const QString Saver = QStringLiteral("saver");
inline const QString TimeSpeed = QStringLiteral("time_speed");
inline const QString TimeStretcher = QStringLiteral("time_stretcher");
// Spec 129.
inline const QString PEmitter = QStringLiteral("p_emitter");
inline const QString PGravity = QStringLiteral("p_gravity");
inline const QString PTurbulence = QStringLiteral("p_turbulence");
inline const QString PMerge = QStringLiteral("p_merge");
inline const QString PRender = QStringLiteral("p_render");
// Spec 130.
inline const QString ImagePlane3D = QStringLiteral("image_plane_3d");
inline const QString Shape3D = QStringLiteral("shape_3d");
inline const QString Text3D = QStringLiteral("text_3d");
inline const QString Merge3D = QStringLiteral("merge_3d");
inline const QString Camera3D = QStringLiteral("camera_3d");
inline const QString AmbientLight = QStringLiteral("ambient_light");
inline const QString DirectionalLight = QStringLiteral("directional_light");
inline const QString PointLight = QStringLiteral("point_light");
inline const QString SpotLight = QStringLiteral("spot_light");
inline const QString Renderer3D = QStringLiteral("renderer_3d");
// Spec 131.
inline const QString Paint = QStringLiteral("paint");
/// Prefix of the effect nodes: "fx:" + EffectRegistry id.
inline const QString EffectPrefix = QStringLiteral("fx:");
} // namespace nodetypes

/// Time Speed / Time Stretcher interpolation (spec 128).
namespace timeinterps {
inline const QString Nearest = QStringLiteral("nearest");
inline const QString Blend = QStringLiteral("blend");
} // namespace timeinterps

/// Paint node (spec 131): the brush tool and how long a new stroke lasts.
namespace painttools {
inline const QString Stroke = QStringLiteral("stroke");
inline const QString Clone = QStringLiteral("clone");
inline const QString Eraser = QStringLiteral("eraser");
inline const QString Smear = QStringLiteral("smear");
} // namespace painttools
namespace paintdurations {
inline const QString OneFrame = QStringLiteral("one");
inline const QString Whole = QStringLiteral("whole");
inline const QString Frames = QStringLiteral("frames");
} // namespace paintdurations

/// pEmitter sprite style and region (spec 129).
namespace particlestyles {
inline const QString Point = QStringLiteral("point");
inline const QString Blob = QStringLiteral("blob");
inline const QString Bitmap = QStringLiteral("bitmap");
} // namespace particlestyles
namespace particleregions {
inline const QString Point = QStringLiteral("point");
inline const QString Line = QStringLiteral("line");
inline const QString Rectangle = QStringLiteral("rectangle");
inline const QString Circle = QStringLiteral("circle");
} // namespace particleregions

/// Shape3D shapes, Camera3D "view by" and Renderer3D shading / anti-aliasing (spec 130).
namespace shapes3d {
inline const QString Plane = QStringLiteral("plane");
inline const QString Cube = QStringLiteral("cube");
inline const QString Sphere = QStringLiteral("sphere");
inline const QString Cylinder = QStringLiteral("cylinder");
inline const QString Cone = QStringLiteral("cone");
} // namespace shapes3d
namespace cameraviews {
inline const QString AngleOfView = QStringLiteral("angleOfView");
inline const QString FocalLength = QStringLiteral("focalLength");
} // namespace cameraviews
namespace shadings3d {
inline const QString Lambert = QStringLiteral("lambert");
inline const QString BlinnPhong = QStringLiteral("blinnPhong");
} // namespace shadings3d

/// Saver file formats (spec 128).
namespace saverformats {
inline const QString Png = QStringLiteral("png");
inline const QString Tiff = QStringLiteral("tiff");
} // namespace saverformats

/// Well-known port ids.
namespace nodeports {
inline const QString Input = QStringLiteral("input");
inline const QString Background = QStringLiteral("background");
inline const QString Foreground = QStringLiteral("foreground");
inline const QString Mask = QStringLiteral("mask");
/// Tracker: the picture the point is followed in (the main input when not connected).
inline const QString Source = QStringLiteral("source");
/// Planar Tracker: a mask that limits where the plane is tracked (white = track here); analysis
/// only, like Source.
inline const QString TrackMask = QStringLiteral("trackMask");
/// Displace and Vector Distort: the picture that holds the displacement / the vectors.
inline const QString Map = QStringLiteral("map");
/// Custom Tool: the optional second picture (r2 g2 b2 a2).
inline const QString Image2 = QStringLiteral("image2");
/// Keyers and Matte Control: masks that take the matte out (Garbage) or keep it (Solid), and the
/// Delta Keyer's clean plate (a picture of the screen without the subject).
inline const QString GarbageMatte = QStringLiteral("garbageMatte");
inline const QString SolidMatte = QStringLiteral("solidMatte");
inline const QString CleanPlate = QStringLiteral("cleanPlate");
/// pMerge: the second particle stream; pEmitter: the picture used as a Bitmap sprite.
inline const QString Input2 = QStringLiteral("input2");
inline const QString Sprite = QStringLiteral("sprite");
/// 3D (spec 130): the picture on a plane or shape; the scenes Merge3D joins; the scene Renderer3D draws.
inline const QString Texture = QStringLiteral("texture");
inline const QString Scene1 = QStringLiteral("scene1");
inline const QString Scene2 = QStringLiteral("scene2");
inline const QString Scene3 = QStringLiteral("scene3");
inline const QString Scene4 = QStringLiteral("scene4");
} // namespace nodeports

/// Parameter keys of the built-in nodes (see NodeDescriptor for the units).
namespace nodekeys {
// Shared geometry.
inline const QString CenterX = QStringLiteral("centerX"); ///< 0..1, left to right
inline const QString CenterY = QStringLiteral("centerY"); ///< 0..1, bottom to top
inline const QString Size = QStringLiteral("size"); ///< factor
inline const QString Aspect = QStringLiteral("aspect"); ///< Transform: x / y size ratio
inline const QString Angle = QStringLiteral("angle"); ///< degrees counter-clockwise
inline const QString PivotX = QStringLiteral("pivotX"); ///< Transform: 0..1
inline const QString PivotY = QStringLiteral("pivotY");
inline const QString FlipH = QStringLiteral("flipH"); ///< bool
inline const QString FlipV = QStringLiteral("flipV"); ///< bool
// Merge / Dissolve.
inline const QString BlendMode = QStringLiteral("blendMode"); ///< choice (blendmodes::)
inline const QString Opacity = QStringLiteral("opacity"); ///< 0..1
inline const QString Operation = QStringLiteral("operation"); ///< Merge: choice (mergeops::)
inline const QString AlphaGain = QStringLiteral("alphaGain"); ///< Merge: 0..1
inline const QString BurnIn = QStringLiteral("burnIn"); ///< Merge: 0..1
inline const QString SubAdd = QStringLiteral("subAdd"); ///< Merge: 0 subtractive .. 1 additive
// Merge / Transform resampling.
inline const QString Edges = QStringLiteral("edges"); ///< choice (edgemodes::)
inline const QString InvertTransform = QStringLiteral("invertTransform"); ///< bool
inline const QString FilterMethod = QStringLiteral("filterMethod"); ///< choice (filtermethods::)
inline const QString UseFrameFormat = QStringLiteral("useFrameFormat"); ///< Merge: bool
inline const QString RefWidth = QStringLiteral("refWidth"); ///< Merge: Reference Size, px
inline const QString RefHeight = QStringLiteral("refHeight");
inline const QString Mix = QStringLiteral("mix"); ///< Dissolve: 0 = background, 1 = foreground
// Crop (timeline pixels from each edge).
inline const QString Left = QStringLiteral("left");
inline const QString Right = QStringLiteral("right");
inline const QString Top = QStringLiteral("top");
inline const QString Bottom = QStringLiteral("bottom");
// Resize (timeline pixels).
inline const QString Width = QStringLiteral("width"); ///< Resize: px; masks: fraction of frame
inline const QString Height = QStringLiteral("height");
inline const QString KeepAspect = QStringLiteral("keepAspect"); ///< Resize: bool
// Tracker. The tracked point itself is CenterX / CenterY (keyframed by the tracking).
inline const QString PatternSize = QStringLiteral("patternSize"); ///< px, side of the pattern box
inline const QString SearchSize = QStringLiteral("searchSize"); ///< px, side of the search box
inline const QString TrackMode = QStringLiteral("trackMode"); ///< choice (trackmodes::)
inline const QString RefFrame = QStringLiteral("refFrame"); ///< comp frame where the offset is 0
// Tracker with several patterns (spec section 118): 1 .. kMaxPatterns patterns in one node.
// Pattern 1 is CenterX / CenterY; the others have hidden keys, see patternKey().
inline constexpr int kMaxPatterns = 4;
inline const QString Patterns = QStringLiteral("patterns"); ///< number of patterns in use, static
inline const QString ActivePattern = QStringLiteral("activePattern"); ///< the one the viewer box and Track buttons use (1-based), static
inline const QString TrackPosition = QStringLiteral("trackPosition"); ///< bool: output follows the patterns' middle
inline const QString TrackRotation = QStringLiteral("trackRotation"); ///< bool: output turns (2 or more patterns)
inline const QString TrackScale = QStringLiteral("trackScale"); ///< bool: output scales (2 or more patterns)
/// Key of the X or Y position of pattern @p n (1-based); pattern 1 is CenterX / CenterY.
inline QString patternKey(int n, bool y)
{
    if (n <= 1)
        return y ? QStringLiteral("centerY") : QStringLiteral("centerX");
    return QStringLiteral("pattern%1%2").arg(n).arg(y ? QLatin1Char('Y') : QLatin1Char('X'));
}
// Planar Tracker: the region's corners, normalised like CenterX / CenterY (y from the bottom),
// keyframed by the tracking. Also uses PatternSize, SearchSize, TrackMode and RefFrame.
inline constexpr int kPlanarCorners = 4;
/// Key of corner @p corner (0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left) of the
/// Planar Tracker, "cornerNX" / "cornerNY" with N = corner + 1.
inline QString planarCorner(int corner, bool y)
{
    return QStringLiteral("corner%1%2")
        .arg(corner + 1)
        .arg(y ? QLatin1Char('Y') : QLatin1Char('X'));
}
// Loader / Saver / Time Speed / Time Stretcher (spec 128).
inline const QString LoaderPath = QStringLiteral("loaderPath"); ///< file: an image, one file of a sequence, or a movie
inline const QString GlobalIn = QStringLiteral("globalIn"); ///< comp frame where the clip starts
inline const QString TrimIn = QStringLiteral("trimIn"); ///< frames cut from the start of the clip
inline const QString TrimOut = QStringLiteral("trimOut"); ///< frames cut from the end of the clip
inline const QString HoldFirst = QStringLiteral("holdFirst"); ///< frames the first frame is held before it plays
inline const QString HoldLast = QStringLiteral("holdLast"); ///< frames the last frame is held after it played
inline const QString Loop = QStringLiteral("loop"); ///< bool
inline const QString Reverse = QStringLiteral("reverse"); ///< bool
inline const QString SaverPath = QStringLiteral("saverPath"); ///< output file name; the frame number is added
inline const QString SaverFormat = QStringLiteral("saverFormat"); ///< choice (saverformats::)
inline const QString SaverStart = QStringLiteral("saverStart"); ///< number of the first written file
inline const QString SaverActive = QStringLiteral("saverActive"); ///< bool: Save When Rendered
inline const QString TimeSpeedKey = QStringLiteral("timeSpeed"); ///< Time Speed: speed factor
inline const QString TimeDelay = QStringLiteral("timeDelay"); ///< Time Speed: frames
inline const QString TimeInterp = QStringLiteral("timeInterp"); ///< choice (timeinterps::)
inline const QString SourceTime = QStringLiteral("sourceTime"); ///< Time Stretcher: frame of the input shown
// Particles (spec 129). Positions use the node conventions; lengths are fractions of the frame width.
inline const QString PNumber = QStringLiteral("pNumber"); ///< pEmitter: particles per frame
inline const QString PNumberVar = QStringLiteral("pNumberVar");
inline const QString PLife = QStringLiteral("pLife"); ///< frames
inline const QString PLifeVar = QStringLiteral("pLifeVar");
inline const QString PVelocity = QStringLiteral("pVelocity"); ///< frame widths per frame
inline const QString PVelocityVar = QStringLiteral("pVelocityVar");
inline const QString PAngleVar = QStringLiteral("pAngleVar"); ///< degrees (the direction is Angle)
inline const QString PRotation = QStringLiteral("pRotation"); ///< degrees
inline const QString PRotationVar = QStringLiteral("pRotationVar");
inline const QString PSpin = QStringLiteral("pSpin"); ///< degrees per frame
inline const QString PSpinVar = QStringLiteral("pSpinVar");
inline const QString PColorEnd = QStringLiteral("pColorEnd"); ///< colour at the end of life (Color is the start)
inline const QString PSize = QStringLiteral("pSize"); ///< diameter at birth, frame widths
inline const QString PSizeEnd = QStringLiteral("pSizeEnd"); ///< size at the end of life
inline const QString PSizeVar = QStringLiteral("pSizeVar"); ///< fraction of the size
inline const QString POpacity = QStringLiteral("pOpacity");
inline const QString POpacityEnd = QStringLiteral("pOpacityEnd");
inline const QString PStyle = QStringLiteral("pStyle"); ///< choice (particlestyles::)
inline const QString PRegion = QStringLiteral("pRegion"); ///< choice (particleregions::)
inline const QString PActive = QStringLiteral("pActive"); ///< forces: bool
inline const QString PStart = QStringLiteral("pStart"); ///< first frame that emits
inline const QString PEnd = QStringLiteral("pEnd"); ///< last frame that emits
inline const QString PStrength = QStringLiteral("pStrength"); ///< forces
inline const QString PDensity = QStringLiteral("pDensity"); ///< pTurbulence: noise cells across the frame width
inline const QString PEvolution = QStringLiteral("pEvolution"); ///< pTurbulence: noise change per frame
// 3D (spec 130). Units: scene units (an Image Plane 3D of Size 1 is one unit wide); angles in degrees.
inline const QString TranslateX = QStringLiteral("translateX");
inline const QString TranslateY = QStringLiteral("translateY");
inline const QString TranslateZ = QStringLiteral("translateZ");
inline const QString RotateX = QStringLiteral("rotateX");
inline const QString RotateY = QStringLiteral("rotateY");
inline const QString RotateZ = QStringLiteral("rotateZ");
inline const QString ScaleUniform = QStringLiteral("scaleUniform");
inline const QString ScaleX = QStringLiteral("scaleX");
inline const QString ScaleY = QStringLiteral("scaleY");
inline const QString ScaleZ = QStringLiteral("scaleZ");
inline const QString Shape = QStringLiteral("shape"); ///< Shape3D: choice (shapes3d::)
inline const QString Subdivisions = QStringLiteral("subdivisions"); ///< Shape3D: segments round / across
inline const QString SubdivisionsHeight = QStringLiteral("subdivisionsHeight");
inline const QString Specular = QStringLiteral("specular"); ///< material, 0..1
inline const QString Shininess = QStringLiteral("shininess"); ///< material, Blinn-Phong exponent
inline const QString ViewBy = QStringLiteral("viewBy"); ///< Camera3D: choice (cameraviews::)
inline const QString AngleOfView = QStringLiteral("angleOfView"); ///< horizontal, degrees
inline const QString FocalLength = QStringLiteral("focalLength"); ///< mm, with a 36 mm wide film back
inline const QString NearClip = QStringLiteral("nearClip");
inline const QString FarClip = QStringLiteral("farClip");
inline const QString LightColor = QStringLiteral("lightColor");
inline const QString Intensity = QStringLiteral("intensity");
inline const QString ConeAngle = QStringLiteral("coneAngle"); ///< Spot: half angle of the cone, degrees
inline const QString Penumbra = QStringLiteral("penumbra"); ///< Spot: soft part of the cone, 0..1
inline const QString Lighting = QStringLiteral("lighting"); ///< Renderer3D: bool
inline const QString Shading = QStringLiteral("shading"); ///< Renderer3D: choice (shadings3d::)
inline const QString AntiAlias = QStringLiteral("antiAlias"); ///< Renderer3D: samples per pixel side, 1..4
// Paint (spec 131). Sizes are fractions of the frame width; positions are normalised, y from the bottom.
inline const QString PaintTool = QStringLiteral("paintTool"); ///< choice (painttools::)
inline const QString BrushSize = QStringLiteral("brushSize");
inline const QString BrushSoftness = QStringLiteral("brushSoftness");
inline const QString BrushOpacity = QStringLiteral("brushOpacity");
inline const QString CloneOffsetX = QStringLiteral("cloneOffsetX"); ///< where Clone copies from, relative to the brush
inline const QString CloneOffsetY = QStringLiteral("cloneOffsetY");
inline const QString StrokeDuration = QStringLiteral("strokeDuration"); ///< choice (paintdurations::)
inline const QString StrokeFrames = QStringLiteral("strokeFrames"); ///< frames, with the Frames duration
inline const QString PaintStrokes = QStringLiteral("paintStrokes"); ///< hidden: the strokes (core/PaintStrokes.h)
// Letterbox.
inline const QString TargetAspect = QStringLiteral("targetAspect"); ///< width / height, e.g. 2.39
inline const QString Color = QStringLiteral("color"); ///< colour static
// Background.
inline const QString Mode = QStringLiteral("mode"); ///< Background: solid / gradient / four
inline const QString Color2 = QStringLiteral("color2");
inline const QString Color3 = QStringLiteral("color3");
inline const QString Color4 = QStringLiteral("color4");
inline const QString GradientAngle = QStringLiteral("gradientAngle"); ///< degrees ccw, 0 = l->r
// Text.
inline const QString Text = QStringLiteral("text"); ///< string
inline const QString Font = QStringLiteral("font"); ///< family name
inline const QString FontSize = QStringLiteral("fontSize"); ///< timeline px
inline const QString WriteOnStart = QStringLiteral("writeOnStart"); ///< Text: 0..1
inline const QString WriteOnEnd = QStringLiteral("writeOnEnd"); ///< Text: 0..1, default 1
inline const QString Bold = QStringLiteral("bold");
inline const QString Italic = QStringLiteral("italic");
inline const QString Underline = QStringLiteral("underline");
inline const QString Strikethrough = QStringLiteral("strikethrough");
inline const QString OutlineColor = QStringLiteral("outlineColor");
inline const QString OutlineWidth = QStringLiteral("outlineWidth"); ///< timeline px
inline const QString Shadow = QStringLiteral("shadow"); ///< bool
// Text+ (spec section 116): Layout, spacing, shading elements and the Follower.
inline const QString LayoutType = QStringLiteral("layoutType"); ///< choice (textlayouts::)
inline const QString Justify = QStringLiteral("justify"); ///< choice (justifies::)
inline const QString JustifyV = QStringLiteral("justifyV"); ///< choice (justifies::)
inline const QString FrameWidth = QStringLiteral("frameWidth"); ///< Frame layout, fraction of the frame width
inline const QString FrameHeight = QStringLiteral("frameHeight"); ///< fraction of the frame height
inline const QString CircleRadius = QStringLiteral("circleRadius"); ///< Circle layout, fraction of the frame width
inline const QString CircleAngle = QStringLiteral("circleAngle"); ///< degrees ccw from the right, 90 = the top
inline const QString PathOffset = QStringLiteral("pathOffset"); ///< Path layout: shift along the path, -1..1
inline const QString TextTracking = QStringLiteral("textTracking"); ///< percent of letter spacing added
inline const QString LineSpacing = QStringLiteral("lineSpacing"); ///< percent, 100 = the font's own
inline const QString Kerning = QStringLiteral("kerning"); ///< string: numbers, percent of the size per character
inline constexpr int kShadeElements = 3; ///< extra shading elements (2 .. 4)
/// Key of field @p field ("enable", "outline", "color", "opacity", "thickness", "x", "y") of
/// extra shading element @p n (2 .. 4).
inline QString shadeKey(int n, const char* field)
{
    return QStringLiteral("shade%1_%2").arg(n).arg(QLatin1String(field));
}
inline const QString FollowDelay = QStringLiteral("followDelay"); ///< frames per character, -100..100
inline const QString FollowX = QStringLiteral("followX"); ///< px, +right
inline const QString FollowY = QStringLiteral("followY"); ///< px, +up
inline const QString FollowSize = QStringLiteral("followSize"); ///< percent, 100 = unchanged
inline const QString FollowAngle = QStringLiteral("followAngle"); ///< degrees counter-clockwise
inline const QString FollowOpacity = QStringLiteral("followOpacity"); ///< percent
// Fast Noise.
inline const QString Detail = QStringLiteral("detail"); ///< octaves
inline const QString Contrast = QStringLiteral("contrast");
inline const QString Brightness = QStringLiteral("brightness");
inline const QString Scale = QStringLiteral("scale"); ///< noise cells across the frame width
inline const QString Seethe = QStringLiteral("seethe"); ///< evolution per second
inline const QString Seed = QStringLiteral("seed");
// Masks.
inline const QString SoftEdge = QStringLiteral("softEdge"); ///< fraction of the frame width
inline const QString Invert = QStringLiteral("invert"); ///< bool
inline const QString Level = QStringLiteral("level"); ///< mask opacity 0..1
inline const QString CornerRadius = QStringLiteral("cornerRadius"); ///< Rectangle: fraction of the shorter side, 0..1
inline const QString RotoAssist = QStringLiteral("rotoAssist"); ///< bool: points snap to the nearest picture edge
inline const QString PaintMode = QStringLiteral("paintMode"); ///< choice (paintmodes::), how the mask joins the one plugged in
// Luma Keyer: Channel (channelsources::), Low / High (0..1), Soft Range (0..1), Invert (above).
inline const QString KeyChannel = QStringLiteral("keyChannel");
inline const QString KeyLow = QStringLiteral("keyLow");
inline const QString KeyHigh = QStringLiteral("keyHigh");
inline const QString SoftRange = QStringLiteral("softRange");
// Delta Keyer (Color = Background Color; Gain above): screen colour key.
inline const QString KeyBalance = QStringLiteral("keyBalance"); ///< 0..1 weight of the larger other channel
inline const QString PreSoft = QStringLiteral("preMatteSoft"); ///< Pre-Matte Soft Range 0..1
inline const QString CleanBackground = QStringLiteral("cleanBackground"); ///< 0..1
inline const QString CleanForeground = QStringLiteral("cleanForeground"); ///< 0..1
inline const QString MatteLow = QStringLiteral("matteLow"); ///< Matte Threshold Low
inline const QString MatteHigh = QStringLiteral("matteHigh"); ///< Matte Threshold High
inline const QString Despill = QStringLiteral("despill"); ///< 0..1
// Soft Glow (Gain above): Filter (blurfilters::), size, passes, colour channels and scale.
inline const QString BlurFilter = QStringLiteral("blurFilter"); ///< choice (blurfilters::)
inline const QString NumPasses = QStringLiteral("numPasses"); ///< Multi-Box passes
inline const QString GlowThreshold = QStringLiteral("glowThreshold"); ///< brightness above which pixels glow
inline const QString GlowSize = QStringLiteral("glowSize"); ///< timeline px (X when unlocked)
inline const QString GlowSizeY = QStringLiteral("glowSizeY"); ///< timeline px, used when Lock X/Y is off
inline const QString LockXY = QStringLiteral("lockXY"); ///< bool
inline const QString GlowRed = QStringLiteral("glowRed"); ///< bool: colour channels the glow has
inline const QString GlowGreen = QStringLiteral("glowGreen");
inline const QString GlowBlue = QStringLiteral("glowBlue");
inline const QString GlowAlpha = QStringLiteral("glowAlpha");
inline const QString GlowScaleR = QStringLiteral("glowScaleR"); ///< Color Scale per channel
inline const QString GlowScaleG = QStringLiteral("glowScaleG");
inline const QString GlowScaleB = QStringLiteral("glowScaleB");
inline const QString GlowScaleA = QStringLiteral("glowScaleA");
// Light Wrap (Saturation above).
inline const QString WrapIntensity = QStringLiteral("wrapIntensity"); ///< 0..4
inline const QString WrapDiffuse = QStringLiteral("wrapDiffuse"); ///< timeline px, blur of the wrap
inline const QString WrapFgBlur = QStringLiteral("wrapFgBlur"); ///< timeline px, blur of the matte
inline const QString WrapBgBlur = QStringLiteral("wrapBgBlur"); ///< timeline px, blur of the background
inline const QString WrapLumaTolerance = QStringLiteral("wrapLumaTolerance"); ///< 0..1
inline const QString WrapLumaBased = QStringLiteral("wrapLumaBased"); ///< bool
inline const QString WrapGlow = QStringLiteral("wrapGlow"); ///< bool: not limited to the foreground
inline const QString WrapOnly = QStringLiteral("wrapOnly"); ///< bool: the wrap alone
inline const QString WrapUseConstant = QStringLiteral("wrapUseConstant"); ///< bool
inline const QString WrapColor = QStringLiteral("wrapColor"); ///< colour of Use Constant Color
inline const QString WrapMerge = QStringLiteral("wrapMerge"); ///< choice (wrapmerges::)
// Displace (Edges above, CenterX / CenterY for Radial) and Vector Distort.
inline const QString DisplaceType = QStringLiteral("displaceType"); ///< choice (displacetypes::)
inline const QString DisplaceXChannel = QStringLiteral("displaceXChannel"); ///< choice (channelsources::, red..luma)
inline const QString DisplaceYChannel = QStringLiteral("displaceYChannel");
inline const QString DisplaceXRefraction = QStringLiteral("displaceXRefraction"); ///< timeline px
inline const QString DisplaceYRefraction = QStringLiteral("displaceYRefraction");
inline const QString DisplaceOffset = QStringLiteral("displaceOffset"); ///< map value that moves nothing
inline const QString DisplaceLightPower = QStringLiteral("displaceLightPower");
inline const QString DisplaceLightAngle = QStringLiteral("displaceLightAngle");
inline const QString VectorRange = QStringLiteral("vectorRange"); ///< timeline px
inline const QString VectorMultiplier = QStringLiteral("vectorMultiplier");
inline const QString VectorSteps = QStringLiteral("vectorSteps");
// Vector Motion Blur (VectorRange above; Edges).
inline const QString VmbScale = QStringLiteral("vmbScale"); ///< multiplies the vector length
inline const QString VmbBias = QStringLiteral("vmbBias"); ///< -1..1: where along the vector
inline const QString VmbSamples = QStringLiteral("vmbSamples"); ///< 2..64
// Brightness Contrast (Saturation above).
inline const QString BcRed = QStringLiteral("bcRed");
inline const QString BcGreen = QStringLiteral("bcGreen");
inline const QString BcBlue = QStringLiteral("bcBlue");
inline const QString BcAlpha = QStringLiteral("bcAlpha");
inline const QString BcGain = QStringLiteral("bcGain");
inline const QString BcLift = QStringLiteral("bcLift");
inline const QString BcGamma = QStringLiteral("bcGamma");
inline const QString BcContrast = QStringLiteral("bcContrast");
inline const QString BcBrightness = QStringLiteral("bcBrightness");
inline const QString BcLow = QStringLiteral("bcLow");
inline const QString BcHigh = QStringLiteral("bcHigh");
/// Channel Mapper: output channel @p out (0 R, 1 G, 2 B, 3 A) takes @p in times this number.
inline QString mapperKey(int out, int in)
{
    return QStringLiteral("map%1%2").arg(QChar(QLatin1Char("RGBA"[out])), QChar(QLatin1Char("RGBA"[in])));
}
/// Channel Mapper: added to output channel @p out.
inline QString mapperOffsetKey(int out)
{
    return QStringLiteral("mapOffset%1").arg(QLatin1Char("RGBA"[out]));
}
// Color Curves and Hue Curves: Curve statics (core/CurveParam.h text, x and y 0..1).
inline const QString CurveLuma = QStringLiteral("curveLuma");
inline const QString CurveRed = QStringLiteral("curveRed");
inline const QString CurveGreen = QStringLiteral("curveGreen");
inline const QString CurveBlue = QStringLiteral("curveBlue");
inline const QString CurveAlpha = QStringLiteral("curveAlpha");
inline const QString CurveHueHue = QStringLiteral("curveHueHue");
inline const QString CurveHueSat = QStringLiteral("curveHueSat");
inline const QString CurveHueLum = QStringLiteral("curveHueLum");
inline const QString CurveLumSat = QStringLiteral("curveLumSat");
inline const QString CurveSatSat = QStringLiteral("curveSatSat");
// Custom Tool: Numbers 1 to 8 (keyframeable), Intermediate 1 to 4 and the four channel
// formulas (strings).
inline QString customNumberKey(int i) { return QStringLiteral("customN%1").arg(i + 1); }
inline QString customInterKey(int i) { return QStringLiteral("customI%1").arg(i + 1); }
inline const QString CustomRed = QStringLiteral("customR");
inline const QString CustomGreen = QStringLiteral("customG");
inline const QString CustomBlue = QStringLiteral("customB");
inline const QString CustomAlpha = QStringLiteral("customA");
// Grid Warp: Columns / Rows (core/VideoEffectKeys4.h names, redeclared here), points in px.
inline constexpr int kGridMaxSide = 9;
inline const QString Columns = QStringLiteral("columns");
inline const QString Rows = QStringLiteral("rows");
/// Movement in timeline px of grid point (@p row, @p column) on x or y (row 0 at the top).
inline QString gridKey(int row, int column, int axis)
{
    return QStringLiteral("grid%1%2%3").arg(row).arg(column).arg(axis == 0 ? 'X' : 'Y');
}
// Keyers and Matte Control (Invert, Gain, KeyBalance, PreSoft, CleanBackground, CleanForeground,
// MatteLow, MatteHigh, Despill above; BlurFilter and NumPasses of Soft Glow below the matte blur).
inline const QString PreBlur = QStringLiteral("preBlur"); ///< px, blur of the picture the key reads
inline const QString PreMatteErode = QStringLiteral("preMatteErode"); ///< px, + grows the matte
inline const QString PreMatteBlur = QStringLiteral("preMatteBlur"); ///< px
inline const QString MatteBlur = QStringLiteral("matteBlur"); ///< px
inline const QString MatteErode = QStringLiteral("matteErode"); ///< px, + dilates, - erodes
inline const QString MatteGamma = QStringLiteral("matteGamma");
inline const QString RestoreFringe = QStringLiteral("restoreFringe"); ///< 0..1
inline const QString SpillMethod = QStringLiteral("spillMethod"); ///< choice (spillmethods::)
inline const QString FringeGamma = QStringLiteral("fringeGamma");
inline const QString FringeSize = QStringLiteral("fringeSize"); ///< -1..1
inline const QString FringeShape = QStringLiteral("fringeShape"); ///< -1..1
inline const QString FringeCyanRed = QStringLiteral("fringeCyanRed"); ///< -1..1
inline const QString FringeMagentaGreen = QStringLiteral("fringeMagentaGreen");
inline const QString FringeYellowBlue = QStringLiteral("fringeYellowBlue");
inline const QString ReplaceMode = QStringLiteral("replaceMode"); ///< choice (replacemodes::)
inline const QString ReplaceColor = QStringLiteral("replaceColor");
inline const QString SolidInvert = QStringLiteral("solidInvert"); ///< bool
inline const QString GarbageInvert = QStringLiteral("garbageInvert"); ///< bool
inline const QString ViewMode = QStringLiteral("viewMode"); ///< choice (viewmodes::)
inline const QString UkRedLevel = QStringLiteral("ukRedLevel"); ///< Ultra Keyer difference levels
inline const QString UkGreenLevel = QStringLiteral("ukGreenLevel");
inline const QString UkBlueLevel = QStringLiteral("ukBlueLevel");
inline const QString MatteSeparation = QStringLiteral("matteSeparation"); ///< 0..1
inline const QString PreMatteSize = QStringLiteral("preMatteSize"); ///< px, closes holes
inline const QString SubtractBackground = QStringLiteral("subtractBackground"); ///< bool
// Color Corrector.
inline const QString Lift = QStringLiteral("lift"); ///< added to the shadows, -1..1
inline const QString Gamma = QStringLiteral("gamma"); ///< midtone power, 1 = unchanged
inline const QString Gain = QStringLiteral("gain"); ///< multiplier
inline const QString Saturation = QStringLiteral("saturation"); ///< 1 = unchanged
inline const QString InBlack = QStringLiteral("inBlack"); ///< levels, 0..1
inline const QString InWhite = QStringLiteral("inWhite");
inline const QString LevelGamma = QStringLiteral("levelGamma");
inline const QString OutBlack = QStringLiteral("outBlack");
inline const QString OutWhite = QStringLiteral("outWhite");
// Channel Booleans (choice of channelsources::).
inline const QString ToRed = QStringLiteral("toRed");
inline const QString ToGreen = QStringLiteral("toGreen");
inline const QString ToBlue = QStringLiteral("toBlue");
inline const QString ToAlpha = QStringLiteral("toAlpha");
// Common Controls (the Settings section of every tool but masks and MediaOut). They are not in
// NodeDescriptor::params: a node carries them only once one is changed, so older files and new
// nodes stay as they were.
inline const QString CommonBlend = QStringLiteral("ccBlend"); ///< 0..1, default 1
inline const QString CommonRed = QStringLiteral("ccRed"); ///< bool, default on
inline const QString CommonGreen = QStringLiteral("ccGreen");
inline const QString CommonBlue = QStringLiteral("ccBlue");
inline const QString CommonAlpha = QStringLiteral("ccAlpha");
inline const QString MaskInverted = QStringLiteral("ccMaskInverted"); ///< Apply Mask Inverted
inline const QString MultiplyByMask = QStringLiteral("ccMultiplyByMask");
inline const QString ProcessAtZero = QStringLiteral("ccProcessAtZero"); ///< default on
// Motion Blur (Merge and Transform only).
inline const QString MotionBlur = QStringLiteral("ccMotionBlur"); ///< bool, default off
inline const QString MotionBlurQuality = QStringLiteral("ccMbQuality"); ///< 1..10, default 2
inline const QString ShutterAngle = QStringLiteral("ccMbShutter"); ///< degrees, default 100
inline const QString CenterBias = QStringLiteral("ccMbBias"); ///< -1..1, default 0
inline const QString SampleSpread = QStringLiteral("ccMbSpread"); ///< 0..2, default 1
} // namespace nodekeys

/// Paint Mode of a mask node (nodekeys::PaintMode): how the mask joins the mask plugged into it.
/// M = the incoming matte, m = this mask (after Invert and Level).
namespace paintmodes {
inline const QString Merge = QStringLiteral("merge"); ///< m + M (1 - m): the union, default
inline const QString Add = QStringLiteral("add"); ///< min(M + m, 1)
inline const QString Subtract = QStringLiteral("subtract"); ///< max(M - m, 0)
inline const QString Minimum = QStringLiteral("minimum");
inline const QString Maximum = QStringLiteral("maximum");
inline const QString Multiply = QStringLiteral("multiply");
inline const QString Replace = QStringLiteral("replace"); ///< m
inline const QString Invert = QStringLiteral("invert"); ///< M where m is 0, 1 - M where m is 1
inline const QString Copy = QStringLiteral("copy"); ///< m where this mask covers, M elsewhere
inline const QString Ignore = QStringLiteral("ignore"); ///< M
[[nodiscard]] inline QStringList ids()
{
    return {Merge, Add, Subtract, Minimum, Maximum, Multiply, Replace, Invert, Copy, Ignore};
}
[[nodiscard]] inline int index(const QString& id) { return ids().indexOf(id); }
} // namespace paintmodes

/// Merge blend modes (nodekeys::BlendMode values), in the order of the choice list.
namespace blendmodes {
inline const QString Normal = QStringLiteral("normal");
inline const QString Screen = QStringLiteral("screen");
inline const QString Multiply = QStringLiteral("multiply");
inline const QString Add = QStringLiteral("add");
inline const QString Overlay = QStringLiteral("overlay");
inline const QString Darken = QStringLiteral("darken");
inline const QString Lighten = QStringLiteral("lighten");
inline const QString Difference = QStringLiteral("difference");
inline const QString Subtract = QStringLiteral("subtract");
inline const QString HardLight = QStringLiteral("hardLight");
inline const QString SoftLight = QStringLiteral("softLight");
inline const QString ColorDodge = QStringLiteral("colorDodge");
inline const QString ColorBurn = QStringLiteral("colorBurn");
inline const QString Exclusion = QStringLiteral("exclusion");
inline const QString LinearBurn = QStringLiteral("linearBurn");
inline const QString LinearLight = QStringLiteral("linearLight");
inline const QString VividLight = QStringLiteral("vividLight");
inline const QString PinLight = QStringLiteral("pinLight");
inline const QString HardMix = QStringLiteral("hardMix");
// Added in 1.7 (indices 19 and up). Hue .. Luminosity use the whole colour, the rest each channel.
inline const QString Hue = QStringLiteral("hue");
inline const QString Saturation = QStringLiteral("saturation");
inline const QString Color = QStringLiteral("color");
inline const QString Luminosity = QStringLiteral("luminosity");
inline const QString Divide = QStringLiteral("divide");
inline const QString DarkerColor = QStringLiteral("darkerColor");
inline const QString LighterColor = QStringLiteral("lighterColor");
inline const QString Average = QStringLiteral("average");
inline const QString Negation = QStringLiteral("negation");
inline const QString Reflect = QStringLiteral("reflect");
inline const QString Glow = QStringLiteral("glow");
inline const QString Phoenix = QStringLiteral("phoenix");
inline const QString Freeze = QStringLiteral("freeze");
inline const QString Heat = QStringLiteral("heat");
/// Index of @p mode (0 = Normal for unknown values): the number the blend functions
/// (compmath::blendChannel(), the GLSL kBlendFunctionGlsl) switch on. Stable: new modes are
/// appended.
[[nodiscard]] int index(const QString& mode);
/// Every mode id in index() order.
[[nodiscard]] const QStringList& ids();
/// The modes in the order of the Inspector's Composite Mode list (Resolve's order: Normal,
/// Add, Subtract, Difference, Multiply, Screen, Overlay, Hard Light, Soft Light, ...).
[[nodiscard]] const QStringList& compositeOrder();
/// Translated name of mode @p id ("Soft Light"); @p id itself if unknown.
[[nodiscard]] QString displayName(const QString& id);
} // namespace blendmodes

/// Merge operations of the Normal apply mode (nodekeys::Operation), Porter-Duff style.
namespace mergeops {
inline const QString Over = QStringLiteral("over");
inline const QString In = QStringLiteral("in");
inline const QString HeldOut = QStringLiteral("heldOut");
inline const QString Atop = QStringLiteral("atop");
inline const QString XOr = QStringLiteral("xor");
/// Index (0 Over .. 4 XOr; 0 for unknown values) and ids in that order.
[[nodiscard]] int index(const QString& op);
[[nodiscard]] const QStringList& ids();
} // namespace mergeops

/// What lies outside the picture when Merge / Transform resample it (nodekeys::Edges).
namespace edgemodes {
inline const QString Canvas = QStringLiteral("canvas"); ///< transparent
inline const QString Wrap = QStringLiteral("wrap");
inline const QString Duplicate = QStringLiteral("duplicate"); ///< the border pixels repeat
inline const QString Mirror = QStringLiteral("mirror");
[[nodiscard]] int index(const QString& mode);
[[nodiscard]] const QStringList& ids();
} // namespace edgemodes

/// Resampling filters (nodekeys::FilterMethod).
namespace filtermethods {
inline const QString Nearest = QStringLiteral("nearest");
inline const QString Linear = QStringLiteral("linear");
inline const QString Cubic = QStringLiteral("cubic"); ///< cubic B-spline
inline const QString CatmullRom = QStringLiteral("catmullRom");
inline const QString Mitchell = QStringLiteral("mitchell");
inline const QString Lanczos = QStringLiteral("lanczos");
inline const QString Gaussian = QStringLiteral("gaussian");
[[nodiscard]] int index(const QString& method);
[[nodiscard]] const QStringList& ids();
} // namespace filtermethods

/// Background modes (nodekeys::Mode).
namespace backgroundmodes {
inline const QString Solid = QStringLiteral("solid");
inline const QString Gradient = QStringLiteral("gradient"); ///< color -> color2 along the angle
inline const QString FourCorner = QStringLiteral("four"); ///< tl color, tr 2, bl 3, br 4
} // namespace backgroundmodes

/// Tracker output modes (nodekeys::TrackMode values).
namespace trackmodes {
inline const QString MatchMove = QStringLiteral("matchMove"); ///< the input follows the track
inline const QString Stabilize = QStringLiteral("stabilize"); ///< the input cancels the track
/// Planar Tracker only: the input is pinned onto the tracked plane (its corners go to the
/// tracked corners). MatchMove is "unsteady" there (the input moves, turns and scales with the
/// plane), Stabilize is "steady" (it cancels that).
inline const QString CornerPin = QStringLiteral("cornerPin");
} // namespace trackmodes

/// Layout Type of the Text node.
namespace textlayouts {
inline const QString Point = QStringLiteral("point");
inline const QString Frame = QStringLiteral("frame");
inline const QString Circle = QStringLiteral("circle");
inline const QString Path = QStringLiteral("path");
} // namespace textlayouts

/// Justify choices (horizontal: left / center / right; vertical: top / center / bottom).
namespace justifies {
inline const QString Start = QStringLiteral("start"); ///< left or top
inline const QString Center = QStringLiteral("center");
inline const QString End = QStringLiteral("end"); ///< right or bottom
} // namespace justifies

/// Blur filters of Soft Glow and the matte blurs (the choice list order).
namespace blurfilters {
inline const QString Box = QStringLiteral("box");
inline const QString Bartlett = QStringLiteral("bartlett");
inline const QString MultiBox = QStringLiteral("multibox");
inline const QString Gaussian = QStringLiteral("gaussian");
[[nodiscard]] QStringList ids();
/// Index in ids() (Gaussian for unknown values).
[[nodiscard]] int index(const QString& id);
} // namespace blurfilters

/// Displace types.
namespace displacetypes {
inline const QString XY = QStringLiteral("xy");
inline const QString Radial = QStringLiteral("radial");
} // namespace displacetypes

/// Spill suppression methods of the keyers (choice order).
namespace spillmethods {
inline const QString None = QStringLiteral("none");
inline const QString Rare = QStringLiteral("rare");
inline const QString Medium = QStringLiteral("medium");
inline const QString WellDone = QStringLiteral("welldone");
inline const QString Burnt = QStringLiteral("burnt");
[[nodiscard]] int index(const QString& id);
} // namespace spillmethods

/// Replace modes of the keyers (choice order).
namespace replacemodes {
inline const QString None = QStringLiteral("none");
inline const QString Source = QStringLiteral("source");
inline const QString Hard = QStringLiteral("hard");
inline const QString Soft = QStringLiteral("soft");
[[nodiscard]] int index(const QString& id);
} // namespace replacemodes

/// What a keyer shows (choice order).
namespace viewmodes {
inline const QString Final = QStringLiteral("final");
inline const QString PreMatte = QStringLiteral("preMatte");
inline const QString Matte = QStringLiteral("matte");
[[nodiscard]] int index(const QString& id);
} // namespace viewmodes

/// Light Wrap Highlight Merge operations.
namespace wrapmerges {
inline const QString Add = QStringLiteral("add");
inline const QString Screen = QStringLiteral("screen");
} // namespace wrapmerges

/// Channel Booleans sources, in the order of the choice list.
namespace channelsources {
inline const QString Red = QStringLiteral("red");
inline const QString Green = QStringLiteral("green");
inline const QString Blue = QStringLiteral("blue");
inline const QString Alpha = QStringLiteral("alpha");
inline const QString Luma = QStringLiteral("luma");
inline const QString Black = QStringLiteral("black");
inline const QString White = QStringLiteral("white");
/// Index of @p source in the choice list (-1 for unknown values).
[[nodiscard]] int index(const QString& source);
} // namespace channelsources

/// Process-wide catalogue of node types: the built-in nodes plus one "fx:<id>" node per
/// EffectRegistry video effect (generated on first access, so a new effect appears on the
/// Compositing page without any change here). Read-only after construction; safe to read
/// from any thread.
class NodeRegistry
{
public:
    /// The global catalogue.
    static const NodeRegistry& instance();

    /// Descriptor of type @p id, or nullptr. O(1).
    [[nodiscard]] const NodeDescriptor* find(const QString& id) const;
    /// All descriptors: built-ins in library order, then the effect nodes.
    [[nodiscard]] const QVector<NodeDescriptor>& all() const { return m_descriptors; }
    /// Descriptors of @p category in library order.
    [[nodiscard]] QVector<const NodeDescriptor*> byCategory(NodeCategory category) const;
    /// ParamSet with the defaults of type @p id (empty if unknown).
    [[nodiscard]] ParamSet defaultParams(const QString& id) const;
    /// True if node type @p id has the Common Controls (everything but masks and MediaOut).
    [[nodiscard]] bool hasCommonControls(const QString& id) const;
    /// The Common Controls as parameter descriptors (Blend, Red, Green, Blue, Alpha, Apply
    /// Mask Inverted, Multiply By Mask, Process When Blend Is 0.0). Merge has no Blend here:
    /// its own Blend (nodekeys::Opacity) is the same control.
    [[nodiscard]] QVector<ParamDescriptor> commonControls(const QString& id) const;
    /// Translated name of a library section.
    [[nodiscard]] static QString categoryName(NodeCategory category);
    /// Node type id of EffectRegistry effect @p effectId ("fx:" + id).
    [[nodiscard]] static QString effectNodeType(const QString& effectId)
    {
        return nodetypes::EffectPrefix + effectId;
    }

private:
    NodeRegistry();
    void add(NodeDescriptor d);

    QVector<NodeDescriptor> m_descriptors;
    QHash<QString, qsizetype> m_index;
};

} // namespace kk
