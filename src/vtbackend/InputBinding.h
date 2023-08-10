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

#include <vtbackend/InputGenerator.h>
#include <vtbackend/MatchModes.h>

#include <fmt/format.h>

namespace terminal
{

template <typename Input, typename Binding>
struct InputBinding
{
    MatchModes modes;
    modifier modifier;
    Input input;
    Binding binding;
};

template <typename Input, typename Binding>
bool match(InputBinding<Input, Binding> const& binding, MatchModes modes, modifier modifier, Input input)
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
