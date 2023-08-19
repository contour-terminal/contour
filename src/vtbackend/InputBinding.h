// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/InputGenerator.h>
#include <vtbackend/MatchModes.h>

#include <fmt/format.h>

namespace terminal
{

template <typename Input, typename Binding>
struct InputBinding
{
    MatchModes modes;
    Modifier modifier;
    Input input;
    Binding binding;
};

template <typename Input, typename Binding>
bool match(InputBinding<Input, Binding> const& binding, MatchModes modes, Modifier modifier, Input input)
{
    return binding.modes == modes && binding.modifier == modifier && binding.input == input;
}

template <typename I, typename O>
bool operator==(InputBinding<I, O> const& a, InputBinding<I, O> const& b) noexcept
{
    return a.modes == b.modes && a.modifier == b.modifier && a.input == b.input;
}

template <typename I, typename O>
bool operator!=(InputBinding<I, O> const& a, InputBinding<I, O> const& b) noexcept
{
    return !(a == b);
}

template <typename I, typename O>
bool operator<(InputBinding<I, O> const& a, InputBinding<I, O> const& b) noexcept
{
    if (a.modes < b.modes)
        return true;
    if (a.modes != b.modes)
        return false;

    if (a.modifier < b.modifier)
        return true;
    if (a.modifier != b.modifier)
        return false;

    if (a.input < b.input)
        return true;

    return false;
}

} // namespace terminal

template <typename I, typename O>
struct fmt::formatter<terminal::InputBinding<I, O>>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(terminal::InputBinding<I, O> const& binding, format_context& ctx)
        -> format_context::iterator
    {
        return fmt::format_to(ctx.out(), "{} {} {}", binding.modes, binding.modifier, binding.input);
    }
};
