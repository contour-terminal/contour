// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/input/MatchModes.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace vtbackend
{

template <typename Input, typename Binding>
struct InputBinding
{
    MatchModes modes;
    Modifiers modifiers;
    Input input;
    Binding binding;

    /// For a mouse button: the multi-click count (1..ClickCounter::MaxClickCount) this binding fires on.
    /// nullopt fires on any press no click-count binding claimed. Always nullopt for keys and characters.
    std::optional<uint8_t> clickCount {};
};

/// Whether @p binding is the binding for this input.
/// @param clickCount The press's multi-click count for a click-count lookup; nullopt for an ordinary one.
///                   It must equal the binding's own: a plain binding never answers a click-count lookup,
///                   nor a click-count binding an ordinary one.
template <typename Input, typename Binding>
bool match(InputBinding<Input, Binding> const& binding,
           MatchModes modes,
           Modifiers modifiers,
           Input input,
           std::optional<uint8_t> clickCount = std::nullopt)
{
    return binding.modes == modes && binding.modifiers == modifiers && binding.input == input
           && binding.clickCount == clickCount;
}

/// How a binding's click count reads after its input, e.g. " x3" for Ctrl+Left x3.
/// @param clickCount The binding's count; nullopt for an ordinary binding.
/// @return The suffix; empty for an ordinary binding.
[[nodiscard]] inline std::string clickCountSuffix(std::optional<uint8_t> clickCount)
{
    return clickCount ? std::format(" x{}", *clickCount) : std::string {};
}

template <typename I, typename O>
bool operator==(InputBinding<I, O> const& a, InputBinding<I, O> const& b) noexcept
{
    return a.modes == b.modes && a.modifiers == b.modifiers && a.input == b.input
           && a.clickCount == b.clickCount;
}

template <typename I, typename O>
bool operator!=(InputBinding<I, O> const& a, InputBinding<I, O> const& b) noexcept
{
    return !(a == b);
}

} // namespace vtbackend

template <typename I, typename O>
struct std::formatter<vtbackend::InputBinding<I, O>>
{
    constexpr auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    auto format(vtbackend::InputBinding<I, O> const& binding, auto& ctx) const
    {
        return std::format_to(ctx.out(),
                              "{} {} {}{}",
                              binding.modes,
                              binding.modifiers,
                              binding.input,
                              vtbackend::clickCountSuffix(binding.clickCount));
    }
};
