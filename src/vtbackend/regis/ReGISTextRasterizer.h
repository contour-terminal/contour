// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/regis/ReGISFont.h>

#include <vtpty/ImageSize.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace vtbackend::regis
{

/// A rasterized ReGIS glyph as an alpha-coverage mask.
///
/// @c coverage holds one byte per pixel (0 = transparent, 255 = fully covered), row-major, and is
/// composited with the pen colour when the glyph is drawn. This lets a high-quality text engine
/// deliver anti-aliased glyphs without the ReGIS canvas knowing how they were produced.
struct ReGISGlyphBitmap
{
    vtpty::ImageSize size {};
    std::vector<uint8_t> coverage {};
};

/// Rasterizes ReGIS text glyphs into coverage masks.
///
/// This is the dependency-injection seam that keeps clean architecture: vtbackend defines the
/// interface and ships a self-contained bitmap-font default (so the terminal engine and its headless
/// tests need no font stack), while the display layer may inject a text_shaper-backed implementation
/// for crisp, shaped glyphs. vtbackend never depends on the font-rendering stack.
class ReGISTextRasterizer
{
  public:
    virtual ~ReGISTextRasterizer() = default;

    /// Rasterizes @p codepoint into a @p cellSize coverage mask.
    /// @return the glyph mask, or nullopt if the code point has no glyph.
    [[nodiscard]] virtual std::optional<ReGISGlyphBitmap> rasterize(char32_t codepoint,
                                                                    vtpty::ImageSize cellSize) const = 0;
};

/// The built-in ReGIS text rasterizer: scales the embedded @ref ReGISBitmapFont into an
/// anti-aliased coverage mask. This is the default used by the terminal engine and all headless
/// tests; the display layer can substitute a text_shaper-backed rasterizer.
class EmbeddedReGISTextRasterizer final: public ReGISTextRasterizer
{
  public:
    [[nodiscard]] std::optional<ReGISGlyphBitmap> rasterize(char32_t codepoint,
                                                            vtpty::ImageSize cellSize) const override;

  private:
    ReGISBitmapFont _font {};
};

} // namespace vtbackend::regis
