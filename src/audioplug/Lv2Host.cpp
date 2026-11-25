// SPDX-License-Identifier: GPL-3.0-or-later
// Lv2Host — see Lv2Host.h. Hosts LV2 effects: audio ports (the first non-side-chain inputs and
// outputs are the main ones), control input ports become parameters, a control output port
// that reports latency is read after every block; atom ports get an empty sequence (no MIDI, no
// patch messages, no plugin GUI). Features given to plugins: urid:map, urid:unmap, options (sample
// rate and block lengths), bufsz:boundedBlockLength.
#include "audioplug/Lv2Host.h"

#include "audioplug/AudioPluginCore.h"

#include <QCoreApplication>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>

#ifdef KK_HAVE_LV2
#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#pragma GCC diagnostic ignored "-Wdeprecated-declarations" // the old uri-map is what old plugins ask for
#include <lv2/uri-map/uri-map.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <map>
#include <string>
#include <vector>
#endif

namespace kk::audioplug::lv2 {

#ifndef KK_HAVE_LV2

bool supported()
{
    return false;
}

BundleInfo describe(const QString& bundleFolder)
{
    BundleInfo b;
    b.path = bundleFolder;
    b.format = QString::fromLatin1(kFormatLv2);
    b.error = QStringLiteral("this build of Kinoklip has no LV2 support (lilv was not found when it was built)");
    return b;
}

std::unique_ptr<Instance> create(const PluginInfo&, const QString&, double, QString* error)
{
    *error = QStringLiteral("this build of Kinoklip has no LV2 support");
    return nullptr;
}

#else

bool supported()
{
    return true;
}

namespace {

QMutex gWorldMutex;
LilvWorld* gWorld = nullptr;
QStringList gLoadedBundles;

/// The world that knows the specifications of LV2 and every bundle that was asked for. lilv
/// is not thread-safe: callers hold gWorldMutex.
LilvWorld* world()
{
    if (!gWorld) {
        gWorld = lilv_world_new();
        // Where the LV2 specifications are: LV2_PATH, the copy that travels with the AppImage
        // (share/kinoklip/lv2-specs next to the program) and the usual folders.
        QStringList specDirs = QString::fromLocal8Bit(qgetenv("LV2_PATH")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
        specDirs << QCoreApplication::applicationDirPath() + QStringLiteral("/../share/kinoklip/lv2-specs")
                 << QDir::homePath() + QStringLiteral("/.lv2") << QStringLiteral("/usr/lib/lv2")
                 << QStringLiteral("/usr/local/lib/lv2") << QStringLiteral("/usr/lib64/lv2")
                 << QStringLiteral("/app/lib/lv2"); // the Flatpak
        LilvNode* path = lilv_new_string(gWorld, specDirs.join(QLatin1Char(':')).toLocal8Bit().constData());
        lilv_world_set_option(gWorld, LILV_OPTION_LV2_PATH, path);
        lilv_node_free(path);
        lilv_world_load_specifications(gWorld);
        lilv_world_load_plugin_classes(gWorld);
    }
    return gWorld;
}

bool loadBundle(const QString& folder)
{
    if (gLoadedBundles.contains(folder))
        return true;
    QByteArray path = folder.toLocal8Bit();
    if (!path.endsWith('/'))
        path += '/';
    LilvNode* uri = lilv_new_file_uri(world(), nullptr, path.constData());
    lilv_world_load_bundle(world(), uri);
    lilv_node_free(uri);
    gLoadedBundles.append(folder);
    return true;
}

QString nodeString(const LilvNode* n)
{
    return n && (lilv_node_is_string(n) || lilv_node_is_uri(n)) ? QString::fromUtf8(lilv_node_as_string(n)) : QString();
}

QString takeString(LilvNode* n)
{
    const QString s = nodeString(n);
    lilv_node_free(n);
    return s;
}

QString unitSymbol(const QString& uri)
{
    static const struct
    {
        const char* suffix;
        const char* text;
    } kUnits[] = {{"/units#db", "dB"}, {"/units#hz", "Hz"}, {"/units#khz", "kHz"}, {"/units#ms", "ms"},
        {"/units#s", "s"}, {"/units#pc", "%"}, {"/units#semitone12TET", "semitones"}, {"/units#cent", "cents"},
        {"/units#oct", "octaves"}, {"/units#bpm", "BPM"}, {"/units#min", "min"}, {"/units#mhz", "MHz"},
        {"/units#coef", ""}, {"/units#degree", "°"}};
    for (const auto& u : kUnits) {
        if (uri.endsWith(QLatin1String(u.suffix)))
            return QString::fromUtf8(u.text);
    }
    return {};
}

struct Nodes
{
    LilvWorld* w;
    LilvNode* audioPort;
    LilvNode* controlPort;
    LilvNode* inputPort;
    LilvNode* outputPort;
    LilvNode* cvPort;
    LilvNode* atomPort;
    LilvNode* sideChain;
    LilvNode* toggled;
    LilvNode* integer;
    LilvNode* enumeration;
    LilvNode* logarithmic;
    LilvNode* sampleRate;
    LilvNode* notOnGui;
    LilvNode* reportsLatency;
    LilvNode* connectionOptional;
    LilvNode* unitUnit;
    LilvNode* designation;
    LilvNode* latency;
    LilvNode* instrumentClass;
    LilvNode* groupProp;
    LilvNode* label;
    explicit Nodes(LilvWorld* world_)
        : w(world_)
    {
        auto u = [&](const char* s) { return lilv_new_uri(w, s); };
        audioPort = u(LV2_CORE__AudioPort);
        controlPort = u(LV2_CORE__ControlPort);
        inputPort = u(LV2_CORE__InputPort);
        outputPort = u(LV2_CORE__OutputPort);
        cvPort = u(LV2_CORE__CVPort);
        atomPort = u(LV2_ATOM__AtomPort);
        sideChain = u(LV2_CORE_PREFIX "isSideChain");
        toggled = u(LV2_CORE__toggled);
        integer = u(LV2_CORE__integer);
        enumeration = u(LV2_CORE__enumeration);
        logarithmic = u("http://lv2plug.in/ns/ext/port-props#logarithmic");
        sampleRate = u(LV2_CORE__sampleRate);
        notOnGui = u("http://lv2plug.in/ns/ext/port-props#notOnGUI");
        reportsLatency = u(LV2_CORE__reportsLatency);
        connectionOptional = u(LV2_CORE__connectionOptional);
        unitUnit = u("http://lv2plug.in/ns/extensions/units#unit");
        designation = u(LV2_CORE__designation);
        latency = u(LV2_CORE__latency);
        instrumentClass = u("http://lv2plug.in/ns/lv2core#InstrumentPlugin");
        groupProp = u("http://lv2plug.in/ns/ext/port-groups#group");
        label = u("http://www.w3.org/2000/01/rdf-schema#label");
    }
    ~Nodes()
    {
        for (LilvNode* n : {audioPort, controlPort, inputPort, outputPort, cvPort, atomPort, sideChain, toggled,
                 integer, enumeration, logarithmic, sampleRate, notOnGui, reportsLatency, connectionOptional,
                 unitUnit, designation, latency, instrumentClass, groupProp, label})
            lilv_node_free(n);
    }
};

bool supportedFeature(const char* uri)
{
    static const char* const kOk[] = {LV2_URID__map, LV2_URID__unmap, LV2_OPTIONS__options, LV2_LOG__log,
        LV2_URI_MAP_URI,
        LV2_BUF_SIZE__boundedBlockLength, LV2_CORE__hardRTCapable, LV2_CORE__inPlaceBroken, LV2_CORE__isLive,
        "http://lv2plug.in/ns/ext/buf-size#fixedBlockLength", "http://lv2plug.in/ns/ext/buf-size#powerOf2BlockLength"};
    for (const char* ok : kOk) {
        if (!std::strcmp(ok, uri))
            return true;
    }
    return false;
}

PluginInfo describePlugin(const Nodes& n, const LilvPlugin* plugin)
{
    PluginInfo info;
    info.format = QString::fromLatin1(kFormatLv2);
    info.id = QString::fromUtf8(lilv_node_as_uri(lilv_plugin_get_uri(plugin)));
    info.name = takeString(lilv_plugin_get_name(plugin));
    if (info.name.isEmpty())
        info.name = info.id;
    info.vendor = takeString(lilv_plugin_get_author_name(plugin));
    if (const LilvPluginClass* c = lilv_plugin_get_class(plugin))
        info.category = nodeString(lilv_plugin_class_get_label(c));

    // features the plugin cannot do without
    if (LilvNodes* req = lilv_plugin_get_required_features(plugin)) {
        QStringList missing;
        LILV_FOREACH (nodes, i, req) {
            const char* uri = lilv_node_as_uri(lilv_nodes_get(req, i));
            if (uri && !supportedFeature(uri))
                missing.append(QString::fromUtf8(uri));
        }
        lilv_nodes_free(req);
        if (!missing.isEmpty())
            info.error = QStringLiteral("it needs host features Kinoklip does not offer (%1)").arg(missing.join(QLatin1String(", ")));
    }

    const uint32_t portCount = lilv_plugin_get_num_ports(plugin);
    info.portCount = portCount;
    for (uint32_t i = 0; i < portCount; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
        const bool isInput = lilv_port_is_a(plugin, port, n.inputPort);
        const bool isOutput = lilv_port_is_a(plugin, port, n.outputPort);
        if (lilv_port_is_a(plugin, port, n.audioPort)) {
            if (lilv_port_has_property(plugin, port, n.sideChain))
                continue;
            if (isInput)
                info.audioInPorts.append(i);
            else if (isOutput)
                info.audioOutPorts.append(i);
        } else if (lilv_port_is_a(plugin, port, n.controlPort)) {
            if (isOutput) {
                if (lilv_port_has_property(plugin, port, n.reportsLatency))
                    info.latencyPort = int(i);
                else if (LilvNodes* des = lilv_port_get_value(plugin, port, n.designation)) {
                    if (lilv_nodes_contains(des, n.latency))
                        info.latencyPort = int(i);
                    lilv_nodes_free(des);
                }
                continue;
            }
            if (!isInput)
                continue;
            ParamInfo p;
            p.id = i;
            p.key = nodeString(lilv_port_get_symbol(plugin, port));
            p.label = takeString(lilv_port_get_name(plugin, port));
            if (p.label.isEmpty())
                p.label = p.key;
            LilvNode *def = nullptr, *mn = nullptr, *mx = nullptr;
            lilv_port_get_range(plugin, port, &def, &mn, &mx);
            p.def = def ? double(lilv_node_as_float(def)) : 0.0;
            p.min = mn ? double(lilv_node_as_float(mn)) : std::min(0.0, p.def);
            p.max = mx ? double(lilv_node_as_float(mx)) : std::max(1.0, p.def);
            lilv_node_free(def);
            lilv_node_free(mn);
            lilv_node_free(mx);
            if (lilv_port_has_property(plugin, port, n.sampleRate)) { // limits as multiples of the rate
                p.min *= 48000.0;
                p.max *= 48000.0;
                p.def *= 48000.0;
            }
            p.toggle = lilv_port_has_property(plugin, port, n.toggled);
            p.integer = p.toggle || lilv_port_has_property(plugin, port, n.integer);
            p.logarithmic = lilv_port_has_property(plugin, port, n.logarithmic) && p.min > 0.0;
            p.hidden = lilv_port_has_property(plugin, port, n.notOnGui);
            if (LilvNodes* units = lilv_port_get_value(plugin, port, n.unitUnit)) {
                if (const LilvNode* first = lilv_nodes_get_first(units))
                    p.unit = unitSymbol(nodeString(first));
                lilv_nodes_free(units);
            }
            if (LilvScalePoints* points = lilv_port_get_scale_points(plugin, port)) {
                struct Point
                {
                    double value;
                    QString label;
                };
                std::vector<Point> list;
                LILV_FOREACH (scale_points, s, points) {
                    const LilvScalePoint* sp = lilv_scale_points_get(points, s);
                    const LilvNode* v = lilv_scale_point_get_value(sp);
                    const LilvNode* l = lilv_scale_point_get_label(sp);
                    if (v && l && (lilv_node_is_float(v) || lilv_node_is_int(v)))
                        list.push_back({double(lilv_node_as_float(v)), nodeString(l)});
                }
                lilv_scale_points_free(points);
                std::sort(list.begin(), list.end(), [](const Point& a, const Point& b) { return a.value < b.value; });
                // a list of choices when the port says so, or when every integer step has a label
                const bool declared = lilv_port_has_property(plugin, port, n.enumeration);
                if (!p.toggle && list.size() >= 2 && (declared || (p.integer && list.size() <= 64))) {
                    p.enumeration = true;
                    p.integer = true;
                    for (const Point& pt : list) {
                        p.enumValues.append(pt.value);
                        p.enumLabels.append(pt.label);
                    }
                }
            }
            if (!(p.max >= p.min) || !std::isfinite(p.min) || !std::isfinite(p.max)) {
                p.min = 0.0;
                p.max = 1.0;
            }
            if (!std::isfinite(p.def))
                p.def = p.min;
            p.def = std::clamp(p.def, p.min, p.max);
            info.params.append(p);
        } else if (!lilv_port_is_a(plugin, port, n.atomPort) && !lilv_port_is_a(plugin, port, n.cvPort)) {
            if (info.error.isEmpty() && !lilv_port_has_property(plugin, port, n.connectionOptional))
                info.error = QStringLiteral("it has a port of a kind Kinoklip does not know");
        }
    }
    info.inChannels = int(info.audioInPorts.size());
    info.outChannels = int(info.audioOutPorts.size());
    if (info.error.isEmpty() && (info.inChannels == 0 || info.outChannels == 0))
        info.error = QStringLiteral("it has no audio input and output (an instrument or analyser, not an effect)");
    // the class tells what makes a tail
    const QString cat = info.category.toLower();
    if (cat.contains(QLatin1String("reverb")))
        info.tailSeconds = 4.0;
    else if (cat.contains(QLatin1String("delay")) || cat.contains(QLatin1String("echo")))
        info.tailSeconds = 3.0;
    uniqueKeys(&info);
    return info;
}

// ---- URID map ----

struct UridMap
{
    std::vector<std::string> uris;
    std::map<std::string, uint32_t> ids;
    QMutex mutex;
    LV2_URID_Map map {};
    LV2_URID_Unmap unmap {};
    UridMap()
    {
        map.handle = this;
        map.map = [](LV2_URID_Map_Handle h, const char* uri) -> LV2_URID {
            auto* self = static_cast<UridMap*>(h);
            QMutexLocker lock(&self->mutex);
            const auto it = self->ids.find(uri);
            if (it != self->ids.end())
                return it->second;
            self->uris.emplace_back(uri);
            const uint32_t id = uint32_t(self->uris.size());
            self->ids.emplace(uri, id);
            return id;
        };
        unmap.handle = this;
        unmap.unmap = [](LV2_URID_Unmap_Handle h, LV2_URID urid) -> const char* {
            auto* self = static_cast<UridMap*>(h);
            QMutexLocker lock(&self->mutex);
            return urid >= 1 && urid <= self->uris.size() ? self->uris[urid - 1].c_str() : nullptr;
        };
    }
    LV2_URID id(const char* uri) { return map.map(this, uri); }
};

int silentLogPrintf(LV2_Log_Handle, LV2_URID, const char*, ...)
{
    return 0;
}
int silentLogVprintf(LV2_Log_Handle, LV2_URID, const char*, va_list)
{
    return 0;
}

/// One LV2 plugin instance on planar channels.
class Lv2Core final : public Core
{
public:
    Lv2Core(const PluginInfo& info, const LilvPlugin* plugin, double rate, QString* error)
        : m_info(info)
        , m_ctl(info.portCount, 0.0f)
        , m_silence(Instance::kMaxBlock, 0.0f)
        , m_scratch(Instance::kMaxBlock, 0.0f)
        , m_atoms(info.portCount)
    {
        m_rate = rate;
        // options: rate and block lengths
        m_sampleRate = float(rate);
        const LV2_URID kInt = m_urids.id(LV2_ATOM__Int);
        const LV2_URID kFloat = m_urids.id(LV2_ATOM__Float);
        m_minBlock = 1;
        m_maxBlock = Instance::kMaxBlock;
        m_nominalBlock = 512;
        auto option = [](LV2_URID key, uint32_t size, LV2_URID type, const void* value) {
            LV2_Options_Option o {};
            o.context = LV2_OPTIONS_INSTANCE;
            o.subject = 0;
            o.key = key;
            o.size = size;
            o.type = type;
            o.value = value;
            return o;
        };
        m_options[0] = option(m_urids.id(LV2_PARAMETERS__sampleRate), sizeof(float), kFloat, &m_sampleRate);
        m_options[1] = option(m_urids.id(LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), kInt, &m_minBlock);
        m_options[2] = option(m_urids.id(LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), kInt, &m_maxBlock);
        m_options[3] = option(m_urids.id(LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t), kInt, &m_nominalBlock);
        m_options[4] = LV2_Options_Option {};
        m_featureMap = {LV2_URID__map, &m_urids.map};
        m_featureUnmap = {LV2_URID__unmap, &m_urids.unmap};
        m_featureOptions = {LV2_OPTIONS__options, m_options};
        m_featureBounded = {LV2_BUF_SIZE__boundedBlockLength, nullptr};
        // the legacy uri-map (Calf and other older plugins ask for it) and a log that says nothing
        m_uriMap.callback_data = &m_urids;
        m_uriMap.uri_to_id = [](LV2_URI_Map_Callback_Data data, const char*, const char* uri) -> uint32_t {
            return static_cast<UridMap*>(data)->id(uri);
        };
        m_featureUriMap = {LV2_URI_MAP_URI, &m_uriMap};
        m_log.handle = nullptr;
        m_log.printf = silentLogPrintf;
        m_log.vprintf = silentLogVprintf;
        m_featureLog = {LV2_LOG__log, &m_log};
        const LV2_Feature* features[] = {&m_featureMap, &m_featureUnmap, &m_featureOptions, &m_featureBounded,
            &m_featureUriMap, &m_featureLog, nullptr};
        m_instance = lilv_plugin_instantiate(plugin, rate, features);
        if (!m_instance) {
            *error = QStringLiteral("the plugin could not be instantiated (it may need a library or a host feature that is missing)");
            return;
        }
        // control values start at the defaults; every control port needs somewhere to point
        for (const ParamInfo& p : info.params)
            m_ctl[p.id] = float(p.def);
        m_atomSequence = m_urids.id(LV2_ATOM__Sequence);
        m_atomChunk = m_urids.id(LV2_ATOM__Chunk);
        // ports the host does not drive: atom buffers, CV, side-chain audio, latency and other outputs
        const LilvPlugin* pl = plugin;
        Nodes n(world());
        for (uint32_t i = 0; i < info.portCount; ++i) {
            const LilvPort* port = lilv_plugin_get_port_by_index(pl, i);
            if (lilv_port_is_a(pl, port, n.atomPort)) {
                m_atoms[i].assign(kAtomBytes / 8, 0);
                lilv_instance_connect_port(m_instance, i, m_atoms[i].data());
                m_atomIsInput.push_back(lilv_port_is_a(pl, port, n.inputPort) ? int(i) : -int(i) - 1);
            } else if (lilv_port_is_a(pl, port, n.cvPort)) {
                lilv_instance_connect_port(m_instance, i,
                    lilv_port_is_a(pl, port, n.inputPort) ? m_silence.data() : m_scratch.data());
            } else if (lilv_port_is_a(pl, port, n.controlPort)) {
                lilv_instance_connect_port(m_instance, i, &m_ctl[i]);
            } else if (lilv_port_is_a(pl, port, n.audioPort)) {
                // main ports are connected for every block; the rest now
                const bool isIn = lilv_port_is_a(pl, port, n.inputPort);
                const auto& main = isIn ? info.audioInPorts : info.audioOutPorts;
                if (!main.contains(i))
                    lilv_instance_connect_port(m_instance, i, isIn ? m_silence.data() : m_scratch.data());
            }
        }
        lilv_instance_activate(m_instance);
        // some plugins state their latency only after the first block
        prepareAtoms();
        connectMain(m_silence.data(), m_silence.data(), m_scratch.data(), m_scratch.data());
        lilv_instance_run(m_instance, 64);
        readLatency();
        lilv_instance_deactivate(m_instance);
        lilv_instance_activate(m_instance);
    }

    ~Lv2Core() override
    {
        if (m_instance) {
            lilv_instance_deactivate(m_instance);
            lilv_instance_free(m_instance);
        }
    }

    [[nodiscard]] bool ok() const { return m_instance != nullptr; }

    void run(const float* const* in, float* const* out, int frames, const double* values) override
    {
        for (size_t k = 0; k < m_info.params.size(); ++k) {
            const ParamInfo& p = m_info.params[int(k)];
            m_ctl[p.id] = float(std::clamp(values[k], p.min, p.max));
        }
        prepareAtoms();
        const int inCh = int(m_info.audioInPorts.size());
        const int outCh = int(m_info.audioOutPorts.size());
        // main ports: channel 1 and 2 of the host, the rest silence / scratch
        for (int c = 0; c < inCh; ++c)
            lilv_instance_connect_port(m_instance, m_info.audioInPorts[c],
                c < 2 ? const_cast<float*>(in[std::min(c, 1)]) : m_silence.data());
        for (int c = 0; c < outCh; ++c)
            lilv_instance_connect_port(m_instance, m_info.audioOutPorts[c], c < 2 ? out[std::min(c, 1)] : m_scratch.data());
        lilv_instance_run(m_instance, uint32_t(frames));
        readLatency();
    }

    void reset() override
    {
        lilv_instance_deactivate(m_instance);
        lilv_instance_activate(m_instance);
    }

    [[nodiscard]] int latencyFrames() const override { return m_latency; }

private:
    static constexpr size_t kAtomBytes = 8192;

    void connectMain(float* in0, float* in1, float* out0, float* out1)
    {
        for (int c = 0; c < m_info.audioInPorts.size(); ++c)
            lilv_instance_connect_port(m_instance, m_info.audioInPorts[c], c == 0 ? in0 : c == 1 ? in1 : m_silence.data());
        for (int c = 0; c < m_info.audioOutPorts.size(); ++c)
            lilv_instance_connect_port(m_instance, m_info.audioOutPorts[c], c == 0 ? out0 : c == 1 ? out1 : m_scratch.data());
    }

    /// An empty sequence on every atom input, a free chunk on every atom output.
    void prepareAtoms()
    {
        for (int tag : m_atomIsInput) {
            const bool isIn = tag >= 0;
            const uint32_t port = isIn ? uint32_t(tag) : uint32_t(-tag - 1);
            auto* atom = reinterpret_cast<LV2_Atom_Sequence*>(m_atoms[port].data());
            if (isIn) {
                atom->atom.type = m_atomSequence;
                atom->atom.size = sizeof(LV2_Atom_Sequence_Body);
                atom->body.unit = 0;
                atom->body.pad = 0;
            } else {
                atom->atom.type = m_atomChunk;
                atom->atom.size = uint32_t(kAtomBytes - sizeof(LV2_Atom));
            }
        }
    }

    void readLatency()
    {
        if (m_info.latencyPort >= 0) {
            const float l = m_ctl[size_t(m_info.latencyPort)];
            m_latency = std::isfinite(l) ? int(std::clamp(l, 0.0f, float(1 << 20))) : 0;
        }
    }

    PluginInfo m_info;
    double m_rate = 48000.0;
    std::vector<float> m_ctl;
    std::vector<float> m_silence, m_scratch;
    std::vector<std::vector<uint64_t>> m_atoms; ///< 8-byte aligned atom buffers by port index
    std::vector<int> m_atomIsInput; ///< port index (input) or -index-1 (output) of the atom ports
    UridMap m_urids;
    LV2_URID m_atomSequence = 0, m_atomChunk = 0;
    float m_sampleRate = 48000.0f;
    int32_t m_minBlock = 1, m_maxBlock = 0, m_nominalBlock = 0;
    LV2_Options_Option m_options[5] {};
    LV2_Feature m_featureMap {}, m_featureUnmap {}, m_featureOptions {}, m_featureBounded {};
    LV2_Feature m_featureUriMap {}, m_featureLog {};
    LV2_URI_Map_Feature m_uriMap {};
    LV2_Log_Log m_log {};
    LilvInstance* m_instance = nullptr;
    int m_latency = 0;
};

} // namespace

BundleInfo describe(const QString& bundleFolder)
{
    BundleInfo bundle;
    bundle.path = bundleFolder;
    bundle.format = QString::fromLatin1(kFormatLv2);
    QMutexLocker lock(&gWorldMutex);
    loadBundle(bundleFolder);
    Nodes n(world());
    const LilvPlugins* all = lilv_world_get_all_plugins(world());
    const QString wanted = QDir::cleanPath(bundleFolder);
    LILV_FOREACH (plugins, i, all) {
        const LilvPlugin* plugin = lilv_plugins_get(all, i);
        const LilvNode* bundleUri = lilv_plugin_get_bundle_uri(plugin);
        if (!bundleUri)
            continue;
        char* local = lilv_file_uri_parse(lilv_node_as_uri(bundleUri), nullptr);
        const QString folder = local ? QDir::cleanPath(QString::fromLocal8Bit(local)) : QString();
        lilv_free(local);
        if (folder != wanted)
            continue;
        bundle.plugins.append(describePlugin(n, plugin));
    }
    if (bundle.plugins.isEmpty()) {
        bundle.error = QStringLiteral("the bundle contains no plugin (is manifest.ttl missing or wrong?)");
        return bundle;
    }
    // Instantiate and run one block of every usable plugin: one that dies does so here.
    for (PluginInfo& info : bundle.plugins) {
        if (!info.error.isEmpty())
            continue;
        LilvNode* uri = lilv_new_uri(world(), info.id.toUtf8().constData());
        const LilvPlugin* plugin = lilv_plugins_get_by_uri(all, uri);
        lilv_node_free(uri);
        QString error;
        Lv2Core core(info, plugin, 48000.0, &error);
        if (!core.ok())
            info.error = error;
    }
    return bundle;
}

std::unique_ptr<Instance> create(
    const PluginInfo& info, const QString& bundleFolder, double sampleRate, QString* error)
{
    QMutexLocker lock(&gWorldMutex);
    loadBundle(bundleFolder);
    const LilvPlugins* all = lilv_world_get_all_plugins(world());
    LilvNode* uri = lilv_new_uri(world(), info.id.toUtf8().constData());
    const LilvPlugin* plugin = lilv_plugins_get_by_uri(all, uri);
    lilv_node_free(uri);
    if (!plugin) {
        *error = QStringLiteral("the plugin is not in the bundle any more");
        return nullptr;
    }
    const int copies = info.inChannels == 1 && info.outChannels == 1 ? 2 : 1;
    std::vector<std::unique_ptr<Core>> cores;
    std::vector<double> defaults;
    for (const ParamInfo& p : info.params)
        defaults.push_back(p.def);
    for (int c = 0; c < copies; ++c) {
        auto core = std::make_unique<Lv2Core>(info, plugin, sampleRate, error);
        if (!core->ok())
            return nullptr;
        cores.push_back(std::move(core));
    }
    return std::make_unique<ChannelAdapter>(
        std::move(cores), info.inChannels, info.outChannels, int(info.params.size()), defaults);
}

#endif // KK_HAVE_LV2

} // namespace kk::audioplug::lv2
