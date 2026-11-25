// SPDX-License-Identifier: GPL-3.0-or-later
// A tiny OpenFX plugin for the host tests (tests/ofx/test_ofx.cpp): a gain / invert filter with
// one parameter of several kinds. Written against the OFX C API only, like any third-party plugin.
// Built three times: the plain plugin, one that aborts while loading (KK_OFX_CRASH) and one that
// never returns from loading (KK_OFX_HANG).
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>

namespace {

OfxHost* g_host = nullptr;
const OfxPropertySuiteV1* g_prop = nullptr;
const OfxImageEffectSuiteV1* g_effect = nullptr;
const OfxParameterSuiteV1* g_param = nullptr;
const OfxMultiThreadSuiteV1* g_thread = nullptr;

std::mutex g_statsMutex;
std::set<std::thread::id> g_threads;
std::atomic<int> g_renders {0};
std::atomic<int> g_identity {0};
std::atomic<int> g_changed {0};
double g_lastScale = 0;
int g_lastWidth = 0;
double g_lastTime = -1;

struct Instance
{
    OfxParamHandle gain = nullptr, invert = nullptr, mode = nullptr, offset = nullptr,
                   tint = nullptr;
    OfxImageClipHandle source = nullptr, output = nullptr;
};

void defineParams(OfxImageEffectHandle effect)
{
    OfxParamSetHandle set;
    g_effect->getParamSet(effect, &set);
    OfxPropertySetHandle p;
    g_param->paramDefine(set, kOfxParamTypeDouble, "gain", &p);
    g_prop->propSetString(p, kOfxPropLabel, 0, "Gain");
    g_prop->propSetString(p, kOfxParamPropHint, 0, "Multiplies the colour");
    g_prop->propSetDouble(p, kOfxParamPropDefault, 0, 1.0);
    g_prop->propSetDouble(p, kOfxParamPropMin, 0, 0.0);
    g_prop->propSetDouble(p, kOfxParamPropMax, 0, 10.0);
    g_prop->propSetDouble(p, kOfxParamPropDisplayMin, 0, 0.0);
    g_prop->propSetDouble(p, kOfxParamPropDisplayMax, 0, 4.0);
    g_prop->propSetInt(p, kOfxParamPropAnimates, 0, 1);

    g_param->paramDefine(set, kOfxParamTypeBoolean, "invert", &p);
    g_prop->propSetString(p, kOfxPropLabel, 0, "Invert");
    g_prop->propSetInt(p, kOfxParamPropDefault, 0, 0);

    g_param->paramDefine(set, kOfxParamTypeChoice, "mode", &p);
    g_prop->propSetString(p, kOfxPropLabel, 0, "Curve");
    g_prop->propSetString(p, kOfxParamPropChoiceOption, 0, "Linear");
    g_prop->propSetString(p, kOfxParamPropChoiceOption, 1, "Squared");
    g_prop->propSetInt(p, kOfxParamPropDefault, 0, 0);

    g_param->paramDefine(set, kOfxParamTypeDouble2D, "offset", &p);
    g_prop->propSetString(p, kOfxPropLabel, 0, "Offset");
    g_prop->propSetDouble(p, kOfxParamPropDefault, 0, 0.0);
    g_prop->propSetDouble(p, kOfxParamPropDefault, 1, 0.0);
    g_prop->propSetDouble(p, kOfxParamPropMin, 0, -1.0);
    g_prop->propSetDouble(p, kOfxParamPropMin, 1, -1.0);
    g_prop->propSetDouble(p, kOfxParamPropMax, 0, 1.0);
    g_prop->propSetDouble(p, kOfxParamPropMax, 1, 1.0);

    g_param->paramDefine(set, kOfxParamTypeRGB, "tint", &p);
    g_prop->propSetString(p, kOfxPropLabel, 0, "Tint");
    for (int k = 0; k < 3; ++k)
        g_prop->propSetDouble(p, kOfxParamPropDefault, k, 1.0);

    g_param->paramDefine(set, kOfxParamTypeInteger, "steps", &p);
    g_prop->propSetString(p, kOfxPropLabel, 0, "Steps");
    g_prop->propSetInt(p, kOfxParamPropDefault, 0, 1);
    g_prop->propSetInt(p, kOfxParamPropMin, 0, 1);
    g_prop->propSetInt(p, kOfxParamPropMax, 0, 8);

    g_param->paramDefine(set, kOfxParamTypeString, "note", &p);
    g_prop->propSetString(p, kOfxPropLabel, 0, "Note");
    g_prop->propSetString(p, kOfxParamPropDefault, 0, "hello");
}

struct Values
{
    double gain, tint[3];
    int invert, mode;
};

Values valuesAt(Instance* inst, double time)
{
    Values v {};
    g_param->paramGetValueAtTime(inst->gain, time, &v.gain);
    g_param->paramGetValueAtTime(inst->invert, time, &v.invert);
    g_param->paramGetValueAtTime(inst->mode, time, &v.mode);
    g_param->paramGetValueAtTime(inst->tint, time, &v.tint[0], &v.tint[1], &v.tint[2]);
    return v;
}

struct Job
{
    const float* src;
    float* dst;
    int width, height, srcRow, dstRow; ///< row sizes in floats
    Values v;
};

void renderRows(unsigned threadIndex, unsigned threadMax, void* arg)
{
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        g_threads.insert(std::this_thread::get_id());
    }
    auto* job = static_cast<Job*>(arg);
    const int y0 = int(std::int64_t(job->height) * threadIndex / threadMax);
    const int y1 = int(std::int64_t(job->height) * (threadIndex + 1) / threadMax);
    for (int y = y0; y < y1; ++y) {
        const float* s = job->src + std::int64_t(y) * job->srcRow;
        float* d = job->dst + std::int64_t(y) * job->dstRow;
        for (int x = 0; x < job->width; ++x, s += 4, d += 4) {
            const float a = s[3];
            for (int c = 0; c < 3; ++c) {
                double value = s[c] * job->v.gain * job->v.tint[c];
                if (job->v.mode == 1)
                    value = value * value;
                if (job->v.invert)
                    value = a - value;
                d[c] = float(value);
            }
            d[3] = a;
        }
    }
}

