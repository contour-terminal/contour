// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Contour as ssh's `SSH_ASKPASS` helper. ssh runs the program with the prompt as its only argument and
/// reads the answer from its standard output; this one relays the prompt to the GUI that started the ssh
/// and prints what the user answered.

#include <core/Environment.hpp>

#include <chrono>
#include <iosfwd>
#include <optional>
#include <span>

namespace contour::askpass
{

/// How long the helper keeps trying to reach the GUI's server, which listens before ssh is started: this only
/// covers a pipe whose instances are all busy for a moment.
inline constexpr auto DefaultAskpassConnectBudget = std::chrono::milliseconds { 500 };

/// Runs as the askpass helper when `CONTOUR_ASKPASS_ENDPOINT` is set; main() calls it before it builds
/// any application object, as the prompt is the only argument and no verb parser may see it.
///
/// The answer is printed, with a newline, to @p out and to nothing else: it is never logged, at any level.
/// Anything but an answer (the user cancelled, the GUI is unreachable, the nonce was refused, the reply
/// is not one) exits 1 with nothing on @p out.
/// @param environment Where the variables are read from.
/// @param args The helper's argv: the program, then the prompt.
/// @param out Where the answer goes.
/// @param err Where a reason for a failure goes (never the answer).
/// @param connectBudget How long to keep trying to reach the server.
/// @return The exit code, when this process is an askpass helper; nullopt otherwise.
[[nodiscard]] std::optional<int> runAskpassIfRequested(
    core::Environment const& environment,
    std::span<char const* const> args,
    std::ostream& out,
    std::ostream& err,
    std::chrono::milliseconds connectBudget = DefaultAskpassConnectBudget);

/// runAskpassIfRequested() onto the process's standard output and error.
/// @param environment Where the variables are read from.
/// @param args The helper's argv.
/// @return The exit code, when this process is an askpass helper; nullopt otherwise.
[[nodiscard]] std::optional<int> runAskpassIfRequested(core::Environment const& environment,
                                                       std::span<char const* const> args);

} // namespace contour::askpass
