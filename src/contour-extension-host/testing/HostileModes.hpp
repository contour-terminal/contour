// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The ways the hostile extension host misbehaves, one row each: what the crash and hostility tests start in
/// place of the real one, to prove Contour survives a host that does not play by the rules. Never installed.

#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>

namespace contour::extension_host::testing
{

/// How the hostile host misbehaves.
enum class HostileMode : uint8_t
{
    /// Serves as the real host does, but never reads the interrupt page: an interrupted call runs on, as a
    /// guest stuck where the engine cannot stop it would.
    IgnoreInterrupt,
};

/// One way the hostile host misbehaves, and its name on the command line.
struct HostileModeRow
{
    HostileMode mode;      ///< The misbehaviour.
    std::string_view name; ///< What `--hostile=<name>` names it.
};

/// Every way the hostile host misbehaves.
inline constexpr auto HostileModes = std::array {
    HostileModeRow { .mode = HostileMode::IgnoreInterrupt, .name = "ignore-interrupt" },
};

/// The only argument the hostile host takes beyond the real one's.
inline constexpr auto HostileOption = std::string_view { "--hostile=" };

/// @param name A mode's name.
/// @return The mode of that name; nothing if there is none.
[[nodiscard]] constexpr std::optional<HostileMode> hostileModeNamed(std::string_view name) noexcept
{
    auto const row = std::ranges::find(HostileModes, name, &HostileModeRow::name);
    return row != HostileModes.end() ? std::optional { row->mode } : std::nullopt;
}

} // namespace contour::extension_host::testing
