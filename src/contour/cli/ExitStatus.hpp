// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace contour::cli
{

/// The exit status of a `contour` CLI verb: one table, shared by every verb that reports through it.
///
/// Reserved, and added by the verbs that need them: `3` (TargetNotFound, the session or pane a control
/// verb named does not exist) and `130` (Interrupted, the verb ended on Ctrl+C).
enum class ExitStatus : uint8_t
{
    Ok = 0,          ///< The verb did what it was asked.
    Failed = 1,      ///< The verb could not do it; the reason is on standard error.
    BadUsage = 2,    ///< The command line was malformed.
    Unreachable = 4, ///< The daemon the verb talks to is not running.
};

/// @param status A verb's outcome.
/// @return The process exit code it stands for; used at `main`'s boundary.
[[nodiscard]] constexpr int exitCode(ExitStatus status) noexcept
{
    return static_cast<int>(status);
}

} // namespace contour::cli
