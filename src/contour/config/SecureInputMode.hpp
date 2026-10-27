// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace contour::config
{

/// Whether a pane whose program hides what is typed (a password prompt) shows a lock, and on macOS turns
/// on Secure Keyboard Entry while it has focus.
///
/// Zero is `Off`, the Classic edition's default; Native2026 defaults to `Auto`.
enum class SecureInputMode : uint8_t
{
    Off = 0, //!< Never a lock, and never secure keyboard entry.
    Auto,    //!< A lock while the terminal hides typed text; secure keyboard entry while that pane has focus.
};

/// The number of SecureInputMode enumerators, derived from the last one.
inline constexpr size_t SecureInputModeCount = static_cast<size_t>(SecureInputMode::Auto) + 1;

namespace detail
{
    /// The secure-input spellings. The tokens are written into the configuration, so once shipped they are
    /// fixed.
    inline constexpr auto SecureInputModeTable = std::array {
        ConfigEnumInfo<SecureInputMode> { SecureInputMode::Off, "off", "Off" },
        ConfigEnumInfo<SecureInputMode> { SecureInputMode::Auto, "auto", "When a password is asked for" },
    };
    static_assert(SecureInputModeTable.size() == SecureInputModeCount,
                  "every SecureInputMode needs a spelling");
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<SecureInputMode> const> configEnumValues() noexcept
{
    return detail::SecureInputModeTable;
}

/// The configuration token of @p mode.
/// @param mode The mode.
/// @return `"off"` or `"auto"`.
[[nodiscard]] constexpr std::string_view secureInputModeName(SecureInputMode mode) noexcept
{
    auto const values = configEnumValues<SecureInputMode>();
    for (auto const& row: values)
        if (row.value == mode)
            return row.token;
    return values.front().token;
}

} // namespace contour::config
