// SPDX-License-Identifier: GPL-3.0-or-later
// GlGrade — GPU evaluation of a prepared grade. Every GLSL function below has a C++ twin in
// core/GradeMath.cpp (primaries, curves, HSL, qualifier, windows) or render/GradeEval.cpp /
// render/CubeLut.cpp (node mixing, LUT sampling); test_graderender compares the two paths.
#include "render/GlGrade.h"

#include <QString>

#include <algorithm>
#include <string>

namespace kk::render {

namespace {

using namespace grademath;

/// Texture units of the corrector program.
constexpr int kUnitInput = 0;
constexpr int kUnitCurves = 1;
constexpr int kUnitLut3d = 2;
constexpr int kUnitLut1d = 3;
constexpr int kUnitPoints = 4;
constexpr int kUnitMatte = 5;

const std::string kCorrectorSource = std::string(R"(
uniform sampler2D uInput;
uniform sampler2D uCurves;
uniform sampler3D uLut3d;
uniform sampler2D uLut1d;
uniform sampler2D uPoints;
uniform sampler2D uMatte;  // the qualifier matte after Matte Finesse (when uUseMatte)
uniform int uUseMatte;
uniform int uMode;        // 0 = graded picture, 1 = the key (alpha for a Layer mixer), 2 = ungated,
                          // 3 = the raw qualifier matte
uniform int uCorrects;
uniform int uPrimaries;
uniform vec4 uLift, uGamma, uGain, uOffset;
uniform float uContrast, uPivot, uSaturation, uTemperature, uTint, uLumMix;
uniform mat3 uHue;
uniform int uSoftClipOn;
uniform vec4 uSoftClip;   // low, low soft, high, high soft
uniform int uLogOn;       // Log palette: 1 when any wheel is off neutral
uniform vec4 uLogS, uLogM, uLogH, uLogO;   // Shadow, Midtone, Highlight, Offset wheels (rgb, master)
uniform vec2 uLogRange;   // low, high boundary
uniform vec2 uShadHi;     // Shad, Hi (fractions)
uniform float uBoost;     // Color Boost (fraction)
uniform int uMixer;       // RGB Mixer: 0 off, else bit 0 on, bit 1 Monochrome, bit 2 Preserve Luminance
uniform mat3 uMixM;       // row-major on the CPU (out = M * in)
uniform vec3 uMixGrey;
uniform int uAgx;         // AgX: 0 off, 1 on
uniform int uAgxIn, uAgxLook, uAgxOut;
uniform float uAgxExp;    // stops
uniform int uCurveMask;
uniform int uLutMode;     // 0 none, 1 1D, 2 3D trilinear, 3 3D tetrahedral
uniform float uLutN;
uniform vec3 uLutMin, uLutMax;
uniform int uKeyed;
uniform float uMix;
uniform int uQual, uQualInvert, uQualUse;   // use: 1 hue, 2 sat, 4 lum, 8 red, 16 green, 32 blue
uniform int uQualMode;    // 0 HSL, 1 RGB, 2 Luma, 3 3D
uniform vec3 uQualR, uQualG, uQualB;   // low, high, softness
uniform int uQ3Count;
uniform vec4 uQ3[32];     // picked colour, subtract flag
uniform vec2 uQ3Range;    // range, softness
uniform vec3 uQualHue;    // centre, half width, softness (turns)
uniform vec3 uQualSat;    // low, high, softness
uniform vec3 uQualLum;
uniform int uWinCount;
uniform int uWinShape[8];
uniform int uWinInvert[8];
uniform float uWinOpacity[8];
uniform vec4 uWinGeom[8];     // centre x, y, half width, half height (pixels)
uniform vec4 uWinRotSoft[8];  // cos, sin, soft in, soft out
uniform ivec2 uWinPoints[8];  // first point, count (polygon)

const vec3 kLuma = vec3()")
    + std::to_string(kLumaR) + ", " + std::to_string(kLumaG) + ", " + std::to_string(kLumaB) + R"();
const int kCurveSize = )"
    + std::to_string(kCurveTableSize) + R"(;
const float kTempRange = )"
    + std::to_string(kTemperatureRange) + R"(;
const float kTintRange = )"
    + std::to_string(kTintRange) + R"(;
const float kGammaRange = )"
    + std::to_string(kGammaRange) + R"(;
