// SPDX-License-Identifier: GPL-3.0-or-later
// HdrConvert — what the encoder needs for Rec.2020 / PQ / HLG renders: the transfer functions,
// the SDR (BT.709 primaries, sRGB-ish gamma) to wide-gamut / HDR conversion of compositor
// frames, and the encoder parameter strings for HDR10 metadata (x265, SVT-AV1).
//
// The approach follows FFmpeg's own tagging (libx265.c / libsvtav1.c, "-color_primaries bt2020
// -color_trc smpte2084 -colorspace bt2020nc" plus ST 2086 / CTA-861.3 side data) and the
// x265 / SVT-AV1 documentation of master-display and max-cll.
//
// Limit of this first slice: the compositor delivers 8-bit SDR images, so an HDR render is an
// SDR picture placed inside the PQ / HLG container (diffuse white at 203 nits, BT.2408), not
// real HDR highlights. Keeping HDR footage HDR needs the half-float render path; see the
// TODO in VideoEncoder.cpp and docs/roadmap.md (1.13, Deliver: HDR export).
#pragma once

#include "export/ExportSettings.h"

#include <QByteArray>
#include <QRgb>

#include <array>
#include <cstdint>
#include <vector>

namespace kk::exporter {

/// Output transfer function of a conversion.
enum class HdrTransfer {
    Sdr, ///< unchanged curve (sRGB-ish), only the primaries change
    Pq, ///< SMPTE ST 2084
    Hlg ///< ARIB STD-B67
};

/// Diffuse white (reference white) in nits, BT.2408: SDR 100 % white lands here in PQ.
inline constexpr double kHdrReferenceWhiteNits = 203.0;
/// Scene-linear level (peak = 1) of HLG's reference white: signal 0.75 = 203 nits (BT.2408).
inline constexpr double kHlgReferenceWhiteScene = 0.2646;

/// PQ inverse EOTF: absolute luminance in nits (0..10000) to signal 0..1.
[[nodiscard]] double pqEncode(double nits);
/// HLG OETF: scene-linear 0..1 to signal 0..1.
[[nodiscard]] double hlgEncode(double sceneLinear);
/// sRGB decoding curve: signal 0..1 to linear light.
[[nodiscard]] double srgbDecode(double signal);
/// sRGB encoding curve: linear light to signal 0..1.
[[nodiscard]] double srgbEncode(double linear);

/// Maps 8-bit SDR frames to packed 16-bit RGB in the output colour space; swscale then makes
/// 10-bit BT.2020 non-constant-luminance YUV of it.
class SdrToHdr
{
public:
    /// @p rec2020: convert the BT.709 primaries to BT.2020 (always true for PQ / HLG).
    SdrToHdr(HdrTransfer transfer, bool rec2020);

    /// Converts one row of premultiplied-ARGB pixels (alpha ignored) to RGB48 (R, G, B order).
    void convertRow(const QRgb* in, uint16_t* out, int width) const;
    /// Converts one pixel (test hook); channels 0..255 in, 0..65535 out.
    [[nodiscard]] std::array<uint16_t, 3> convertPixel(int r, int g, int b) const;

private:
    bool m_rec2020;
    std::array<float, 256> m_linear {}; ///< 8-bit signal to linear light
    std::vector<float> m_encode; ///< linear (sqrt-indexed) to output signal 0..65535
};

/// "G(...)B(...)R(...)WP(...)L(...)" for x265's master-display (0.00002 chromaticity units,
/// 0.0001 nit luminance units), P3-D65 primaries.
[[nodiscard]] QByteArray x265MasterDisplay(const HdrMetadata& hdr);
/// Value to add to x265-params for @p video's colour tags and (PQ) HDR10 metadata; empty when
/// the render is plain Rec.709.
[[nodiscard]] QByteArray x265HdrParams(const VideoExportSettings& video);
/// Value of svtav1-params for the same; empty when the render is plain Rec.709.
[[nodiscard]] QByteArray svtAv1HdrParams(const VideoExportSettings& video);

} // namespace kk::exporter
