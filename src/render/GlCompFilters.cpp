// SPDX-License-Identifier: GPL-3.0-or-later
// GlCompFilters implementation: GLSL twins of render/CompFilterMath.cpp.
#include "render/GlCompFilters.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

const char* const kHighlightFragment = R"(
uniform sampler2D uTex;
uniform float uThreshold;
void main()
{
    vec4 c = texture(uTex, vUv);
    float a = c.a;
    vec3 s = a > 0.0 ? c.rgb / a : vec3(0.0);
    vec3 over = max(s - uThreshold, 0.0);
    float top = max(over.r, max(over.g, over.b));
    fragColor = vec4(a * over, a * clamp(top / max(1.0 - uThreshold, 1e-3), 0.0, 1.0));
}
)";

const char* const kGlowCombineFragment = R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform vec4 uMul;      // gain * colour scale per channel, 0 for a channel that is off
void main()
{
    vec4 c = texture(uTex, vUv);
    vec4 add = texture(uSecond, vUv) * uMul;
    float a = c.a + (1.0 - c.a) * min(add.a, 1.0);
    fragColor = vec4(min(c.rgb + add.rgb, vec3(a)), a);
}
)";

const char* const kAlphaFragment = R"(
uniform sampler2D uTex;
void main()
{
    fragColor = vec4(texture(uTex, vUv).a);
}
)";

const char* const kWrapEdgeFragment = R"(
uniform sampler2D uTex;     // the blurred background
uniform sampler2D uSecond;  // the blurred foreground alpha (in every channel)
uniform vec4 uA;            // luma tolerance, saturation, luma based, use constant
uniform vec3 uConst;
void main()
{
    vec4 bg = texture(uTex, vUv);
    float ab = texture(uSecond, vUv).a;
    vec3 s = bg.a > 0.0 ? bg.rgb / bg.a : vec3(0.0);
    float lum = dot(s, vec3(0.2126, 0.7152, 0.0722));
    vec3 col = uA.w > 0.5 ? uConst : s;
    if (uA.z < 0.5)
        col /= max(max(col.r, max(col.g, col.b)), 1e-3);
    float gate = uA.x <= 0.0 ? 1.0 : clamp((lum - uA.x) / 0.1, 0.0, 1.0);
    float cl = dot(col, vec3(0.2126, 0.7152, 0.0722));
    float edge = clamp(1.0 - ab, 0.0, 1.0) * gate;
    fragColor = vec4(max(vec3(cl) + (col - cl) * uA.y, 0.0) * edge, edge);
}
)";

const char* const kWrapCombineFragment = R"(
uniform sampler2D uTex;     // the foreground
uniform sampler2D uSecond;  // the diffused wrap
uniform vec4 uA;            // intensity, glow, wrap only, screen
void main()
{
    vec4 fg = texture(uTex, vUv);
    vec4 w = texture(uSecond, vUv);
    float inside = uA.y > 0.5 ? 1.0 : fg.a;
    vec3 wr = w.rgb * uA.x * inside;
    float wa = min(w.a * uA.x, 1.0) * inside;
    if (uA.z > 0.5) {
        fragColor = vec4(min(wr, vec3(wa)), wa);
        return;
    }
    float a = uA.y > 0.5 ? fg.a + (1.0 - fg.a) * wa : fg.a;
    vec3 v = uA.w > 0.5 ? fg.rgb + wr * (1.0 - min(fg.rgb, vec3(1.0))) : fg.rgb + wr;
    fragColor = vec4(min(v, vec3(a)), a);
}
)";

bool build(GlKit& kit, GlProgram& program, const char* name, const char* source, QString* why)
{
    if (!program.build(kit.gl(), name, source, why))
        return false;
    GlFunctions* gl = kit.gl();
    gl->glUseProgram(program.id);
    gl->glUniform1i(program.uniform(gl, "uTex"), 0);
    gl->glUniform1i(program.uniform(gl, "uSecond"), 1);
    gl->glUseProgram(0);
    return true;
}

/// Runs @p fn(target, amount) (an isotropic blur; empty = unchanged) for different amounts along
/// x and y: the picture is scaled down along its longer-blurred axis by the ratio of the
/// amounts, blurred with the smaller amount and scaled back.
template<typename Fn>
PooledTarget anisotropic(GlKit& kit, const RenderTarget& src, double ax, double ay, Fn fn)
{
    const double hi = std::max(ax, ay), lo = std::min(ax, ay);
    if (hi < 0.25)
        return {};
    if (lo >= hi * 0.98)
        return fn(src, hi);
    const double ratio = std::max(lo / hi, 0.02);
    const QSize small = ax > ay ? QSize(std::max(8, int(src.size.width() * ratio)), src.size.height())
                                : QSize(src.size.width(), std::max(8, int(src.size.height() * ratio)));
    PooledTarget down = kit.resample(src, small);
    PooledTarget blurred = fn(*down, hi * ratio);
    return kit.resample(blurred ? *blurred : *down, src.size);
}

} // namespace

