// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>

#include <vtbackend/input/MatchModes.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace contour::config
{

/// Whether two mode filters can both hold for one terminal state.
///
/// @param a The first filter.
/// @param b The second filter.
/// @return False iff some flag is required on by one filter and required off by the other.
[[nodiscard]] constexpr bool modesOverlap(vtbackend::MatchModes a, vtbackend::MatchModes b) noexcept
{
    return (a.enabled() & b.disabled()) == 0 && (a.disabled() & b.enabled()) == 0;
}

/// Which table of InputMappings a collision was found in.
enum class BindingKind : uint8_t
{
    Key = 0,
    Char,
    Mouse,
};

/// Two bindings that can fire for one key press.
struct BindingCollision
{
    BindingKind kind = BindingKind::Key; ///< The table both bindings are in.
    std::size_t first = 0;               ///< Index of the earlier binding in that table.
    std::size_t second = 0;              ///< Index of the later binding in that table.
    std::string chord {};                ///< The shared chord, spelled as std::format spells it.
};

namespace detail
{
    /// Spells a binding's input for a failure message.
    template <typename Input>
    [[nodiscard]] std::string spellInput(Input const& input)
    {
        if constexpr (std::is_same_v<Input, char32_t>)
        {
            if (input > U' ' && input < U'\x7F')
                return std::format("{}", static_cast<char>(input));
            return std::format("U+{:04X}", static_cast<uint32_t>(input));
        }
        else
            return std::format("{}", input);
    }

    template <typename Input>
    void collectCollisions(std::span<vtbackend::InputBinding<Input, ActionList> const> bindings,
                           BindingKind kind,
                           std::vector<BindingCollision>& out)
    {
        for (auto const [i, a]: std::views::enumerate(bindings))
            for (auto const [j, b]: std::views::enumerate(bindings | std::views::drop(i + 1)))
                if (a.input == b.input && a.modifiers == b.modifiers && a.clickCount == b.clickCount
                    && modesOverlap(a.modes, b.modes))
                    out.push_back(
                        BindingCollision { .kind = kind,
                                           .first = static_cast<std::size_t>(i),
                                           .second = static_cast<std::size_t>(i + 1 + j),
                                           .chord = std::format("{} {}{}",
                                                                a.modifiers,
                                                                spellInput(a.input),
                                                                vtbackend::clickCountSuffix(a.clickCount)) });
    }
} // namespace detail

/// Finds every pair of bindings, in one table, that can fire for the same key press: the same input,
/// modifiers and click count, and mode filters that overlap.
///
/// Key, character and mouse inputs are different inputs, so only pairs inside one table are compared.
///
/// @param mappings The bindings to check.
/// @return One entry per colliding pair, tables in the order key, char, mouse.
[[nodiscard]] inline std::vector<BindingCollision> findBindingCollisions(InputMappings const& mappings)
{
    auto result = std::vector<BindingCollision> {};
    detail::collectCollisions<vtbackend::Key>(mappings.keyMappings, BindingKind::Key, result);
    detail::collectCollisions<char32_t>(mappings.charMappings, BindingKind::Char, result);
    detail::collectCollisions<vtbackend::MouseButton>(mappings.mouseMappings, BindingKind::Mouse, result);
    return result;
}

} // namespace contour::config