OfxStatus describe(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle p;
    g_effect->getPropertySet(effect, &p);
    g_prop->propSetString(p, kOfxPropLabel, 0, "Kinoklip Test Gain");
    g_prop->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Kinoklip Test");
    g_prop->propSetString(p, kOfxPropPluginDescription, 0, "Gain and invert, for the host tests");
    g_prop->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    g_prop->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    g_prop->propSetString(
        p, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    g_prop->propSetInt(p, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
    g_prop->propSetInt(p, kOfxImageEffectPropSupportsTiles, 0, 0);
    return kOfxStatOK;
}

OfxStatus describeInContext(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle p;
    g_effect->clipDefine(effect, kOfxImageEffectOutputClipName, &p);
    g_prop->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    g_effect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &p);
    g_prop->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    defineParams(effect);
    return kOfxStatOK;
}

OfxStatus createInstance(OfxImageEffectHandle effect)
{
    auto* inst = new Instance;
    OfxParamSetHandle set;
    g_effect->getParamSet(effect, &set);
    g_param->paramGetHandle(set, "gain", &inst->gain, nullptr);
    g_param->paramGetHandle(set, "invert", &inst->invert, nullptr);
    g_param->paramGetHandle(set, "mode", &inst->mode, nullptr);
    g_param->paramGetHandle(set, "offset", &inst->offset, nullptr);
    g_param->paramGetHandle(set, "tint", &inst->tint, nullptr);
    g_effect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &inst->source, nullptr);
    g_effect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &inst->output, nullptr);
    OfxPropertySetHandle p;
    g_effect->getPropertySet(effect, &p);
    g_prop->propSetPointer(p, kOfxPropInstanceData, 0, inst);
    return kOfxStatOK;
}

Instance* instanceOf(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle p;
    g_effect->getPropertySet(effect, &p);
    void* data = nullptr;
    g_prop->propGetPointer(p, kOfxPropInstanceData, 0, &data);
    return static_cast<Instance*>(data);
}

