// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.hpp>
#include <text_shaper/shaper.hpp>

#include <memory>

namespace text
{

class FontLocator;

/**
 * Text shaping and rendering engine using open source technologies,
 * fontconfig + harfbuzz + freetype.
 */
class OpenShaper: public Shaper
{
  public:
    explicit OpenShaper(DPI dpi, FontLocator& locator);

    void set_dpi(DPI dpi) override;

    void set_locator(FontLocator& locator) override;

    void clear_cache() override;

    void set_font_fallback_limit(int limit) override;

    /// Sets how many distinct sizes resize_font() may open a face at before it refuses to open more.
    ///
    /// Defaults to a bound generous enough that no real document reaches it; exists so that a test
    /// can drive the refusal without paying for hundreds of real font loads to get there.
    void set_resized_font_limit(size_t limit);

    [[nodiscard]] std::optional<FontKey> load_font(FontDescription const& description,
                                                   FontSize size) override;

    [[nodiscard]] FontMetrics metrics(FontKey key) const override;
    [[nodiscard]] FontKey resize_font(FontKey key, FontSize size) override;

    void shape(FontKey font,
               std::u32string_view codepoints,
               gsl::span<unsigned> clusters,
               unicode::Script script,
               unicode::PresentationStyle presentation,
               ShapeResult& result) override;

    [[nodiscard]] std::optional<GlyphPosition> shape(FontKey font, char32_t codepoint) override;

    [[nodiscard]] std::optional<RasterizedGlyph> rasterize(GlyphKey glyph,
                                                           RenderMode mode,
                                                           float outlineThickness = 0.0f) override;

  private:
    struct PrivateOpenShaper;
    std::unique_ptr<PrivateOpenShaper, void (*)(PrivateOpenShaper*)> _d;
};

} // namespace text
