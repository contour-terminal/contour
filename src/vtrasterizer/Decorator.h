// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <fmt/format.h>

#include <array>
#include <limits>
#include <optional>
#include <string>

namespace vtrasterizer
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
    auto constexpr Mappings = std::array {
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

    for (auto const& mapping: Mappings)
        if (mapping.first == value)
            return { mapping.second };

    return std::nullopt;
}
// }}}

} // namespace vtrasterizer

template <>
struct std::numeric_limits<vtrasterizer::Decorator>
{
    using Decorator = vtrasterizer::Decorator;
    constexpr static Decorator min() noexcept { return Decorator::Underline; }
    constexpr static Decorator max() noexcept { return Decorator::Encircle; }
    constexpr static size_t count() noexcept
    {
        return static_cast<size_t>(max()) - static_cast<size_t>(min()) + 1;
    }
};

template <>
struct fmt::formatter<vtrasterizer::Decorator>: formatter<std::string_view>
{
    auto format(vtrasterizer::Decorator value, format_context& ctx) -> format_context::iterator
    {
        auto constexpr Mappings = std::array {
            "underline", "double-underline", "curly-underline", "dotted-underline", "dashed-underline",
            "overline",  "crossed-out",      "framed",          "encircle",
        };
        return formatter<std::string_view>::format(Mappings.at(static_cast<size_t>(value)), ctx);
    }
};
