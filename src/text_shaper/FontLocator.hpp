// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/Font.hpp>

#include <gsl/span>
#include <gsl/span_ext>

#include <format>
#include <optional>
#include <variant>
#include <vector>

namespace text
{

/// Holds the system path to a font file.
struct FontPath
{
    std::string value;

    // in case the font file this path points to is a collection file (e.g. TTC), then this index
    // can be used to mandate which font within this TTC is to be used.
    int collectionIndex = 0;

    std::optional<FontWeight> weight = std::nullopt;
    std::optional<FontSlant> slant = std::nullopt;
};

/// Holds a view into the contents of a font file.
struct FontMemoryRef
{
    std::string identifier;  //!< a unique identifier for this font
    gsl::span<uint8_t> data; //!< font file contents (non-owned)
};

/// Represents a font source (such as file path or memory).
using FontSource = std::variant<FontPath, FontMemoryRef>;

/// Holds a list of fonts.
using FontSourceList = std::vector<FontSource>;

/**
 * Font location API.
 *
 * Used for locating fonts and fallback fonts to be used
 * for text shaping and glyph rendering.
 */
class FontLocator
{
  public:
    virtual ~FontLocator() = default;

    /**
     * Enumerates all available fonts.
     */
    [[nodiscard]] virtual FontSourceList all() = 0;

    /**
     * Locates the font matching the given description the best
     * and an ordered list of fallback fonts.
     */
    [[nodiscard]] virtual FontSourceList locate(FontDescription const& description) = 0;

    /**
     * Resolves the given codepoint sequence into an ordered list of
     * possible fonts that can be used for text shaping the given
     * codepoint sequence.
     */
    [[nodiscard]] virtual FontSourceList resolve(gsl::span<char32_t const> codepoints) = 0;
};

} // namespace text

template <>
struct std::formatter<text::FontPath>: std::formatter<std::string>
{
    auto format(text::FontPath spec, auto& ctx) const
    {
        auto weightMod = spec.weight ? std::format(" {}", spec.weight.value()) : "";
        auto slantMod = spec.slant ? std::format(" {}", spec.slant.value()) : "";
        return formatter<std::string>::format(std::format("path {}{}{}", spec.value, weightMod, slantMod),
                                              ctx);
    }
};

template <>
struct std::formatter<text::FontMemoryRef>: std::formatter<std::string>
{
    auto format(text::FontMemoryRef ref, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("in-memory: {}", ref.identifier), ctx);
    }
};

template <>
struct std::formatter<text::FontSource>: std::formatter<std::string>
{
    auto format(text::FontSource source, auto& ctx) const
    {
        std::string text;
        if (std::holds_alternative<text::FontPath>(source))
            text = std::format("{}", std::get<text::FontPath>(source));
        else if (std::holds_alternative<text::FontMemoryRef>(source))
            text = std::format("{}", std::get<text::FontMemoryRef>(source));
        return formatter<std::string>::format(text, ctx);
    }
};
