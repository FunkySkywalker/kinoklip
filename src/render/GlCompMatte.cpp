// SPDX-License-Identifier: GPL-3.0-or-later
// GlCompMatte implementation: GLSL twins of render/CompMatteMath.cpp.
#include "render/GlCompMatte.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

// Shared by the key and final passes: the screen channel and what it is measured against.
const char* const kKeyCommon = R"(
uniform vec4 uTune;     // gain, balance, soft range, kind (0 Delta, 1 Ultra)
uniform vec4 uLevels;   // red, green, blue level, matte separation
vec3 straight(vec4 c) { return c.a > 0.0 ? c.rgb / c.a : vec3(0.0); }
int dominantOf(vec3 v)
{
    int d = 0;
    if (v[1] > v[d]) d = 1;
    if (v[2] > v[d]) d = 2;
    return d;
}
float others(vec3 v, int d)
{
    int o1 = (d + 1) % 3, o2 = (d + 2) % 3;
    if (uTune.w > 0.5)
        return max(v[o1] * uLevels[o1], v[o2] * uLevels[o2]);
    return uTune.y * max(v[o1], v[o2]) + (1.0 - uTune.y) * min(v[o1], v[o2]);
}
)";

const char* const kKeyFragment = R"(
uniform sampler2D uTex;     // the picture the key reads
uniform sampler2D uSecond;  // the clean plate
uniform vec3 uBg;
uniform ivec4 uFlags;       // has plate
void main()
{
    vec4 t = texture(uTex, vUv);
    float v = 0.0;
    if (t.a > 0.0) {
        vec3 c = straight(t);
        vec3 bg = uBg;
        if (uFlags.x == 1) {
            vec4 p = texture(uSecond, vUv);
            if (p.a > 0.0) bg = p.rgb / p.a;
        }
        int d = dominantOf(bg);
        float ref = max(bg[d] - others(bg, d), 1e-3);
        float screen = (c[d] - others(c, d)) / ref;
        if (uTune.w < 0.5) screen *= uTune.x;
        float m = 1.0 - clamp(screen, 0.0, 1.0);
        if (uTune.w < 0.5)
            m = m + (m * m * (3.0 - 2.0 * m) - m) * uTune.z;
        else
            m = clamp((m - 0.5) * (1.0 + 4.0 * uLevels.w) + 0.5, 0.0, 1.0);
        v = m;
    }
    fragColor = vec4(v);
}
)";

const char* const kShapeFragment = R"(
uniform sampler2D uTex;
uniform vec4 uShape;    // gamma, clean background, clean foreground, invert
uniform vec4 uThr;      // low, high, restore fringe (unused here), unused
void main()
{
    float v = clamp(texture(uTex, vUv).a, 0.0, 1.0);
    if (uShape.x != 1.0) v = pow(v, 1.0 / uShape.x);
    v = clamp((v - uShape.y) / max(1.0 - uShape.y - uShape.z, 1e-3), 0.0, 1.0);
    v = clamp((v - uThr.x) / max(uThr.y - uThr.x, 1e-3), 0.0, 1.0);
    if (uShape.w > 0.5) v = 1.0 - v;
    fragColor = vec4(v);
}
)";

const char* const kRestoreFragment = R"(
uniform sampler2D uTex;     // the matte before the shape
uniform sampler2D uSecond;  // the shaped matte
uniform sampler2D uThird;   // the shaped matte, blurred
uniform vec4 uThr;          // restore fringe, invert
void main()
{
    float cur = clamp(texture(uTex, vUv).a, 0.0, 1.0);
    float shaped = texture(uSecond, vUv).a;
    float near = texture(uThird, vUv).a;
    float v = clamp(shaped + uThr.x * max(cur - shaped, 0.0) * clamp(near * 3.0, 0.0, 1.0), 0.0, 1.0);
    if (uThr.y > 0.5) v = 1.0 - v;
    fragColor = vec4(v);
}
)";

const char* const kAlphaFragment = R"(
uniform sampler2D uTex;
void main()
{
    fragColor = vec4(texture(uTex, vUv).a);
}
)";