const float kMinSoft = 1e-4;
const float kLogRamp = )" + std::to_string(kLogRamp) + R"(;
const float kShadHi = )" + std::to_string(kShadHiStrength) + R"(;

float luma(vec3 c) { return dot(c, kLuma); }

float lgg(float x, float lift, float gamma, float gain)
{
    float v = max(gain * (x + lift * (1.0 - x)), 0.0);
    return pow(v, exp2(-kGammaRange * gamma));
}
vec3 lgg3(vec3 x, vec3 lift, vec3 gamma, vec3 gain)
{
    return vec3(lgg(x.r, lift.r, gamma.r, gain.r), lgg(x.g, lift.g, gamma.g, gain.g),
        lgg(x.b, lift.b, gamma.b, gain.b));
}

vec3 primaries(vec3 c)
{
    c += uOffset.rgb + uOffset.a + uLogO.rgb + uLogO.a;
    c.r *= 1.0 + kTempRange * uTemperature;
    c.b *= 1.0 - kTempRange * uTemperature;
    c.g *= 1.0 - kTintRange * uTint;
    if (uLogOn != 0) {
        float ly = clamp(luma(c), 0.0, 1.0);
        float wS = 1.0 - smoothstep(uLogRange.x - kLogRamp, uLogRange.x + kLogRamp, ly);
        float wH = smoothstep(uLogRange.y - kLogRamp, uLogRange.y + kLogRamp, ly);
        float wM = max(1.0 - wS - wH, 0.0);
        c += wS * (uLogS.rgb + uLogS.a) + wM * (uLogM.rgb + uLogM.a) + wH * (uLogH.rgb + uLogH.a);
    }
    c = lgg3(c, uLift.rgb, uGamma.rgb, uGain.rgb);
    float y = luma(c);
    float y2 = lgg(y, uLift.a, uGamma.a, uGain.a);
    vec3 rgbMaster = lgg3(c, vec3(uLift.a), vec3(uGamma.a), vec3(uGain.a));
    vec3 lumaMaster = c + (y2 - y);
    c = rgbMaster + (lumaMaster - rgbMaster) * uLumMix;
    c = (c - uPivot) * uContrast + uPivot;
    if (uShadHi.x != 0.0 || uShadHi.y != 0.0) {
        float sy = clamp(luma(c), 0.0, 1.0);
        float wShadow = 1.0 - smoothstep(0.0, 0.5, sy);
        float wHigh = smoothstep(0.5, 1.0, sy);
        vec3 x = clamp(c, 0.0, 1.0);
        c += uShadHi.x * wShadow * kShadHi * (uShadHi.x > 0.0 ? 1.0 - x : x);
        c += uShadHi.y * wHigh * kShadHi * (uShadHi.y > 0.0 ? 1.0 - x : x);
    }
    c = uHue * c;
    float y3 = luma(c);
    c = y3 + (c - y3) * uSaturation;
    if (uBoost != 0.0) {
        float mx = max(c.r, max(c.g, c.b));
        float mn = min(c.r, min(c.g, c.b));
        float s = mx > 1e-5 ? clamp((mx - mn) / mx, 0.0, 1.0) : 0.0;
        float factor = 1.0 + uBoost * (1.0 - s);
        float y4 = luma(c);
        c = y4 + (c - y4) * factor;
    }
    return c;
}

// Mirrors grademath::applyMixer (CPU); darktable channelmixer.c process_gray for Monochrome.
vec3 mixer(vec3 c)
{
    bool mono = (uMixer & 2) != 0;
    bool keep = (uMixer & 4) != 0;
    if (mono) {
        vec3 w = uMixGrey;
        float sum = w.r + w.g + w.b;
        if (keep && abs(sum) > 1e-5)
            w /= sum;
        float g = max(dot(w, c), 0.0);
        return vec3(g);
    }
    vec3 o = uMixM * c;
    if (keep) {
        float yOut = luma(o);
        if (yOut > 1e-5)
            o *= luma(c) / yOut;
    }
    return o;
}

// Mirrors grademath::applyAgx (CPU): the minimal AgX port (MIT, Missing Deadlines), constants
// from Sobotka's AgX OCIO config. The matrices are written column by column, as GLSL wants.
float agxLinear(float x)
{
    if (uAgxIn == 0)
        return pow(max(x, 0.0), 2.4);
    if (uAgxIn == 1)
        return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
    if (uAgxIn == 3) {
        float pt = 47.28711236 * (0.01 + 0.05641088) * (0.01 + 0.05641088);
        return x >= pt ? exp2((x - 0.69336945) / 0.08550479) - 0.00964052
                       : sqrt(max(x, 0.0) / 47.28711236) - 0.05641088;
    }
    if (uAgxIn == 4) {
        if (x >= 171.2102946929 / 1023.0)
            return pow(10.0, (x * 1023.0 - 420.0) / 261.5) * (0.18 + 0.01) - 0.01;
        return (x * 1023.0 - 95.0) * 0.01125 / (171.2102946929 - 95.0);
    }
    if (uAgxIn == 5) {
        if (x < 0.181)
            return (x - 0.125) / 5.6;
        return pow(10.0, (x - 0.598206) / 0.241514) - 0.00873;
    }
    if (uAgxIn == 6) {
        if (x > 5.367655 * 0.010591 + 0.092809)
            return (pow(10.0, (x - 0.385537) / 0.247190) - 0.052272) / 5.555556;
        return (x - 0.092809) / 5.367655;
    }
    return x;
}

vec3 agxCurve(vec3 x)
{
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2
        + 0.1191 * x - 0.00232;
}

vec3 agx(vec3 c)
{
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    vec3 lin = vec3(agxLinear(c.r), agxLinear(c.g), agxLinear(c.b)) * exp2(uAgxExp);
    vec3 e = clamp(log2(max(inset * lin, vec3(1e-10))), minEv, maxEv);
    e = agxCurve((e - minEv) / (maxEv - minEv));
    if (uAgxLook != 0) {
        vec3 slope = vec3(1.0);
        float power = 1.0;
        float sat = 1.0;
        if (uAgxLook == 1) {
            power = 1.35;
            sat = 1.4;
        } else if (uAgxLook == 2) {
            slope = vec3(1.0, 0.9, 0.5);
            power = 0.8;
            sat = 0.8;
        } else {
            sat = 0.0;
        }
        e = pow(max(e * slope, vec3(0.0)), vec3(power));
        float y = luma(e);
        e = vec3(y) + sat * (e - vec3(y));
    }
    vec3 o = max(outset * e, vec3(0.0));
    if (uAgxOut == 0)
        o = pow(pow(o, vec3(2.2)), vec3(1.0 / 2.4));
    return min(o, vec3(1.0));
}

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
    // 4 hue-hue, 5 hue-sat, 6 hue-lum, 7 lum-sat, 8 sat-sat
    if ((uCurveMask & 0x3f0) == 0)
        return c;
    vec3 hsl = rgbToHsl(c);
    float h = hsl.x;
    float s0 = hsl.y;
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
    if (curveOn(6) || curveOn(9)) {
        float lumFactor = 1.0;
        if (curveOn(6))
            lumFactor *= curve(6, h);
        if (curveOn(9))
            lumFactor *= curve(9, s0);
        float y2 = luma(c);
        c += y2 * (lumFactor - 1.0);
    }
    return c;
}

// Mirrors grademath::softClip.
float softClip1(float x)
{
    float wl = uSoftClip.y * 0.5;
    if (wl <= 0.0) {
        x = max(x, uSoftClip.x);
    } else if (x < uSoftClip.x + wl) {
        if (x <= uSoftClip.x - wl) {
            x = uSoftClip.x;
        } else {
            float t = (uSoftClip.x + wl) - x;
            x = (uSoftClip.x + wl) - t + t * t / (4.0 * wl);
        }
    }
    float wh = uSoftClip.w * 0.5;
    if (wh <= 0.0) {
        x = min(x, uSoftClip.z);
    } else if (x > uSoftClip.z - wh) {
        if (x >= uSoftClip.z + wh) {
            x = uSoftClip.z;
        } else {
            float t = x - (uSoftClip.z - wh);
            x = (uSoftClip.z - wh) + t - t * t / (4.0 * wh);
        }
    }
    return x;
}
vec3 softClip(vec3 c) { return vec3(softClip1(c.r), softClip1(c.g), softClip1(c.b)); }

vec3 lutAt(ivec3 i) { return texelFetch(uLut3d, i, 0).rgb; }
vec3 lut(vec3 c)
{
    int n = int(uLutN);
    vec3 t = clamp((c - uLutMin) / (uLutMax - uLutMin), 0.0, 1.0) * float(n - 1);
    if (uLutMode == 1) {
        vec3 r;
        for (int ch = 0; ch < 3; ++ch) {
            int i0 = min(int(t[ch]), n - 2);
            float f = t[ch] - float(i0);
            r[ch] = texelFetch(uLut1d, ivec2(i0, 0), 0)[ch] * (1.0 - f)
                + texelFetch(uLut1d, ivec2(i0 + 1, 0), 0)[ch] * f;
        }
        return r;
    }
    ivec3 i0 = min(ivec3(t), ivec3(n - 2));
    vec3 f = t - vec3(i0);
    if (uLutMode == 2) {
        vec3 acc = vec3(0.0);
        for (int corner = 0; corner < 8; ++corner) {
            ivec3 d = ivec3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
            vec3 w3 = mix(1.0 - f, f, vec3(d));
            acc += w3.x * w3.y * w3.z * lutAt(i0 + d);
        }
        return acc;
    }
    // Tetrahedral (CubeLut::applyTetrahedral()).
    float fr = f.r, fg = f.g, fb = f.b;
    ivec3 a, b;
    vec4 w;
    if (fr >= fg) {
        if (fg >= fb) { a = ivec3(1, 0, 0); b = ivec3(1, 1, 0); w = vec4(1.0 - fr, fr - fg, fg - fb, fb); }
        else if (fr >= fb) { a = ivec3(1, 0, 0); b = ivec3(1, 0, 1); w = vec4(1.0 - fr, fr - fb, fb - fg, fg); }
        else { a = ivec3(0, 0, 1); b = ivec3(1, 0, 1); w = vec4(1.0 - fb, fb - fr, fr - fg, fg); }
    } else {
        if (fb > fg) { a = ivec3(0, 0, 1); b = ivec3(0, 1, 1); w = vec4(1.0 - fb, fb - fg, fg - fr, fr); }
        else if (fb > fr) { a = ivec3(0, 1, 0); b = ivec3(0, 1, 1); w = vec4(1.0 - fg, fg - fb, fb - fr, fr); }
        else { a = ivec3(0, 1, 0); b = ivec3(1, 1, 0); w = vec4(1.0 - fg, fg - fr, fr - fb, fb); }
    }
    return w.x * lutAt(i0) + w.y * lutAt(i0 + a) + w.z * lutAt(i0 + b) + w.w * lutAt(i0 + ivec3(1));
}

