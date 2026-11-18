// SPDX-License-Identifier: GPL-3.0-or-later
// NodeRegistry implementation: the built-in Compositing page nodes and the generated effect
// nodes. Behaviour: docs/behavior-spec.md section 42a.
#include "core/NodeRegistry.h"

#include "core/CurveParam.h"
#include "core/EffectBuilders.h"

#include <QCoreApplication>
#include <QHash>

namespace kk {

using namespace builders;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::NodeRegistry", text);
}

const QString kPx = QStringLiteral("px");
const QString kDegrees = QStringLiteral("°");

NodePort port(const QString& id, const QString& label, PortKind kind, bool required,
    bool analysisOnly = false)
{
    return NodePort {id, label, kind, required, analysisOnly};
}

/// @p p with the tooltip @p text.
ParamDescriptor tip(ParamDescriptor p, const QString& text)
{
    p.tooltip = text;
    return p;
}

NodePort maskPort()
{
    return port(nodeports::Mask, tr("Effect Mask"), PortKind::Mask, false);
}

NodePort inputPort()
{
    return port(nodeports::Input, tr("Input"), PortKind::Image, true);
}

NodePort garbagePort()
{
    return port(nodeports::GarbageMatte, tr("Garbage Matte"), PortKind::Mask, false);
}

NodePort solidPort()
{
    return port(nodeports::SolidMatte, tr("Solid Matte"), PortKind::Mask, false);
}

ParamDescriptor viewModeParam()
{
    return choice(nodekeys::ViewMode, tr("View Mode"),
        {viewmodes::Final, viewmodes::PreMatte, viewmodes::Matte},
        {tr("Final Result"), tr("Pre Matte"), tr("Matte")}, viewmodes::Final);
}

/// The Matte tab of the keyers and Matte Control: filter, blur, grow / shrink, gamma, thresholds,
/// restore fringe, invert. @p withClean adds Clean Background / Foreground.
QVector<ParamDescriptor> matteTabParams(const QString& lowLabel, const QString& highLabel, bool withClean)
{
    namespace k = nodekeys;
    QVector<ParamDescriptor> out {choice(k::BlurFilter, tr("Filter"), blurfilters::ids(),
                                      {tr("Box"), tr("Bartlett"), tr("Multi-box"), tr("Gaussian")},
                                      blurfilters::Gaussian),
        integer(k::NumPasses, tr("Num Passes"), 1, 10, 4),
        number(k::MatteBlur, tr("Blur"), 0, 100, 0, 0.5, kPx),
        number(k::MatteErode, tr("Erode / Dilate"), -50, 50, 0, 0.5, kPx),
        number(k::MatteGamma, tr("Gamma"), 0.1, 10, 1, 0.01)};
    if (withClean) {
        out += {number(k::CleanBackground, tr("Clean Background"), 0, 1, 0, 0.01),
            number(k::CleanForeground, tr("Clean Foreground"), 0, 1, 0, 0.01)};
    }
    out += {number(k::MatteLow, lowLabel, 0, 1, 0, 0.01),
        number(k::MatteHigh, highLabel, 0, 1, 1, 0.01),
        number(k::RestoreFringe, tr("Restore Fringe"), 0, 1, 0, 0.01),
        boolean(k::Invert, tr("Invert Matte"), false)};
    return out;
}

/// The Fringe tab: spill method and strength (@p strengthLabel), fringe gamma, size, shape and colour.
QVector<ParamDescriptor> fringeTabParams(const QString& strengthLabel, const QString& method)
{
    namespace k = nodekeys;
    return {choice(k::SpillMethod, tr("Spill Method"),
                {spillmethods::None, spillmethods::Rare, spillmethods::Medium,
                    spillmethods::WellDone, spillmethods::Burnt},
                {tr("None"), tr("Rare"), tr("Medium"), tr("Well Done"), tr("Burnt")}, method),
        number(k::Despill, strengthLabel, 0, 1, 1, 0.01),
        number(k::FringeGamma, tr("Fringe Gamma"), 0.1, 10, 1, 0.01),
        number(k::FringeSize, tr("Fringe Size"), -1, 1, 0, 0.01),
        number(k::FringeShape, tr("Fringe Shape"), -1, 1, 0, 0.01),
        number(k::FringeCyanRed, tr("Cyan / Red"), -1, 1, 0, 0.01),
        number(k::FringeMagentaGreen, tr("Magenta / Green"), -1, 1, 0, 0.01),
        number(k::FringeYellowBlue, tr("Yellow / Blue"), -1, 1, 0, 0.01)};
}

QVector<ParamDescriptor> maskInvertParams()
{
    return {boolean(nodekeys::SolidInvert, tr("Invert Solid Matte"), false),
        boolean(nodekeys::GarbageInvert, tr("Invert Garbage Matte"), false)};
}

NodeDescriptor node(const QString& id, const QString& name, const QString& shortName,
    const QString& nameBase, NodeCategory category, const QString& description)
{
    NodeDescriptor d;
    d.id = id;
    d.name = name;
    d.shortName = shortName;
    d.nameBase = nameBase;
    d.category = category;
    d.description = description;
    return d;
}

/// Centre of the shared geometry (0.5, 0.5 = the middle of the frame).
QVector<ParamDescriptor> centre()
{
    return {number(nodekeys::CenterX, tr("Center X"), -10, 10, 0.5, 0.001),
        number(nodekeys::CenterY, tr("Center Y"), -10, 10, 0.5, 0.001)};
}

ParamDescriptor angle()
{
    return number(nodekeys::Angle, tr("Angle"), -3600, 3600, 0, 0.1, kDegrees);
}

/// The parameters every mask node shares (after its own shape parameters).
QVector<ParamDescriptor> maskCommon()
{
    return {number(nodekeys::SoftEdge, tr("Soft Edge"), 0, 1, 0, 0.001),
        boolean(nodekeys::Invert, tr("Invert"), false),
        number(nodekeys::Level, tr("Level"), 0, 1, 1, 0.01),
        choice(nodekeys::PaintMode, tr("Paint Mode"), paintmodes::ids(),
            {tr("Merge"), tr("Add"), tr("Subtract"), tr("Minimum"), tr("Maximum"), tr("Multiply"),
                tr("Replace"), tr("Invert"), tr("Copy"), tr("Ignore")},
            paintmodes::Merge)};
}

/// Edges choice (Merge, Transform).
ParamDescriptor edgesParam()
{
    return choice(nodekeys::Edges, tr("Edges"), edgemodes::ids(),
        {tr("Canvas"), tr("Wrap"), tr("Duplicate"), tr("Mirror")}, edgemodes::Canvas);
}

ParamDescriptor filterParam()
{
    return choice(nodekeys::FilterMethod, tr("Filter Method"), filtermethods::ids(),
        {tr("Nearest Neighbor"), tr("Linear"), tr("Cubic"), tr("Catmull-Rom"), tr("Mitchell"),
            tr("Lanczos"), tr("Gaussian")},
        filtermethods::Linear);
}

QString shortNameOf(const QString& name)
{
    // "Gaussian Blur" -> "GBlu": initials of all words but the last + its first letters.
    const QStringList words = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return {};
    QString s;
    for (qsizetype i = 0; i + 1 < words.size(); ++i)
        s += words[i].left(1);
    s += words.last().left(std::max<qsizetype>(2, 4 - s.size()));
    return s.left(4);
}

QString nameBaseOf(const QString& name)
{
    QString s;
    for (const QChar c : name) {
        if (c.isLetterOrNumber())
            s += c;
    }
    return s.isEmpty() ? QStringLiteral("Node") : s;
}

/// Translated names of the blend modes in blendmodes::ids() order (the Merge choice list).
QStringList blendModeNames()
{
    QStringList names;
    for (const QString& id : blendmodes::ids())
        names.append(blendmodes::displayName(id));
    return names;
}

} // namespace

const QStringList& blendmodes::ids()
{
    static const QStringList modes {Normal, Screen, Multiply, Add, Overlay, Darken, Lighten,
        Difference, Subtract, HardLight, SoftLight, ColorDodge, ColorBurn, Exclusion, LinearBurn,
        LinearLight, VividLight, PinLight, HardMix, Hue, Saturation, Color, Luminosity, Divide,
        DarkerColor, LighterColor, Average, Negation, Reflect, Glow, Phoenix, Freeze, Heat};
    return modes;
}

int blendmodes::index(const QString& mode)
{
    return std::max<int>(0, int(ids().indexOf(mode)));
}

const QStringList& blendmodes::compositeOrder()
{
    static const QStringList order {Normal, Add, Subtract, Difference, Multiply, Screen, Overlay,
        HardLight, SoftLight, Darken, Lighten, ColorDodge, ColorBurn, Exclusion, LinearBurn,
        LinearLight, VividLight, PinLight, HardMix};
    return order;
}

