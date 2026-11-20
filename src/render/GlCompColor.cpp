// SPDX-License-Identifier: GPL-3.0-or-later
// GlCompColor implementation: GLSL twins of render/CompColorMath.cpp. Every shader reads the
// picture, turns it into straight colour, computes, clamps to 0..1 and premultiplies again, as
// the CPU reference does (CpuComposition::pointwise).
#include "render/GlCompColor.h"

#include "core/Expression.h"
#include "core/NodeRegistry.h"
#include "render/RenderLog.h"

#include <algorithm>

namespace kk::render {

namespace {

const char* const kStraight = R"(
vec4 kk_straight(vec4 c)
{
    return c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(c.rgb, 0.0);
}
vec4 kk_premult(vec3 s, float a)
{
    a = clamp(a, 0.0, 1.0);
    return vec4(clamp(s, 0.0, 1.0) * a, a);
}
)";

const char* const kBcFragment = R"(
uniform sampler2D uTex;
uniform vec4 uA;   // gain, lift, gamma, contrast
uniform vec4 uB;   // brightness, saturation, low, high
uniform ivec4 uC;  // red, green, blue, alpha switches
float chain(float v)
{
    float range = max(uB.w - uB.z, 0.001);
    v = (v - uB.z) / range;
    v *= uA.x;
    v += uA.y * (1.0 - v);
    v = pow(max(v, 0.0), 1.0 / uA.z);
    v = (v - 0.5) * (1.0 + uA.w) + 0.5;
    return v + uB.x;
}
void main()
{
    vec4 c = kk_straight(texelFetch(uTex, ivec2(floor(vUv * vec2(textureSize(uTex, 0)))), 0));
    for (int i = 0; i < 3; ++i) {
        if (uC[i] != 0)
            c[i] = chain(c[i]);
    }
    if (uB.y != 1.0) {
        float y = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
        c.rgb = vec3(y) + (c.rgb - vec3(y)) * uB.y;
    }
    if (uC.w != 0)
        c.a = chain(c.a);
    fragColor = kk_premult(c.rgb, c.a);
}
)";

const char* const kAlphaFragment = R"(
uniform sampler2D uTex;
uniform int uDivide;
void main()
{
    vec4 c = kk_straight(texelFetch(uTex, ivec2(floor(vUv * vec2(textureSize(uTex, 0)))), 0));
    if (uDivide != 0) {
        if (c.a > 0.0)
            c.rgb = min(c.rgb / c.a, vec3(1.0));
    } else {
        c.rgb *= c.a;
    }
    fragColor = kk_premult(c.rgb, c.a);
}
)";

const char* const kMapperFragment = R"(
uniform sampler2D uTex;
uniform vec4 uM[4];   // row o: how much of R, G, B, A output channel o takes
uniform vec4 uOff;
void main()
{
    vec4 c = kk_straight(texelFetch(uTex, ivec2(floor(vUv * vec2(textureSize(uTex, 0)))), 0));
    vec4 o = vec4(dot(uM[0], c), dot(uM[1], c), dot(uM[2], c), dot(uM[3], c)) + uOff;
    fragColor = kk_premult(o.rgb, o.a);
}
)";

