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
#pragma aonce

#include <terminal/MatchModes.h>
#include <terminal/InputGenerator.h>

#include <fmt/format.h>

namespace terminal {

template <typename Input, typename Binding>
struct InputBinding
{
    MatchModes modes;
    Modifier modifier;
    Input input;
    Binding binding;
};

template <typename Input, typename Binding>
bool match(InputBinding<Input, Binding> const& _binding,
           MatchModes _modes,
           Modifier _modifier,
           Input _input)
{
    return _binding.modes == _modes
        && _binding.modifier == _modifier
        && _binding.input == _input;
}

template <typename I, typename O>
bool operator==(InputBinding<I, O> const& a, InputBinding<I, O> const& b) noexcept
{
    return a.modes == b.modes
        && a.modifier == b.modifier
        && a.input == b.input;
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

}

namespace fmt
{
    template <typename I, typename O>
    struct formatter<terminal::InputBinding<I, O>> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::InputBinding<I, O> const& _binding, FormatContext& _ctx)
        {
            return format_to(_ctx.out(), "{} {} {}",
                    _binding.modes,
                    _binding.modifier,
                    _binding.input);
        }
    };
}