const char* const kFinalFragment = R"(
uniform sampler2D uTex;     // the source picture
uniform sampler2D uSecond;  // the matte (or the pre-matte for the view)
uniform sampler2D uThird;   // the clean plate
uniform sampler2D uSolid;
uniform sampler2D uGarbage;
uniform vec3 uBg;
uniform ivec4 uFlags;       // spill method, replace mode, has plate, has solid
uniform ivec4 uFlags2;      // has garbage, solid invert, garbage invert, subtract background
uniform vec4 uSpill;        // despill, fringe gamma, fringe size, fringe shape
uniform vec3 uFringe;
uniform vec3 uReplace;
uniform int uView;          // 0 final, 1 pre-matte, 2 matte
void main()
{
    float m = clamp(texture(uSecond, vUv).a, 0.0, 1.0);
    if (uView == 1) {
        fragColor = vec4(vec3(m), 1.0);
        return;
    }
    if (uFlags.w == 1) {
        float s = texture(uSolid, vUv).a;
        m = max(m, uFlags2.y == 1 ? 1.0 - s : s);
    }
    if (uFlags2.x == 1) {
        float g = texture(uGarbage, vUv).a;
        m *= 1.0 - (uFlags2.z == 1 ? 1.0 - g : g);
    }
    m = clamp(m, 0.0, 1.0);
    if (uView == 2) {
        fragColor = vec4(vec3(m), 1.0);
        return;
    }
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    vec3 src = straight(t);
    vec3 bg = uBg;
    if (uFlags.z == 1) {
        vec4 p = texture(uThird, vUv);
        if (p.a > 0.0) bg = p.rgb / p.a;
    }
    vec3 c = src;
    if (uFlags2.w == 1 && m < 1.0 && m > 1e-3)
        c = clamp((c - (1.0 - m) * bg) / m, 0.0, 1.0);
    if (uFlags.x != 0) {
        int d = dominantOf(bg);
        int o1 = (d + 1) % 3, o2 = (d + 2) % 3;
        float limit;
        if (uFlags.x == 1) limit = max(c[o1], c[o2]);
        else if (uFlags.x == 2)
            limit = uTune.w > 0.5 ? 0.5 * (max(c[o1], c[o2]) + min(c[o1], c[o2])) : others(c, d);
        else if (uFlags.x == 3) limit = min(c[o1], c[o2]);
        else limit = min(c[o1], c[o2]) * 0.9;
        c[d] = c[d] + (min(c[d], limit) - c[d]) * uSpill.x;
    }
    float h = clamp(0.5 + 0.5 * uSpill.z, 0.05, 1.0);
    float peak = 0.5 + 0.25 * uSpill.w;
    float w = clamp(1.0 - abs(m - peak) / h, 0.0, 1.0);
    if (uSpill.y != 1.0)
        c += (pow(clamp(c, 0.0, 1.0), vec3(1.0 / uSpill.y)) - c) * w;
    c += uFringe * 0.5 * w;
    if (uFlags.y == 1) c += (src - c) * (1.0 - m);
    else if (uFlags.y == 2) c += (uReplace - c) * (1.0 - clamp((m - 0.98) / 0.02, 0.0, 1.0));
    else if (uFlags.y == 3) c += (uReplace - c) * (1.0 - m);
    c = clamp(c, 0.0, 1.0);
    float a = t.a * m;
    fragColor = vec4(c * a, a);
}
)";

const char* const kMaxFragment = R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
void main()
{
    fragColor = max(texture(uTex, vUv), texture(uSecond, vUv));
}
)";

const char* const kControlFragment = R"(
uniform sampler2D uTex;     // the picture
uniform sampler2D uSecond;  // the filtered matte
uniform sampler2D uSolid;
uniform sampler2D uGarbage;
uniform ivec4 uFlags;       // has solid, has garbage, solid invert, garbage invert
void main()
{
    float m = clamp(texture(uSecond, vUv).a, 0.0, 1.0);
    if (uFlags.x == 1) {
        float s = texture(uSolid, vUv).a;
        m = max(m, uFlags.z == 1 ? 1.0 - s : s);
    }
    if (uFlags.y == 1) {
        float g = texture(uGarbage, vUv).a;
        m *= 1.0 - (uFlags.w == 1 ? 1.0 - g : g);
    }
    m = clamp(m, 0.0, 1.0);
    vec4 t = texture(uTex, vUv);
    vec3 s = t.a > 0.0 ? t.rgb / t.a : vec3(0.0);
    fragColor = vec4(s * m, m);
}
)";

bool build(GlKit& kit, GlProgram& program, const char* name, const QByteArray& source, QString* why)
{
    if (!program.build(kit.gl(), name, source.constData(), why))
        return false;
    GlFunctions* gl = kit.gl();
    gl->glUseProgram(program.id);
    const char* units[] = {"uTex", "uSecond", "uThird", "uSolid", "uGarbage"};
    for (int i = 0; i < 5; ++i) {
        const GLint loc = program.uniform(gl, units[i]);
        if (loc >= 0)
            gl->glUniform1i(loc, i);
    }
    gl->glUseProgram(0);
    return true;
}

} // namespace