// The Color page curve stage (GlGrade.cpp) on straight colour: rows 0 to 8 of uCurves are the
// Grade tables (Y, R, G, B, hue vs hue, hue vs sat, hue vs lum, lum vs sat, sat vs sat), row 9
// the alpha table.
const char* const kCurvesFragment = R"(
uniform sampler2D uTex;
uniform sampler2D uCurves;
uniform int uCurveMask;
uniform int uAlphaOn;
const int kCurveSize = 256;
float curve(int index, float x)
{
    float pos = clamp(x, 0.0, 1.0) * float(kCurveSize - 1);
    int i = min(int(pos), kCurveSize - 2);
    float f = pos - float(i);
    float a = texelFetch(uCurves, ivec2(i, index), 0).r;
    float b = texelFetch(uCurves, ivec2(i + 1, index), 0).r;
    return a + (b - a) * f;
}
bool curveOn(int index) { return (uCurveMask & (1 << index)) != 0; }
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 rgbToHsl(vec3 c)
{
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float l = (mx + mn) * 0.5;
    float d = mx - mn;
    float h = 0.0;
    float s = 0.0;
    if (d > 1e-6) {
        s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
        if (mx == c.r)
            h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
        else if (mx == c.g)
            h = (c.b - c.r) / d + 2.0;
        else
            h = (c.r - c.g) / d + 4.0;
        h /= 6.0;
    }
    return vec3(h - floor(h), s, l);
}
float hueToChannel(float p, float q, float t)
{
    t -= floor(t);
    if (t < 1.0 / 6.0)
        return p + (q - p) * 6.0 * t;
    if (t < 0.5)
        return q;
    if (t < 2.0 / 3.0)
        return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}
vec3 hslToRgb(vec3 hsl)
{
    if (hsl.y <= 0.0)
        return vec3(hsl.z);
    float q = hsl.z < 0.5 ? hsl.z * (1.0 + hsl.y) : hsl.z + hsl.y - hsl.z * hsl.y;
    float p = 2.0 * hsl.z - q;
    return vec3(hueToChannel(p, q, hsl.x + 1.0 / 3.0), hueToChannel(p, q, hsl.x),
        hueToChannel(p, q, hsl.x - 1.0 / 3.0));
}
vec3 curves(vec3 c)
{
    for (int ch = 0; ch < 3; ++ch) {
        float v = clamp(c[ch], 0.0, 1.0);
        if (curveOn(0))
            v = curve(0, v);
        if (curveOn(ch + 1))
            v = curve(ch + 1, v);
        c[ch] = v;
    }
    if ((uCurveMask & 0x1f0) == 0)
        return c;
    vec3 hsl = rgbToHsl(c);
    float h = hsl.x;
    float y = clamp(luma(c), 0.0, 1.0);
    float factor = 1.0;
    if (curveOn(5))
        factor *= curve(5, h);
    if (curveOn(7))
        factor *= curve(7, y);
    if (curveOn(8))
        factor *= curve(8, hsl.y);
    if (curveOn(4)) {
        float shifted = h + curve(4, h);
        hsl.x = shifted - floor(shifted);
    }
    hsl.y = clamp(hsl.y * factor, 0.0, 1.0);
    c = hslToRgb(hsl);
    if (curveOn(6)) {
        float y2 = luma(c);
        c += y2 * (curve(6, h) - 1.0);
    }
    return c;
}
void main()
{
    vec4 c = kk_straight(texelFetch(uTex, ivec2(floor(vUv * vec2(textureSize(uTex, 0)))), 0));
    c.rgb = curves(c.rgb);
    if (uAlphaOn != 0)
        c.a = curve(9, c.a);
    fragColor = kk_premult(c.rgb, c.a);
}
)";

constexpr int kCurveRows = 10;

QByteArray customSource(const QString& body)
{
    const QString head = QStringLiteral(R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform float uN[8];
uniform vec4 uInfo;   // time, frame width, frame height, has Image 2
)");
    const QString mainText = QStringLiteral(R"(
void main()
{
    ivec2 sz = textureSize(uTex, 0);
    ivec2 ip = ivec2(floor(vUv * vec2(sz)));
    vec4 c1 = kk_straight(texelFetch(uTex, ip, 0));
    vec4 c2 = vec4(0.0);
    if (uInfo.w > 0.5)
        c2 = kk_straight(texelFetch(uSecond, clamp(ip, ivec2(0), textureSize(uSecond, 0) - 1), 0));
    float v_time = uInfo.x;
    float v_x = (float(ip.x) + 0.5) / float(sz.x);
    float v_y = 1.0 - (float(ip.y) + 0.5) / float(sz.y);
    float v_w = uInfo.y;
    float v_h = uInfo.z;
    float v_n1 = uN[0], v_n2 = uN[1], v_n3 = uN[2], v_n4 = uN[3];
    float v_n5 = uN[4], v_n6 = uN[5], v_n7 = uN[6], v_n8 = uN[7];
    float v_r1 = c1.r, v_g1 = c1.g, v_b1 = c1.b, v_a1 = c1.a;
    float v_r2 = c2.r, v_g2 = c2.g, v_b2 = c2.b, v_a2 = c2.a;
    float v_i1 = 0.0, v_i2 = 0.0, v_i3 = 0.0, v_i4 = 0.0;
    vec4 result = c1;
%1    fragColor = kk_premult(result.rgb, result.a);
}
)").arg(body);
    return (QString::fromLatin1(kStraight) + expr::Program::pixelGlslHelpers() + head + mainText)
        .toUtf8();
}

} // namespace