float rangeKey(float v, vec3 r)
{
    float soft = max(r.z, kMinSoft);
    float k = 1.0;
    if (r.x > 0.0)
        k *= smoothstep(r.x - soft, r.x, v);
    if (r.y < 1.0)
        k *= 1.0 - smoothstep(r.y, r.y + soft, v);
    return k;
}
float qualifierKey(vec3 c)
{
    if (uQual == 0)
        return 1.0;
    c = clamp(c, 0.0, 1.0);
    float key = 1.0;
    if (uQualMode == 0) {
        vec3 hsl = rgbToHsl(c);
        if ((uQualUse & 1) != 0) {
            float rel = hsl.x - uQualHue.x + 0.5;
            float dist = abs(rel - floor(rel) - 0.5);
            float soft = max(uQualHue.z, kMinSoft);
            key *= 1.0 - smoothstep(uQualHue.y, uQualHue.y + soft, dist);
        }
        if ((uQualUse & 2) != 0)
            key *= rangeKey(hsl.y, uQualSat);
        if ((uQualUse & 4) != 0)
            key *= rangeKey(hsl.z, uQualLum);
    } else if (uQualMode == 2) {
        key = rangeKey(luma(c), uQualLum);
    } else if (uQualMode == 1) {
        if ((uQualUse & 8) != 0)
            key *= rangeKey(c.r, uQualR);
        if ((uQualUse & 16) != 0)
            key *= rangeKey(c.g, uQualG);
        if ((uQualUse & 32) != 0)
            key *= rangeKey(c.b, uQualB);
    } else {
        float inK = 0.0, outK = 0.0;
        for (int i = 0; i < uQ3Count; ++i) {
            float k = 1.0 - smoothstep(uQ3Range.x, uQ3Range.x + max(uQ3Range.y, kMinSoft),
                              distance(c, uQ3[i].rgb));
            if (uQ3[i].a > 0.5)
                outK = max(outK, k);
            else
                inK = max(inK, k);
        }
        key = inK * (1.0 - outK);
    }
    return uQualInvert != 0 ? 1.0 - key : key;
}

float windowCoverage(int i, vec2 p)
{
    vec2 d = p - uWinGeom[i].xy;
    float cs = uWinRotSoft[i].x, sn = uWinRotSoft[i].y;
    float lx = cs * d.x + sn * d.y;
    float ly = -sn * d.x + cs * d.y;
    float dist = 0.0;
    int shape = uWinShape[i];
    if (shape == 0) {
        float rx = max(uWinGeom[i].z, 1e-3);
        float ry = max(uWinGeom[i].w, 1e-3);
        float k = sqrt((lx / rx) * (lx / rx) + (ly / ry) * (ly / ry));
        dist = (k - 1.0) * min(rx, ry);
    } else if (shape == 1) {
        float qx = abs(lx) - uWinGeom[i].z;
        float qy = abs(ly) - uWinGeom[i].w;
        float ox = max(qx, 0.0), oy = max(qy, 0.0);
        dist = sqrt(ox * ox + oy * oy) + min(max(qx, qy), 0.0);
    } else if (shape == 3) {
        dist = ly;
    } else {
        int first = uWinPoints[i].x;
        int n = uWinPoints[i].y;
        float best = 1e30;
        bool inside = false;
        for (int k = 0, j = n - 1; k < n; j = k++) {
            vec2 A = texelFetch(uPoints, ivec2(first + j, 0), 0).xy;
            vec2 B = texelFetch(uPoints, ivec2(first + k, 0), 0).xy;
            float ex = B.x - A.x, ey = B.y - A.y;
            float wx = lx - A.x, wy = ly - A.y;
            float len2 = ex * ex + ey * ey;
            float t = len2 > 0.0 ? clamp((wx * ex + wy * ey) / len2, 0.0, 1.0) : 0.0;
            float qx = wx - ex * t, qy = wy - ey * t;
            best = min(best, qx * qx + qy * qy);
            if ((A.y > ly) != (B.y > ly) && lx < A.x + (ly - A.y) * ex / ey)
                inside = !inside;
        }
        dist = sqrt(best) * (inside ? -1.0 : 1.0);
    }
    return 1.0 - smoothstep(-uWinRotSoft[i].z, uWinRotSoft[i].w, dist);
}
float windowsMatte(vec2 p)
{
    if (uWinCount == 0)
        return 1.0;
    float m = 0.0;
    for (int i = 0; i < uWinCount; ++i) {
        float c = windowCoverage(i, p);
        if (uWinInvert[i] != 0)
            c = 1.0 - c;
        c *= uWinOpacity[i];
        m = m + c - m * c;
    }
    return m;
}

vec3 correct(vec3 c)
{
    vec3 g = c;
    if (uCorrects != 0) {
        if (uPrimaries != 0)
            g = primaries(g);
        if (uMixer != 0)
            g = mixer(g);
        if (uCurveMask != 0)
            g = curves(g);
        if (uSoftClipOn != 0)
            g = softClip(g);
        if (uLutMode != 0)
            g = lut(g);
        if (uAgx != 0)
            g = agx(g);
    }
    return g;
}

void main()
{
    vec4 src = texelFetch(uInput, ivec2(gl_FragCoord.xy), 0);
    vec3 c = src.a > 0.0 ? src.rgb / src.a : vec3(0.0);
    if (uMode == 2) {
        // The node's colour corrections on the whole picture (a blur follows, the key mixes).
        fragColor = vec4(correct(c) * src.a, src.a);
        return;
    }
    if (uMode == 3) {
        float k = qualifierKey(c);
        fragColor = vec4(k, k, k, 1.0);
        return;
    }
    float key = uMix;
    if (uKeyed != 0) {
        float q = uUseMatte != 0 ? texelFetch(uMatte, ivec2(gl_FragCoord.xy), 0).r : qualifierKey(c);
        key *= q * windowsMatte(gl_FragCoord.xy);
    }
    if (uMode == 1) {
        fragColor = vec4(key, key, key, 1.0);
        return;
    }
    vec3 g = c;
    if (key > 0.0)
        g = correct(c);
    vec3 o = c + (g - c) * key;
    fragColor = vec4(o * src.a, src.a);
}
)";

// Sampler arrays need constant indices in GLSL 3.30: eight named inputs.
const char* const kParallelSource = R"(
uniform sampler2D uIn0, uIn1, uIn2, uIn3, uIn4, uIn5, uIn6, uIn7;
uniform int uCount;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 first = texelFetch(uIn0, p, 0);
    vec3 sum = first.rgb;
    if (uCount > 1) sum += texelFetch(uIn1, p, 0).rgb;
    if (uCount > 2) sum += texelFetch(uIn2, p, 0).rgb;
    if (uCount > 3) sum += texelFetch(uIn3, p, 0).rgb;
    if (uCount > 4) sum += texelFetch(uIn4, p, 0).rgb;
    if (uCount > 5) sum += texelFetch(uIn5, p, 0).rgb;
    if (uCount > 6) sum += texelFetch(uIn6, p, 0).rgb;
    if (uCount > 7) sum += texelFetch(uIn7, p, 0).rgb;
    fragColor = vec4(sum / float(uCount), first.a);
}
)";

