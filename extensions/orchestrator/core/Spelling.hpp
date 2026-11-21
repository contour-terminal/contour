// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace orchestrator
{

/// One row of a table that names what a string a tool prints means.
template <typename Meaning>
struct Spelling
{
    std::string_view text; ///< What the tool prints.
    Meaning meaning;       ///< What it means.
};

/// @param table The rows that name what each string means.
/// @param text What a tool printed.
/// @param fallback What a string no row spells means.
/// @return The meaning of @p text in @p table, or @p fallback if no row spells it.
template <typename Meaning, std::size_t N>
[[nodiscard]] constexpr Meaning meaningOf(std::array<Spelling<Meaning>, N> const& table,
                                          std::string_view text,
                                          Meaning fallback) noexcept
{
    auto const row = std::ranges::find(table, text, &Spelling<Meaning>::text);
    return row != table.end() ? row->meaning : fallback;
}

} // namespace orchestrator
