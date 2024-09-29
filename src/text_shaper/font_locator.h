// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <format>
#include <optional>
#include <variant>
#include <vector>

namespace text
{

/// Holds the system path to a font file.
struct font_path
{
    std::string value;

    // in case the font file this path points to is a collection file (e.g. TTC), then this index
    // can be used to mandate which font within this TTC is to be used.
    int collectionIndex = 0;

    std::optional<font_weight> weight = std::nullopt;
    std::optional<font_slant> slant = std::nullopt;
};

/// Holds a view into the contents of a font file.
struct font_memory_ref
{
    std::string identifier;  //!< a unique identifier for this font
    gsl::span<uint8_t> data; //!< font file contents (non-owned)
};

/// Represents a font source (such as file path or memory).
using font_source = std::variant<font_path, font_memory_ref>;

/// Holds a list of fonts.
using font_source_list = std::vector<font_source>;

/**
 * Font location API.
 *
 * Used for locating fonts and fallback fonts to be used
 * for text shaping and glyph rendering.
 */
class font_locator
{
  public:
    virtual ~font_locator() = default;

    /**
     * Enumerates all available fonts.
     */
    [[nodiscard]] virtual font_source_list all() = 0;

    /**
     * Locates the font matching the given description the best
     * and an ordered list of fallback fonts.
     */
    [[nodiscard]] virtual font_source_list locate(font_description const& description) = 0;

    /**
     * Resolves the given codepoint sequence into an ordered list of
     * possible fonts that can be used for text shaping the given
     * codepoint sequence.
     */
    [[nodiscard]] virtual font_source_list resolve(gsl::span<const char32_t> codepoints) = 0;
};

} // namespace text

template <>
struct std::formatter<text::font_path>: std::formatter<std::string>
{
    auto format(text::font_path spec, auto& ctx) const
    {
        auto weightMod = spec.weight ? std::format(" {}", spec.weight.value()) : "";
        auto slantMod = spec.slant ? std::format(" {}", spec.slant.value()) : "";
        return formatter<std::string>::format(std::format("path {}{}{}", spec.value, weightMod, slantMod),
                                              ctx);
    }
};

template <>
struct std::formatter<text::font_memory_ref>: std::formatter<std::string>
{
    auto format(text::font_memory_ref ref, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("in-memory: {}", ref.identifier), ctx);
    }
};

template <>
struct std::formatter<text::font_source>: std::formatter<std::string>
{
    auto format(text::font_source source, auto& ctx) const
    {
        std::string text;
        if (std::holds_alternative<text::font_path>(source))
            text = std::format("{}", std::get<text::font_path>(source));
        else if (std::holds_alternative<text::font_memory_ref>(source))
            text = std::format("{}", std::get<text::font_memory_ref>(source));
        return formatter<std::string>::format(text, ctx);
    }
};