// One Layer mixer step: result = (1 - a) result + a in + (o - in) (grademath::layerOver()),
// in premultiplied colour (every node keeps the input's alpha, so the step is the same).
const char* const kLayerSource = R"(
uniform sampler2D uResult;
uniform sampler2D uIn;
uniform sampler2D uOut;
uniform sampler2D uKey;
uniform int uUseKey;
uniform int uOpaque;      // the top input is a mixer or the grade's input: it replaces
uniform float uConstAlpha;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 r = texelFetch(uResult, p, 0);
    vec4 o = texelFetch(uOut, p, 0);
    if (uOpaque != 0) {
        fragColor = vec4(o.rgb, r.a);
        return;
    }
    vec4 i = texelFetch(uIn, p, 0);
    float a = uUseKey != 0 ? texelFetch(uKey, p, 0).r : uConstAlpha;
    fragColor = vec4((1.0 - a) * r.rgb + a * i.rgb + (o.rgb - i.rgb), r.a);
}
)";

// Blur palette (grademath::BlurStage). One separable pass: the Gaussian of uRadius taps each
// side along uDir on the premultiplied picture (edges clamped), the same weights as
// grademath::blurKernel().
const char* const kBlurSource = R"(
uniform sampler2D uSrc;
uniform ivec2 uDir;
uniform int uRadius;
uniform float uSigma;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (uRadius <= 0) {
        fragColor = texelFetch(uSrc, p, 0);
        return;
    }
    ivec2 hi = textureSize(uSrc, 0) - ivec2(1);
    vec4 acc = vec4(0.0);
    float sum = 0.0;
    for (int k = -uRadius; k <= uRadius; ++k) {
        float w = exp(-float(k * k) / (2.0 * uSigma * uSigma));
        acc += w * texelFetch(uSrc, clamp(p + uDir * k, ivec2(0), hi), 0);
        sum += w;
    }
    fragColor = acc / sum;
}
)";

// Puts the blurred pictures back (blurCombine()): per channel c + (b - c) mix + (c - b) sharp
// coring, b the straight colour of the blurred premultiplied picture of that channel.
const std::string kBlurCombineSource = std::string(R"(
uniform sampler2D uSrc, uB0, uB1, uB2;
uniform vec3 uMix, uSharp;
uniform float uCoring;
uniform float uMidWeight;   // 1 for Midtone Detail: scale the correction by 4 y (1 - y)
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 s = texelFetch(uSrc, p, 0);
    if (s.a <= 0.0) {
        fragColor = s;
        return;
    }
    vec3 c = s.rgb / s.a;
    vec4 q0 = texelFetch(uB0, p, 0);
    vec4 q1 = texelFetch(uB1, p, 0);
    vec4 q2 = texelFetch(uB2, p, 0);
    vec3 b = vec3(q0.a > 0.0 ? q0.r / q0.a : c.r, q1.a > 0.0 ? q1.g / q1.a : c.g,
        q2.a > 0.0 ? q2.b / q2.a : c.b);
    vec3 d = c - b;
    vec3 core = uCoring > 0.0 ? smoothstep(vec3(0.0), vec3(uCoring * )") + std::to_string(kCoringRange) + R"(), abs(d)) : vec3(1.0);
    float wgt = 1.0;
    if (uMidWeight > 0.5) {
        float my = clamp(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
        wgt = 4.0 * my * (1.0 - my);
    }
    vec3 o = c + ((b - c) * uMix + d * uSharp * core) * wgt;
    fragColor = vec4(o * s.a, s.a);
}
)";

// A node with a blur: out = in + (graded - in) key.
const char* const kBlurMixSource = R"(
uniform sampler2D uIn, uGraded, uKey;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 i = texelFetch(uIn, p, 0);
    vec4 g = texelFetch(uGraded, p, 0);
    float key = texelFetch(uKey, p, 0).r;
    fragColor = vec4(i.rgb + (g.rgb - i.rgb) * key, i.a);
}
)";

// Matte Finesse (grademath::applyMatteFinesse()). The matte lives in the red channel (grey
// picture, alpha 1). Erode / dilate: min / max over 2 radius + 1 texels along uDir.
const char* const kMatteMorphSource = R"(
uniform sampler2D uSrc;
uniform ivec2 uDir;
uniform int uRadius;
uniform int uDilate;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 hi = textureSize(uSrc, 0) - ivec2(1);
    float best = uDilate != 0 ? 0.0 : 1.0;
    for (int k = -uRadius; k <= uRadius; ++k) {
        float v = texelFetch(uSrc, clamp(p + uDir * k, ivec2(0), hi), 0).r;
        best = uDilate != 0 ? max(best, v) : min(best, v);
    }
    fragColor = vec4(best, best, best, 1.0);
}
)";

// Denoise: the median of the (2 radius + 1)^2 window (at most 7 x 7), by partial selection.
const char* const kMatteMedianSource = R"(
uniform sampler2D uSrc;
uniform int uRadius;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 hi = textureSize(uSrc, 0) - ivec2(1);
    float v[49];
    int n = 0;
    for (int j = -uRadius; j <= uRadius; ++j)
        for (int i = -uRadius; i <= uRadius; ++i)
            v[n++] = texelFetch(uSrc, clamp(p + ivec2(i, j), ivec2(0), hi), 0).r;
    int mid = n / 2;
    for (int i = 0; i <= mid; ++i) {
        int m = i;
        for (int k = i + 1; k < n; ++k)
            if (v[k] < v[m])
                m = k;
        float t = v[i];
        v[i] = v[m];
        v[m] = t;
    }
    float r = v[mid];
    fragColor = vec4(r, r, r, 1.0);
}
)";

// In / Out Ratio (grademath::matteRatio()).
const char* const kMatteRatioSource = R"(
uniform sampler2D uSrc;
uniform float uRatio;
void main()
{
    float b = texelFetch(uSrc, ivec2(gl_FragCoord.xy), 0).r;
    float r = b;
    if (uRatio > 0.0) {
        float t = 0.5 * uRatio;
        r = clamp((b - t) / (1.0 - t), 0.0, 1.0);
    } else if (uRatio < 0.0) {
        float t = -0.5 * uRatio;
        r = clamp(b / (1.0 - t), 0.0, 1.0);
    }
    fragColor = vec4(r, r, r, 1.0);
}
)";

GLuint makeTexture(GlFunctions* gl, GLenum target)
{
    GLuint tex = 0;
    gl->glGenTextures(1, &tex);
    gl->glBindTexture(target, tex);
    gl->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (target == GL_TEXTURE_3D)
        gl->glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    gl->glBindTexture(target, 0);
    return tex;
}

} // namespace