OfxStatus isIdentity(OfxImageEffectHandle effect, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    double time = 0;
    g_prop->propGetDouble(in, kOfxPropTime, 0, &time);
    const Values v = valuesAt(instanceOf(effect), time);
    ++g_identity;
    if (v.gain == 1.0 && !v.invert && v.mode == 0 && v.tint[0] == 1.0 && v.tint[1] == 1.0
        && v.tint[2] == 1.0) {
        g_prop->propSetString(out, kOfxPropName, 0, kOfxImageEffectSimpleSourceClipName);
        g_prop->propSetDouble(out, kOfxPropTime, 0, time);
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in)
{
    Instance* inst = instanceOf(effect);
    double time = 0;
    double scale[2] = {1, 1};
    int window[4] = {0, 0, 0, 0};
    g_prop->propGetDouble(in, kOfxPropTime, 0, &time);
    g_prop->propGetDoubleN(in, kOfxImageEffectPropRenderScale, 2, scale);
    g_prop->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, window);
    OfxPropertySetHandle src = nullptr, dst = nullptr;
    if (g_effect->clipGetImage(inst->source, time, nullptr, &src) != kOfxStatOK)
        return kOfxStatFailed;
    if (g_effect->clipGetImage(inst->output, time, nullptr, &dst) != kOfxStatOK) {
        g_effect->clipReleaseImage(src);
        return kOfxStatFailed;
    }
    void *sp = nullptr, *dp = nullptr;
    int sRow = 0, dRow = 0;
    int sb[4], db[4];
    g_prop->propGetPointer(src, kOfxImagePropData, 0, &sp);
    g_prop->propGetPointer(dst, kOfxImagePropData, 0, &dp);
    g_prop->propGetInt(src, kOfxImagePropRowBytes, 0, &sRow);
    g_prop->propGetInt(dst, kOfxImagePropRowBytes, 0, &dRow);
    g_prop->propGetIntN(src, kOfxImagePropBounds, 4, sb);
    g_prop->propGetIntN(dst, kOfxImagePropBounds, 4, db);
    Job job;
    job.src = static_cast<const float*>(sp) + std::int64_t(window[1] - sb[1]) * (sRow / 4)
        + std::int64_t(window[0] - sb[0]) * 4;
    job.dst = static_cast<float*>(dp) + std::int64_t(window[1] - db[1]) * (dRow / 4)
        + std::int64_t(window[0] - db[0]) * 4;
    job.width = window[2] - window[0];
    job.height = window[3] - window[1];
    job.srcRow = sRow / 4;
    job.dstRow = dRow / 4;
    job.v = valuesAt(inst, time);
    unsigned cpus = 1;
    g_thread->multiThreadNumCPUs(&cpus);
    g_thread->multiThread(renderRows, std::min<unsigned>(cpus, 4), &job);
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        g_lastScale = scale[0];
        g_lastWidth = job.width;
        g_lastTime = time;
    }
    ++g_renders;
    g_effect->clipReleaseImage(src);
    g_effect->clipReleaseImage(dst);
    return kOfxStatOK;
}

OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    auto effect = reinterpret_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (!std::strcmp(action, kOfxActionLoad)) {
#ifdef KK_OFX_CRASH
        std::abort();
#endif
#ifdef KK_OFX_HANG
        for (;;)
            ::sleep(1);
#endif
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxActionDescribe))
        return describe(effect);
    if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext))
        return describeInContext(effect);
    if (!std::strcmp(action, kOfxActionCreateInstance))
        return createInstance(effect);
    if (!std::strcmp(action, kOfxActionDestroyInstance)) {
        delete instanceOf(effect);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxActionInstanceChanged)) {
        ++g_changed;
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionIsIdentity))
        return isIdentity(effect, in, out);
    if (!std::strcmp(action, kOfxImageEffectActionRender))
        return render(effect, in);
    return kOfxStatReplyDefault;
}

void setHost(OfxHost* host)
{
    g_host = host;
    g_prop = static_cast<const OfxPropertySuiteV1*>(host->fetchSuite(host->host, kOfxPropertySuite, 1));
    g_effect = static_cast<const OfxImageEffectSuiteV1*>(
        host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
    g_param = static_cast<const OfxParameterSuiteV1*>(host->fetchSuite(host->host, kOfxParameterSuite, 1));
    g_thread = static_cast<const OfxMultiThreadSuiteV1*>(
        host->fetchSuite(host->host, kOfxMultiThreadSuite, 1));
}

OfxPlugin g_plugin = {kOfxImageEffectPluginApi, 1, "org.kinoklip.test.Gain", 1, 2, setHost, mainEntry};

} // namespace

extern "C" {

__attribute__((visibility("default"))) int OfxGetNumberOfPlugins(void)
{
    return 1;
}

__attribute__((visibility("default"))) OfxPlugin* OfxGetPlugin(int nth)
{
    return nth == 0 ? &g_plugin : nullptr;
}

__attribute__((visibility("default"))) int kkTestThreadsUsed()
{
    std::lock_guard<std::mutex> lock(g_statsMutex);
    return int(g_threads.size());
}
__attribute__((visibility("default"))) int kkTestRenderCount()
{
    return g_renders.load();
}
__attribute__((visibility("default"))) int kkTestIdentityCount()
{
    return g_identity.load();
}
__attribute__((visibility("default"))) int kkTestChangedCount()
{
    return g_changed.load();
}
__attribute__((visibility("default"))) double kkTestLastScale()
{
    std::lock_guard<std::mutex> lock(g_statsMutex);
    return g_lastScale;
}
__attribute__((visibility("default"))) int kkTestLastWidth()
{
    std::lock_guard<std::mutex> lock(g_statsMutex);
    return g_lastWidth;
}
}
