// SPDX-License-Identifier: GPL-3.0-or-later
// GradeEval — a grade (core/Grade.h) evaluated at one frame for one pixel grid ("prepared"):
// the node list in evaluation order with every parameter, curve table, LUT and window
// resolved. The GPU pass (GlGrade) uploads exactly these values; the CPU path below runs the
// same maths per pixel (the reference the GPU is tested against, and the CPU fallback's
// renderer).
#pragma once

#include "core/GradeMath.h"
#include "render/CubeLut.h"

#include <QImage>

#include <memory>
#include <vector>

namespace kk::render {

/// One node of a PreparedGrade.
struct PreparedGradeNode
{
    quint64 id = 0;
    GradeNodeType type = GradeNodeType::Corrector;
    /// Indices into PreparedGrade::nodes of the inputs (-1 = the grade's input picture).
    /// Correctors: one; mixers: 1..kMaxMixerInputs.
    std::vector<int> inputs;
    // ---- Corrector ----
    bool bypassed = false; ///< disabled: output = input, layer alpha 0
    bool corrects = false; ///< primaries, curves or LUT change something (else output = input)
    bool keyed = false; ///< qualifier or windows limit the node
    /// The key (alpha) is needed although the node is identity: a Layer mixer reads it.
    bool keyNeeded = false;
    grademath::PrimaryValues primaries;
    bool primariesIdentity = true;
    std::shared_ptr<const grademath::CurveTables> curves; ///< null = identity
    std::shared_ptr<const CubeLut> lut; ///< null = none (or unreadable)
    bool tetrahedral = true;
    GradeQualifier qualifier;
    std::vector<grademath::WindowValues> windows;
    float mix = 1.0f;
    /// The Blur palette of the node (radii in pixels of the prepared grid), applied after the
    /// colour corrections to the whole picture and then limited by the key like them.
    grademath::BlurValues blur;
    bool blurs = false; ///< blur.active() and the node mix > 0
    /// The qualifier's Matte Finesse in pixels of the prepared grid; `finesses` when the
    /// qualifier is on and cleans its matte (the key then needs whole-picture buffers).
    grademath::MatteFinesse finesse;
    bool finesses = false;
};

/// A grade ready to render at one frame.
struct PreparedGrade
{
    std::vector<PreparedGradeNode> nodes; ///< evaluation order
    int output = -1; ///< index of the output node (-1 = the input passes unchanged)
    /// True if rendering changes nothing (empty / disconnected grade, every node identity).
    bool identity = true;
};

/// Prepares @p grade at @p frame (clip-local for clip grades, timeline frame for the
/// timeline grade) for pixels laid out by @p geometry. LUTs come from loadCubeLut() (cached;
/// an unreadable file is skipped with a warning).
[[nodiscard]] PreparedGrade prepareGrade(
    const ClipGrade& grade, int64_t frame, const grademath::PictureGeometry& geometry);

/// Grades straight colour @p rgb (in place) at pixel centre (@p px, @p py): the whole node
/// graph for one pixel. @p scratch holds one rgb triple + key per node (resized as needed).
void gradePixel(
    const PreparedGrade& grade, float rgb[3], float px, float py, std::vector<float>& scratch);

/// The qualifier matte of node input @p rgb (w x h x 3 straight floats): qualifierKey() per
/// pixel, then the Matte Finesse @p finesse. The Highlight view and the CPU grade use it.
[[nodiscard]] std::vector<float> qualifierMatte(const GradeQualifier& q,
    const grademath::MatteFinesse& finesse, const std::vector<float>& rgb, int w, int h);

/// Gaussian blur of the premultiplied RGBA float image @p src (width @p w, height @p h, four
/// floats per pixel, edges clamped) into @p dst: separable, @p sigmaH / @p sigmaV pixels (0 =
/// none along that axis). The GPU blur pass runs the same kernel (grademath::blurKernel()).
void gaussianBlurRgba(
    const std::vector<float>& src, std::vector<float>& dst, int w, int h, float sigmaH, float sigmaV);

/// Applies blur @p stage to straight colour @p rgb (w x h x 3 floats, in place); @p alpha (w x h,
/// 0..1) weights the blur so transparent pixels do not leak into the picture.
void applyBlurStage(const grademath::BlurStage& stage, std::vector<float>& rgb,
    const std::vector<float>& alpha, int w, int h);

/// Grades @p image (any format; converted to ARGB32_Premultiplied) in place on the CPU:
/// colour un-premultiplied per pixel, graded, premultiplied again, rounded to 8 bits. Rows are
/// split over worker threads. Transparent pixels are left as they are.
void applyGrade(const PreparedGrade& grade, QImage& image);

/// Convenience: prepareGrade() + applyGrade() with the image covering the whole picture.
void applyGrade(const ClipGrade& grade, int64_t frame, QImage& image);

} // namespace kk::render