bool GlGrade::initialize(GlKit& kit, QString* why)
{
    GlFunctions* gl = kit.gl();
    QString log;
    if (!m_corrector.build(gl, "grade_corrector", kCorrectorSource.c_str(), &log)
        || !m_parallel.build(gl, "grade_parallel", kParallelSource, &log)
        || !m_layer.build(gl, "grade_layer", kLayerSource, &log)
        || !m_blur.build(gl, "grade_blur", kBlurSource, &log)
        || !m_blurCombine.build(gl, "grade_blur_combine", kBlurCombineSource.c_str(), &log)
        || !m_blurMix.build(gl, "grade_blur_mix", kBlurMixSource, &log)
        || !m_matteMorph.build(gl, "grade_matte_morph", kMatteMorphSource, &log)
        || !m_matteMedian.build(gl, "grade_matte_median", kMatteMedianSource, &log)
        || !m_matteRatio.build(gl, "grade_matte_ratio", kMatteRatioSource, &log)) {
        *why = log;
        return false;
    }
    auto u = [&](const char* name) { return m_corrector.uniform(gl, name); };
    m_u.mode = u("uMode");
    m_u.corrects = u("uCorrects");
    m_u.primaries = u("uPrimaries");
    m_u.lift = u("uLift");
    m_u.gamma = u("uGamma");
    m_u.gain = u("uGain");
    m_u.offset = u("uOffset");
    m_u.contrast = u("uContrast");
    m_u.pivot = u("uPivot");
    m_u.saturation = u("uSaturation");
    m_u.temperature = u("uTemperature");
    m_u.tint = u("uTint");
    m_u.lumMix = u("uLumMix");
    m_u.hue = u("uHue");
    m_u.softClipOn = u("uSoftClipOn");
    m_u.softClip = u("uSoftClip");
    m_u.logOn = u("uLogOn");
    m_u.logS = u("uLogS");
    m_u.logM = u("uLogM");
    m_u.logH = u("uLogH");
    m_u.logO = u("uLogO");
    m_u.logRange = u("uLogRange");
    m_u.shadHi = u("uShadHi");
    m_u.boost = u("uBoost");
    m_u.mixer = u("uMixer");
    m_u.mixM = u("uMixM");
    m_u.mixGrey = u("uMixGrey");
    m_u.agx = u("uAgx");
    m_u.agxIn = u("uAgxIn");
    m_u.agxLook = u("uAgxLook");
    m_u.agxOut = u("uAgxOut");
    m_u.agxExp = u("uAgxExp");
    m_u.curveMask = u("uCurveMask");
    m_u.lutMode = u("uLutMode");
    m_u.lutN = u("uLutN");
    m_u.lutMin = u("uLutMin");
    m_u.lutMax = u("uLutMax");
    m_u.keyed = u("uKeyed");
    m_u.mix = u("uMix");
    m_u.qual = u("uQual");
    m_u.qualInvert = u("uQualInvert");
    m_u.qualUse = u("uQualUse");
    m_u.qualHue = u("uQualHue");
    m_u.qualSat = u("uQualSat");
    m_u.qualLum = u("uQualLum");
    m_u.qualMode = u("uQualMode");
    m_u.qualR = u("uQualR");
    m_u.qualG = u("uQualG");
    m_u.qualB = u("uQualB");
    m_u.q3Count = u("uQ3Count");
    m_u.q3 = u("uQ3");
    m_u.q3Range = u("uQ3Range");
    m_u.useMatte = u("uUseMatte");
    m_u.winCount = u("uWinCount");
    m_u.winShape = u("uWinShape");
    m_u.winInvert = u("uWinInvert");
    m_u.winOpacity = u("uWinOpacity");
    m_u.winGeom = u("uWinGeom");
    m_u.winRotSoft = u("uWinRotSoft");
    m_u.winPoints = u("uWinPoints");
    gl->glUseProgram(m_corrector.id);
    gl->glUniform1i(u("uInput"), kUnitInput);
    gl->glUniform1i(u("uCurves"), kUnitCurves);
    gl->glUniform1i(u("uLut3d"), kUnitLut3d);
    gl->glUniform1i(u("uLut1d"), kUnitLut1d);
    gl->glUniform1i(u("uPoints"), kUnitPoints);
    gl->glUniform1i(u("uMatte"), kUnitMatte);

    m_parallelCount = m_parallel.uniform(gl, "uCount");
    gl->glUseProgram(m_parallel.id);
    for (int i = 0; i < kMaxMixerInputs; ++i) {
        const QByteArray name = "uIn" + QByteArray::number(i);
        gl->glUniform1i(m_parallel.uniform(gl, name.constData()), i);
    }
    gl->glUseProgram(m_layer.id);
    gl->glUniform1i(m_layer.uniform(gl, "uResult"), 0);
    gl->glUniform1i(m_layer.uniform(gl, "uIn"), 1);
    gl->glUniform1i(m_layer.uniform(gl, "uOut"), 2);
    gl->glUniform1i(m_layer.uniform(gl, "uKey"), 3);
    m_layerConstAlpha = m_layer.uniform(gl, "uConstAlpha");
    m_layerUseKey = m_layer.uniform(gl, "uUseKey");
    m_layerOpaque = m_layer.uniform(gl, "uOpaque");
    gl->glUseProgram(m_blur.id);
    gl->glUniform1i(m_blur.uniform(gl, "uSrc"), 0);
    m_blurDir = m_blur.uniform(gl, "uDir");
    m_blurRadius = m_blur.uniform(gl, "uRadius");
    m_blurSigma = m_blur.uniform(gl, "uSigma");
    gl->glUseProgram(m_blurCombine.id);
    gl->glUniform1i(m_blurCombine.uniform(gl, "uSrc"), 0);
    gl->glUniform1i(m_blurCombine.uniform(gl, "uB0"), 1);
    gl->glUniform1i(m_blurCombine.uniform(gl, "uB1"), 2);
    gl->glUniform1i(m_blurCombine.uniform(gl, "uB2"), 3);
    m_combineMix = m_blurCombine.uniform(gl, "uMix");
    m_combineSharp = m_blurCombine.uniform(gl, "uSharp");
    m_combineCoring = m_blurCombine.uniform(gl, "uCoring");
    m_combineMid = m_blurCombine.uniform(gl, "uMidWeight");
    gl->glUseProgram(m_blurMix.id);
    gl->glUniform1i(m_blurMix.uniform(gl, "uIn"), 0);
    gl->glUniform1i(m_blurMix.uniform(gl, "uGraded"), 1);
    gl->glUniform1i(m_blurMix.uniform(gl, "uKey"), 2);
    gl->glUseProgram(m_matteMorph.id);
    gl->glUniform1i(m_matteMorph.uniform(gl, "uSrc"), 0);
    m_morphDir = m_matteMorph.uniform(gl, "uDir");
    m_morphRadius = m_matteMorph.uniform(gl, "uRadius");
    m_morphDilate = m_matteMorph.uniform(gl, "uDilate");
    gl->glUseProgram(m_matteMedian.id);
    gl->glUniform1i(m_matteMedian.uniform(gl, "uSrc"), 0);
    m_medianRadius = m_matteMedian.uniform(gl, "uRadius");
    gl->glUseProgram(m_matteRatio.id);
    gl->glUniform1i(m_matteRatio.uniform(gl, "uSrc"), 0);
    m_ratioValue = m_matteRatio.uniform(gl, "uRatio");
    gl->glUseProgram(0);

    m_curveTex = makeTexture(gl, GL_TEXTURE_2D);
    gl->glBindTexture(GL_TEXTURE_2D, m_curveTex);
    gl->glTexImage2D(
        GL_TEXTURE_2D, 0, GL_R32F, kCurveTableSize, kGradeCurveCount, 0, GL_RED, GL_FLOAT, nullptr);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    m_lut3d = makeTexture(gl, GL_TEXTURE_3D);
    m_lut1d = makeTexture(gl, GL_TEXTURE_2D);
    m_pointsTex = makeTexture(gl, GL_TEXTURE_2D);
    return true;
}

