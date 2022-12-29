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

#include <fmt/format.h>

#include <array>
#include <limits>
#include <optional>
#include <string>

namespace terminal::rasterizer
{

/// Dectorator, to decorate a grid cell, eventually containing a character
///
/// It should be possible to render multiple decoration onto the same coordinates.
enum class Decorator
{
    /// Draws an underline
    Underline,
    /// Draws a doubly underline
    DoubleUnderline,
    /// Draws a curly underline
    CurlyUnderline,
    /// Draws a dotted underline
    DottedUnderline,
    /// Draws a dashed underline
    DashedUnderline,
    /// Draws an overline
    Overline,
    /// Draws a strike-through line
    CrossedOut,
    /// Draws a box around the glyph, this is literally the bounding box of a grid cell.
    /// This could be used for debugging.
    /// TODO: That should span the box around the whole (potentially wide) character
    Framed,
    /// Puts a circle-shape around into the cell (and ideally around the glyph)
    /// TODO: How'd that look like with double-width characters?
    Encircle,
};

std::optional<Decorator> to_decorator(std::string const& value) noexcept;

// {{{ impl
inline std::optional<Decorator> to_decorator(std::string const& value) noexcept
{
    using std::pair;
    auto constexpr mappings = std::array {
        pair { "underline", Decorator::Underline },
        pair { "dotted-underline", Decorator::DottedUnderline },
        pair { "double-underline", Decorator::DoubleUnderline },
        pair { "curly-underline", Decorator::CurlyUnderline },
        pair { "dashed-underline", Decorator::DashedUnderline },
        pair { "overline", Decorator::Overline },
        pair { "crossed-out", Decorator::CrossedOut },
        pair { "framed", Decorator::Framed },
        pair { "encircle", Decorator::Encircle },
    };

    for (auto const& mapping: mappings)
        if (mapping.first == value)
            return { mapping.second };

    return std::nullopt;
}
// }}}

} // namespace terminal::rasterizer

namespace std
{
template <>
struct numeric_limits<terminal::rasterizer::Decorator>
{
    using Decorator = terminal::rasterizer::Decorator;
    constexpr static Decorator min() noexcept { return Decorator::Underline; }
    constexpr static Decorator max() noexcept { return Decorator::Encircle; }
    constexpr static size_t count() noexcept
    {
        return static_cast<size_t>(max()) - static_cast<size_t>(min()) + 1;
    }
};
} // namespace std

namespace fmt
{
template <>
struct formatter<terminal::rasterizer::Decorator>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::rasterizer::Decorator value, FormatContext& ctx)
    {
        auto constexpr mappings = std::array {
            "underline", "double-underline", "curly-underline", "dotted-underline", "dashed-underline",
            "overline",  "crossed-out",      "framed",          "encircle",
        };
        return fmt::format_to(ctx.out(), "{}", mappings.at(static_cast<size_t>(value)));
    }
};
} // namespace fmt
