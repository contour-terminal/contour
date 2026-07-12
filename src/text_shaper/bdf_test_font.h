// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.h>
#include <text_shaper/font_locator.h>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace text::test
{

/// A glyph to synthesize into a test font.
struct bdf_glyph
{
    char32_t codepoint {};
    int advance {}; ///< The glyph's horizontal advance (BDF's DWIDTH), in pixels.
};

/// A synthetic BDF font, built in memory and served through font_memory_ref.
///
/// Font fallback can only be tested against fonts that really exist, since FreeType has to load them and
/// HarfBuzz has to shape with them -- but depending on system fonts would make the tests depend on the
/// machine they run on. BDF is a plain-text bitmap format, so a font with exactly the glyph coverage, the
/// advances and the spacing class a test needs can simply be written out as a string.
///
/// Two properties matter here and are both directly controllable:
///   - coverage: which codepoints the font has a glyph for, and therefore which ones fall back;
///   - the advance (DWIDTH), independent of the bitmap's width, which is how a proportional font ends up
///     reporting an advance narrower than the terminal's cell.
///
/// The instance owns the font bytes; font_memory_ref only borrows them, so it must outlive the shaper.
class bdf_font
{
  public:
    /// @param identifier Unique name for this font, used as the font source's identifier.
    /// @param monospace  Whether the font declares itself fixed-width (BDF's SPACING property). Fallback
    ///                   font selection prefers fonts whose spacing matches the primary's.
    /// @param glyphs     The glyphs the font covers. Any other codepoint shapes to .notdef.
    bdf_font(std::string identifier, bool monospace, std::vector<bdf_glyph> const& glyphs):
        _identifier { std::move(identifier) }, _bytes { render(monospace, glyphs) }
    {
    }

    /// @return A source borrowing this font's bytes. Valid for as long as this object lives.
    [[nodiscard]] font_source source() noexcept
    {
        return font_memory_ref { .identifier = _identifier, .data = gsl::span<uint8_t> { _bytes } };
    }

    /// The one size this font may be loaded at. BDF is a fixed-size bitmap format, so requesting any
    /// other size fails outright.
    static constexpr auto Size = font_size { 9.0 };
    static constexpr auto Dpi = DPI { .x = 96, .y = 96 };

  private:
    [[nodiscard]] static std::vector<uint8_t> render(bool monospace, std::vector<bdf_glyph> const& glyphs)
    {
        auto const spacing = monospace ? 'C' : 'P';

        auto text = std::format("STARTFONT 2.1\n"
                                "FONT -Test-Synthetic-Medium-R-Normal--12-120-75-75-{}-80-ISO10646-1\n"
                                "SIZE 9 96 96\n"
                                "FONTBOUNDINGBOX 8 12 0 -2\n"
                                "STARTPROPERTIES 3\n"
                                "FONT_ASCENT 10\n"
                                "FONT_DESCENT 2\n"
                                "SPACING \"{}\"\n"
                                "ENDPROPERTIES\n"
                                "CHARS {}\n",
                                spacing,
                                spacing,
                                glyphs.size());

        for (auto const& glyph: glyphs)
            text += std::format("STARTCHAR U+{:04X}\n"
                                "ENCODING {}\n"
                                "SWIDTH 500 0\n"
                                "DWIDTH {} 0\n"
                                "BBX 8 12 0 -2\n"
                                "BITMAP\n"
                                "00\n00\n7E\n42\n42\n42\n42\n42\n42\n7E\n00\n00\n"
                                "ENDCHAR\n",
                                static_cast<uint32_t>(glyph.codepoint),
                                static_cast<uint32_t>(glyph.codepoint),
                                glyph.advance);

        text += "ENDFONT\n";

        return std::vector<uint8_t> { text.begin(), text.end() };
    }

    std::string _identifier;
    std::vector<uint8_t> _bytes;
};

} // namespace text::test
