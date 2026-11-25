// SPDX-License-Identifier: GPL-3.0-or-later
// ClapHost — see ClapHost.h. A host for CLAP effects: audio ports (the main port gets the clip's
// stereo sound, the others silence), parameters (changes go in as parameter events at the start
// of a block), latency and tail. No plugin GUI, no note ports, no state.
#include "audioplug/ClapHost.h"

#include "audioplug/AudioPluginCore.h"

#include <clap/clap.h>

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

namespace kk::audioplug::clap {

namespace {

thread_local bool tlInProcess = false;

// ---- the host the plugins see ----

bool hostIsMainThread(const clap_host_t*) { return !tlInProcess; }
bool hostIsAudioThread(const clap_host_t*) { return tlInProcess; }
const clap_host_thread_check_t kThreadCheck = {hostIsMainThread, hostIsAudioThread};

void hostParamsRescan(const clap_host_t*, clap_param_rescan_flags) { }
void hostParamsClear(const clap_host_t*, clap_id, clap_param_clear_flags) { }
void hostParamsRequestFlush(const clap_host_t*) { }
const clap_host_params_t kHostParams = {hostParamsRescan, hostParamsClear, hostParamsRequestFlush};

void hostLatencyChanged(const clap_host_t*) { }
const clap_host_latency_t kHostLatency = {hostLatencyChanged};

void hostTailChanged(const clap_host_t*) { }
const clap_host_tail_t kHostTail = {hostTailChanged};

void hostLog(const clap_host_t*, clap_log_severity, const char*) { }
const clap_host_log_t kHostLog = {hostLog};

bool hostAudioPortsSupported(const clap_host_t*, uint32_t) { return false; }
void hostAudioPortsRescan(const clap_host_t*, uint32_t) { }
const clap_host_audio_ports_t kHostAudioPorts = {hostAudioPortsSupported, hostAudioPortsRescan};

const void* hostGetExtension(const clap_host_t*, const char* id)
{
    if (!id)
        return nullptr;
    if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK))
        return &kThreadCheck;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &kHostParams;
    if (!std::strcmp(id, CLAP_EXT_LATENCY))
        return &kHostLatency;
    if (!std::strcmp(id, CLAP_EXT_TAIL))
        return &kHostTail;
    if (!std::strcmp(id, CLAP_EXT_LOG))
        return &kHostLog;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &kHostAudioPorts;
    return nullptr;
}
void hostRequestNothing(const clap_host_t*) { }

const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "Kinoklip", "Kinoklip", "https://github.com/SeniorMatt/kinoklip",
    "1", hostGetExtension, hostRequestNothing, hostRequestNothing, hostRequestNothing};

// ---- loaded libraries (kept for the life of the process: a plugin may keep threads or atexit hooks) ----

struct Library
{
    void* handle = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
    QString error;
};

QMutex gLibraryMutex;
QHash<QString, Library> gLibraries;

Library& openLibrary(const QString& path)
{
    QMutexLocker lock(&gLibraryMutex);
    auto it = gLibraries.find(path);
    if (it != gLibraries.end())
        return *it;
    Library lib;
    const QByteArray file = path.toLocal8Bit();
    lib.handle = dlopen(file.constData(), RTLD_NOW | RTLD_LOCAL);
    if (!lib.handle) {
        const char* e = dlerror();
        lib.error = QStringLiteral("cannot load the file: %1").arg(QString::fromLocal8Bit(e ? e : "unknown error"));
    } else {
        lib.entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib.handle, "clap_entry"));
        if (!lib.entry) {
            lib.error = QStringLiteral("the file has no clap_entry (not a CLAP plugin)");
        } else if (!clap_version_is_compatible(lib.entry->clap_version)) {
            lib.error = QStringLiteral("the plugin was built for an incompatible CLAP version");
            lib.entry = nullptr;
        } else if (!lib.entry->init(file.constData())) {
            lib.error = QStringLiteral("the plugin refused to start (clap_entry.init failed)");
            lib.entry = nullptr;
        } else {
            lib.factory = static_cast<const clap_plugin_factory_t*>(
                lib.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
            if (!lib.factory)
                lib.error = QStringLiteral("the file has no plugin factory");
        }
    }
    return *gLibraries.insert(path, lib);
}

