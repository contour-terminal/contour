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

namespace terminal {

struct [[nodiscard]] WindowSize {
    int width;
	int height;
};

constexpr bool operator==(WindowSize const& _a, WindowSize const& _b) noexcept
{
    return _a.width == _b.width && _a.height == _b.height;
}

constexpr bool operator!=(WindowSize const& _a, WindowSize const& _b) noexcept
{
    return !(_a == _b);
}

}

namespace fmt {
    template <>
    struct formatter<terminal::WindowSize> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const terminal::WindowSize& value, FormatContext& ctx)
        {
            return format_to(ctx.out(), "({}, {})", value.width, value.height);
        }
    };
}
