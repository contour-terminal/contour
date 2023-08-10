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

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>

#include <type_traits>
#include <utility>

namespace terminal
{

/// Character graphics rendition information.
struct graphics_attributes
{
    color foregroundColor { DefaultColor() };
    color backgroundColor { DefaultColor() };
    color underlineColor { DefaultColor() };
    cell_flags flags {};
};

static_assert(std::is_trivially_copy_assignable_v<graphics_attributes>);

constexpr bool operator==(graphics_attributes const& a, graphics_attributes const& b) noexcept
{
    return a.backgroundColor == b.backgroundColor && a.foregroundColor == b.foregroundColor
           && a.flags == b.flags && a.underlineColor == b.underlineColor;
}

constexpr bool operator!=(graphics_attributes const& a, graphics_attributes const& b) noexcept
{
    return !(a == b);
}

} // namespace terminal
