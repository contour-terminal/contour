/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <text_shaper/font.h>

#include <fmt/format.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <optional>
#include <variant>
#include <vector>

namespace text
{

/// Holds the system path to a font file.
struct font_path
{
    std::string value;
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

namespace fmt // {{{
{
template <>
struct formatter<text::font_path>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_path path, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "path {}", path.value);
    }
};

template <>
struct formatter<text::font_memory_ref>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_memory_ref ref, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "in-memory: {}", ref.identifier);
    }
};

template <>
struct formatter<text::font_source>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_source source, FormatContext& ctx)
    {
        if (std::holds_alternative<text::font_path>(source))
            return fmt::format_to(ctx.out(), "{}", std::get<text::font_path>(source));
        if (std::holds_alternative<text::font_memory_ref>(source))
            return fmt::format_to(ctx.out(), "{}", std::get<text::font_memory_ref>(source));
        return fmt::format_to(ctx.out(), "UNKNOWN SOURCE");
    }
};
} // namespace fmt