bool GlCompMatte::initialize(GlKit& kit, QString* why)
{
    GlFunctions* gl = kit.gl();
    const QByteArray common(kKeyCommon);
    if (!build(kit, m_key, "compmatte.key", common + kKeyFragment, why)
        || !build(kit, m_shape, "compmatte.shape", kShapeFragment, why)
        || !build(kit, m_restore, "compmatte.restore", kRestoreFragment, why)
        || !build(kit, m_final, "compmatte.final", common + kFinalFragment, why)
        || !build(kit, m_alpha, "compmatte.alpha", kAlphaFragment, why)
        || !build(kit, m_control, "compmatte.control", kControlFragment, why)
        || !build(kit, m_max, "compmatte.max", kMaxFragment, why))
        return false;
    m_kBg = m_key.uniform(gl, "uBg");
    m_kTune = m_key.uniform(gl, "uTune");
    m_kLevels = m_key.uniform(gl, "uLevels");
    m_kFlags = m_key.uniform(gl, "uFlags");
    m_sShape = m_shape.uniform(gl, "uShape");
    m_sThr = m_shape.uniform(gl, "uThr");
    m_rThr = m_restore.uniform(gl, "uThr");
    m_fBg = m_final.uniform(gl, "uBg");
    m_fTune = m_final.uniform(gl, "uTune");
    m_fLevels = m_final.uniform(gl, "uLevels");
    m_fFlags = m_final.uniform(gl, "uFlags");
    m_fFlags2 = m_final.uniform(gl, "uFlags2");
    m_fSpill = m_final.uniform(gl, "uSpill");
    m_fFringe = m_final.uniform(gl, "uFringe");
    m_fReplace = m_final.uniform(gl, "uReplace");
    m_fView = m_final.uniform(gl, "uView");
    m_cFlags = m_control.uniform(gl, "uFlags");
    return true;
}

void GlCompMatte::destroy(GlKit& kit)
{
    for (GlProgram* p : {&m_key, &m_shape, &m_restore, &m_final, &m_alpha, &m_control, &m_max})
        p->destroy(kit.gl());
}