void GlGrade::destroy(GlKit& kit)
{
    GlFunctions* gl = kit.gl();
    m_corrector.destroy(gl);
    m_parallel.destroy(gl);
    m_layer.destroy(gl);
    m_blur.destroy(gl);
    m_blurCombine.destroy(gl);
    m_blurMix.destroy(gl);
    m_matteMorph.destroy(gl);
    m_matteMedian.destroy(gl);
    m_matteRatio.destroy(gl);
    for (GLuint* tex : {&m_curveTex, &m_lut3d, &m_lut1d, &m_pointsTex}) {
        if (*tex)
            gl->glDeleteTextures(1, tex);
        *tex = 0;
    }
    m_uploadedLut.reset();
    m_pointsCapacity = 0;
}

void GlGrade::uploadCurves(GlFunctions* gl, const CurveTables& tables)
{
    gl->glBindTexture(GL_TEXTURE_2D, m_curveTex);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kCurveTableSize, kGradeCurveCount, GL_RED, GL_FLOAT,
        tables.table.data());
    gl->glBindTexture(GL_TEXTURE_2D, 0);
}

void GlGrade::uploadLut(GlFunctions* gl, const std::shared_ptr<const CubeLut>& lut)
{
    if (lut == m_uploadedLut)
        return;
    const int n = lut->size();
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (lut->is3d()) {
        gl->glBindTexture(GL_TEXTURE_3D, m_lut3d);
        gl->glTexImage3D(
            GL_TEXTURE_3D, 0, GL_RGB32F, n, n, n, 0, GL_RGB, GL_FLOAT, lut->data().constData());
        gl->glBindTexture(GL_TEXTURE_3D, 0);
    } else {
        gl->glBindTexture(GL_TEXTURE_2D, m_lut1d);
        gl->glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGB32F, n, 1, 0, GL_RGB, GL_FLOAT, lut->data().constData());
        gl->glBindTexture(GL_TEXTURE_2D, 0);
    }
    m_uploadedLut = lut;
}

void GlGrade::uploadPoints(GlFunctions* gl, const PreparedGradeNode& node, std::vector<int>& ranges)
{
    ranges.assign(node.windows.size() * 2, 0);
    m_points.clear();
    for (size_t i = 0; i < node.windows.size(); ++i) {
        const WindowValues& w = node.windows[i];
        if (GradeWindowShape(w.shape) != GradeWindowShape::Polygon)
            continue;
        ranges[i * 2] = int(m_points.size() / 2);
        ranges[i * 2 + 1] = int(w.points.size() / 2);
        m_points.insert(m_points.end(), w.points.begin(), w.points.end());
    }
    if (m_points.empty())
        return;
    const int count = int(m_points.size() / 2);
    gl->glBindTexture(GL_TEXTURE_2D, m_pointsTex);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (count > m_pointsCapacity) {
        m_pointsCapacity = std::max(count, 64);
        gl->glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RG32F, m_pointsCapacity, 1, 0, GL_RG, GL_FLOAT, nullptr);
    }
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, count, 1, GL_RG, GL_FLOAT, m_points.data());
    gl->glBindTexture(GL_TEXTURE_2D, 0);
}

