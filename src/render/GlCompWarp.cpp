// SPDX-License-Identifier: GPL-3.0-or-later
// GlCompWarp implementation: GLSL twins of render/CompWarpMath.cpp. The grid field is the
// shader of the Warper effect (GlMeshEffects.cpp).
#include "render/GlCompWarp.h"

#include "render/CompMath.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

const char* const kDisplaceFragment = R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform vec4 uA;    // x refraction px, y refraction px, offset, radial (0 / 1)
uniform ivec4 uCh;  // x channel, y channel, edges, light on
uniform vec4 uB;    // centre x, centre y (0..1, y up), light power, light angle (radians)
float chan(vec4 c, int ch)
{
    if (ch == 3) return c.a;
    vec3 s = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
    if (ch == 4) return dot(s, vec3(0.2126, 0.7152, 0.0722));
    return s[ch];
}
void main()
{
    ivec2 sz = textureSize(uTex, 0);
    ivec2 ip = ivec2(floor(vUv * vec2(sz)));
    vec2 pc = vec2(ip) + 0.5;
    vec4 m = texelFetch(uSecond, ip, 0);
    vec2 d;
    if (uA.w > 0.5) {
        vec2 r = pc - vec2(uB.x, 1.0 - uB.y) * vec2(sz);
        float len = length(r);
        vec2 dir = len > 1e-6 ? r / len : vec2(0.0);
        float amount = (chan(m, uCh.x) - uA.z) * uA.x;
        d = vec2(dir.x * amount, -dir.y * amount);
    } else {
        d = vec2((chan(m, uCh.x) - uA.z) * uA.x, (chan(m, uCh.y) - uA.z) * uA.y);
    }
    vec4 c = kkResample(uTex, (pc + vec2(-d.x, d.y)) / vec2(sz), uCh.z, 1);
    float shade = 1.0;
    if (uCh.w == 1) {
        ivec2 hi = sz - 1;
        float vl = chan(texelFetch(uSecond, clamp(ip + ivec2(-1, 0), ivec2(0), hi), 0), uCh.x);
        float vr = chan(texelFetch(uSecond, clamp(ip + ivec2(1, 0), ivec2(0), hi), 0), uCh.x);
        float vu = chan(texelFetch(uSecond, clamp(ip + ivec2(0, -1), ivec2(0), hi), 0), uCh.x);
        float vd = chan(texelFetch(uSecond, clamp(ip + ivec2(0, 1), ivec2(0), hi), 0), uCh.x);
        float slope = (vr - vl) * cos(uB.w) + (vu - vd) * sin(uB.w);
        shade = max(0.0, 1.0 + uB.z * 4.0 * slope);
    }
    fragColor = vec4(min(c.rgb * shade, vec3(c.a)), c.a);
}
)";

const char* const kVectorFragment = R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform float uF;       // range * multiplier * scale * 2
uniform int uSteps;
uniform int uEdges;
void main()
{
    ivec2 sz = textureSize(uTex, 0);
    vec2 pc = vUv * vec2(sz);
    pc = floor(pc) + 0.5;
    for (int i = 0; i < uSteps; ++i) {
        vec4 m = texture(uSecond, pc / vec2(sz));
        vec2 v = m.a > 0.0 ? (m.rg / m.a - 0.5) * uF : vec2(0.0);
        pc += vec2(-v.x, v.y) / float(uSteps);
    }
    fragColor = kkResample(uTex, pc / vec2(sz), uEdges, 1);
}
)";

const char* const kGridFragment = R"(
uniform sampler2D uTex;
uniform int uCols;
uniform int uRows;
uniform vec2 uD[81];    // displacement per node in content units
vec2 node(int c, int r)
{
    c = clamp(c, 0, uCols - 1);
    r = clamp(r, 0, uRows - 1);
    return uD[r * uCols + c];
}
vec4 catmullRom(float t)
{
    float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0,
        -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}
vec2 field(vec2 t)
{
    vec2 top = vec2(uCols - 1, uRows - 1);
    vec2 g = clamp(t * top, vec2(0.0), top);
    ivec2 i = ivec2(floor(g));
    vec2 f = g - vec2(i);
    vec4 wx = catmullRom(f.x);
    vec4 wy = catmullRom(f.y);
    vec2 sum = vec2(0.0);
    for (int j = -1; j <= 2; ++j) {
        vec2 row = vec2(0.0);
        for (int c = -1; c <= 2; ++c)
            row += wx[c + 1] * node(i.x + c, i.y + j);
        sum += wy[j + 1] * row;
    }
    return sum;
}
void main()
{
    vec2 t = vUv;
    vec2 s = t;
    for (int n = 0; n < 5; ++n)
        s = t - field(s);
    bool outside = any(lessThan(s, vec2(0.0))) || any(greaterThan(s, vec2(1.0)));
    fragColor = outside ? vec4(0.0) : texture(uTex, s);
}
)";

