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

#include <terminal/CellFlags.h>
#include <terminal/Color.h>

#include <utility>

namespace terminal {

/// Character graphics rendition information.
struct GraphicsAttributes
{
    Color foregroundColor{DefaultColor()};
    Color backgroundColor{DefaultColor()};
    Color underlineColor{DefaultColor()};
    CellFlags styles{};
};

constexpr bool operator==(GraphicsAttributes const& a, GraphicsAttributes const& b) noexcept
{
    return a.backgroundColor == b.backgroundColor
        && a.foregroundColor == b.foregroundColor
        && a.styles == b.styles
        && a.underlineColor == b.underlineColor;
}

constexpr bool operator!=(GraphicsAttributes const& a, GraphicsAttributes const& b) noexcept
{
    return !(a == b);
}

}
