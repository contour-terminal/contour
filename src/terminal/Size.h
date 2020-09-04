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

struct [[nodiscard]] Size {
    int width;
	int height;
};

constexpr int area(Size size) noexcept
{
    return size.width * size.height;
}

constexpr bool operator<(Size a, Size b) noexcept
{
    return area(a) < area(b);
}

constexpr bool operator==(Size const& _a, Size const& _b) noexcept
{
    return _a.width == _b.width && _a.height == _b.height;
}

constexpr bool operator!=(Size const& _a, Size const& _b) noexcept
{
    return !(_a == _b);
}

constexpr Size operator+(Size _a, Size _b) noexcept
{
    return Size{
        _a.width + _b.width,
        _a.height + _b.height
    };
}

constexpr Size operator-(Size _a, Size _b) noexcept
{
    return Size{
        _a.width - _b.width,
        _a.height - _b.height
    };
}

constexpr Size operator*(Size _a, Size _b) noexcept
{
    return Size{
        _a.width * _b.width,
        _a.height * _b.height
    };
}

constexpr Size operator/(Size _a, Size _b) noexcept
{
    return Size{
        _a.width / _b.width,
        _a.height / _b.height
    };
}

} // end namespace terminal

namespace fmt {
    template <>
    struct formatter<terminal::Size> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const terminal::Size& value, FormatContext& ctx)
        {
            return format_to(ctx.out(), "{}x{}", value.width, value.height);
        }
    };
}
