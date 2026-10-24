// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace contour::config
{

/// Which generation of built-in defaults a configuration starts from.
///
/// A default that changes would otherwise reach every existing `contour.yml` that does not spell the
/// setting out, which is nearly all of them: a file that sets only a font would silently inherit new
/// colours, a status line and a different tab bar. So the defaults are versioned. A file states the
/// edition it was written against as `defaults_edition`, the generated configuration states the newest
/// one, and a file that states none is from before the key existed and keeps `Classic`.
///
/// Zero is `Classic` so that a zero-initialised value means "what Contour always did".
enum class DefaultsEdition : uint8_t
{
    Classic = 0, //!< The defaults every release before the 2026 redesign shipped.
    Native2026,  //!< The 2026 redesign's first-launch defaults.
};

namespace detail
{
    /// The edition spellings. The tokens are written into every generated `contour.yml`, so once
    /// shipped they are fixed.
    inline constexpr auto DefaultsEditionTable = std::array {
        ConfigEnumInfo<DefaultsEdition> { DefaultsEdition::Classic, "classic", "Classic" },
        ConfigEnumInfo<DefaultsEdition> { DefaultsEdition::Native2026, "native2026", "Native (2026)" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<DefaultsEdition> const> configEnumValues() noexcept
{
    return detail::DefaultsEditionTable;
}

} // namespace contour::config
