// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vtrasterizer
{

/// Decorator, to decorate a grid cell, eventually containing a character
///
/// It should be possible to render multiple decoration onto the same coordinates.
enum class Decorator : uint8_t
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
    /// TODO: How would that look like with double-width characters?
    Encircle,
    /// The thin line under the text an input method is composing: one point thick, where the font's
    /// underline is. Internal -- drawn for CellFlag::InputMethodPreedit, never named in a configuration.
    PreeditUnderline,

    /// Not a decorator: how many there are. Stays last, so a new decorator is counted wherever it is added.
    ///
    /// What std::numeric_limits<Decorator> reports, and so what counts the renderer's direct-mapped tile
    /// slots: one per decorator, then the dotted underline's further phases.
    Count,
};

/// Every decorator paired with the token that names it in a configuration file.
///
/// The single source of truth for those tokens: @ref toDecorator parses with it, the
/// `std::formatter` below prints with it, and a settings UI can build its option list from it. The
/// parse table and the print table used to be written out separately -- the latter as a bare array
/// indexed by the enumerator, so reordering the enumeration silently renamed every decorator.
inline constexpr auto DecoratorNames = std::array {
    std::pair { std::string_view { "underline" }, Decorator::Underline },
    std::pair { std::string_view { "double-underline" }, Decorator::DoubleUnderline },
    std::pair { std::string_view { "curly-underline" }, Decorator::CurlyUnderline },
    std::pair { std::string_view { "dotted-underline" }, Decorator::DottedUnderline },
    std::pair { std::string_view { "dashed-underline" }, Decorator::DashedUnderline },
    std::pair { std::string_view { "overline" }, Decorator::Overline },
    std::pair { std::string_view { "crossed-out" }, Decorator::CrossedOut },
    std::pair { std::string_view { "framed" }, Decorator::Framed },
    std::pair { std::string_view { "encircle" }, Decorator::Encircle },
};

/// The decorators no configuration names, with the name the formatter prints them by.
///
/// Kept out of @ref DecoratorNames, which is the configuration's vocabulary: a row there would let a
/// hyperlink decoration ask for it, and put it in the settings page's option list.
inline constexpr auto InternalDecoratorNames = std::array {
    std::pair { std::string_view { "preedit-underline" }, Decorator::PreeditUnderline },
};

/// Further spellings of a decorator that a configuration file accepts but never prints.
///
/// `dotted` is here because the configuration documentation showed `normal: dotted` for years, so
/// files in the wild spell it that way. A row here is read by @ref toDecorator and nowhere else:
/// the formatter and the settings page's option list use @ref DecoratorNames, so an alias neither
/// changes what is written back nor appears as a second choice in the combo box.
inline constexpr auto DecoratorAliases = std::array {
    std::pair { std::string_view { "dotted" }, Decorator::DottedUnderline },
};

/// Looks a configuration token up in @ref DecoratorNames, then in @ref DecoratorAliases.
/// @param value The token, already lowercased; matching is exact.
/// @return The decorator it names, or nothing when @p value is not a known spelling.
[[nodiscard]] std::optional<Decorator> toDecorator(std::string_view value) noexcept;

// {{{ impl
inline std::optional<Decorator> toDecorator(std::string_view value) noexcept
{
    for (auto const& [token, decorator]: DecoratorNames)
        if (token == value)
            return { decorator };

    for (auto const& [alias, decorator]: DecoratorAliases)
        if (alias == value)
            return { decorator };

    return std::nullopt;
}
// }}}

} // namespace vtrasterizer

template <>
struct std::numeric_limits<vtrasterizer::Decorator>
{
    using Decorator = vtrasterizer::Decorator;
    constexpr static Decorator min() noexcept { return Decorator::Underline; }
    constexpr static Decorator max() noexcept
    {
        return static_cast<Decorator>(static_cast<size_t>(Decorator::Count) - 1);
    }
    constexpr static size_t count() noexcept { return static_cast<size_t>(Decorator::Count); }
};

namespace vtrasterizer::detail
{
/// Whether the names tables name every decorator below the count exactly once, and nothing at or past it.
///
/// Together with the size check below this makes the tables and the count agree: a decorator added with
/// no name, which Decorator::Count counts all the same, fails to compile.
[[nodiscard]] consteval bool everyDecoratorIsNamedOnce()
{
    auto constexpr Count = std::numeric_limits<Decorator>::count();
    auto seen = std::array<int, Count> {};
    auto const tally = [&](Decorator decorator) {
        auto const index = static_cast<size_t>(decorator);
        if (index >= Count)
            return false;
        ++seen[index];
        return true;
    };
    for (auto const& [token, decorator]: DecoratorNames)
        if (!tally(decorator))
            return false;
    for (auto const& [token, decorator]: InternalDecoratorNames)
        if (!tally(decorator))
            return false;
    return std::ranges::all_of(seen, [](int times) { return times == 1; });
}
} // namespace vtrasterizer::detail

static_assert(
    vtrasterizer::DecoratorNames.size() + vtrasterizer::InternalDecoratorNames.size()
        == std::numeric_limits<vtrasterizer::Decorator>::count(),
    "Every Decorator has one name, in DecoratorNames or InternalDecoratorNames: the count of direct-mapped "
    "tile slots, Decorator::Count, includes a decorator whether it is named or not.");
static_assert(vtrasterizer::detail::everyDecoratorIsNamedOnce(),
              "A Decorator is named twice, or Decorator::Count is named: each decorator needs one name.");

template <>
struct std::formatter<vtrasterizer::Decorator>: formatter<std::string_view>
{
    auto format(vtrasterizer::Decorator value, auto& ctx) const
    {
        // Looked up by value rather than indexed by the enumerator, so the table's order is free to
        // differ from the enumeration's without renaming anything.
        for (auto const& [token, decorator]: vtrasterizer::DecoratorNames)
            if (decorator == value)
                return formatter<std::string_view>::format(token, ctx);
        for (auto const& [token, decorator]: vtrasterizer::InternalDecoratorNames)
            if (decorator == value)
                return formatter<std::string_view>::format(token, ctx);
        return formatter<std::string_view>::format("underline", ctx);
    }
};
