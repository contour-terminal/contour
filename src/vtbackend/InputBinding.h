// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/InputGenerator.h>
#include <vtbackend/MatchModes.h>

#include <format>

namespace vtbackend
{

template <typename Input, typename Binding>
struct InputBinding
{
    MatchModes modes;
    Modifiers modifiers;
    Input input;
    Binding binding;
};

template <typename Input, typename Binding>
bool match(InputBinding<Input, Binding> const& binding, MatchModes modes, Modifiers modifiers, Input input)
{
    return binding.modes == modes && binding.modifiers == modifiers && binding.input == input;
}

template <typename I, typename O>
bool operator==(InputBinding<I, O> const& a, InputBinding<I, O> const& b) noexcept
{
    return a.modes == b.modes && a.modifiers == b.modifiers && a.input == b.input;
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

    if (a.modifiers < b.modifiers)
        return true;
    if (a.modifiers != b.modifiers)
        return false;

    if (a.input < b.input)
        return true;

    return false;
}

} // namespace vtbackend

template <typename I, typename O>
struct std::formatter<vtbackend::InputBinding<I, O>>
{
    auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    auto format(vtbackend::InputBinding<I, O> const& binding, auto& ctx) const
    {
        return std::format_to(ctx.out(), "{} {} {}", binding.modes, binding.modifiers, binding.input);
    }
};