bool GlCompFilters::initialize(GlKit& kit, QString* why)
{
    GlFunctions* gl = kit.gl();
    if (!build(kit, m_highlight, "compfx.highlight", kHighlightFragment, why)
        || !build(kit, m_glowCombine, "compfx.glowcombine", kGlowCombineFragment, why)
        || !build(kit, m_alpha, "compfx.alpha", kAlphaFragment, why)
        || !build(kit, m_wrapEdge, "compfx.wrapedge", kWrapEdgeFragment, why)
        || !build(kit, m_wrapCombine, "compfx.wrapcombine", kWrapCombineFragment, why))
        return false;
    m_hlThreshold = m_highlight.uniform(gl, "uThreshold");
    m_gcMul = m_glowCombine.uniform(gl, "uMul");
    m_weA = m_wrapEdge.uniform(gl, "uA");
    m_weConst = m_wrapEdge.uniform(gl, "uConst");
    m_wcA = m_wrapCombine.uniform(gl, "uA");
    return true;
}

void GlCompFilters::destroy(GlKit& kit)
{
    for (GlProgram* p : {&m_highlight, &m_glowCombine, &m_alpha, &m_wrapEdge, &m_wrapCombine})
        p->destroy(kit.gl());
}

PooledTarget GlCompFilters::pass(
    GlKit& kit, QSize size, const GlProgram& program)
{
    PooledTarget out = kit.acquire(size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    kit.gl()->glUseProgram(program.id);
    kit.setFullscreen(program);
    return out;
}

PooledTarget GlCompFilters::blur(GlKit& kit, const RenderTarget& src, int kind, double sizeX,
    double sizeY, int passes)
{
    using namespace compmath;
    if (kind == GaussianBlur) {
        return anisotropic(kit, src, sizeX * 0.5, sizeY * 0.5,
            [&](const RenderTarget& t, double sigma) { return kit.gaussianBlur(t, sigma); });
    }
    int n = 1;
    if (kind == BartlettBlur)
        n = 2;
    else if (kind == MultiBoxBlur)
        n = std::clamp(passes, 1, 10);
    PooledTarget current;
    for (int i = 0; i < n; ++i) {
        const RenderTarget& from = current ? *current : src;
        PooledTarget next = anisotropic(kit, from, sizeX / n, sizeY / n,
            [&](const RenderTarget& t, double radius) { return kit.boxBlur(t, radius); });
        if (next)
            current = std::move(next);
    }
    return current;
}

PooledTarget GlCompFilters::softGlow(
    GlKit& kit, const RenderTarget& in, const compmath::SoftGlowParams& g, double scale)
{
    GlFunctions* gl = kit.gl();
    PooledTarget hl = pass(kit, in.size, m_highlight);
    gl->glUniform1f(m_hlThreshold, g.threshold);
    kit.bindTexture(0, in.texture);
    kit.drawQuad();
    PooledTarget blurred = blur(kit, *hl, g.filter, g.sizeX * scale, g.sizeY * scale, g.passes);
    const RenderTarget& glow = blurred ? *blurred : *hl;
    PooledTarget out = pass(kit, in.size, m_glowCombine);
    float mul[4];
    for (int i = 0; i < 4; ++i)
        mul[i] = g.channel[i] ? g.gain * g.scale[i] : 0.0f;
    gl->glUniform4f(m_gcMul, mul[0], mul[1], mul[2], mul[3]);
    kit.bindTexture(0, in.texture);
    kit.bindTexture(1, glow.texture);
    kit.drawQuad();
    return out;
}

PooledTarget GlCompFilters::lightWrap(GlKit& kit, const RenderTarget& fg, const RenderTarget& bg,
    const compmath::LightWrapParams& l, double scale)
{
    GlFunctions* gl = kit.gl();
    PooledTarget alpha = pass(kit, fg.size, m_alpha);
    kit.bindTexture(0, fg.texture);
    kit.drawQuad();
    const double fgSigma = l.fgBlur * scale * 0.5, bgSigma = l.bgBlur * scale * 0.5;
    PooledTarget ab = kit.gaussianBlur(*alpha, fgSigma);
    PooledTarget bb = kit.gaussianBlur(bg, bgSigma);
    PooledTarget edge = pass(kit, fg.size, m_wrapEdge);
    gl->glUniform4f(m_weA, l.lumaTolerance, l.saturation, l.lumaBased ? 1.0f : 0.0f,
        l.useConstant ? 1.0f : 0.0f);
    gl->glUniform3f(m_weConst, l.constant[0], l.constant[1], l.constant[2]);
    kit.bindTexture(0, bb ? bb->texture : bg.texture);
    kit.bindTexture(1, ab ? ab->texture : alpha->texture);
    kit.drawQuad();
    PooledTarget wd = kit.gaussianBlur(*edge, l.diffuse * scale * 0.5);
    PooledTarget out = pass(kit, fg.size, m_wrapCombine);
    gl->glUniform4f(m_wcA, l.intensity, l.glow ? 1.0f : 0.0f, l.wrapOnly ? 1.0f : 0.0f,
        l.screen ? 1.0f : 0.0f);
    kit.bindTexture(0, fg.texture);
    kit.bindTexture(1, wd ? wd->texture : edge->texture);
    kit.drawQuad();
    return out;
}

} // namespace kk::render