QString joinFeatures(const char* const* features)
{
    QStringList l;
    for (; features && *features; ++features)
        l.append(QString::fromUtf8(*features));
    return l.join(QLatin1String(", "));
}

// ---- parameter events ----

struct EventList
{
    std::vector<clap_event_param_value_t> events;
    clap_input_events_t in {};
    clap_output_events_t out {};
    EventList()
    {
        in.ctx = this;
        in.size = [](const clap_input_events_t* l) -> uint32_t {
            return uint32_t(static_cast<const EventList*>(l->ctx)->events.size());
        };
        in.get = [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
            const auto* self = static_cast<const EventList*>(l->ctx);
            return i < self->events.size() ? &self->events[i].header : nullptr;
        };
        out.ctx = this;
        out.try_push = [](const clap_output_events_t*, const clap_event_header_t*) -> bool { return true; };
    }
    EventList(const EventList&) = delete;
};

// ---- one plugin instance ----

class ClapCore final : public Core
{
public:
    ClapCore(const clap_plugin_t* plugin, const PluginInfo& info, int inCh, int outCh,
        std::vector<uint32_t> inPortChannels, std::vector<uint32_t> outPortChannels, int mainIn, int mainOut)
        : m_plugin(plugin)
        , m_params(info.params)
        , m_inCh(inCh)
        , m_outCh(outCh)
        , m_mainIn(mainIn)
        , m_mainOut(mainOut)
        , m_sent(info.params.size(), std::nan(""))
        , m_silence(Instance::kMaxBlock, 0.0f)
        , m_scratch(Instance::kMaxBlock, 0.0f)
    {
        m_events.events.reserve(m_params.size() + 1);
        // one buffer description per port; the channel pointer arrays are filled once, the main
        // ports are repointed for every block
        for (size_t p = 0; p < inPortChannels.size(); ++p) {
            m_inPtrs.emplace_back(std::max<uint32_t>(inPortChannels[p], 1), m_silence.data());
            clap_audio_buffer_t b {};
            b.channel_count = std::max<uint32_t>(inPortChannels[p], 1);
            b.constant_mask = 0;
            m_inBuffers.push_back(b);
        }
        for (size_t p = 0; p < outPortChannels.size(); ++p) {
            m_outPtrs.emplace_back(std::max<uint32_t>(outPortChannels[p], 1), m_scratch.data());
            clap_audio_buffer_t b {};
            b.channel_count = std::max<uint32_t>(outPortChannels[p], 1);
            m_outBuffers.push_back(b);
        }
        for (size_t p = 0; p < m_inBuffers.size(); ++p)
            m_inBuffers[p].data32 = m_inPtrs[p].data();
        for (size_t p = 0; p < m_outBuffers.size(); ++p)
            m_outBuffers[p].data32 = m_outPtrs[p].data();
        if (const auto* lat = static_cast<const clap_plugin_latency_t*>(
                m_plugin->get_extension(m_plugin, CLAP_EXT_LATENCY)))
            m_latencyExt = lat;
    }

    ~ClapCore() override
    {
        if (m_processing)
            m_plugin->stop_processing(m_plugin);
        if (m_active)
            m_plugin->deactivate(m_plugin);
        m_plugin->destroy(m_plugin);
    }

    bool activate(double rate)
    {
        m_rate = rate;
        if (!m_plugin->activate(m_plugin, rate, 1, uint32_t(Instance::kMaxBlock)))
            return false;
        m_active = true;
        if (!m_plugin->start_processing(m_plugin)) {
            m_plugin->deactivate(m_plugin);
            m_active = false;
            return false;
        }
        m_processing = true;
        if (m_latencyExt) // valid once the plugin is active
            m_latency = int(std::min<uint32_t>(m_latencyExt->get(m_plugin), 1u << 20));
        return true;
    }