bool buildWith(GlKit& kit, GlProgram& program, const char* name, const QByteArray& source,
    QString* why)
{
    if (!program.build(kit.gl(), name, source.constData(), why))
        return false;
    GlFunctions* gl = kit.gl();
    gl->glUseProgram(program.id);
    gl->glUniform1i(program.uniform(gl, "uTex"), 0);
    gl->glUniform1i(program.uniform(gl, "uSecond"), 1);
    gl->glUseProgram(0);
    return true;
}

} // namespace

bool GlCompWarp::initialize(GlKit& kit, QString* why)
{
    GlFunctions* gl = kit.gl();
    const QByteArray resample(compmath::kResampleGlsl);
    if (!buildWith(kit, m_displace, "compwarp.displace", resample + kDisplaceFragment, why)
        || !buildWith(kit, m_vector, "compwarp.vector", resample + kVectorFragment, why)
        || !buildWith(kit, m_grid, "compwarp.grid", kGridFragment, why))
        return false;
    m_dA = m_displace.uniform(gl, "uA");
    m_dCh = m_displace.uniform(gl, "uCh");
    m_dB = m_displace.uniform(gl, "uB");
    m_vF = m_vector.uniform(gl, "uF");
    m_vSteps = m_vector.uniform(gl, "uSteps");
    m_vEdges = m_vector.uniform(gl, "uEdges");
    m_gCols = m_grid.uniform(gl, "uCols");
    m_gRows = m_grid.uniform(gl, "uRows");
    m_gD = m_grid.uniform(gl, "uD");
    return true;
}

void GlCompWarp::destroy(GlKit& kit)
{
    for (GlProgram* p : {&m_displace, &m_vector, &m_grid})
        p->destroy(kit.gl());
}

PooledTarget GlCompWarp::displace(GlKit& kit, const RenderTarget& in, const RenderTarget& map,
    const compmath::DisplaceParams& d, double scale)
{
    GlFunctions* gl = kit.gl();
    PooledTarget out = kit.acquire(in.size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    gl->glUseProgram(m_displace.id);
    kit.setFullscreen(m_displace);
    gl->glUniform4f(m_dA, float(d.xRefraction * scale), float(d.yRefraction * scale), d.offset,
        d.radial ? 1.0f : 0.0f);
    gl->glUniform4i(m_dCh, d.xChannel, d.yChannel, d.edges, d.lightPower != 0.0f ? 1 : 0);
    gl->glUniform4f(m_dB, d.centre[0], d.centre[1], d.lightPower,
        float(d.lightAngle * std::numbers::pi / 180.0));
    kit.bindTexture(0, in.texture);
    kit.bindTexture(1, map.texture);
    kit.drawQuad();
    return out;
}

PooledTarget GlCompWarp::vectorDistort(GlKit& kit, const RenderTarget& in, const RenderTarget& map,
    const compmath::VectorDistortParams& v, double scale)
{
    GlFunctions* gl = kit.gl();
    PooledTarget out = kit.acquire(in.size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    gl->glUseProgram(m_vector.id);
    kit.setFullscreen(m_vector);
    gl->glUniform1f(m_vF, float(double(v.range) * v.multiplier * scale * 2.0));
    gl->glUniform1i(m_vSteps, v.steps);
    gl->glUniform1i(m_vEdges, v.edges);
    kit.bindTexture(0, in.texture);
    kit.bindTexture(1, map.texture);
    kit.drawQuad();
    return out;
}

PooledTarget GlCompWarp::gridWarp(
    GlKit& kit, const RenderTarget& in, const compmath::GridWarpParams& g)
{
    if (g.cols < 2 || g.rows < 2 || g.isIdentity())
        return {};
    GlFunctions* gl = kit.gl();
    PooledTarget out = kit.acquire(in.size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    gl->glUseProgram(m_grid.id);
    kit.setFullscreen(m_grid);
    gl->glUniform1i(m_gCols, g.cols);
    gl->glUniform1i(m_gRows, g.rows);
    gl->glUniform2fv(m_gD, g.cols * g.rows, g.d.data());
    kit.bindTexture(0, in.texture);
    kit.drawQuad();
    return out;
}

} // namespace kk::render