bool GlCompColor::initialize(GlKit& kit, QString* why)
{
    GlFunctions* gl = kit.gl();
    const QByteArray pre(kStraight);
    const auto build = [&](GlProgram& p, const char* name, const char* body, int seconds = 0) {
        if (!p.build(gl, name, (pre + body).constData(), why))
            return false;
        gl->glUseProgram(p.id);
        gl->glUniform1i(p.uniform(gl, "uTex"), 0);
        if (seconds)
            gl->glUniform1i(p.uniform(gl, "uCurves"), 1);
        gl->glUseProgram(0);
        return true;
    };
    if (!build(m_bc, "compcolor.bc", kBcFragment) || !build(m_alpha, "compcolor.alpha", kAlphaFragment)
        || !build(m_mapper, "compcolor.mapper", kMapperFragment)
        || !build(m_curves, "compcolor.curves", kCurvesFragment, 1))
        return false;
    m_bcA = m_bc.uniform(gl, "uA");
    m_bcB = m_bc.uniform(gl, "uB");
    m_bcC = m_bc.uniform(gl, "uC");
    m_alDivide = m_alpha.uniform(gl, "uDivide");
    m_mapM = m_mapper.uniform(gl, "uM");
    m_mapOff = m_mapper.uniform(gl, "uOff");
    m_cvMask = m_curves.uniform(gl, "uCurveMask");
    m_cvAlpha = m_curves.uniform(gl, "uAlphaOn");
    gl->glGenTextures(1, &m_curveTex);
    gl->glBindTexture(GL_TEXTURE_2D, m_curveTex);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, grademath::kCurveTableSize, kCurveRows, 0, GL_RED,
        GL_FLOAT, nullptr);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void GlCompColor::destroy(GlKit& kit)
{
    GlFunctions* gl = kit.gl();
    for (GlProgram* p : {&m_bc, &m_alpha, &m_mapper, &m_curves})
        p->destroy(gl);
    for (auto& c : m_custom)
        c->program.destroy(gl);
    m_custom.clear();
    if (m_curveTex)
        gl->glDeleteTextures(1, &m_curveTex);
    m_curveTex = 0;
}