    void run(const float* const* in, float* const* out, int frames, const double* values) override
    {
        // parameter changes of this block
        m_events.events.clear();
        for (size_t i = 0; i < m_params.size(); ++i) {
            const double v = std::clamp(values[i], m_params[i].min, m_params[i].max);
            if (m_params[i].readOnly || (!std::isnan(m_sent[i]) && m_sent[i] == v))
                continue;
            m_sent[i] = v;
            clap_event_param_value_t e {};
            e.header.size = sizeof(e);
            e.header.time = 0;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = CLAP_EVENT_PARAM_VALUE;
            e.header.flags = 0;
            e.param_id = m_params[i].id;
            e.cookie = nullptr;
            e.note_id = -1;
            e.port_index = -1;
            e.channel = -1;
            e.key = -1;
            e.value = v;
            m_events.events.push_back(e);
        }
        // main ports: the host's channels (the rest of a wider port stays silent / scratch)
        const int usedIn = std::min(m_inCh, 2);
        const int usedOut = std::min(m_outCh, 2);
        if (m_mainIn >= 0) {
            for (int c = 0; c < usedIn; ++c)
                m_inPtrs[size_t(m_mainIn)][size_t(c)] = const_cast<float*>(in[std::min(c, 1)]);
        }
        if (m_mainOut >= 0) {
            for (int c = 0; c < usedOut; ++c)
                m_outPtrs[size_t(m_mainOut)][size_t(c)] = out[std::min(c, 1)];
        }
        clap_process_t pr {};
        pr.steady_time = m_steady;
        pr.frames_count = uint32_t(frames);
        pr.transport = nullptr;
        pr.audio_inputs = m_inBuffers.empty() ? nullptr : m_inBuffers.data();
        pr.audio_outputs = m_outBuffers.empty() ? nullptr : m_outBuffers.data();
        pr.audio_inputs_count = uint32_t(m_inBuffers.size());
        pr.audio_outputs_count = uint32_t(m_outBuffers.size());
        pr.in_events = &m_events.in;
        pr.out_events = &m_events.out;
        // a plugin that fails the block leaves the input untouched
        tlInProcess = true;
        const clap_process_status status = m_plugin->process(m_plugin, &pr);
        tlInProcess = false;
        m_steady += frames;
        if (status == CLAP_PROCESS_ERROR) {
            for (int c = 0; c < usedOut; ++c)
                std::copy(in[std::min(c, 1)], in[std::min(c, 1)] + frames, out[std::min(c, 1)]);
        }
        if (m_latencyExt)
            m_latency = int(std::min<uint32_t>(m_latencyExt->get(m_plugin), 1u << 20));
    }

    void reset() override
    {
        if (m_plugin->reset)
            m_plugin->reset(m_plugin);
        std::fill(m_sent.begin(), m_sent.end(), std::nan(""));
    }

    [[nodiscard]] int latencyFrames() const override { return m_latency; }

private:
    const clap_plugin_t* m_plugin;
    QVector<ParamInfo> m_params;
    int m_inCh, m_outCh;
    int m_mainIn, m_mainOut;
    std::vector<double> m_sent;
    std::vector<float> m_silence, m_scratch;
    std::vector<std::vector<float*>> m_inPtrs, m_outPtrs;
    std::vector<clap_audio_buffer_t> m_inBuffers, m_outBuffers;
    EventList m_events;
    const clap_plugin_latency_t* m_latencyExt = nullptr;
    int m_latency = 0;
    int64_t m_steady = 0;
    double m_rate = 48000.0;
    bool m_active = false;
    bool m_processing = false;
};

struct Ports
{
    std::vector<uint32_t> in, out; ///< channels of each port
    int mainIn = -1, mainOut = -1; ///< index of the main port
};

Ports readPorts(const clap_plugin_t* plugin)
{
    Ports ports;
    const auto* ext = static_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    if (!ext) {
        ports.in = {2};
        ports.out = {2};
        ports.mainIn = ports.mainOut = 0;
        return ports;
    }
    for (int dir = 0; dir < 2; ++dir) {
        const bool isInput = dir == 0;
        const uint32_t n = ext->count(plugin, isInput);
        auto& list = isInput ? ports.in : ports.out;
        int& main = isInput ? ports.mainIn : ports.mainOut;
        for (uint32_t i = 0; i < std::min<uint32_t>(n, 16); ++i) {
            clap_audio_port_info_t info {};
            if (!ext->get(plugin, i, isInput, &info)) {
                list.push_back(0);
                continue;
            }
            list.push_back(info.channel_count);
            if ((info.flags & CLAP_AUDIO_PORT_IS_MAIN) && main < 0)
                main = int(i);
        }
        if (main < 0 && !list.empty())
            main = 0;
    }
    return ports;
}

