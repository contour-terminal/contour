// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <vtbackend/screen/Settings.hpp>

#include <array>
#include <span>

namespace contour::config
{

namespace detail
{
    /// The spellings of `input_method_editor_style`. Owned by vtbackend, so the table hangs off the type from
    /// here; the reader, the written-back spelling and a settings page's options all come from these rows.
    /// The tokens are written into the configuration, so once shipped they are fixed.
    inline constexpr auto PreeditStyleTable = std::array {
        ConfigEnumInfo<vtbackend::PreeditStyle> {
            vtbackend::PreeditStyle::Highlighted, "highlighted", "Highlighted" },
        ConfigEnumInfo<vtbackend::PreeditStyle> {
            vtbackend::PreeditStyle::Inline, "inline", "Inline, underlined" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::PreeditStyle> const> configEnumValues() noexcept
{
    return detail::PreeditStyleTable;
}

} // namespace contour::config
