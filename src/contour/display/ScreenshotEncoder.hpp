// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/vt/Screenshot.hpp>

#include <vtrasterizer/GridMetrics.hpp>

#include <QtCore/QRect>
#include <QtGui/QImage>

namespace contour::display
{

/// Maps a cell region onto the pixels it occupies in a rendered frame.
///
/// Split out from @ref encodeScreenshot so the arithmetic can be checked without a frame to crop:
/// the mapping is @c GridMetrics' (margins and cell size), and getting it wrong shifts every pixel
/// screenshot by a margin without failing anything.
///
/// Device pixels, not the item-local LOGICAL coordinates @c contour::geometry::cellRectangle answers
/// in: this rectangle indexes a readback buffer, where the device-pixel ratio has not been divided out.
///
/// @param area    The region, as zero-based cell offsets with both corners inclusive.
/// @param metrics The grid the frame was rendered with.
/// @return The pixel rectangle @p area covers, in the frame's device pixels.
[[nodiscard]] QRect pixelRectOf(vtbackend::Rect area, vtrasterizer::GridMetrics const& metrics) noexcept;

/// Crops @p frame to @p area and encodes it as @p format, answering an `OSC 533` request.
///
/// @param frame   The rendered frame, as read back from the render target.
/// @param area    The region to cut out of it. @see pixelRectOf.
/// @param metrics The grid @p frame was rendered with.
/// @param format  The representation to produce; must be a @ref vtbackend::screenshot::Producer::Renderer
///                format.
/// @return The capture, or the status to refuse the request with.
[[nodiscard]] vtbackend::screenshot::CaptureResult encodeScreenshot(QImage const& frame,
                                                                    vtbackend::Rect area,
                                                                    vtrasterizer::GridMetrics const& metrics,
                                                                    vtbackend::screenshot::Format format);

} // namespace contour::display