const clap_plugin_t* createPlugin(Library& lib, const QString& id, QString* error)
{
    const QByteArray idUtf8 = id.toUtf8();
    const clap_plugin_t* plugin = lib.factory->create_plugin(lib.factory, &kHost, idUtf8.constData());
    if (!plugin) {
        *error = QStringLiteral("the plugin could not be created");
        return nullptr;
    }
    if (!plugin->init(plugin)) {
        plugin->destroy(plugin);
        *error = QStringLiteral("the plugin failed to initialise");
        return nullptr;
    }
    return plugin;
}

} // namespace

BundleInfo describe(const QString& clapFile)
{
    BundleInfo bundle;
    bundle.path = clapFile;
    bundle.format = QString::fromLatin1(kFormatClap);
    Library& lib = openLibrary(clapFile);
    if (!lib.factory) {
        bundle.error = lib.error.isEmpty() ? QStringLiteral("the file has no plugin factory") : lib.error;
        return bundle;
    }
    const uint32_t count = lib.factory->get_plugin_count(lib.factory);
    for (uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* d = lib.factory->get_plugin_descriptor(lib.factory, i);
        if (!d || !d->id)
            continue;
        PluginInfo info;
        info.format = QString::fromLatin1(kFormatClap);
        info.id = QString::fromUtf8(d->id);
        info.name = QString::fromUtf8(d->name ? d->name : d->id);
        info.vendor = QString::fromUtf8(d->vendor ? d->vendor : "");
        info.version = QString::fromUtf8(d->version ? d->version : "");
        info.description = QString::fromUtf8(d->description ? d->description : "");
        info.category = joinFeatures(d->features);
        QString error;
        const clap_plugin_t* plugin = createPlugin(lib, info.id, &error);
        if (!plugin) {
            info.error = error;
            bundle.plugins.append(info);
            continue;
        }
        const Ports ports = readPorts(plugin);
        info.inChannels = ports.mainIn >= 0 ? int(ports.in[size_t(ports.mainIn)]) : 0;
        info.outChannels = ports.mainOut >= 0 ? int(ports.out[size_t(ports.mainOut)]) : 0;
        if (const auto* params = static_cast<const clap_plugin_params_t*>(
                plugin->get_extension(plugin, CLAP_EXT_PARAMS))) {
            const uint32_t n = params->count(plugin);
            for (uint32_t k = 0; k < std::min<uint32_t>(n, 512); ++k) {
                clap_param_info_t pi {};
                if (!params->get_info(plugin, k, &pi))
                    continue;
                ParamInfo p;
                p.id = pi.id;
                p.key = QStringLiteral("p%1").arg(pi.id);
                p.label = QString::fromUtf8(pi.name);
                p.group = QString::fromUtf8(pi.module).replace(QLatin1Char('/'), QLatin1String(" > "));
                p.min = pi.min_value;
                p.max = pi.max_value;
                p.def = pi.default_value;
                p.integer = (pi.flags & CLAP_PARAM_IS_STEPPED) != 0;
                p.toggle = p.integer && p.min == 0.0 && p.max == 1.0;
                p.hidden = (pi.flags & CLAP_PARAM_IS_HIDDEN) != 0;
                p.readOnly = (pi.flags & CLAP_PARAM_IS_READONLY) != 0;
                if ((pi.flags & CLAP_PARAM_IS_ENUM) && p.integer && p.max - p.min < 64) {
                    p.enumeration = true;
                    for (double v = p.min; v <= p.max; v += 1.0) {
                        char text[256] = {};
                        if (!params->value_to_text(plugin, pi.id, v, text, sizeof(text)) || !text[0])
                            std::snprintf(text, sizeof(text), "%g", v);
                        p.enumLabels.append(QString::fromUtf8(text));
                        p.enumValues.append(v);
                    }
                }
                if (!(p.max > p.min)) // a parameter without a range cannot be edited
                    p.hidden = true;
                if (!std::isfinite(p.min) || !std::isfinite(p.max) || !std::isfinite(p.def)) {
                    p.min = 0.0;
                    p.max = 1.0;
                    p.def = 0.0;
                }
                // the display text shows the unit ("-6.0 dB"): take the trailing letters
                char text[256] = {};
                if (!p.enumeration && !p.toggle
                    && params->value_to_text(plugin, pi.id, p.def, text, sizeof(text))) {
                    const QString t = QString::fromUtf8(text).trimmed();
                    const qsizetype sp = t.lastIndexOf(QLatin1Char(' '));
                    if (sp > 0 && t.size() - sp <= 6) {
                        bool number = false;
                        t.left(sp).toDouble(&number);
                        if (number)
                            p.unit = t.mid(sp + 1);
                    }
                }
                info.params.append(p);
            }
        }
        if (const auto* tail = static_cast<const clap_plugin_tail_t*>(plugin->get_extension(plugin, CLAP_EXT_TAIL))) {
            // asked at default settings; a plugin whose tail depends on parameters is estimated
            const uint32_t t = tail->get(plugin);
            info.tailSeconds = t == UINT32_MAX ? 10.0 : std::min(30.0, double(t) / 48000.0);
        }
        // Run one silent block: a plugin that dies on its first block dies here, in the probe.
        if (ports.mainIn >= 0 && ports.mainOut >= 0 && info.inChannels > 0 && info.outChannels > 0) {
            if (plugin->activate(plugin, 48000.0, 1, uint32_t(Instance::kMaxBlock))) {
                if (plugin->start_processing(plugin)) {
                    std::vector<float> in(Instance::kMaxBlock, 0.0f), out(Instance::kMaxBlock, 0.0f);
                    std::vector<std::vector<float*>> ip, op;
                    std::vector<clap_audio_buffer_t> ib, ob;
                    for (uint32_t c : ports.in) {
                        ip.emplace_back(std::max<uint32_t>(c, 1), in.data());
                        clap_audio_buffer_t b {};
                        b.channel_count = std::max<uint32_t>(c, 1);
                        ib.push_back(b);
                    }
                    for (uint32_t c : ports.out) {
                        op.emplace_back(std::max<uint32_t>(c, 1), out.data());
                        clap_audio_buffer_t b {};
                        b.channel_count = std::max<uint32_t>(c, 1);
                        ob.push_back(b);
                    }
                    for (size_t k = 0; k < ib.size(); ++k)
                        ib[k].data32 = ip[k].data();
                    for (size_t k = 0; k < ob.size(); ++k)
                        ob[k].data32 = op[k].data();
                    EventList events;
                    clap_process_t pr {};
                    pr.frames_count = 256;
                    pr.audio_inputs = ib.data();
                    pr.audio_outputs = ob.data();
                    pr.audio_inputs_count = uint32_t(ib.size());
                    pr.audio_outputs_count = uint32_t(ob.size());
                    pr.in_events = &events.in;
                    pr.out_events = &events.out;
                    tlInProcess = true;
                    plugin->process(plugin, &pr);
                    tlInProcess = false;
                    plugin->stop_processing(plugin);
                }
                plugin->deactivate(plugin);
            } else {
                info.error = QStringLiteral("the plugin could not be activated at 48 kHz");
            }
        } else if (info.error.isEmpty()) {
            info.error = QStringLiteral("it has no audio input and output (an instrument or analyser, not an effect)");
        }
        plugin->destroy(plugin);
        uniqueKeys(&info);
        bundle.plugins.append(info);
    }
    if (bundle.plugins.isEmpty())
        bundle.error = QStringLiteral("the file contains no plugin");
    return bundle;
}