void GlGrade::drawCorrector(
    GlKit& kit, const PreparedGradeNode& n, GLuint input, int mode, GLuint matte)
{
    GlFunctions* gl = kit.gl();
    const PrimaryValues& p = n.primaries;
    std::vector<int> ranges;
    uploadPoints(gl, n, ranges);
    if (n.curves && (mode == 0 || mode == 2))
        uploadCurves(gl, *n.curves);
    if (n.lut && (mode == 0 || mode == 2))
        uploadLut(gl, n.lut);
    gl->glUseProgram(m_corrector.id);
    kit.setFullscreen(m_corrector);
    gl->glUniform1i(m_u.mode, mode);
    gl->glUniform1i(m_u.corrects, n.corrects ? 1 : 0);
    gl->glUniform1i(m_u.primaries, n.primariesIdentity ? 0 : 1);
    gl->glUniform4f(m_u.lift, p.lift[0], p.lift[1], p.lift[2], p.lift[3]);
    gl->glUniform4f(m_u.gamma, p.gamma[0], p.gamma[1], p.gamma[2], p.gamma[3]);
    gl->glUniform4f(m_u.gain, p.gain[0], p.gain[1], p.gain[2], p.gain[3]);
    gl->glUniform4f(m_u.offset, p.offset[0], p.offset[1], p.offset[2], p.offset[3]);
    gl->glUniform1f(m_u.contrast, p.contrast);
    gl->glUniform1f(m_u.pivot, p.pivot);
    gl->glUniform1f(m_u.saturation, p.saturation);
    gl->glUniform1f(m_u.temperature, p.temperature);
    gl->glUniform1f(m_u.tint, p.tint);
    gl->glUniform1f(m_u.lumMix, p.lumMix);
    gl->glUniformMatrix3fv(m_u.hue, 1, GL_TRUE, p.hueMatrix); // row-major on the CPU
    gl->glUniform1i(m_u.softClipOn, n.softClips ? 1 : 0);
    gl->glUniform4f(m_u.softClip, n.softClip.low, n.softClip.lowSoft, n.softClip.high,
        n.softClip.highSoft);
    gl->glUniform1i(m_u.logOn, p.logActive() ? 1 : 0);
    gl->glUniform4f(m_u.logS, p.logShadow[0], p.logShadow[1], p.logShadow[2], p.logShadow[3]);
    gl->glUniform4f(m_u.logM, p.logMid[0], p.logMid[1], p.logMid[2], p.logMid[3]);
    gl->glUniform4f(m_u.logH, p.logHigh[0], p.logHigh[1], p.logHigh[2], p.logHigh[3]);
    gl->glUniform4f(m_u.logO, p.logOffset[0], p.logOffset[1], p.logOffset[2], p.logOffset[3]);
    gl->glUniform2f(m_u.logRange, p.logLowRange, p.logHighRange);
    gl->glUniform2f(m_u.shadHi, p.shadows, p.highlights);
    gl->glUniform1f(m_u.boost, p.colorBoost);
    gl->glUniform1i(m_u.mixer,
        n.mixes ? (1 | (n.mixer.mono ? 2 : 0) | (n.mixer.preserveLum ? 4 : 0)) : 0);
    gl->glUniformMatrix3fv(m_u.mixM, 1, GL_TRUE, n.mixer.m); // row-major on the CPU
    gl->glUniform3f(m_u.mixGrey, n.mixer.grey[0], n.mixer.grey[1], n.mixer.grey[2]);
    gl->glUniform1i(m_u.agx, n.agxOn ? 1 : 0);
    gl->glUniform1i(m_u.agxIn, int(n.agx.input));
    gl->glUniform1i(m_u.agxLook, int(n.agx.look));
    gl->glUniform1i(m_u.agxOut, int(n.agx.output));
    gl->glUniform1f(m_u.agxExp, n.agx.exposure);
    gl->glUniform1i(m_u.curveMask, n.curves ? n.curves->mask() : 0);
    int lutMode = 0;
    if (n.lut)
        lutMode = !n.lut->is3d() ? 1 : (n.tetrahedral ? 3 : 2);
    gl->glUniform1i(m_u.lutMode, lutMode);
    if (n.lut) {
        gl->glUniform1f(m_u.lutN, float(n.lut->size()));
        const float* mn = n.lut->domainMin();
        const float* mx = n.lut->domainMax();
        gl->glUniform3f(m_u.lutMin, mn[0], mn[1], mn[2]);
        gl->glUniform3f(m_u.lutMax, mx[0], mx[1], mx[2]);
    }
    gl->glUniform1i(m_u.keyed, n.keyed ? 1 : 0);
    gl->glUniform1f(m_u.mix, n.mix);
    const GradeQualifier& q = n.qualifier;
    gl->glUniform1i(m_u.qual, q.enabled ? 1 : 0);
    gl->glUniform1i(m_u.qualInvert, q.invert ? 1 : 0);
    gl->glUniform1i(m_u.qualUse,
        (q.useHue ? 1 : 0) | (q.useSat ? 2 : 0) | (q.useLum ? 4 : 0) | (q.useRed ? 8 : 0)
            | (q.useGreen ? 16 : 0) | (q.useBlue ? 32 : 0));
    gl->glUniform1i(m_u.qualMode, int(q.mode));
    gl->glUniform3f(m_u.qualR, float(q.redLow), float(q.redHigh), float(q.redSoft));
    gl->glUniform3f(m_u.qualG, float(q.greenLow), float(q.greenHigh), float(q.greenSoft));
    gl->glUniform3f(m_u.qualB, float(q.blueLow), float(q.blueHigh), float(q.blueSoft));
    {
        const int colours = std::min(int(q.colors.size()), kMaxQualifierColors);
        float packed[kMaxQualifierColors * 4] = {};
        for (int i = 0; i < colours; ++i) {
            const GradeQualifierColor& c = q.colors[i];
            const float v[4] = {c.r, c.g, c.b, c.subtract ? 1.0f : 0.0f};
            std::copy(v, v + 4, packed + i * 4);
        }
        gl->glUniform1i(m_u.q3Count, colours);
        if (colours > 0)
            gl->glUniform4fv(m_u.q3, colours, packed);
        gl->glUniform2f(m_u.q3Range, float(q.colorRange), float(q.colorSoft));
    }
    gl->glUniform1i(m_u.useMatte, matte ? 1 : 0);
    gl->glUniform3f(m_u.qualHue, float(q.hueCenter), float(q.hueWidth) * 0.5f, float(q.hueSoft));
    gl->glUniform3f(m_u.qualSat, float(q.satLow), float(q.satHigh), float(q.satSoft));
    gl->glUniform3f(m_u.qualLum, float(q.lumLow), float(q.lumHigh), float(q.lumSoft));
    const int count = std::min(int(n.windows.size()), kMaxGradeWindows);
    int shape[kMaxGradeWindows] = {}, invert[kMaxGradeWindows] = {};
    float opacity[kMaxGradeWindows] = {};
    float geom[kMaxGradeWindows * 4] = {}, rotSoft[kMaxGradeWindows * 4] = {};
    int points[kMaxGradeWindows * 2] = {};
    for (int i = 0; i < count; ++i) {
        const WindowValues& w = n.windows[size_t(i)];
        shape[i] = w.shape;
        invert[i] = w.invert ? 1 : 0;
        opacity[i] = w.opacity;
        const float g[4] = {w.cx, w.cy, w.halfW, w.halfH};
        const float r[4] = {w.cosR, w.sinR, w.softIn, w.softOut};
        std::copy(g, g + 4, geom + i * 4);
        std::copy(r, r + 4, rotSoft + i * 4);
        points[i * 2] = ranges[size_t(i) * 2];
        points[i * 2 + 1] = ranges[size_t(i) * 2 + 1];
    }
    gl->glUniform1i(m_u.winCount, count);
    if (count > 0) {
        gl->glUniform1iv(m_u.winShape, count, shape);
        gl->glUniform1iv(m_u.winInvert, count, invert);
        gl->glUniform1fv(m_u.winOpacity, count, opacity);
        gl->glUniform4fv(m_u.winGeom, count, geom);
        gl->glUniform4fv(m_u.winRotSoft, count, rotSoft);
        gl->glUniform2iv(m_u.winPoints, count, points);
    }
    kit.bindTexture(kUnitInput, input);
    kit.bindTexture(kUnitCurves, m_curveTex);
    gl->glActiveTexture(GL_TEXTURE0 + kUnitLut3d);
    gl->glBindTexture(GL_TEXTURE_3D, m_lut3d);
    kit.bindTexture(kUnitLut1d, m_lut1d);
    kit.bindTexture(kUnitPoints, m_pointsTex);
    kit.bindTexture(kUnitMatte, matte ? matte : input);
    kit.setBlendOver(false);
    kit.drawQuad();
    gl->glActiveTexture(GL_TEXTURE0 + kUnitLut3d);
    gl->glBindTexture(GL_TEXTURE_3D, 0);
    gl->glActiveTexture(GL_TEXTURE0);
}

PooledTarget GlGrade::blurStage(
    GlKit& kit, GLuint source, const BlurStage& stage, const QSize& size)
{
    GlFunctions* gl = kit.gl();
    PooledTarget blurred[3];
    auto pass = [&](PooledTarget& target, GLuint from, bool horizontal, float sigma) {
        kit.bindTarget(*target);
        gl->glUseProgram(m_blur.id);
        kit.setFullscreen(m_blur);
        gl->glUniform2i(m_blurDir, horizontal ? 1 : 0, horizontal ? 0 : 1);
        gl->glUniform1i(m_blurRadius, blurTapRadius(sigma));
        gl->glUniform1f(m_blurSigma, sigma);
        kit.bindTexture(0, from);
        kit.setBlendOver(false);
        kit.drawQuad();
    };
    const int groups = stage.uniformSigma() ? 1 : 3;
    for (int g = 0; g < groups; ++g) {
        PooledTarget horizontal = kit.acquire(size);
        pass(horizontal, source, true, stage.sigmaH[g]);
        blurred[g] = kit.acquire(size);
        pass(blurred[g], horizontal->texture, false, stage.sigmaV[g]);
    }
    PooledTarget out = kit.acquire(size);
    kit.bindTarget(*out);
    gl->glUseProgram(m_blurCombine.id);
    kit.setFullscreen(m_blurCombine);
    gl->glUniform3f(m_combineMix, stage.mix[0], stage.mix[1], stage.mix[2]);
    gl->glUniform3f(m_combineSharp, stage.sharp[0], stage.sharp[1], stage.sharp[2]);
    gl->glUniform1f(m_combineCoring, stage.coring);
    gl->glUniform1f(m_combineMid, stage.midWeight ? 1.0f : 0.0f);
    kit.bindTexture(0, source);
    for (int ch = 0; ch < 3; ++ch)
        kit.bindTexture(1 + ch, blurred[groups == 1 ? 0 : ch]->texture);
    kit.setBlendOver(false);
    kit.drawQuad();
    gl->glActiveTexture(GL_TEXTURE0);
    return out;
}