PooledTarget GlCompColor::brightnessContrast(
    GlKit& kit, const RenderTarget& in, const compmath::BrightnessContrastParams& b)
{
    GlFunctions* gl = kit.gl();
    PooledTarget out = kit.acquire(in.size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    gl->glUseProgram(m_bc.id);
    kit.setFullscreen(m_bc);
    gl->glUniform4f(m_bcA, b.gain, b.lift, b.gamma, b.contrast);
    gl->glUniform4f(m_bcB, b.brightness, b.saturation, b.low, b.high);
    gl->glUniform4i(m_bcC, b.channel[0], b.channel[1], b.channel[2], b.channel[3]);
    kit.bindTexture(0, in.texture);
    kit.drawQuad();
    return out;
}

PooledTarget GlCompColor::alpha(GlKit& kit, const RenderTarget& in, bool divide)
{
    GlFunctions* gl = kit.gl();
    PooledTarget out = kit.acquire(in.size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    gl->glUseProgram(m_alpha.id);
    kit.setFullscreen(m_alpha);
    gl->glUniform1i(m_alDivide, divide ? 1 : 0);
    kit.bindTexture(0, in.texture);
    kit.drawQuad();
    return out;
}

PooledTarget GlCompColor::channelMapper(
    GlKit& kit, const RenderTarget& in, const compmath::ChannelMapperParams& c)
{
    GlFunctions* gl = kit.gl();
    PooledTarget out = kit.acquire(in.size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    gl->glUseProgram(m_mapper.id);
    kit.setFullscreen(m_mapper);
    gl->glUniform4fv(m_mapM, 4, &c.m[0][0]);
    gl->glUniform4fv(m_mapOff, 1, c.offset);
    kit.bindTexture(0, in.texture);
    kit.drawQuad();
    return out;
}

PooledTarget GlCompColor::curves(
    GlKit& kit, const RenderTarget& in, const compmath::CurvesNodeParams& c)
{
    GlFunctions* gl = kit.gl();
    float rows[grademath::kCurveTableSize * kCurveRows];
    std::copy(&c.tables.table[0][0],
        &c.tables.table[0][0] + grademath::kCurveTableSize * kGradeCurveCount, rows);
    std::copy(c.alpha.begin(), c.alpha.end(),
        rows + grademath::kCurveTableSize * kGradeCurveCount);
    gl->glBindTexture(GL_TEXTURE_2D, m_curveTex);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, grademath::kCurveTableSize, kCurveRows, GL_RED,
        GL_FLOAT, rows);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    PooledTarget out = kit.acquire(in.size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    gl->glUseProgram(m_curves.id);
    kit.setFullscreen(m_curves);
    gl->glUniform1i(m_cvMask, c.tables.mask());
    gl->glUniform1i(m_cvAlpha, c.alphaOn ? 1 : 0);
    kit.bindTexture(0, in.texture);
    kit.bindTexture(1, m_curveTex);
    kit.drawQuad();
    return out;
}

PooledTarget GlCompColor::customTool(GlKit& kit, const RenderTarget& in, const RenderTarget* second,
    const compmath::CustomToolParams& c, const ParamSet& params, double time, QSizeF frame)
{
    GlFunctions* gl = kit.gl();
    const QString body = compmath::customToolGlsl(c, params);
    std::shared_ptr<Custom> custom = m_custom.value(body);
    if (!custom) {
        if (m_custom.size() >= 24) { // formulas typed while editing: keep the cache small
            for (auto& old : m_custom)
                old->program.destroy(gl);
            m_custom.clear();
        }
        custom = std::make_shared<Custom>();
        QString why;
        const QByteArray source = customSource(body);
        if (!custom->program.build(gl, "compcolor.custom", source.constData(), &why)) {
            qCWarning(lcRender).noquote() << "Custom Tool shader failed:" << why;
            return {};
        }
        gl->glUseProgram(custom->program.id);
        gl->glUniform1i(custom->program.uniform(gl, "uTex"), 0);
        gl->glUniform1i(custom->program.uniform(gl, "uSecond"), 1);
        gl->glUseProgram(0);
        custom->n = custom->program.uniform(gl, "uN");
        custom->info = custom->program.uniform(gl, "uInfo");
        m_custom.insert(body, custom);
    }
    PooledTarget out = kit.acquire(in.size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    gl->glUseProgram(custom->program.id);
    kit.setFullscreen(custom->program);
    float n[compmath::kCustomNumbers];
    for (int i = 0; i < compmath::kCustomNumbers; ++i)
        n[i] = float(c.n[i]);
    gl->glUniform1fv(custom->n, compmath::kCustomNumbers, n);
    gl->glUniform4f(custom->info, float(time), float(frame.width()), float(frame.height()),
        second ? 1.0f : 0.0f);
    kit.bindTexture(0, in.texture);
    kit.bindTexture(1, second ? second->texture : in.texture);
    kit.drawQuad();
    return out;
}

} // namespace kk::render