std::unique_ptr<Instance> create(
    const PluginInfo& info, const QString& clapFile, double sampleRate, QString* error)
{
    Library& lib = openLibrary(clapFile);
    if (!lib.factory) {
        *error = lib.error;
        return nullptr;
    }
    // a mono plugin runs twice (left, right)
    const int copies = info.inChannels == 1 && info.outChannels == 1 ? 2 : 1;
    std::vector<std::unique_ptr<Core>> cores;
    std::vector<double> defaults;
    for (const ParamInfo& p : info.params)
        defaults.push_back(p.def);
    for (int c = 0; c < copies; ++c) {
        const clap_plugin_t* plugin = createPlugin(lib, info.id, error);
        if (!plugin)
            return nullptr;
        const Ports ports = readPorts(plugin);
        auto core = std::make_unique<ClapCore>(plugin, info, info.inChannels, info.outChannels, ports.in,
            ports.out, ports.mainIn, ports.mainOut);
        if (!core->activate(sampleRate)) {
            *error = QStringLiteral("the plugin could not be activated");
            return nullptr;
        }
        cores.push_back(std::move(core));
    }
    return std::make_unique<ChannelAdapter>(
        std::move(cores), info.inChannels, info.outChannels, int(info.params.size()), defaults);
}

} // namespace kk::audioplug::clap