QString blendmodes::displayName(const QString& id)
{
    static const QHash<QString, const char*> names {{Normal, QT_TR_NOOP("Normal")},
        {Screen, QT_TR_NOOP("Screen")}, {Multiply, QT_TR_NOOP("Multiply")},
        {Add, QT_TR_NOOP("Add")}, {Overlay, QT_TR_NOOP("Overlay")}, {Darken, QT_TR_NOOP("Darken")},
        {Lighten, QT_TR_NOOP("Lighten")}, {Difference, QT_TR_NOOP("Difference")},
        {Subtract, QT_TR_NOOP("Subtract")}, {HardLight, QT_TR_NOOP("Hard Light")},
        {SoftLight, QT_TR_NOOP("Soft Light")}, {ColorDodge, QT_TR_NOOP("Color Dodge")},
        {ColorBurn, QT_TR_NOOP("Color Burn")}, {Exclusion, QT_TR_NOOP("Exclusion")},
        {LinearBurn, QT_TR_NOOP("Linear Burn")}, {LinearLight, QT_TR_NOOP("Linear Light")},
        {VividLight, QT_TR_NOOP("Vivid Light")}, {PinLight, QT_TR_NOOP("Pin Light")},
        {HardMix, QT_TR_NOOP("Hard Mix")}, {Hue, QT_TR_NOOP("Hue")},
        {Saturation, QT_TR_NOOP("Saturation")}, {Color, QT_TR_NOOP("Color")},
        {Luminosity, QT_TR_NOOP("Luminosity")}, {Divide, QT_TR_NOOP("Divide")},
        {DarkerColor, QT_TR_NOOP("Darker Color")}, {LighterColor, QT_TR_NOOP("Lighter Color")},
        {Average, QT_TR_NOOP("Average")}, {Negation, QT_TR_NOOP("Negation")},
        {Reflect, QT_TR_NOOP("Reflect")}, {Glow, QT_TR_NOOP("Glow")},
        {Phoenix, QT_TR_NOOP("Phoenix")}, {Freeze, QT_TR_NOOP("Freeze")},
        {Heat, QT_TR_NOOP("Heat")}};
    const auto it = names.constFind(id);
    return it == names.constEnd() ? id : QCoreApplication::translate("kk::blendmodes", it.value());
}

const QStringList& mergeops::ids()
{
    static const QStringList v {Over, In, HeldOut, Atop, XOr};
    return v;
}
int mergeops::index(const QString& op)
{
    return std::max<int>(0, int(ids().indexOf(op)));
}

const QStringList& edgemodes::ids()
{
    static const QStringList v {Canvas, Wrap, Duplicate, Mirror};
    return v;
}
int edgemodes::index(const QString& mode)
{
    return std::max<int>(0, int(ids().indexOf(mode)));
}

const QStringList& filtermethods::ids()
{
    static const QStringList v {Nearest, Linear, Cubic, CatmullRom, Mitchell, Lanczos, Gaussian};
    return v;
}
int filtermethods::index(const QString& method)
{
    const int i = int(ids().indexOf(method));
    return i < 0 ? 1 : i; // Linear
}

int spillmethods::index(const QString& id)
{
    return std::max(0, int(QStringList {None, Rare, Medium, WellDone, Burnt}.indexOf(id)));
}

int replacemodes::index(const QString& id)
{
    return std::max(0, int(QStringList {None, Source, Hard, Soft}.indexOf(id)));
}

int viewmodes::index(const QString& id)
{
    return std::max(0, int(QStringList {Final, PreMatte, Matte}.indexOf(id)));
}

QStringList blurfilters::ids()
{
    return {Box, Bartlett, MultiBox, Gaussian};
}

int blurfilters::index(const QString& id)
{
    const int i = int(ids().indexOf(id));
    return i < 0 ? 3 : i; // Gaussian
}

int channelsources::index(const QString& source)
{
    static const QStringList sources {Red, Green, Blue, Alpha, Luma, Black, White};
    return int(sources.indexOf(source));
}

