// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace contour::cli
{

/// The exit status of a `contour` CLI verb: one table, shared by every verb that reports through it.
///
/// A verb that waits on a command (`contour wait`, `contour send --wait`) exits with that command's own
/// status instead, which is why this is a table of the verb's outcomes and not of every exit code.
enum class ExitStatus : uint8_t
{
    Ok = 0,             ///< The verb did what it was asked.
    Failed = 1,         ///< The verb could not do it; the reason is on standard error.
    BadUsage = 2,       ///< The command line was malformed.
    TargetNotFound = 3, ///< The session, pane or block a control verb named does not exist.
    Unreachable = 4,    ///< The daemon the verb talks to is not running.
    Interrupted = 130,  ///< The verb ended on Ctrl+C.
};

/// What a control verb's exit status means to the script that reads it: the one text the agent skill renders
/// and `docs/control-api.md`'s "Exit statuses" table repeats, which a test holds the two to.
/// @param status A verb's outcome.
/// @return Its meaning, or empty for a value that is no ExitStatus.
[[nodiscard]] constexpr std::string_view meaningOf(ExitStatus status) noexcept
{
    switch (status)
    {
        case ExitStatus::Ok: return "Done.";
        case ExitStatus::Failed:
            return "The call failed; the reason is on standard error as `Error: <message>`.";
        case ExitStatus::BadUsage:
            return "The command line was malformed, or a setting it names cannot be used (a `--tls-ca` that "
                   "does "
                   "not parse).";
        case ExitStatus::TargetNotFound:
            return "The pane, tab, window or block named does not exist, or the name matches several.";
        case ExitStatus::Unreachable: return "No daemon could be reached.";
        case ExitStatus::Interrupted: return "Interrupted by Ctrl+C.";
    }
    return {};
}

/// Every ExitStatus, in value order, for the readers that list them all. An enumerator missing here but
/// known to meaningOf() (whose switch the compiler keeps exhaustive) fails a test.
inline constexpr auto ExitStatuses = std::array {
    ExitStatus::Ok,          ExitStatus::Failed,      ExitStatus::BadUsage, ExitStatus::TargetNotFound,
    ExitStatus::Unreachable, ExitStatus::Interrupted,
};

/// @param status A verb's outcome.
/// @return The process exit code it stands for; used at `main`'s boundary.
[[nodiscard]] constexpr int exitCode(ExitStatus status) noexcept
{
    return static_cast<int>(status);
}

} // namespace contour::cli
