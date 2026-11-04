// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Image.hpp>
#include <vtbackend/graphics/regis/ReGISContext.hpp>
#include <vtbackend/graphics/regis/ReGISRasterizer.hpp>

namespace vtbackend::regis
{

/// Everything a screen's ReGIS interpreter keeps between DCS strings: the interpreter context, and the
/// canvas the strings draw on.
///
/// The text rasterizer is not part of it: it holds no per-session state (it is the embedded font, or the
/// display's font engine, injected by the frontend).
struct ReGISState
{
    ReGISContext context {};   ///< The interpreter's position, colours, write controls and macrographs.
    ImageSize canvasSize {};   ///< The canvas size in pixels.
    Image::Data pixels {};     ///< The canvas, RGBA, `canvasSize.area() * 4` bytes.
    unsigned patternPhase = 0; ///< The rasterizer's line-pattern phase.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.context);
        visitor(2, self.canvasSize);
        visitor(3, self.pixels);
        visitor(4, self.patternPhase);
    }

    /// @param rasterizer The live canvas.
    /// @param context    The live interpreter context.
    /// @return What both hold now.
    [[nodiscard]] static ReGISState capture(ReGISContext const& context, ReGISRasterizer const& rasterizer)
    {
        return ReGISState { .context = context,
                            .canvasSize = rasterizer.size(),
                            .pixels = rasterizer.data(),
                            .patternPhase = rasterizer.patternPhase() };
    }

    /// @return Whether the pixel buffer is the size the canvas size says; a state that fails this cannot be
    ///         restored.
    [[nodiscard]] bool isConsistent() const noexcept { return pixels.size() == canvasSize.area() * 4; }
};

} // namespace vtbackend::regis