PooledTarget GlCompMatte::run(GlKit& kit, QSize size, const GlProgram& program)
{
    PooledTarget out = kit.acquire(size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    kit.gl()->glUseProgram(program.id);
    kit.setFullscreen(program);
    return out;
}

PooledTarget GlCompMatte::matteFilter(GlKit& kit, GlCompFilters& fx, const RenderTarget& matte,
    const compmath::MatteFilterParams& m, double scale)
{
    using namespace compmath;
    GlFunctions* gl = kit.gl();
    PooledTarget cur;
    const auto current = [&]() -> const RenderTarget& { return cur ? *cur : matte; };
    if (m.erode != 0.0) {
        PooledTarget e = kit.morphology(matte, std::abs(m.erode) * scale, m.erode > 0);
        if (e)
            cur = std::move(e);
    }
    if (m.blur > 0.0) {
        PooledTarget b = fx.blur(kit, current(), m.filter, m.blur * scale, m.blur * scale, m.passes);
        if (b)
            cur = std::move(b);
    }
    const bool shapeIdentity = m.gamma == 1.0f && m.cleanBg == 0.0f && m.cleanFg == 0.0f
        && m.low == 0.0f && m.high == 1.0f;
    if (m.restoreFringe <= 0.0f) {
        if (shapeIdentity && !m.invert)
            return cur;
        PooledTarget out = run(kit, matte.size, m_shape);
        gl->glUniform4f(m_sShape, m.gamma, m.cleanBg, m.cleanFg, m.invert ? 1.0f : 0.0f);
        gl->glUniform4f(m_sThr, m.low, m.high, 0, 0);
        kit.bindTexture(0, current().texture);
        kit.drawQuad();
        return out;
    }
    // Restore Fringe: the shaped matte (no invert), its blur, then the combination.
    PooledTarget shaped = run(kit, matte.size, m_shape);
    gl->glUniform4f(m_sShape, m.gamma, m.cleanBg, m.cleanFg, 0.0f);
    gl->glUniform4f(m_sThr, m.low, m.high, 0, 0);
    kit.bindTexture(0, current().texture);
    kit.drawQuad();
    PooledTarget near = kit.gaussianBlur(*shaped, 3.0 * scale);
    PooledTarget out = run(kit, matte.size, m_restore);
    gl->glUniform4f(m_rThr, m.restoreFringe, m.invert ? 1.0f : 0.0f, 0, 0);
    kit.bindTexture(0, current().texture);
    kit.bindTexture(1, shaped->texture);
    kit.bindTexture(2, near ? near->texture : shaped->texture);
    kit.drawQuad();
    return out;
}

PooledTarget GlCompMatte::keyer(GlKit& kit, GlCompFilters& fx, const RenderTarget& src,
    const RenderTarget* plate, const RenderTarget* garbage, const RenderTarget* solid,
    const compmath::KeyerParams& k, double scale)
{
    using namespace compmath;
    GlFunctions* gl = kit.gl();
    PooledTarget blurred;
    if (k.preBlur > 0)
        blurred = kit.gaussianBlur(src, k.preBlur * scale * 0.5);
    const RenderTarget& read = blurred ? *blurred : src;
    PooledTarget m0 = run(kit, src.size, m_key);
    gl->glUniform3f(m_kBg, k.bg[0], k.bg[1], k.bg[2]);
    gl->glUniform4f(m_kTune, k.gain, k.balance, k.soft, k.kind == UltraKind ? 1.0f : 0.0f);
    gl->glUniform4f(m_kLevels, k.levels[0], k.levels[1], k.levels[2], k.separation);
    gl->glUniform4i(m_kFlags, plate ? 1 : 0, 0, 0, 0);
    kit.bindTexture(0, read.texture);
    kit.bindTexture(1, plate ? plate->texture : kit.transparentTexture());
    kit.drawQuad();
    // Pre-matte: erode / blur, then Ultra's closing.
    PooledTarget pre = matteFilter(kit, fx, *m0, k.pre, scale);
    if (k.closeSize > 0) {
        const RenderTarget& base = pre ? *pre : *m0;
        PooledTarget closed = kit.gaussianBlur(base, k.closeSize * scale * 0.5);
        if (closed) {
            PooledTarget joined = run(kit, src.size, m_max);
            kit.bindTexture(0, base.texture);
            kit.bindTexture(1, closed->texture);
            kit.drawQuad();
            pre = std::move(joined);
        }
    }
    const RenderTarget& preMatte = pre ? *pre : *m0;
    PooledTarget final = k.view == 1 ? PooledTarget() : matteFilter(kit, fx, preMatte, k.matte, scale);
    const RenderTarget& matte = k.view == 1 ? preMatte : (final ? *final : preMatte);
    PooledTarget out = run(kit, src.size, m_final);
    gl->glUniform3f(m_fBg, k.bg[0], k.bg[1], k.bg[2]);
    gl->glUniform4f(m_fTune, k.gain, k.balance, k.soft, k.kind == UltraKind ? 1.0f : 0.0f);
    gl->glUniform4f(m_fLevels, k.levels[0], k.levels[1], k.levels[2], k.separation);
    gl->glUniform4i(m_fFlags, k.spill, k.replace, plate ? 1 : 0, solid ? 1 : 0);
    gl->glUniform4i(m_fFlags2, garbage ? 1 : 0, k.solidInvert ? 1 : 0, k.garbageInvert ? 1 : 0,
        k.subtractBackground ? 1 : 0);
    gl->glUniform4f(m_fSpill, k.despill, k.fringeGamma, k.fringeSize, k.fringeShape);
    gl->glUniform3f(m_fFringe, k.fringeColor[0], k.fringeColor[1], k.fringeColor[2]);
    gl->glUniform3f(m_fReplace, k.replaceColor[0], k.replaceColor[1], k.replaceColor[2]);
    gl->glUniform1i(m_fView, k.view);
    kit.bindTexture(0, src.texture);
    kit.bindTexture(1, matte.texture);
    kit.bindTexture(2, plate ? plate->texture : kit.transparentTexture());
    kit.bindTexture(3, solid ? solid->texture : kit.transparentTexture());
    kit.bindTexture(4, garbage ? garbage->texture : kit.transparentTexture());
    kit.drawQuad();
    return out;
}

PooledTarget GlCompMatte::matteControl(GlKit& kit, GlCompFilters& fx, const RenderTarget& src,
    const RenderTarget* garbage, const RenderTarget* solid,
    const compmath::MatteControlParams& m, double scale)
{
    GlFunctions* gl = kit.gl();
    PooledTarget a = run(kit, src.size, m_alpha);
    kit.bindTexture(0, src.texture);
    kit.drawQuad();
    PooledTarget f = matteFilter(kit, fx, *a, m.matte, scale);
    const RenderTarget& matte = f ? *f : *a;
    PooledTarget out = run(kit, src.size, m_control);
    gl->glUniform4i(m_cFlags, solid ? 1 : 0, garbage ? 1 : 0, m.solidInvert ? 1 : 0,
        m.garbageInvert ? 1 : 0);
    kit.bindTexture(0, src.texture);
    kit.bindTexture(1, matte.texture);
    kit.bindTexture(3, solid ? solid->texture : kit.transparentTexture());
    kit.bindTexture(4, garbage ? garbage->texture : kit.transparentTexture());
    kit.drawQuad();
    return out;
}

} // namespace kk::render
