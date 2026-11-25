// SPDX-License-Identifier: GPL-3.0-or-later
// A tiny CLAP test plugin: stereo gain in dB with an Invert switch (parameters 1 and 2). Built three
// ways (tests/audioplug/test_audioplug.cpp):
//   plain          org.kinoklip.test.gain   a stereo gain
//   KK_CLAP_CRASH  org.kinoklip.test.crash  aborts in its init
//   KK_CLAP_DELAY  org.kinoklip.test.delay  a 32 frame delay that reports its latency
#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(KK_CLAP_CRASH)
#define KK_ID "org.kinoklip.test.crash"
#define KK_NAME "Kinoklip Test Crash"
#elif defined(KK_CLAP_DELAY)
#define KK_ID "org.kinoklip.test.delay"
#define KK_NAME "Kinoklip Test Delay"
#else
#define KK_ID "org.kinoklip.test.gain"
#define KK_NAME "Kinoklip Test Gain"
#endif

namespace {

constexpr int kDelayFrames = 32;

struct Gain
{
    clap_plugin_t plugin;
    double gainDb = 0.0;
    double invert = 0.0;
    std::vector<float> line[2];
    int pos = 0;
};

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, nullptr};
const clap_plugin_descriptor_t kDescriptor = {CLAP_VERSION_INIT, KK_ID, KK_NAME, "Kinoklip", "", "", "", "1.0",
    "A gain for the Kinoklip tests", kFeatures};

Gain* self(const clap_plugin_t* p) { return static_cast<Gain*>(p->plugin_data); }

// ---- params ----
uint32_t paramsCount(const clap_plugin_t*) { return 2; }
bool paramsGetInfo(const clap_plugin_t*, uint32_t i, clap_param_info_t* info)
{
    std::memset(info, 0, sizeof(*info));
    if (i == 0) {
        info->id = 1;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        std::strcpy(info->name, "Gain");
        info->min_value = -60.0;
        info->max_value = 24.0;
        info->default_value = 0.0;
        return true;
    }
    if (i == 1) {
        info->id = 2;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
        std::strcpy(info->name, "Invert");
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = 0.0;
        return true;
    }
    return false;
}
bool paramsGetValue(const clap_plugin_t* p, clap_id id, double* v)
{
    *v = id == 1 ? self(p)->gainDb : self(p)->invert;
    return id == 1 || id == 2;
}
bool paramsValueToText(const clap_plugin_t*, clap_id id, double v, char* out, uint32_t size)
{
    std::snprintf(out, size, id == 1 ? "%.1f dB" : "%g", v);
    return true;
}
bool paramsTextToValue(const clap_plugin_t*, clap_id, const char* text, double* v)
{
    *v = std::atof(text);
    return true;
}
void applyEvents(Gain* g, const clap_input_events_t* events)
{
    const uint32_t n = events ? events->size(events) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t* h = events->get(events, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
            (e->param_id == 1 ? g->gainDb : g->invert) = e->value;
        }
    }
}
void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*)
{
    applyEvents(self(p), in);
}
const clap_plugin_params_t kParams = {paramsCount, paramsGetInfo, paramsGetValue, paramsValueToText, paramsTextToValue, paramsFlush};

// ---- audio ports: one stereo in, one stereo out ----
uint32_t portsCount(const clap_plugin_t*, bool) { return 1; }
bool portsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info)
{
    if (index != 0)
        return false;
    info->id = isInput ? 0 : 1;
    std::strcpy(info->name, isInput ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kPorts = {portsCount, portsGet};

uint32_t latencyGet(const clap_plugin_t*)
{
#ifdef KK_CLAP_DELAY
    return kDelayFrames;
#else
    return 0;
#endif
}
const clap_plugin_latency_t kLatency = {latencyGet};

// ---- plugin ----
bool init(const clap_plugin_t*)
{
#ifdef KK_CLAP_CRASH
    std::abort();
#endif
    return true;
}
void destroy(const clap_plugin_t* p) { delete self(p); }
bool activate(const clap_plugin_t* p, double, uint32_t, uint32_t)
{
    Gain* g = self(p);
    g->line[0].assign(kDelayFrames, 0.0f);
    g->line[1].assign(kDelayFrames, 0.0f);
    g->pos = 0;
    return true;
}
void deactivate(const clap_plugin_t*) { }
bool startProcessing(const clap_plugin_t*) { return true; }
void stopProcessing(const clap_plugin_t*) { }
void reset(const clap_plugin_t* p)
{
    activate(p, 0, 0, 0);
}
clap_process_status process(const clap_plugin_t* p, const clap_process_t* pr)
{
    Gain* g = self(p);
    applyEvents(g, pr->in_events);
    const float factor = float(std::pow(10.0, g->gainDb / 20.0) * (g->invert >= 0.5 ? -1.0 : 1.0));
    for (uint32_t c = 0; c < 2; ++c) {
        const float* in = pr->audio_inputs[0].data32[c];
        float* out = pr->audio_outputs[0].data32[c];
        int pos = g->pos;
        for (uint32_t i = 0; i < pr->frames_count; ++i) {
#ifdef KK_CLAP_DELAY
            const float delayed = g->line[c][size_t(pos)];
            g->line[c][size_t(pos)] = in[i];
            pos = (pos + 1) % kDelayFrames;
            out[i] = delayed * factor;
#else
            out[i] = in[i] * factor;
#endif
        }
        if (c == 1)
            g->pos = pos;
    }
    return CLAP_PROCESS_CONTINUE;
}
const void* getExtension(const clap_plugin_t*, const char* id)
{
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &kParams;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &kPorts;
    if (!std::strcmp(id, CLAP_EXT_LATENCY))
        return &kLatency;
    return nullptr;
}
void onMainThread(const clap_plugin_t*) { }

// ---- factory ----
uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &kDescriptor : nullptr; }
const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t*, const char* id)
{
    if (std::strcmp(id, KK_ID))
        return nullptr;
    auto* g = new Gain;
    g->plugin = {&kDescriptor, g, init, destroy, activate, deactivate, startProcessing, stopProcessing, reset,
        process, getExtension, onMainThread};
    return &g->plugin;
}
const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char*) { return true; }
void entryDeinit() { }
const void* entryGetFactory(const char* id)
{
    return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr;
}

} // namespace

extern "C" __attribute__((visibility("default"))) const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, entryInit, entryDeinit, entryGetFactory};
