// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Settings window's search: which rows a query finds, and in which order. Qt-free.

#include <contour/window/SettingsSections.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace contour::window
{

/// One row the search looks through.
struct SettingsSearchEntry
{
    std::string key;                                     ///< The row's key: `gutter.timestamps`.
    std::string label;                                   ///< What the row is called.
    std::string help;                                    ///< The row's help line.
    SettingsGroup group = SettingsGroup::GeneralWindows; ///< Where the row is.
    /// Set where the row is a link to a page of its own (then @c group is unused): the page it opens.
    std::optional<SettingsPageTarget> target {};
};

namespace detail
{
    /// Whether @p text contains @p needle, ignoring ASCII case.
    [[nodiscard]] inline bool containsIgnoringCase(std::string_view text, std::string_view needle)
    {
        auto const lower = [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        return !std::ranges::search(text, needle, {}, lower, lower).empty();
    }

    /// What a row is searched by, in the order the results come in: label, help, key.
    inline constexpr auto SearchedFields = std::array {
        &SettingsSearchEntry::label,
        &SettingsSearchEntry::help,
        &SettingsSearchEntry::key,
    };
} // namespace detail

/// The rows @p query finds: a case-insensitive substring of the label, the help or the key. Rows whose label
/// matches come first, then those whose help does, then those whose key does; each in the order of @p
/// entries.
/// @param query   What was typed; surrounding blanks are ignored, and a blank query finds nothing.
/// @param entries The rows.
/// @return The indices of the rows found, into @p entries.
[[nodiscard]] inline std::vector<std::size_t> matchSettings(std::string_view query,
                                                            std::span<SettingsSearchEntry const> entries)
{
    auto const isBlank = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    auto const first = std::ranges::find_if_not(query, isBlank);
    auto const last = std::ranges::find_if_not(query | std::views::reverse, isBlank).base();
    auto found = std::vector<std::size_t> {};
    if (first >= last)
        return found;
    auto const needle = std::string_view { first, last };

    for (auto const member: detail::SearchedFields)
        for (auto const index: std::views::iota(std::size_t { 0 }, entries.size()))
            if (!std::ranges::contains(found, index)
                && detail::containsIgnoringCase(entries[index].*member, needle))
                found.push_back(index);
    return found;
}

} // namespace contour::window
