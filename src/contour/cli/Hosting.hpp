// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a GUI launch hosts its sessions -- in its own process, or in the daemon it attaches to -- as
/// `daemon.mode` and the outcome of the attach decide it. Pure, so every launch path is decided by one
/// table and tested without a window or a daemon.

#include <contour/config/Config.hpp>

#include <core/Utils.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include <vthost/ClientSizePolicy.hpp>

namespace contour::cli
{

/// How an attempt to attach to the daemon ended. A daemon that answered and refused this client is
/// Unreachable too: no decision tells the two apart, so no enumerator does until one has to.
enum class AttachResult : uint8_t
{
    Attached = 0,    ///< The daemon accepted the connection.
    Unreachable = 1, ///< No daemon answered or none could be started, or the one that answered refused.
};

/// Where a launch hosts its sessions.
enum class HostingDecision : uint8_t
{
    InProcess = 0, ///< In this process, as configured.
    InProcessWithNotice =
        1,        ///< In this process, because the daemon wanted could not be had; the user is told.
    Attached = 2, ///< In the daemon this process attached to.
    Fail = 3,     ///< Nowhere: the daemon was required and could not be had.
};

/// What a launch runs in its first session.
enum class LaunchProgram : uint8_t
{
    Profile = 0, ///< The profile's shell: what a daemon session runs too.
    /// A program the command line names (`-e`/`execute`, or the verbatim command). A daemon session runs
    /// the host's profile, so such a launch cannot be attached; its row in the table says what happens.
    Named = 1,
};

/// One row of the hosting table: what a mode decides once the attach it asked for ended, and for a launch
/// that names a program, which makes no attach at all.
struct HostingRow
{
    config::DaemonMode mode;
    HostingDecision onAttached;   ///< The attach succeeded.
    HostingDecision otherwise;    ///< It did not, or was never made.
    HostingDecision namedProgram; ///< The launch names a program (LaunchProgram::Named).
};

/// The hosting table, one row per mode: `in_process` never attaches, `auto` falls back to in-process with a
/// notice -- and without one for a launch naming a program, which it never tries to attach -- `required`
/// fails loudly, a launch naming a program included.
inline constexpr auto HostingTable = std::array {
    HostingRow { .mode = config::DaemonMode::InProcess,
                 .onAttached = HostingDecision::InProcess,
                 .otherwise = HostingDecision::InProcess,
                 .namedProgram = HostingDecision::InProcess },
    HostingRow { .mode = config::DaemonMode::Auto,
                 .onAttached = HostingDecision::Attached,
                 .otherwise = HostingDecision::InProcessWithNotice,
                 .namedProgram = HostingDecision::InProcess },
    HostingRow { .mode = config::DaemonMode::Required,
                 .onAttached = HostingDecision::Attached,
                 .otherwise = HostingDecision::Fail,
                 .namedProgram = HostingDecision::Fail },
};

/// Why a `required` launch naming a program ends before any window: the HostingDecision::Fail of the table's
/// `namedProgram` column.
inline constexpr std::string_view NamedProgramRefusal =
    "daemon.mode is required, but a program given on the command line cannot be attached to the daemon; "
    "drop it or use --daemon-mode=in_process";

/// @param mode The resolved `daemon.mode`.
/// @return Whether a launch in @p mode tries to attach to a daemon at all.
[[nodiscard]] constexpr bool attemptsAttach(config::DaemonMode mode) noexcept
{
    return mode != config::DaemonMode::InProcess;
}

/// @param mode The resolved `daemon.mode`.
/// @param program What the launch runs.
/// @return Whether the launch tries to attach: never when it names a program, whose row decides it alone.
[[nodiscard]] constexpr bool attemptsAttach(config::DaemonMode mode, LaunchProgram program) noexcept
{
    return program == LaunchProgram::Profile && attemptsAttach(mode);
}

/// @param mode The resolved `daemon.mode`.
/// @param program What the launch runs.
/// @param result How the attach ended; std::nullopt when none was made.
/// @return Where the launch hosts its sessions.
[[nodiscard]] constexpr HostingDecision decideHosting(config::DaemonMode mode,
                                                      LaunchProgram program,
                                                      std::optional<AttachResult> result) noexcept
{
    for (auto const& row: HostingTable)
        if (row.mode == mode)
        {
            if (program == LaunchProgram::Named)
                return row.namedProgram;
            return result == AttachResult::Attached ? row.onAttached : row.otherwise;
        }
    // A mode the table lacks is not one a parse produces; hosting in-process is what a launch did before
    // modes.
    return HostingDecision::InProcess;
}

/// decideHosting() for a launch running its profile's shell.
/// @param mode The resolved `daemon.mode`.
/// @param result How the attach ended; std::nullopt when none was made.
/// @return Where the launch hosts its sessions.
[[nodiscard]] constexpr HostingDecision decideHosting(config::DaemonMode mode,
                                                      std::optional<AttachResult> result) noexcept
{
    return decideHosting(mode, LaunchProgram::Profile, result);
}

/// The line a window shows when it hosts its sessions itself because the daemon could not be had.
/// @param reason Why the attach failed, as the transport or the daemon said it.
/// @return The notice.
[[nodiscard]] inline std::string hostingNotice(std::string_view reason)
{
    return std::format("Contour could not reach its daemon ({}). This window runs its shells itself; they "
                       "end when it closes.",
                       reason);
}

/// Resolves `daemon.mode` for a launch: a mode the verb forces (`contour client` requires a daemon), else
/// the `--daemon-mode` option, else the configuration's.
/// @param forced The mode the verb forces, if any.
/// @param option The `--daemon-mode` option; empty when not given.
/// @param configured `daemon.mode` from the configuration.
/// @return The mode, or why @p option names none.
[[nodiscard]] inline std::expected<config::DaemonMode, std::string> resolveDaemonMode(
    std::optional<config::DaemonMode> forced, std::string_view option, config::DaemonMode configured)
{
    if (forced)
        return *forced;
    if (option.empty())
        return configured;
    if (auto const mode = config::daemonModeFrom(option))
        return *mode;
    return std::unexpected(std::format("unknown --daemon-mode '{}' (expected one of: {})",
                                       option,
                                       core::joinHumanReadable(config::DaemonModeNames | std::views::keys)));
}

/// Resolves the client-size policy a daemon runs with: the `--size-policy` option, else `daemon.size_policy`.
/// @param option The `--size-policy` option; empty when not given.
/// @param configured `daemon.size_policy` from the configuration.
/// @return The policy, or why @p option names none -- rejected rather than defaulted: silently serving
///         `latest` to someone who asked for `smallest` would look like the option does not work.
[[nodiscard]] inline std::expected<vthost::ClientSizePolicy, std::string> resolveSizePolicy(
    std::string_view option, vthost::ClientSizePolicy configured)
{
    if (option.empty())
        return configured;
    if (auto const policy = vthost::clientSizePolicyFrom(option))
        return *policy;
    return std::unexpected(
        std::format("unknown --size-policy '{}' (expected one of: {})",
                    option,
                    core::joinHumanReadable(vthost::ClientSizePolicyNames | std::views::keys)));
}

/// @param option The `--socket` option; empty when not given.
/// @param configured `daemon.socket` from the configuration; empty for the default path.
/// @return The socket path to resolve (vthost::muxSocketPath): the option's, else the configuration's.
[[nodiscard]] inline std::string resolveSocketOption(std::string option, std::string configured)
{
    return option.empty() ? std::move(configured) : std::move(option);
}

} // namespace contour::cli