PooledTarget GlGrade::matteFinesse(
    GlKit& kit, const PreparedGradeNode& n, GLuint input, const QSize& size)
{
    GlFunctions* gl = kit.gl();
    const MatteFinesse& f = n.finesse;
    PooledTarget cur = kit.acquire(size);
    kit.bindTarget(*cur);
    drawCorrector(kit, n, input, 3, 0);
    // One pass of @p program over the current matte; @p setup sets its uniforms.
    auto pass = [&](GlProgram& program, auto&& setup) {
        PooledTarget next = kit.acquire(size);
        kit.bindTarget(*next);
        gl->glUseProgram(program.id);
        kit.setFullscreen(program);
        setup();
        kit.bindTexture(0, cur->texture);
        kit.setBlendOver(false);
        kit.drawQuad();
        cur = std::move(next);
    };
    auto morph = [&](int radius, bool dilate) {
        if (radius <= 0)
            return;
        for (const bool horizontal : {true, false}) {
            pass(m_matteMorph, [&] {
                gl->glUniform2i(m_morphDir, horizontal ? 1 : 0, horizontal ? 0 : 1);
                gl->glUniform1i(m_morphRadius, radius);
                gl->glUniform1i(m_morphDilate, dilate ? 1 : 0);
            });
        }
    };
    morph(f.cleanBlack, false);
    morph(f.cleanBlack, true);
    morph(f.cleanWhite, true);
    morph(f.cleanWhite, false);
    if (f.denoise > 0)
        pass(m_matteMedian, [&] { gl->glUniform1i(m_medianRadius, f.denoise); });
    morph(std::abs(f.shrinkGrow), f.shrinkGrow > 0);
    if (f.blurSigma > 0.0f) {
        for (const bool horizontal : {true, false}) {
            pass(m_blur, [&] {
                gl->glUniform2i(m_blurDir, horizontal ? 1 : 0, horizontal ? 0 : 1);
                gl->glUniform1i(m_blurRadius, blurTapRadius(f.blurSigma));
                gl->glUniform1f(m_blurSigma, f.blurSigma);
            });
        }
    }
    if (f.ratio != 0.0f)
        pass(m_matteRatio, [&] { gl->glUniform1f(m_ratioValue, f.ratio); });
    gl->glActiveTexture(GL_TEXTURE0);
    return cur;
}

PooledTarget GlGrade::apply(GlKit& kit, const RenderTarget& input, const PreparedGrade& grade)
{
    if (grade.identity || grade.output < 0)
        return {};
    GlFunctions* gl = kit.gl();
    const QSize size = input.size;
    const size_t count = grade.nodes.size();
    // Per node: its output texture (owned target, or a texture of another node / the input)
    // and, for nodes a Layer mixer reads, the key texture or a constant alpha.
    std::vector<PooledTarget> owned(count);
    std::vector<GLuint> texture(count, 0);
    std::vector<PooledTarget> keyTargets(count);
    std::vector<float> constAlpha(count, 1.0f);
    auto textureOf = [&](int index) { return index < 0 ? input.texture : texture[size_t(index)]; };

    for (size_t i = 0; i < count; ++i) {
        const PreparedGradeNode& n = grade.nodes[i];
        switch (n.type) {
        case GradeNodeType::Corrector: {
            const GLuint in = textureOf(n.inputs[0]);
            texture[i] = in;
            if (n.bypassed) {
                constAlpha[i] = 0.0f;
                break;
            }
            constAlpha[i] = n.mix;
            PooledTarget matte; // the qualifier matte after Matte Finesse
            if (n.finesses)
                matte = matteFinesse(kit, n, in, size);
            const GLuint matteTex = matte ? matte->texture : 0;
            if ((n.keyed && n.keyNeeded) || n.blurs) {
                keyTargets[i] = kit.acquire(size);
                kit.bindTarget(*keyTargets[i]);
                drawCorrector(kit, n, in, 1, matteTex);
            }
            if (n.blurs) {
                // Colour corrections on the whole picture, the blur stages, then the key
                // mixes the result with the input.
                PooledTarget current;
                GLuint picture = in;
                if (n.corrects) {
                    current = kit.acquire(size);
                    kit.bindTarget(*current);
                    drawCorrector(kit, n, in, 2);
                    picture = current->texture;
                }
                for (const BlurStage& stage : n.blur.stages) {
                    if (!stage.active())
                        continue;
                    PooledTarget next = blurStage(kit, picture, stage, size);
                    current = std::move(next);
                    picture = current->texture;
                }
                owned[i] = kit.acquire(size);
                kit.bindTarget(*owned[i]);
                gl->glUseProgram(m_blurMix.id);
                kit.setFullscreen(m_blurMix);
                kit.bindTexture(0, in);
                kit.bindTexture(1, picture);
                kit.bindTexture(2, keyTargets[i]->texture);
                kit.setBlendOver(false);
                kit.drawQuad();
                gl->glActiveTexture(GL_TEXTURE0);
                texture[i] = owned[i]->texture;
                break;
            }
            if (!n.corrects)
                break;
            owned[i] = kit.acquire(size);
            kit.bindTarget(*owned[i]);
            drawCorrector(kit, n, in, 0, matteTex);
            texture[i] = owned[i]->texture;
            break;
        }
        case GradeNodeType::Parallel: {
            owned[i] = kit.acquire(size);
            kit.bindTarget(*owned[i]);
            gl->glUseProgram(m_parallel.id);
            kit.setFullscreen(m_parallel);
            const int inputs = std::min(int(n.inputs.size()), kMaxMixerInputs);
            gl->glUniform1i(m_parallelCount, inputs);
            for (int k = 0; k < kMaxMixerInputs; ++k)
                kit.bindTexture(k, textureOf(n.inputs[size_t(std::min(k, inputs - 1))]));
            kit.setBlendOver(false);
            kit.drawQuad();
            gl->glActiveTexture(GL_TEXTURE0);
            texture[i] = owned[i]->texture;
            break;
        }
        case GradeNodeType::Layer: {
            GLuint result = textureOf(n.inputs[0]);
            PooledTarget current;
            for (size_t k = 1; k < n.inputs.size(); ++k) {
                const int in = n.inputs[k];
                const PreparedGradeNode* top = in >= 0 ? &grade.nodes[size_t(in)] : nullptr;
                const bool corrector = top && top->type == GradeNodeType::Corrector;
                PooledTarget next = kit.acquire(size);
                kit.bindTarget(*next);
                gl->glUseProgram(m_layer.id);
                kit.setFullscreen(m_layer);
                gl->glUniform1i(m_layerOpaque, corrector ? 0 : 1);
                const bool useKey = corrector && keyTargets[size_t(in)];
                gl->glUniform1i(m_layerUseKey, useKey ? 1 : 0);
                gl->glUniform1f(m_layerConstAlpha, corrector ? constAlpha[size_t(in)] : 1.0f);
                kit.bindTexture(0, result);
                kit.bindTexture(1, corrector ? textureOf(top->inputs[0]) : textureOf(in));
                kit.bindTexture(2, textureOf(in));
                kit.bindTexture(3, useKey ? keyTargets[size_t(in)]->texture : result);
                kit.setBlendOver(false);
                kit.drawQuad();
                gl->glActiveTexture(GL_TEXTURE0);
                current = std::move(next);
                result = current->texture;
            }
            if (current) {
                owned[i] = std::move(current);
                texture[i] = owned[i]->texture;
            } else {
                texture[i] = result;
            }
            break;
        }
        }
    }
    // The output's picture lives in the target of the last node that changed it.
    const GLuint result = texture[size_t(grade.output)];
    for (PooledTarget& t : owned) {
        if (t && t->texture == result)
            return std::move(t);
    }
    return {}; // the output passes the unchanged input through
}

} // namespace kk::render
