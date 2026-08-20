// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Folding.hpp> // FoldJumpBehavior

#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

namespace contour::config
{

/// One value of a configuration enum, in every vocabulary the application needs it in.
///
/// A configuration enum used to be spelled out in five places — the YAML reader's if-chain, the
/// std::formatter's switch, the settings page's getter, its setter, and its list of combo box options
/// — and a sixth would be needed by every menu that offers it. Each was an independent chance to
/// disagree, and the settings page did: it showed users the raw configuration tokens because that is
/// what it had.
///
/// So a value is described once, in the enum's own table, and the code interprets it. Adding a value
/// is adding a row; adding a whole enum is adding a table plus one @c configEnumValues specialization.
template <typename Enum>
struct ConfigEnumInfo
{
    Enum value;
    std::string_view token; ///< The configuration spelling; also what the settings page stores.
    std::string_view label; ///< What a human reads, in a menu or a settings combo box.
};

namespace detail
{
    /// ASCII-lowercases @p ch.
    ///
    /// Spelled out rather than reached for in crispy: crispy::toLower allocates a std::string and is
    /// neither constexpr nor ASCII-pinned, and this has to run at compile time over the token tables.
    [[nodiscard]] constexpr char toLowerAscii(char ch) noexcept
    {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    }

    /// Whether @p lhs and @p rhs are equal, ignoring ASCII case.
    [[nodiscard]] constexpr bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
    {
        if (lhs.size() != rhs.size())
            return false;
        for (auto const i: std::views::iota(size_t { 0 }, lhs.size()))
            if (toLowerAscii(lhs[i]) != toLowerAscii(rhs[i]))
                return false;
        return true;
    }
} // namespace detail

/// Every value of configuration enum @p Enum, in enumerator order.
///
/// Declared without a definition on purpose: each configuration enum supplies its own table by
/// specializing this, so a missing table is a compile error rather than an empty list.
template <typename Enum>
[[nodiscard]] constexpr std::span<ConfigEnumInfo<Enum> const> configEnumValues() noexcept;

/// The value @p token names, ignoring case.
///
/// @param token A configuration spelling, e.g. "multiple".
/// @return The value, or nullopt when no value carries that token.
template <typename Enum>
[[nodiscard]] constexpr std::optional<Enum> configEnumFromToken(std::string_view token) noexcept
{
    for (auto const& info: configEnumValues<Enum>())
        if (detail::equalsIgnoreCase(info.token, token))
            return info.value;
    return std::nullopt;
}

/// The configuration spelling of @p value.
template <typename Enum>
[[nodiscard]] constexpr std::string_view configEnumToken(Enum value) noexcept
{
    for (auto const& info: configEnumValues<Enum>())
        if (info.value == value)
            return info.token;
    return {};
}

/// What a human should read for @p value.
template <typename Enum>
[[nodiscard]] constexpr std::string_view configEnumLabel(Enum value) noexcept
{
    for (auto const& info: configEnumValues<Enum>())
        if (info.value == value)
            return info.label;
    return {};
}

namespace detail
{
    /// What a targeted Vi jump does when its target sits inside a collapsed fold.
    ///
    /// Owned by vtbackend, so the table hangs off the type from here -- the reader, the settings-page
    /// combo box and the written-back spelling then all come from this one row set.
    inline constexpr auto FoldJumpBehaviorTable = std::array {
        ConfigEnumInfo<vtbackend::FoldJumpBehavior> {
            vtbackend::FoldJumpBehavior::Expand, "expand", "Expand the block" },
        ConfigEnumInfo<vtbackend::FoldJumpBehavior> {
            vtbackend::FoldJumpBehavior::Skip, "skip", "Stop at the prompt line" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::FoldJumpBehavior> const> configEnumValues() noexcept
{
    return detail::FoldJumpBehaviorTable;
}

} // namespace contour::config
