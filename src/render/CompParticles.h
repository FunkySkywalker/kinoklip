// SPDX-License-Identifier: GPL-3.0-or-later
// CompParticles — the particle nodes of the Compositing page (pEmitter, pGravity, pTurbulence,
// pMerge, pRender; spec 129). Pure functions of the composition, the frame and the comp's aspect:
// the same seed and frame always give the same particles, and no state is kept between frames, so
// scrubbing to a frame equals playing up to it. The GPU and the CPU renderer both call
// renderNode(), so they draw the same pixels.
#pragma once

#include "core/Composition.h"

#include <QImage>
#include <QSize>
#include <QVector>

#include <cstdint>
#include <functional>

namespace kk::render::particles {

/// Style of a drawn particle.
enum class Style : quint8 { Point, Blob, Bitmap };

/// One live particle. Positions are in "width units": x 0..1 from the left of the frame, y from
/// the BOTTOM, 1 = the frame width (so the frame is 1 wide and aspect = height / width high).
struct Particle
{
    float x = 0;
    float y = 0;
    float size = 0; ///< diameter, width units
    float rotation = 0; ///< degrees counter-clockwise
    float r = 1, g = 1, b = 1; ///< straight colour, 0..1
    float opacity = 1;
    quint64 emitter = 0; ///< node id of the pEmitter (its Sprite input for the Bitmap style)
    Style style = Style::Blob;
};

/// One route a particle takes from its pEmitter to the node that reads the stream: the forces it
/// passes, in the order they act (the emitter's side first).
struct Path
{
    quint64 emitter = 0;
    QVector<quint64> forces;
};

/// The routes of the stream that node @p stream outputs (a pEmitter, a force or a pMerge). A
/// disabled force or merge passes its first input through; a disabled emitter makes nothing.
[[nodiscard]] QVector<Path> paths(const Composition& comp, quint64 stream);

/// The particles alive at @p frame on @p paths, oldest first; @p aspect = height / width of the
/// picture. Each particle was created at its own frame with the emitter's values of that frame
/// and moved forward one frame at a time by the forces of every frame it lived through.
[[nodiscard]] QVector<Particle> simulate(
    const Composition& comp, const QVector<Path>& paths, int64_t frame, double aspect);

/// simulate(paths(stream)).
[[nodiscard]] QVector<Particle> streamAt(
    const Composition& comp, quint64 stream, int64_t frame, double aspect);

/// The picture an emitter shows as the Sprite of a Bitmap particle (any size; its visible part is used).
using SpriteFn = std::function<QImage(quint64 emitter)>;

/// @p particles drawn over transparent black: premultiplied ARGB32 of @p size.
[[nodiscard]] QImage draw(const QVector<Particle>& particles, QSize size, const SpriteFn& sprite);

/// The picture of pRender node @p render at @p frame: its Input stream drawn at @p size. Empty
/// transparent when nothing is connected.
[[nodiscard]] QImage renderNode(const Composition& comp, const CompNode& render, int64_t frame,
    QSize size, const SpriteFn& sprite);

/// The pEmitters whose particles reach pRender node @p render (for its Sprite pictures).
[[nodiscard]] QVector<quint64> emittersOf(const Composition& comp, const CompNode& render);

/// True for the node types that output a particle stream.
[[nodiscard]] bool isStreamNode(const QString& type);

} // namespace kk::render::particles
