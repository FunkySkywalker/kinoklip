// SPDX-License-Identifier: GPL-3.0-or-later
// A tiny LV2 test plugin: mono gain in dB with an Invert switch (ports: 0 audio in, 1 audio out,
// 2 gain, 3 invert, 4 latency out). Built three ways (tests/audioplug/test_audioplug.cpp):
//   plain         urn:kinoklip:test:gain      a mono gain
//   KK_LV2_CRASH  urn:kinoklip:test:crash     aborts when it is instantiated
//   KK_LV2_DELAY  urn:kinoklip:test:delay     a 32 frame delay that reports its latency
#include <lv2/core/lv2.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#if defined(KK_LV2_CRASH)
#define KK_URI "urn:kinoklip:test:crash"
#elif defined(KK_LV2_DELAY)
#define KK_URI "urn:kinoklip:test:delay"
#else
#define KK_URI "urn:kinoklip:test:gain"
#endif

namespace {

constexpr int kDelayFrames = 32;

struct Gain
{
    const float* in = nullptr;
    float* out = nullptr;
    const float* gainDb = nullptr;
    const float* invert = nullptr;
    float* latency = nullptr;
    std::vector<float> line = std::vector<float>(kDelayFrames, 0.0f);
    int pos = 0;
};

LV2_Handle instantiate(const LV2_Descriptor*, double, const char*, const LV2_Feature* const*)
{
#ifdef KK_LV2_CRASH
    std::abort();
#endif
    return new (std::nothrow) Gain;
}

void connectPort(LV2_Handle h, uint32_t port, void* data)
{
    auto* g = static_cast<Gain*>(h);
    switch (port) {
    case 0: g->in = static_cast<const float*>(data); break;
    case 1: g->out = static_cast<float*>(data); break;
    case 2: g->gainDb = static_cast<const float*>(data); break;
    case 3: g->invert = static_cast<const float*>(data); break;
    case 4: g->latency = static_cast<float*>(data); break;
    default: break;
    }
}

void activate(LV2_Handle h)
{
    auto* g = static_cast<Gain*>(h);
    std::fill(g->line.begin(), g->line.end(), 0.0f);
    g->pos = 0;
}

void run(LV2_Handle h, uint32_t n)
{
    auto* g = static_cast<Gain*>(h);
    const float factor = std::pow(10.0f, *g->gainDb / 20.0f) * (*g->invert >= 0.5f ? -1.0f : 1.0f);
#ifdef KK_LV2_DELAY
    if (g->latency)
        *g->latency = float(kDelayFrames);
    for (uint32_t i = 0; i < n; ++i) {
        const float delayed = g->line[size_t(g->pos)];
        g->line[size_t(g->pos)] = g->in[i];
        g->pos = (g->pos + 1) % kDelayFrames;
        g->out[i] = delayed * factor;
    }
#else
    if (g->latency)
        *g->latency = 0.0f;
    for (uint32_t i = 0; i < n; ++i)
        g->out[i] = g->in[i] * factor;
#endif
}

void deactivate(LV2_Handle) { }
void cleanup(LV2_Handle h) { delete static_cast<Gain*>(h); }
const void* extensionData(const char*) { return nullptr; }

const LV2_Descriptor kDescriptor = {KK_URI, instantiate, connectPort, activate, run, deactivate, cleanup, extensionData};

} // namespace

extern "C" __attribute__((visibility("default"))) const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &kDescriptor : nullptr;
}
