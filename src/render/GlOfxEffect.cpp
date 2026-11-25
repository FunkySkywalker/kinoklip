// SPDX-License-Identifier: GPL-3.0-or-later
// GlOfxEffect — see GlOfxEffect.h.
#include "render/GlOfxEffect.h"

#include "plugins/OfxRegistry.h"

#include <QLoggingCategory>

#include <algorithm>
#include <vector>

Q_LOGGING_CATEGORY(lcOfxRender, "kk.ofx.render")

namespace kk::render {

namespace {

class OfxGlEffect final : public GlEffect
{
public:
    explicit OfxGlEffect(std::shared_ptr<const plugins::OfxPluginEntry> entry)
        : m_entry(std::move(entry))
    { }

    bool initialize(GlKit&, QString* why) override
    {
        plugins::OfxRegistry& registry = plugins::OfxRegistry::instance();
        // A crash while the plugin loads or renders its first frame puts its bundle on the
        // bad-plugin list at the next start.
        registry.markLoading(m_entry->bundlePath);
        QString error;
        m_instance = ofx::Instance::create(m_entry->bundleInfo(), m_entry->info.index, &error);
        if (!m_instance) {
            registry.markLoaded(m_entry->bundlePath);
            *why = error;
            registry.reportRenderError(m_entry->fullId, error);
            return false;
        }
        return true;
    }

    void destroy(GlKit&) override { m_instance.reset(); }

    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        if (!m_instance || m_failedFrames > kGiveUpAfter)
            return {};
        GlFunctions* gl = kit.gl();
        const RenderTarget& layer = *in.layer;
        const int w = layer.size.width(), h = layer.size.height();
        const size_t floats = size_t(w) * size_t(h) * 4;
        m_src.resize(floats);
        m_dst.resize(floats);
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, layer.fbo);
        gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, m_src.data());
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

        const double scale = in.scale > 0 ? in.scale : 1.0;
        ofx::RenderArgs args;
        args.time = double(in.localFrame);
        args.fps = in.fps;
        args.width = w;
        args.height = h;
        args.scale = scale;
        args.originX = -in.paddingPx / scale;
        args.originY = -in.paddingPx / scale;
        args.projectWidth = (w - 2 * in.paddingPx) / scale;
        args.projectHeight = (h - 2 * in.paddingPx) / scale;
        args.interactive = !in.waitForAi;
        const ParamSet& set = *in.params;
        const double pw = args.projectWidth, ph = args.projectHeight;
        const auto& maps = m_entry->params;
        // what the plugin reads at the frame now, and at other frames when it asks
        for (const plugins::OfxParamMap& map : maps)
            m_instance->setValue(map.info.name, plugins::OfxRegistry::valueFor(map, set, args.time, pw, ph));
        m_instance->setEvaluator([&](const QString& name, double time, ofx::ParamValue* out) {
            for (const plugins::OfxParamMap& map : maps) {
                if (map.info.name == name) {
                    *out = plugins::OfxRegistry::valueFor(map, set, time, pw, ph);
                    return true;
                }
            }
            return false;
        });
        QString error;
        const ofx::RenderResult result = m_instance->render(args, m_src.data(), m_dst.data(), &error);
        m_instance->setEvaluator({});
        plugins::OfxRegistry& registry = plugins::OfxRegistry::instance();
        if (result == ofx::RenderResult::Failed) {
            ++m_failedFrames;
            qCWarning(lcOfxRender).noquote() << m_entry->fullId << error;
            registry.reportRenderError(m_entry->fullId, error);
            return {};
        }
        if (!m_loadedMarked) {
            m_loadedMarked = true;
            registry.markLoaded(m_entry->bundlePath);
        }
        m_failedFrames = 0;
        if (result == ofx::RenderResult::Identity)
            return {};
        PooledTarget out = kit.acquire(layer.size, layer.format);
        kit.bindTexture(0, out->texture);
        gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT, m_dst.data());
        kit.bindTexture(0, 0);
        return out;
    }

private:
    static constexpr int kGiveUpAfter = 30; ///< consecutive failed frames before the effect is bypassed
    std::shared_ptr<const plugins::OfxPluginEntry> m_entry;
    std::unique_ptr<ofx::Instance> m_instance;
    std::vector<float> m_src, m_dst; ///< kept between frames
    bool m_loadedMarked = false;
    int m_failedFrames = 0;
};

} // namespace

bool isOfxId(const QString& id)
{
    return id.startsWith(QLatin1String(plugins::kOfxIdPrefix));
}

std::unique_ptr<GlEffect> makeOfxEffect(const QString& fullId)
{
    const auto entry = plugins::OfxRegistry::instance().plugin(fullId);
    if (!entry)
        return nullptr;
    return std::make_unique<OfxGlEffect>(entry);
}

} // namespace kk::render