const NodePort* NodeDescriptor::input(const QString& id) const
{
    for (const NodePort& p : inputs) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

const ParamDescriptor* NodeDescriptor::param(const QString& key) const
{
    for (const ParamDescriptor& p : params) {
        if (p.key == key)
            return &p;
    }
    return nullptr;
}

const NodeRegistry& NodeRegistry::instance()
{
    static const NodeRegistry registry;
    return registry;
}

void NodeRegistry::add(NodeDescriptor d)
{
    m_index.insert(d.id, m_descriptors.size());
    m_descriptors.append(std::move(d));
}

const NodeDescriptor* NodeRegistry::find(const QString& id) const
{
    const auto it = m_index.constFind(id);
    return it == m_index.constEnd() ? nullptr : &m_descriptors[*it];
}

QVector<const NodeDescriptor*> NodeRegistry::byCategory(NodeCategory category) const
{
    QVector<const NodeDescriptor*> out;
    for (const NodeDescriptor& d : m_descriptors) {
        if (d.category == category)
            out.append(&d);
    }
    return out;
}

ParamSet NodeRegistry::defaultParams(const QString& id) const
{
    ParamSet set;
    const NodeDescriptor* d = find(id);
    if (!d)
        return set;
    for (const ParamDescriptor& p : d->params) {
        if (p.isScalar())
            set.setBase(p.key, p.def);
        else
            set.setStatic(p.key, p.defStatic);
    }
    return set;
}

bool NodeRegistry::hasCommonControls(const QString& id) const
{
    const NodeDescriptor* d = find(id);
    return d && id != nodetypes::MediaOut && d->category != NodeCategory::Mask
        && d->category != NodeCategory::Time && id != nodetypes::Saver
        && d->outputKind != PortKind::Particles;
}

QVector<ParamDescriptor> NodeRegistry::commonControls(const QString& id) const
{
    namespace k = nodekeys;
    QVector<ParamDescriptor> out;
    if (!hasCommonControls(id))
        return out;
    if (id != nodetypes::Merge)
        out.append(number(k::CommonBlend, tr("Blend"), 0, 1, 1, 0.01));
    out += {boolean(k::CommonRed, tr("Process Red"), true),
        boolean(k::CommonGreen, tr("Process Green"), true),
        boolean(k::CommonBlue, tr("Process Blue"), true),
        boolean(k::CommonAlpha, tr("Process Alpha"), true),
        boolean(k::MaskInverted, tr("Apply Mask Inverted"), false),
        boolean(k::MultiplyByMask, tr("Multiply By Mask"), false),
        boolean(k::ProcessAtZero, tr("Process When Blend Is 0.0"), true)};
    if (id == nodetypes::Merge || id == nodetypes::Transform)
        out += {boolean(k::MotionBlur, tr("Motion Blur"), false),
            integer(k::MotionBlurQuality, tr("Quality"), 1, 10, 2),
            number(k::ShutterAngle, tr("Shutter Angle"), 0, 720, 100, 1),
            number(k::CenterBias, tr("Center Bias"), -1, 1, 0, 0.01),
            number(k::SampleSpread, tr("Sample Spread"), 0, 2, 1, 0.01)};
    return out;
}

QString NodeRegistry::categoryName(NodeCategory category)
{
    switch (category) {
    case NodeCategory::InputOutput:
        return tr("I/O");
    case NodeCategory::Composite:
        return tr("Composite");
    case NodeCategory::Transform:
        return tr("Transform");
    case NodeCategory::Generator:
        return tr("Generator");
    case NodeCategory::Mask:
        return tr("Mask");
    case NodeCategory::Color:
        return tr("Color");
    case NodeCategory::Matte:
        return tr("Matte");
    case NodeCategory::Filter:
        return tr("Filter");
    case NodeCategory::Warp:
        return tr("Warp");
    case NodeCategory::Time:
        return tr("Time");
    case NodeCategory::Particles:
        return tr("Particles");
    case NodeCategory::Effect:
        return tr("Effects");
    }
    return {};
}

NodeRegistry::NodeRegistry()
{
    namespace k = nodekeys;
    using C = NodeCategory;

    // ---- I/O ----
    {
        NodeDescriptor d = node(nodetypes::MediaIn, tr("MediaIn"), QStringLiteral("MI"),
            QStringLiteral("MediaIn"), C::InputOutput,
            tr("The clip's picture (or another media pool item) at the composition's frame"));
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::MediaOut, tr("MediaOut"), QStringLiteral("MO"),
            QStringLiteral("MediaOut"), C::InputOutput,
            tr("The result of the composition: what the clip shows on the timeline"));
        d.inputs = {inputPort()};
        d.mainInput = nodeports::Input;
        d.outputs = 0;
        add(d);
    }

    {
        // A container of other nodes (spec 120): no ports, never evaluated; made by Group.
        NodeDescriptor d = node(nodetypes::Group, tr("Group"), QStringLiteral("Grp"),
            QStringLiteral("Group"), C::InputOutput,
            tr("Holds other nodes together: a frame around them, or one tile; with controls it is a Macro"));
        d.outputs = 0;
        add(d);
    }

    {
        NodeDescriptor d = node(nodetypes::Loader, tr("Loader"), QStringLiteral("Ldr"),
            QStringLiteral("Loader"), C::InputOutput,
            tr("Reads an image, an image sequence or a movie file from disk into the graph"));
        d.params = {tip(builders::file(k::LoaderPath, tr("Filename"),
                            tr("Images and movies (*)")),
                        tr("The file to read. Pick one file of a numbered sequence (name_0001.png) and the whole sequence is read; a single image is shown on every frame")),
            tip(integer(k::GlobalIn, tr("Global In"), -100000, 100000, 0),
                tr("The composition frame where the first frame of the clip plays")),
            tip(integer(k::TrimIn, tr("Trim In"), 0, 100000, 0),
                tr("How many frames are cut from the start of the clip")),
            tip(integer(k::TrimOut, tr("Trim Out"), 0, 100000, 0),
                tr("How many frames are cut from the end of the clip")),
            tip(integer(k::HoldFirst, tr("Hold First Frame"), 0, 100000, 0),
                tr("How many frames the first frame stays before the clip plays")),
            tip(integer(k::HoldLast, tr("Hold Last Frame"), 0, 100000, 0),
                tr("How many frames the last frame stays after the clip has played")),
            tip(boolean(k::Loop, tr("Loop"), false),
                tr("Plays the clip again and again, with its held frames, instead of showing nothing after the end")),
            tip(boolean(k::Reverse, tr("Reverse"), false), tr("Plays the clip backwards"))};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::Saver, tr("Saver"), QStringLiteral("Svr"),
            QStringLiteral("Saver"), C::InputOutput,
            tr("Writes its input to image files when the composition is rendered; the picture passes through unchanged"));
        d.inputs = {inputPort()};
        d.mainInput = nodeports::Input;
        d.params = {tip(builders::text(k::SaverPath, tr("Filename"), QString()),
                        tr("Where the files go. The frame number is added to the name: shot.png becomes shot0000.png, shot0001.png and so on")),
            tip(choice(k::SaverFormat, tr("Format"),
                    {saverformats::Png, saverformats::Tiff}, {tr("PNG"), tr("TIFF")},
                    saverformats::Png),
                tr("The file format of the images")),
            tip(integer(k::SaverStart, tr("First Number"), 0, 1000000, 0),
                tr("The number added to the first file name (the first frame of the range written)")),
            tip(boolean(k::SaverActive, tr("Save When Rendered"), true),
                tr("Off: this Saver writes nothing when the composition is rendered"))};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::TimeSpeed, tr("Time Speed"), QStringLiteral("Spd"),
            QStringLiteral("TimeSpeed"), C::Time,
            tr("Plays the input faster, slower, backwards or later"));
        d.inputs = {inputPort()};
        d.mainInput = nodeports::Input;
        d.params = {tip(number(k::TimeSpeedKey, tr("Speed"), -100, 100, 1, 0.01),
                        tr("1 plays at the normal speed, 0.5 at half speed, 2 twice as fast, a negative number backwards")),
            tip(number(k::TimeDelay, tr("Delay"), -100000, 100000, 0, 1),
                tr("Frames the input starts later (positive) or earlier (negative)")),
            tip(choice(k::TimeInterp, tr("Interpolation"),
                    {timeinterps::Nearest, timeinterps::Blend}, {tr("Nearest"), tr("Blend")},
                    timeinterps::Nearest),
                tr("Nearest shows the closest frame; Blend mixes the two frames around the time"))};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::TimeStretcher, tr("Time Stretcher"), QStringLiteral("Str"),
            QStringLiteral("TimeStretcher"), C::Time,
            tr("Shows the input at a time you set, which can be keyframed to speed up, slow down, freeze or reverse"));
        d.inputs = {inputPort()};
        d.mainInput = nodeports::Input;
        d.params = {tip(number(k::SourceTime, tr("Source Time"), -100000, 100000, 0, 1),
                        tr("The frame of the input to show now. Keyframe it: 0 at the first frame and 100 at the last frame plays 100 frames of the input; the steeper the curve, the faster")),
            tip(choice(k::TimeInterp, tr("Interpolation"),
                    {timeinterps::Nearest, timeinterps::Blend}, {tr("Nearest"), tr("Blend")},
                    timeinterps::Nearest),
                tr("Nearest shows the closest frame; Blend mixes the two frames around the time"))};
        add(d);
    }

    // ---- Particles (spec 129) ----
    {
        NodeDescriptor d = node(nodetypes::PEmitter, tr("pEmitter"), QStringLiteral("pEm"),
            QStringLiteral("pEmitter"), C::Particles,
            tr("Creates particles every frame from a point, a line, a rectangle or a circle"));
        d.outputKind = PortKind::Particles;
        d.inputs = {port(nodeports::Sprite, tr("Sprite"), PortKind::Image, false)};
        d.params = {tip(integer(k::Seed, tr("Random Seed"), 0, 100000, 0),
                        tr("The same seed gives the same particles; change it for another random pattern")),
            tip(number(k::PNumber, tr("Number"), 0, 10000, 5, 0.1),
                tr("How many particles are born on every frame")),
            tip(number(k::PNumberVar, tr("Number Variance"), 0, 10000, 0, 0.1),
                tr("A random amount, up to this many more or fewer, added to Number on each frame")),
            tip(number(k::PLife, tr("Lifespan"), 1, 10000, 50, 1),
                tr("How many frames a particle lives")),
            tip(number(k::PLifeVar, tr("Lifespan Variance"), 0, 10000, 0, 1),
                tr("A random amount, up to this many frames more or fewer, added to each particle's lifespan")),
            tip(integer(k::PStart, tr("Start Frame"), -100000, 100000, 0),
                tr("The first frame that creates particles")),
            tip(integer(k::PEnd, tr("End Frame"), -100000, 1000000, 100000),
                tr("The last frame that creates particles")),
            tip(choice(k::PRegion, tr("Region"),
                    {particleregions::Point, particleregions::Line, particleregions::Rectangle,
                        particleregions::Circle},
                    {tr("Point"), tr("Line"), tr("Rectangle"), tr("Circle")}, particleregions::Point),
                tr("Where particles appear: at the centre, along a line, inside a rectangle or inside a circle")),
            tip(number(k::CenterX, tr("Center X"), -10, 10, 0.5, 0.001),
                tr("The middle of the region, 0 left, 1 right")),
            tip(number(k::CenterY, tr("Center Y"), -10, 10, 0.5, 0.001),
                tr("The middle of the region, 0 bottom, 1 top")),
            tip(number(k::Width, tr("Region Width"), 0, 10, 0.2, 0.001),
                tr("Width of the line or rectangle, or the diameter of the circle, as a part of the frame width")),
            tip(number(k::Height, tr("Region Height"), 0, 10, 0.2, 0.001),
                tr("Height of the rectangle as a part of the frame width")),
            tip(number(k::PVelocity, tr("Velocity"), -1, 1, 0.01, 0.0005),
                tr("How far a particle moves on every frame, as a part of the frame width")),
            tip(number(k::PVelocityVar, tr("Velocity Variance"), 0, 1, 0, 0.0005),
                tr("A random amount, up to this much more or less, added to each particle's velocity")),
            tip(number(k::Angle, tr("Angle"), -3600, 3600, 90, 0.1, kDegrees),
                tr("The direction the particles fly: 0 to the right, 90 up, 180 to the left, 270 down")),
            tip(number(k::PAngleVar, tr("Angle Variance"), 0, 360, 15, 0.1, kDegrees),
                tr("A random amount, up to this many degrees either way, added to the direction")),
            tip(number(k::PRotation, tr("Rotation"), -3600, 3600, 0, 0.1, kDegrees),
                tr("The turn of a particle when it is born (only a Bitmap shows it)")),
            tip(number(k::PRotationVar, tr("Rotation Variance"), 0, 360, 0, 0.1, kDegrees),
                tr("A random amount, up to this many degrees either way, added to the turn at birth")),
            tip(number(k::PSpin, tr("Spin"), -360, 360, 0, 0.1, kDegrees),
                tr("How many degrees a particle turns on every frame")),
            tip(number(k::PSpinVar, tr("Spin Variance"), 0, 360, 0, 0.1, kDegrees),
                tr("A random amount, up to this many degrees either way, added to the spin")),
            tip(choice(k::PStyle, tr("Style"),
                    {particlestyles::Point, particlestyles::Blob, particlestyles::Bitmap},
                    {tr("Point"), tr("Blob"), tr("Bitmap")}, particlestyles::Blob),
                tr("Point is a hard dot, Blob a soft dot, Bitmap draws the picture on the Sprite input")),
            tip(color(k::Color, tr("Color"), QColor(255, 255, 255)),
                tr("The colour of a particle when it is born")),
            tip(color(k::PColorEnd, tr("End Color"), QColor(255, 255, 255)),
                tr("The colour at the end of its life; the colour changes smoothly between the two")),
            tip(number(k::PSize, tr("Size"), 0, 2, 0.03, 0.001),
                tr("The diameter of a particle when it is born, as a part of the frame width")),
            tip(number(k::PSizeEnd, tr("End Size"), 0, 2, 0.03, 0.001),
                tr("The diameter at the end of its life; the size changes smoothly between the two")),
            tip(number(k::PSizeVar, tr("Size Variance"), 0, 1, 0, 0.01),
                tr("A random part of the size, up to this much more or less, for each particle")),
            tip(number(k::POpacity, tr("Opacity"), 0, 1, 1, 0.01),
                tr("How solid a particle is when it is born")),
            tip(number(k::POpacityEnd, tr("End Opacity"), 0, 1, 0, 0.01),
                tr("How solid it is at the end of its life; 0 fades the particle out"))};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::PGravity, tr("pGravity"), QStringLiteral("pGr"),
            QStringLiteral("pGravity"), C::Particles,
            tr("A constant force that pulls the particles in one direction"));
        d.outputKind = PortKind::Particles;
        d.inputs = {port(nodeports::Input, tr("Particles"), PortKind::Particles, true)};
        d.mainInput = nodeports::Input;
        d.params = {tip(number(k::PStrength, tr("Strength"), -1, 1, 0.0005, 0.0001),
                        tr("How much the speed changes on every frame, as a part of the frame width")),
            tip(number(k::Angle, tr("Direction"), -3600, 3600, 270, 0.1, kDegrees),
                tr("The direction of the pull: 270 is down, 90 up, 0 to the right")),
            tip(boolean(k::PActive, tr("Active"), true),
                tr("Off: the force does nothing"))};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::PTurbulence, tr("pTurbulence"), QStringLiteral("pTb"),
            QStringLiteral("pTurbulence"), C::Particles,
            tr("Pushes the particles about with smooth random noise"));
        d.outputKind = PortKind::Particles;
        d.inputs = {port(nodeports::Input, tr("Particles"), PortKind::Particles, true)};
        d.mainInput = nodeports::Input;
        d.params = {tip(number(k::PStrength, tr("Strength"), 0, 1, 0.002, 0.0001),
                        tr("The most the noise can change the speed on one frame, as a part of the frame width")),
            tip(number(k::PDensity, tr("Density"), 0.1, 200, 4, 0.1),
                tr("How many noise cells fit across the frame width; a high number gives small, busy swirls")),
            tip(number(k::PEvolution, tr("Evolution"), 0, 10, 0.05, 0.01),
                tr("How fast the noise pattern changes from frame to frame")),
            tip(integer(k::Seed, tr("Random Seed"), 0, 100000, 0),
                tr("The same seed gives the same noise; change it for another pattern")),
            tip(boolean(k::PActive, tr("Active"), true),
                tr("Off: the force does nothing"))};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::PMerge, tr("pMerge"), QStringLiteral("pMg"),
            QStringLiteral("pMerge"), C::Particles,
            tr("Joins two streams of particles into one"));
        d.outputKind = PortKind::Particles;
        d.inputs = {port(nodeports::Input, tr("Particles"), PortKind::Particles, true),
            port(nodeports::Input2, tr("Merge Particles"), PortKind::Particles, true)};
        d.mainInput = nodeports::Input;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::PRender, tr("pRender"), QStringLiteral("pRn"),
            QStringLiteral("pRender"), C::Particles,
            tr("Draws a stream of particles as a picture the size of the composition (2D)"));
        d.inputs = {port(nodeports::Input, tr("Particles"), PortKind::Particles, true)};
        d.mainInput = nodeports::Input;
        add(d);
    }

    // ---- Composite ----
    {
        NodeDescriptor d = node(nodetypes::Merge, tr("Merge"), QStringLiteral("Mrg"),
            QStringLiteral("Merge"), C::Composite, tr("Puts the foreground over the background"));
        d.inputs = {port(nodeports::Background, tr("Background"), PortKind::Image, true),
            port(nodeports::Foreground, tr("Foreground"), PortKind::Image, true), maskPort()};
        d.mainInput = nodeports::Background;
        d.params = {choice(k::BlendMode, tr("Apply Mode"), blendmodes::ids(), blendModeNames(),
                        blendmodes::Normal),
            choice(k::Operation, tr("Operator"), mergeops::ids(),
                {tr("Over"), tr("In"), tr("Held Out"), tr("Atop"), tr("XOr")}, mergeops::Over),
            number(k::Opacity, tr("Blend"), 0, 1, 1, 0.01),
            number(k::SubAdd, tr("Subtractive / Additive"), 0, 1, 0, 0.01),
            number(k::AlphaGain, tr("Alpha Gain"), 0, 1, 1, 0.01),
            number(k::BurnIn, tr("Burn In"), 0, 1, 0, 0.01)};
        d.params += centre();
        d.params += {number(k::PivotX, tr("Pivot X"), -10, 10, 0.5, 0.001),
            number(k::PivotY, tr("Pivot Y"), -10, 10, 0.5, 0.001),
            number(k::Size, tr("Size"), 0, 100, 1, 0.01), angle(),
            boolean(k::FlipH, tr("Flip Horizontal"), false),
            boolean(k::FlipV, tr("Flip Vertical"), false),
            boolean(k::InvertTransform, tr("Invert Transform"), false), edgesParam(),
            filterParam(), boolean(k::UseFrameFormat, tr("Use Frame Format Settings"), true),
            number(k::RefWidth, tr("Reference Width"), 1, 16384, 1920, 1, kPx),
            number(k::RefHeight, tr("Reference Height"), 1, 16384, 1080, 1, kPx)};
        d.controls = ViewerControls::Transform;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::Dissolve, tr("Dissolve"), QStringLiteral("Dx"),
            QStringLiteral("Dissolve"), C::Composite,
            tr("Mixes between the background and the foreground"));
        d.inputs = {port(nodeports::Background, tr("Background"), PortKind::Image, true),
            port(nodeports::Foreground, tr("Foreground"), PortKind::Image, true), maskPort()};
        d.mainInput = nodeports::Background;
        d.params = {number(k::Mix, tr("Background / Foreground"), 0, 1, 0, 0.01)};
        add(d);
    }

    // ---- Transform ----
    {
        NodeDescriptor d = node(nodetypes::Transform, tr("Transform"), QStringLiteral("Xf"),
            QStringLiteral("Transform"), C::Transform,
            tr("Moves, scales, rotates and flips the picture"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = centre();
        d.params += {number(k::PivotX, tr("Pivot X"), -10, 10, 0.5, 0.001),
            number(k::PivotY, tr("Pivot Y"), -10, 10, 0.5, 0.001),
            number(k::Size, tr("Size"), 0, 100, 1, 0.01),
            number(k::Aspect, tr("Aspect"), 0.01, 100, 1, 0.01), angle(),
            boolean(k::FlipH, tr("Flip Horizontal"), false),
            boolean(k::FlipV, tr("Flip Vertical"), false),
            boolean(k::InvertTransform, tr("Invert Transform"), false), edgesParam(),
            filterParam()};
        d.controls = ViewerControls::Transform;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::Tracker, tr("Tracker"), QStringLiteral("Trk"),
            QStringLiteral("Tracker"), C::Transform,
            tr("Follows a point in the picture; the input moves with it (Match Move) or is "
               "held steady (Stabilize)"));
        d.inputs = {inputPort(),
            port(nodeports::Source, tr("Tracking Source"), PortKind::Image, false, true)};
        d.mainInput = nodeports::Input;
        d.params = centre();
        d.params += {number(nodekeys::PatternSize, tr("Pattern Size"), 8, 400, 48, 1, kPx),
            number(nodekeys::SearchSize, tr("Search Size"), 16, 1600, 160, 1, kPx),
            choice(nodekeys::TrackMode, tr("Output"),
                {trackmodes::MatchMove, trackmodes::Stabilize}, {tr("Match Move"), tr("Stabilize")},
                trackmodes::MatchMove),
            integer(nodekeys::RefFrame, tr("Reference Frame"), 0, 1000000, 0),
            integer(nodekeys::Patterns, tr("Patterns"), 1, nodekeys::kMaxPatterns, 1),
            integer(nodekeys::ActivePattern, tr("Active Pattern"), 1, nodekeys::kMaxPatterns, 1),
            boolean(nodekeys::TrackPosition, tr("Match Position"), true),
            boolean(nodekeys::TrackRotation, tr("Match Rotation"), true),
            boolean(nodekeys::TrackScale, tr("Match Scale"), true)};
        // Patterns 2 to 4: positions kept as hidden keyframes (the viewer drags them).
        static constexpr double kStartX[4] = {0.5, 0.6, 0.5, 0.6}, kStartY[4] = {0.5, 0.5, 0.6, 0.6};
        for (int n = 2; n <= nodekeys::kMaxPatterns; ++n) {
            for (const bool y : {false, true}) {
                ParamDescriptor p = number(nodekeys::patternKey(n, y),
                    tr("Pattern %1").arg(n) + (y ? tr(" Y") : tr(" X")), -10, 10,
                    y ? kStartY[n - 1] : kStartX[n - 1], 0.001);
                p.hidden = true;
                d.params += p;
            }
        }
        for (const QString& key : {nodekeys::PatternSize, nodekeys::SearchSize, nodekeys::RefFrame,
                 nodekeys::Patterns, nodekeys::ActivePattern}) {
            for (ParamDescriptor& p : d.params) {
                if (p.key == key)
                    p.animatable = false;
            }
        }
        d.controls = ViewerControls::Tracker;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::PlanarTracker, tr("Planar Tracker"),
            QStringLiteral("PTrk"), QStringLiteral("PlanarTracker"), C::Transform,
            tr("Follows a flat surface (four corners); the input is pinned onto it (Corner Pin) "
               "or moves with it (Unsteady) or is held steady (Steady)"));
        d.inputs = {inputPort(),
            port(nodeports::Source, tr("Tracking Source"), PortKind::Image, false, true),
            port(nodeports::TrackMask, tr("Track Mask"), PortKind::Mask, false, true)};
        d.mainInput = nodeports::Input;
        // Default region: the middle half of the frame. Corner order: top-left, top-right,
        // bottom-right, bottom-left (names of the Inspector rows).
        const char* const names[4] = {QT_TRANSLATE_NOOP("kk::NodeRegistry", "Top Left"),
            QT_TRANSLATE_NOOP("kk::NodeRegistry", "Top Right"),
            QT_TRANSLATE_NOOP("kk::NodeRegistry", "Bottom Right"),
            QT_TRANSLATE_NOOP("kk::NodeRegistry", "Bottom Left")};
        const double xs[4] = {0.25, 0.75, 0.75, 0.25}, ys[4] = {0.75, 0.75, 0.25, 0.25};
        for (int i = 0; i < nodekeys::kPlanarCorners; ++i) {
            d.params += number(
                nodekeys::planarCorner(i, false), tr(names[i]) + tr(" X"), -10, 10, xs[i], 0.001);
            d.params += number(
                nodekeys::planarCorner(i, true), tr(names[i]) + tr(" Y"), -10, 10, ys[i], 0.001);
        }
        d.params += {number(nodekeys::PatternSize, tr("Pattern Size"), 8, 400, 21, 1, kPx),
            number(nodekeys::SearchSize, tr("Search Size"), 4, 400, 48, 1, kPx),
            choice(nodekeys::TrackMode, tr("Output"),
                {trackmodes::CornerPin, trackmodes::MatchMove, trackmodes::Stabilize},
                {tr("Corner Pin"), tr("Unsteady (Match Move)"), tr("Steady (Stabilize)")},
                trackmodes::CornerPin),
            integer(nodekeys::RefFrame, tr("Reference Frame"), 0, 1000000, 0)};
        for (const QString& key :
            {nodekeys::PatternSize, nodekeys::SearchSize, nodekeys::RefFrame}) {
            for (ParamDescriptor& p : d.params) {
                if (p.key == key)
                    p.animatable = false;
            }
        }
        d.controls = ViewerControls::Planar;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::CornerPositioner, tr("Corner Positioner"),
            QStringLiteral("CPos"), QStringLiteral("CornerPositioner"), C::Transform,
            tr("Pins the four corners of the picture to new places (a perspective warp)"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        const char* const names[4] = {QT_TRANSLATE_NOOP("kk::NodeRegistry", "Top Left"),
            QT_TRANSLATE_NOOP("kk::NodeRegistry", "Top Right"),
            QT_TRANSLATE_NOOP("kk::NodeRegistry", "Bottom Right"),
            QT_TRANSLATE_NOOP("kk::NodeRegistry", "Bottom Left")};
        const double xs[4] = {0, 1, 1, 0}, ys[4] = {1, 1, 0, 0};
        for (int i = 0; i < nodekeys::kPlanarCorners; ++i) {
            d.params += number(
                nodekeys::planarCorner(i, false), tr(names[i]) + tr(" X"), -10, 10, xs[i], 0.001);
            d.params += number(
                nodekeys::planarCorner(i, true), tr(names[i]) + tr(" Y"), -10, 10, ys[i], 0.001);
        }
        d.controls = ViewerControls::Planar;
        add(d);
    }
    {
        NodeDescriptor d
            = node(nodetypes::Crop, tr("Crop"), QStringLiteral("Crp"), QStringLiteral("Crop"),
                C::Transform, tr("Cuts the edges off the picture (they become transparent)"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {number(k::Left, tr("Left"), 0, 8192, 0, 1, kPx),
            number(k::Right, tr("Right"), 0, 8192, 0, 1, kPx),
            number(k::Top, tr("Top"), 0, 8192, 0, 1, kPx),
            number(k::Bottom, tr("Bottom"), 0, 8192, 0, 1, kPx)};
        add(d);
    }
    {
        NodeDescriptor d
            = node(nodetypes::Resize, tr("Resize"), QStringLiteral("Rsz"), QStringLiteral("Resize"),
                C::Transform, tr("Scales the picture to a size in pixels, centred in the frame"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {number(k::Width, tr("Width"), 1, 16384, 1920, 1, kPx),
            number(k::Height, tr("Height"), 1, 16384, 1080, 1, kPx),
            boolean(k::KeepAspect, tr("Keep Aspect"), false)};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::Letterbox, tr("Letterbox"), QStringLiteral("LBx"),
            QStringLiteral("Letterbox"), C::Transform,
            tr("Covers the picture with bars to a wider (or narrower) aspect ratio"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {number(k::TargetAspect, tr("Aspect Ratio"), 0.1, 10, 2.39, 0.01),
            color(k::Color, tr("Bar Color"), QColor(Qt::black))};
        add(d);
    }

    // ---- Generators ----
    {
        NodeDescriptor d = node(nodetypes::Background, tr("Background"), QStringLiteral("BG"),
            QStringLiteral("Background"), C::Generator,
            tr("A solid colour or a two or four colour gradient"));
        d.inputs = {maskPort()};
        d.params
            = {choice(k::Mode, tr("Type"),
                   {backgroundmodes::Solid, backgroundmodes::Gradient, backgroundmodes::FourCorner},
                   {tr("Solid Color"), tr("Gradient"), tr("Four Corner")}, backgroundmodes::Solid),
                color(k::Color, tr("Color"), QColor(0, 0, 0)),
                color(k::Color2, tr("Color 2"), QColor(255, 255, 255)),
                color(k::Color3, tr("Color 3"), QColor(255, 0, 0)),
                color(k::Color4, tr("Color 4"), QColor(0, 0, 255)),
                number(k::GradientAngle, tr("Gradient Angle"), -360, 360, 0, 0.5, kDegrees)};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::Text, tr("Text"), QStringLiteral("Txt"),
            QStringLiteral("Text"), C::Generator, tr("Text with outline and shadow"));
        d.inputs = {maskPort()};
        d.params = {text(k::Text, tr("Text"), tr("Custom Title")),
            text(k::Font, tr("Font"), QStringLiteral("Sans Serif")),
            number(k::FontSize, tr("Size"), 1, 1000, 96, 1, kPx),
            boolean(k::Bold, tr("Bold"), false), boolean(k::Italic, tr("Italic"), false),
            color(k::Color, tr("Color"), QColor(Qt::white)),
            color(k::OutlineColor, tr("Outline Color"), QColor(Qt::black)),
            number(k::OutlineWidth, tr("Outline Width"), 0, 100, 0, 0.5, kPx),
            boolean(k::Shadow, tr("Drop Shadow"), false),
            number(k::WriteOnStart, tr("Write On Start"), 0, 1, 0, 0.01),
            number(k::WriteOnEnd, tr("Write On End"), 0, 1, 1, 0.01)};
        d.params += centre();
        d.params += {angle()};
        // Layout.
        d.params += {choice(k::LayoutType, tr("Layout"),
                         {textlayouts::Point, textlayouts::Frame, textlayouts::Circle,
                             textlayouts::Path},
                         {tr("Point"), tr("Frame"), tr("Circle"), tr("Path")}, textlayouts::Point),
            choice(k::Justify, tr("Justify"),
                {justifies::Start, justifies::Center, justifies::End},
                {tr("Left"), tr("Center"), tr("Right")}, justifies::Center),
            choice(k::JustifyV, tr("Vertical Justify (Frame)"),
                {justifies::Start, justifies::Center, justifies::End},
                {tr("Top"), tr("Center"), tr("Bottom")}, justifies::Center),
            number(k::FrameWidth, tr("Frame Width"), 0.01, 10, 0.8, 0.001),
            number(k::FrameHeight, tr("Frame Height"), 0.01, 10, 0.5, 0.001),
            number(k::CircleRadius, tr("Circle Radius"), 0.01, 10, 0.25, 0.001),
            number(k::CircleAngle, tr("Circle Angle"), -360, 360, 90, 0.5, kDegrees),
            number(k::PathOffset, tr("Position on Path"), -1, 1, 0, 0.001),
            number(k::TextTracking, tr("Tracking"), -50, 500, 0, 0.5, QStringLiteral("%")),
            number(k::LineSpacing, tr("Line Spacing"), 10, 500, 100, 0.5, QStringLiteral("%")),
            text(k::Kerning, tr("Kerning"), QString())};
        // Shading elements 2 to 4 (element 1 is the main look above), drawn behind it.
        for (int n = 2; n < 2 + k::kShadeElements; ++n) {
            const QString num = QString::number(n);
            d.params += {boolean(k::shadeKey(n, "enable"), tr("Element %1").arg(num), false),
                boolean(k::shadeKey(n, "outline"), tr("Element %1 Outline").arg(num), false),
                color(k::shadeKey(n, "color"), tr("Element %1 Color").arg(num),
                    n == 2 ? QColor(Qt::black) : n == 3 ? QColor(255, 200, 0) : QColor(Qt::red)),
                number(k::shadeKey(n, "opacity"), tr("Element %1 Opacity").arg(num), 0, 1, 1, 0.01),
                number(k::shadeKey(n, "thickness"), tr("Element %1 Thickness").arg(num), 0, 200, 4,
                    0.5, kPx),
                number(k::shadeKey(n, "x"), tr("Element %1 Offset X").arg(num), -1000, 1000, 0, 0.5,
                    kPx),
                number(k::shadeKey(n, "y"), tr("Element %1 Offset Y").arg(num), -1000, 1000, 0, 0.5,
                    kPx)};
        }
        // Follower: the animation of these values is delayed for each character.
        d.params += {integer(k::FollowDelay, tr("Follower Delay"), -100, 100, 0),
            number(k::FollowX, tr("Follower Offset X"), -2000, 2000, 0, 0.5, kPx),
            number(k::FollowY, tr("Follower Offset Y"), -2000, 2000, 0, 0.5, kPx),
            number(k::FollowSize, tr("Follower Size"), 0, 1000, 100, 0.5, QStringLiteral("%")),
            number(k::FollowAngle, tr("Follower Angle"), -3600, 3600, 0, 0.5, kDegrees),
            number(k::FollowOpacity, tr("Follower Opacity"), 0, 100, 100, 0.5, QStringLiteral("%"))};
        for (ParamDescriptor& p : d.params) {
            if (p.key == k::FollowDelay)
                p.animatable = false;
        }
        d.controls = ViewerControls::TextPath;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::FastNoise, tr("Fast Noise"), QStringLiteral("FN"),
            QStringLiteral("FastNoise"), C::Generator,
            tr("Smooth, optionally evolving fractal noise between two colours"));
        d.inputs = {maskPort()};
        d.params = {integer(k::Detail, tr("Detail"), 1, 8, 2),
            number(k::Contrast, tr("Contrast"), 0, 10, 1, 0.01),
            number(k::Brightness, tr("Brightness"), -1, 1, 0, 0.01),
            number(k::Scale, tr("Scale"), 0.1, 1000, 10, 0.1),
            number(k::Seethe, tr("Seethe Rate"), 0, 10, 0, 0.01),
            integer(k::Seed, tr("Seed"), 0, 100000, 0),
            color(k::Color, tr("Color 1"), QColor(Qt::black)),
            color(k::Color2, tr("Color 2"), QColor(Qt::white))};
        d.params += centre();
        d.params += {angle()};
        add(d);
    }

    // ---- Masks ----
    const auto shapeMask = [&](const QString& id, const QString& name, const QString& shortName,
                               const QString& base, const QString& description) {
        NodeDescriptor d = node(id, name, shortName, base, C::Mask, description);
        d.inputs = {port(nodeports::Mask, tr("Mask"), PortKind::Mask, false)};
        d.mainInput = nodeports::Mask;
        d.outputKind = PortKind::Mask;
        d.params = centre();
        d.params += {number(k::Width, tr("Width"), 0, 10, 0.5, 0.001),
            number(k::Height, tr("Height"), 0, 10, 0.5, 0.001), angle()};
        d.params += maskCommon();
        d.controls = ViewerControls::Rect;
        return d;
    };
    {
        NodeDescriptor d = shapeMask(nodetypes::RectangleMask, tr("Rectangle"),
            QStringLiteral("Rct"), QStringLiteral("Rectangle"), tr("A rectangular mask"));
        // After Angle, before the shared Soft Edge / Invert / Level / Paint Mode.
        d.params.insert(d.params.size() - maskCommon().size(),
            number(nodekeys::CornerRadius, tr("Corner Radius"), 0, 1, 0, 0.001));
        add(d);
    }
    add(shapeMask(nodetypes::EllipseMask, tr("Ellipse"), QStringLiteral("Elp"),
        QStringLiteral("Ellipse"), tr("An elliptical mask")));
    {
        NodeDescriptor d = node(nodetypes::PolygonMask, tr("Polygon"), QStringLiteral("Ply"),
            QStringLiteral("Polygon"), C::Mask, tr("A mask from a closed polygon"));
        d.inputs = {port(nodeports::Mask, tr("Mask"), PortKind::Mask, false)};
        d.mainInput = nodeports::Mask;
        d.outputKind = PortKind::Mask;
        d.params = centre();
        d.params += {number(k::Size, tr("Size"), 0, 100, 1, 0.01), angle()};
        d.params += maskCommon();
        d.controls = ViewerControls::Polygon;
        add(d);
    }

    {
        NodeDescriptor d = node(nodetypes::BSplineMask, tr("B-Spline"), QStringLiteral("BSp"),
            QStringLiteral("BSpline"), C::Mask,
            tr("A smooth mask from a closed B-spline; the points pull the curve, a heavier "
               "point pulls harder"));
        d.inputs = {port(nodeports::Mask, tr("Mask"), PortKind::Mask, false)};
        d.mainInput = nodeports::Mask;
        d.outputKind = PortKind::Mask;
        d.params = centre();
        d.params += {number(k::Size, tr("Size"), 0, 100, 1, 0.01), angle()};
        d.params += maskCommon();
        d.controls = ViewerControls::Polygon;
        add(d);
    }

    {
        NodeDescriptor d = node(nodetypes::BezierMask, tr("Bezier"), QStringLiteral("Bzr"),
            QStringLiteral("Bezier"), C::Mask,
            tr("A mask from a closed Bezier curve; every point has two tangent handles"));
        d.inputs = {port(nodeports::Mask, tr("Mask"), PortKind::Mask, false)};
        d.mainInput = nodeports::Mask;
        d.outputKind = PortKind::Mask;
        d.params = centre();
        d.params += {number(k::Size, tr("Size"), 0, 100, 1, 0.01), angle(),
            boolean(k::RotoAssist, tr("Roto Assist"), false)};
        d.params += maskCommon();
        d.controls = ViewerControls::Bezier;
        add(d);
    }

    // ---- Color ----
    {
        NodeDescriptor d = node(nodetypes::ColorCorrector, tr("Color Corrector"),
            QStringLiteral("CC"), QStringLiteral("ColorCorrector"), C::Color,
            tr("Lift, gamma, gain, saturation and levels in one node"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {number(k::Lift, tr("Lift"), -1, 1, 0, 0.001),
            number(k::Gamma, tr("Gamma"), 0.1, 4, 1, 0.01),
            number(k::Gain, tr("Gain"), 0, 4, 1, 0.01),
            number(k::Saturation, tr("Saturation"), 0, 4, 1, 0.01),
            number(k::InBlack, tr("Input Black"), 0, 1, 0, 0.001),
            number(k::InWhite, tr("Input White"), 0, 1, 1, 0.001),
            number(k::LevelGamma, tr("Levels Gamma"), 0.1, 10, 1, 0.01),
            number(k::OutBlack, tr("Output Black"), 0, 1, 0, 0.001),
            number(k::OutWhite, tr("Output White"), 0, 1, 1, 0.001)};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::ChannelBooleans, tr("Channel Booleans"),
            QStringLiteral("Bol"), QStringLiteral("ChannelBooleans"), C::Color,
            tr("Copies, swaps or replaces the colour and alpha channels"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        const QStringList values {channelsources::Red, channelsources::Green, channelsources::Blue,
            channelsources::Alpha, channelsources::Luma, channelsources::Black,
            channelsources::White};
        const QStringList labels {
            tr("Red"), tr("Green"), tr("Blue"), tr("Alpha"), tr("Luma"), tr("Black"), tr("White")};
        d.params = {choice(k::ToRed, tr("To Red"), values, labels, channelsources::Red),
            choice(k::ToGreen, tr("To Green"), values, labels, channelsources::Green),
            choice(k::ToBlue, tr("To Blue"), values, labels, channelsources::Blue),
            choice(k::ToAlpha, tr("To Alpha"), values, labels, channelsources::Alpha)};
        add(d);
    }

    // ---- Colour nodes of spec 123 ----
    {
        NodeDescriptor d = node(nodetypes::BrightnessContrast, tr("Brightness Contrast"),
            QStringLiteral("BC"), QStringLiteral("BrightnessContrast"), C::Color,
            tr("Gain, lift, gamma, contrast, brightness and saturation of the picture"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {tip(boolean(k::BcRed, tr("Red"), true), tr("Apply the controls to the red channel")),
            tip(boolean(k::BcGreen, tr("Green"), true), tr("Apply the controls to the green channel")),
            tip(boolean(k::BcBlue, tr("Blue"), true), tr("Apply the controls to the blue channel")),
            tip(boolean(k::BcAlpha, tr("Alpha"), false), tr("Apply the controls to the alpha channel as well")),
            tip(number(k::BcGain, tr("Gain"), 0, 10, 1, 0.01), tr("Multiplies the values: brightens the highlights most")),
            tip(number(k::BcLift, tr("Lift"), -1, 1, 0, 0.01), tr("Raises (or lowers) the blacks; white stays white")),
            tip(number(k::BcGamma, tr("Gamma"), 0.1, 10, 1, 0.01), tr("Above 1 brightens the middle tones, below 1 darkens them")),
            tip(number(k::BcContrast, tr("Contrast"), -1, 4, 0, 0.01), tr("Pushes the values away from mid grey (0.5), or towards it when negative")),
            tip(number(k::BcBrightness, tr("Brightness"), -1, 1, 0, 0.01), tr("Adds the same amount to every value")),
            tip(number(k::Saturation, tr("Saturation"), 0, 4, 1, 0.01), tr("0 is grey, 1 leaves the colour as it is, above 1 is more vivid")),
            tip(number(k::BcLow, tr("Low"), 0, 1, 0, 0.001), tr("The value that becomes black: everything below it is stretched out of the picture")),
            tip(number(k::BcHigh, tr("High"), 0, 1, 1, 0.001), tr("The value that becomes white: everything above it is stretched out of the picture"))};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::AlphaMultiply, tr("Alpha Multiply"), QStringLiteral("AMu"),
            QStringLiteral("AlphaMultiply"), C::Color,
            tr("Multiplies the colour by the alpha: the edges and soft parts get darker"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::AlphaDivide, tr("Alpha Divide"), QStringLiteral("ADv"),
            QStringLiteral("AlphaDivide"), C::Color,
            tr("Divides the colour by the alpha: the edges and soft parts get brighter. Alpha Multiply undoes it"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::ChannelMapper, tr("Channel Mapper"), QStringLiteral("ChM"),
            QStringLiteral("ChannelMapper"), C::Color,
            tr("Builds each of red, green, blue and alpha from a mix of the four input channels"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        const QStringList names {tr("Red"), tr("Green"), tr("Blue"), tr("Alpha")};
        for (int o = 0; o < 4; ++o) {
            for (int i = 0; i < 4; ++i) {
                d.params.append(tip(number(k::mapperKey(o, i),
                                        tr("%1 from %2").arg(names[o], names[i]), -4, 4,
                                        o == i ? 1 : 0, 0.01),
                    tr("How much of the input's %1 goes into the output's %2 (1 = all of it, 0 = none, negative inverts)")
                        .arg(names[i].toLower(), names[o].toLower())));
            }
            d.params.append(tip(number(k::mapperOffsetKey(o), tr("%1 Offset").arg(names[o]), -1, 1, 0, 0.01),
                tr("Added to the output's %1 after the mix").arg(names[o].toLower())));
        }
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::ColorCurves, tr("Color Curves"), QStringLiteral("CCv"),
            QStringLiteral("ColorCurves"), C::Color,
            tr("Tone curves for the luminance, red, green, blue and alpha channels"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        const QString identity = kk::curve::identityText();
        d.params = {tip(builders::curve(k::CurveLuma, tr("Luminance"), identity), tr("Curve applied to all three colour channels: click to add a point, drag to bend it")),
            tip(builders::curve(k::CurveRed, tr("Red"), identity), tr("Curve of the red channel")),
            tip(builders::curve(k::CurveGreen, tr("Green"), identity), tr("Curve of the green channel")),
            tip(builders::curve(k::CurveBlue, tr("Blue"), identity), tr("Curve of the blue channel")),
            tip(builders::curve(k::CurveAlpha, tr("Alpha"), identity), tr("Curve of the alpha channel"))};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::HueCurves, tr("Hue Curves"), QStringLiteral("HCv"),
            QStringLiteral("HueCurves"), C::Color,
            tr("Changes hue, saturation or brightness of selected colours with curves"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        const QString flat = QStringLiteral("0,0.5 1,0.5");
        d.params = {tip(builders::curve(k::CurveHueHue, tr("Hue vs Hue"), flat), tr("Shifts the hue of the colours at the curve's hue (the middle line leaves them; up or down turns them)")),
            tip(builders::curve(k::CurveHueSat, tr("Hue vs Sat"), flat), tr("Saturation of the colours at the curve's hue (the middle line is no change, the bottom is grey, the top doubles it)")),
            tip(builders::curve(k::CurveHueLum, tr("Hue vs Lum"), flat), tr("Brightness of the colours at the curve's hue (the middle line is no change)")),
            tip(builders::curve(k::CurveLumSat, tr("Lum vs Sat"), flat), tr("Saturation by brightness: dark to bright from left to right")),
            tip(builders::curve(k::CurveSatSat, tr("Sat vs Sat"), flat), tr("Saturation by saturation: grey to vivid from left to right"))};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::CustomTool, tr("Custom Tool"), QStringLiteral("Cus"),
            QStringLiteral("CustomTool"), C::Color,
            tr("Computes red, green, blue and alpha of every pixel from formulas"));
        d.inputs = {inputPort(), port(nodeports::Image2, tr("Image 2"), PortKind::Image, false),
            maskPort()};
        d.mainInput = nodeports::Input;
        const QString vars = tr("Variables: x y (position 0 to 1, y up), w h (frame size), time, "
                                "r1 g1 b1 a1 (Input), r2 g2 b2 a2 (Image 2), n1 to n8, i1 to i4");
        for (int i = 0; i < 8; ++i) {
            d.params.append(tip(number(k::customNumberKey(i), tr("Number %1").arg(i + 1), -1000, 1000, 0, 0.01),
                tr("A value the formulas read as n%1; it can be keyframed").arg(i + 1)));
        }
        for (int i = 0; i < 4; ++i) {
            d.params.append(tip(text(k::customInterKey(i), tr("Intermediate %1").arg(i + 1), QString()),
                tr("A formula whose result the channel formulas read as i%1 (it may use i1 to i%2). %3")
                    .arg(i + 1).arg(i).arg(vars)));
        }
        const QStringList chans {tr("Red"), tr("Green"), tr("Blue"), tr("Alpha")};
        const QStringList defs {QStringLiteral("r1"), QStringLiteral("g1"), QStringLiteral("b1"), QStringLiteral("a1")};
        const QList<QString> keys {k::CustomRed, k::CustomGreen, k::CustomBlue, k::CustomAlpha};
        for (int i = 0; i < 4; ++i) {
            d.params.append(tip(text(keys[i], chans[i], defs[i]),
                tr("Formula for the %1 channel, 0 to 1 (straight colour). Operators + - * / % ^ and comparisons, functions such as sin, pow, clamp, mix, smoothstep, if. %2")
                    .arg(chans[i].toLower(), vars)));
        }
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::VectorMotionBlur, tr("Vector Motion Blur"),
            QStringLiteral("VMB"), QStringLiteral("VectorMotionBlur"), C::Filter,
            tr("Blurs the picture along motion vectors stored in a second picture's red and green"));
        d.inputs = {inputPort(), port(nodeports::Map, tr("Vectors"), PortKind::Image, true),
            maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {tip(number(k::VectorRange, tr("Vector Range"), 0, 2000, 32, 0.5, kPx), tr("How many pixels of motion a red or green value of 0 or 1 stands for (0.5 is no motion)")),
            tip(number(k::VmbScale, tr("Scale"), 0, 8, 1, 0.01), tr("Multiplies the length of the blur")),
            tip(number(k::VmbBias, tr("Center Bias"), -1, 1, 0, 0.01), tr("0 blurs both ways along the vector, 1 only behind the motion, -1 only ahead of it")),
            tip(integer(k::VmbSamples, tr("Samples"), 2, 64, 16), tr("How many pictures are averaged along the vector: more is smoother and slower")),
            tip(edgesParam(), tr("What shows where the blur reaches outside the picture"))};
        add(d);
    }

    // ---- Matte ----
    {
        NodeDescriptor d = node(nodetypes::LumaKeyer, tr("Luma Keyer"), QStringLiteral("LKy"),
            QStringLiteral("LumaKeyer"), C::Matte,
            tr("Keeps the pixels whose brightness (or another channel) lies between Low and High"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {choice(k::KeyChannel, tr("Channel"),
                        {channelsources::Red, channelsources::Green, channelsources::Blue,
                            channelsources::Alpha, channelsources::Luma},
                        {tr("Red"), tr("Green"), tr("Blue"), tr("Alpha"), tr("Luminance")},
                        channelsources::Luma),
            number(k::KeyLow, tr("Low"), 0, 1, 0, 0.001),
            number(k::KeyHigh, tr("High"), 0, 1, 1, 0.001),
            number(k::SoftRange, tr("Soft Range"), 0, 1, 0, 0.001),
            boolean(k::Invert, tr("Invert"), false)};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::DeltaKeyer, tr("Delta Keyer"), QStringLiteral("DKy"),
            QStringLiteral("DeltaKeyer"), C::Matte,
            tr("Keys out a green or blue screen and removes its spill"));
        d.inputs = {inputPort(), garbagePort(), solidPort(),
            port(nodeports::CleanPlate, tr("Clean Plate"), PortKind::Image, false), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {viewModeParam(),
            color(k::Color, tr("Background Color"), QColor(0, 200, 0)),
            number(k::PreBlur, tr("Pre-Blur"), 0, 100, 0, 0.5, kPx),
            number(k::Gain, tr("Gain"), 0, 4, 1, 0.01),
            number(k::KeyBalance, tr("Balance"), 0, 1, 0.5, 0.01),
            number(k::PreSoft, tr("Pre-Matte Soft Range"), 0, 1, 0, 0.01),
            number(k::PreMatteErode, tr("Pre-Matte Erode"), -50, 50, 0, 0.5, kPx),
            number(k::PreMatteBlur, tr("Pre-Matte Blur"), 0, 100, 0, 0.5, kPx)};
        d.params += matteTabParams(tr("Matte Threshold Low"), tr("Matte Threshold High"), true);
        d.params += {choice(k::ReplaceMode, tr("Replace Mode"),
                         {replacemodes::None, replacemodes::Source, replacemodes::Hard,
                             replacemodes::Soft},
                         {tr("None"), tr("Source"), tr("Hard Color"), tr("Soft Color")},
                         replacemodes::None),
            color(k::ReplaceColor, tr("Replace Color"), QColor(128, 128, 128))};
        d.params += fringeTabParams(tr("Despill"), spillmethods::Medium);
        d.params += maskInvertParams();
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::UltraKeyer, tr("Ultra Keyer"), QStringLiteral("UKy"),
            QStringLiteral("UltraKeyer"), C::Matte,
            tr("Keys out a green or blue screen with a pre-matte, a colour-difference key and spill control"));
        d.inputs = {inputPort(), garbagePort(), solidPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {viewModeParam(),
            color(k::Color, tr("Background Color"), QColor(0, 200, 0)),
            number(k::UkRedLevel, tr("Red Level"), 0, 2, 1, 0.01),
            number(k::UkGreenLevel, tr("Green Level"), 0, 2, 1, 0.01),
            number(k::UkBlueLevel, tr("Blue Level"), 0, 2, 1, 0.01),
            number(k::MatteSeparation, tr("Matte Separation"), 0, 1, 0, 0.01),
            number(k::PreMatteSize, tr("Pre Matte Size"), 0, 100, 0, 0.5, kPx),
            boolean(k::SubtractBackground, tr("Subtract Background"), true)};
        d.params += fringeTabParams(tr("Spill Suppression"), spillmethods::Medium);
        d.params += matteTabParams(tr("Threshold Low"), tr("Threshold High"), false);
        d.params += maskInvertParams();
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::MatteControl, tr("Matte Control"), QStringLiteral("MCt"),
            QStringLiteral("MatteControl"), C::Matte,
            tr("Cleans up the alpha of a picture: blur, grow or shrink, thresholds, restore fringe, garbage and solid mattes"));
        d.inputs = {inputPort(), garbagePort(), solidPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = matteTabParams(tr("Threshold Low"), tr("Threshold High"), true);
        d.params += maskInvertParams();
        add(d);
    }

    // ---- Filter ----
    {
        NodeDescriptor d = node(nodetypes::SoftGlow, tr("Soft Glow"), QStringLiteral("SGl"),
            QStringLiteral("SoftGlow"), C::Filter,
            tr("A soft glow around the bright parts of the picture"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {choice(k::BlurFilter, tr("Filter"), blurfilters::ids(),
                        {tr("Box"), tr("Bartlett"), tr("Multi-box"), tr("Gaussian")},
                        blurfilters::Gaussian),
            number(k::GlowThreshold, tr("Threshold"), 0, 4, 0.5, 0.01),
            number(k::Gain, tr("Gain"), 0, 10, 1, 0.01),
            boolean(k::LockXY, tr("Lock X/Y"), true),
            number(k::GlowSize, tr("Glow Size"), 0, 500, 20, 0.5, kPx),
            number(k::GlowSizeY, tr("Glow Size Y"), 0, 500, 20, 0.5, kPx),
            integer(k::NumPasses, tr("Num Passes"), 1, 10, 4),
            boolean(k::GlowRed, tr("Glow Red"), true),
            boolean(k::GlowGreen, tr("Glow Green"), true),
            boolean(k::GlowBlue, tr("Glow Blue"), true),
            boolean(k::GlowAlpha, tr("Glow Alpha"), true),
            number(k::GlowScaleR, tr("Color Scale Red"), 0, 4, 1, 0.01),
            number(k::GlowScaleG, tr("Color Scale Green"), 0, 4, 1, 0.01),
            number(k::GlowScaleB, tr("Color Scale Blue"), 0, 4, 1, 0.01),
            number(k::GlowScaleA, tr("Color Scale Alpha"), 0, 4, 1, 0.01)};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::LightWrap, tr("Light Wrap"), QStringLiteral("LWr"),
            QStringLiteral("LightWrap"), C::Filter,
            tr("Lets the light of the background spill over the edges of the foreground. "
               "Connect the keyed subject to Foreground and the picture it is merged over to "
               "Background, then Merge the result over that same picture"));
        d.inputs = {port(nodeports::Input, tr("Foreground"), PortKind::Image, true),
            port(nodeports::Background, tr("Background"), PortKind::Image, true), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {number(k::WrapIntensity, tr("Intensity"), 0, 4, 0.5, 0.01),
            number(k::WrapDiffuse, tr("Diffuse"), 0, 300, 15, 0.5, kPx),
            number(k::WrapFgBlur, tr("Foreground Blur"), 0, 300, 10, 0.5, kPx),
            number(k::WrapBgBlur, tr("Background Blur"), 0, 300, 0, 0.5, kPx),
            number(k::Saturation, tr("Saturation"), 0, 4, 1, 0.01),
            number(k::WrapLumaTolerance, tr("Luma Tolerance"), 0, 1, 0, 0.01),
            boolean(k::WrapLumaBased, tr("Luminance Based Wrap"), true),
            boolean(k::WrapGlow, tr("Enable Glow"), false),
            boolean(k::WrapOnly, tr("Generate Wrap Only"), false),
            choice(k::WrapMerge, tr("Highlight Merge"),
                {wrapmerges::Add, wrapmerges::Screen}, {tr("Add"), tr("Screen")}, wrapmerges::Add),
            boolean(k::WrapUseConstant, tr("Use Constant Highlight"), false),
            color(k::WrapColor, tr("Constant"), QColor(255, 255, 255))};
        add(d);
    }

    // ---- Warp ----
    {
        NodeDescriptor d = node(nodetypes::GridWarp, tr("Grid Warp"), QStringLiteral("GWp"),
            QStringLiteral("GridWarp"), C::Warp,
            tr("Bends the picture with a grid of points that you drag in the viewer"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.controls = ViewerControls::Grid;
        d.params = {integer(k::Columns, tr("Columns"), 2, k::kGridMaxSide, 4),
            integer(k::Rows, tr("Rows"), 2, k::kGridMaxSide, 4)};
        for (int r = 0; r < k::kGridMaxSide; ++r) {
            for (int c = 0; c < k::kGridMaxSide; ++c) {
                for (int axis = 0; axis < 2; ++axis) {
                    ParamDescriptor p = number(k::gridKey(r, c, axis),
                        tr("Point %1, %2").arg(r + 1).arg(c + 1) + (axis == 0 ? tr(" X") : tr(" Y")),
                        -4000, 4000, 0, 0.5, kPx);
                    p.hidden = true;
                    d.params.append(p);
                }
            }
        }
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::Displace, tr("Displace"), QStringLiteral("Dsp"),
            QStringLiteral("Displace"), C::Warp,
            tr("Moves the pixels of the picture by the values of a second picture"));
        d.inputs = {inputPort(), port(nodeports::Map, tr("Displace Map"), PortKind::Image, true),
            maskPort()};
        d.mainInput = nodeports::Input;
        const QStringList chans {channelsources::Red, channelsources::Green, channelsources::Blue,
            channelsources::Alpha, channelsources::Luma};
        const QStringList chanLabels {tr("Red"), tr("Green"), tr("Blue"), tr("Alpha"), tr("Luminance")};
        d.params = {choice(k::DisplaceType, tr("Type"), {displacetypes::XY, displacetypes::Radial},
                        {tr("X / Y"), tr("Radial")}, displacetypes::XY),
            choice(k::DisplaceXChannel, tr("X Channel"), chans, chanLabels, channelsources::Red),
            choice(k::DisplaceYChannel, tr("Y Channel"), chans, chanLabels, channelsources::Green),
            number(k::DisplaceXRefraction, tr("X Refraction"), -2000, 2000, 20, 0.5, kPx),
            number(k::DisplaceYRefraction, tr("Y Refraction"), -2000, 2000, 20, 0.5, kPx),
            number(k::DisplaceOffset, tr("Offset"), -1, 2, 0.5, 0.01),
            number(k::CenterX, tr("Center X"), -10, 10, 0.5, 0.001),
            number(k::CenterY, tr("Center Y"), -10, 10, 0.5, 0.001),
            number(k::DisplaceLightPower, tr("Light Power"), -4, 4, 0, 0.01),
            number(k::DisplaceLightAngle, tr("Light Angle"), -360, 360, 45, 1, kDegrees),
            edgesParam()};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::VectorDistort, tr("Vector Distort"),
            QStringLiteral("VDs"), QStringLiteral("VectorDistort"), C::Warp,
            tr("Warps the picture along vectors stored in a second picture's red and green"));
        d.inputs = {inputPort(), port(nodeports::Map, tr("Vectors"), PortKind::Image, true),
            maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {number(k::VectorRange, tr("Vector Range"), 0, 2000, 32, 0.5, kPx),
            number(k::VectorMultiplier, tr("Multiplier"), -8, 8, 1, 0.01),
            integer(k::VectorSteps, tr("Steps"), 1, 8, 1), edgesParam()};
        add(d);
    }

    // ---- Every Edit page video effect as a node ----
    for (const EffectDescriptor* e :
        EffectRegistry::instance().byCategory(EffectCategory::VideoEffect)) {
        NodeDescriptor d = node(effectNodeType(e->id), e->name, shortNameOf(e->name),
            nameBaseOf(e->name), C::Effect, e->description);
        d.group = e->group;
        d.effectId = e->id;
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = e->params;
        add(d);
    }
}

} // namespace kk
