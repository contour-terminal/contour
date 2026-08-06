// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace vtbackend::pointer_shape
{

/// What an `OSC 22` request does to the shape stack.
enum class Operation : char
{
    Set = '=',   ///< Replace the current shape.
    Push = '>',  ///< Push a new shape, remembering the one beneath it.
    Pop = '<',   ///< Discard the current shape and reveal the one beneath.
    Query = '?', ///< Ask about names rather than change anything.
};

/// The CSS pointer names this terminal can actually display.
///
/// kitty's protocol speaks CSS names, and answers a query with `1` for a name it recognises and `0`
/// for one it does not -- so this list has to be the shapes Contour can genuinely show, not every
/// name CSS defines. Claiming a shape and then displaying a different one is worse than saying no.
inline constexpr auto SupportedNames = std::array {
    std::string_view { "default" }, ///< the ordinary arrow
    std::string_view { "text" },    ///< the I-beam used over text
    std::string_view { "pointer" }, ///< the hand used over links
    std::string_view { "none" },    ///< hidden
};

/// The shape a terminal shows when an application has not asked for anything else.
inline constexpr auto DefaultName = std::string_view { "text" };

/// @return whether @p name is a pointer shape this terminal can display.
[[nodiscard]] inline bool isSupportedName(std::string_view name) noexcept
{
    return std::ranges::find(SupportedNames, name) != SupportedNames.end();
}

} // namespace vtbackend::pointer_shape
