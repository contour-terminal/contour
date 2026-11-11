// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `contour agent report`: turns a coding agent's hook payload into a `pane.reportStatus` or
/// `pane.clearStatus` call on the daemon hosting the calling pane. A hook must never disturb the agent, so
/// nothing here prints, fails, or takes longer than a couple of seconds.

#include <contour/cli/AgentIntegrations.hpp>
#include <contour/cli/ControlVerbs.hpp>

#include <core/Environment.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/Types.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace contour::cli
{

/// How long a report may take, reading the payload and connecting included.
inline constexpr auto AgentReportTimeout = std::chrono::milliseconds { 2000 };

/// The largest payload a report reads. A larger one -- a PostToolUse carrying a big tool output -- is not
/// read to its end nor parsed, and reports nothing.
inline constexpr auto MaxAgentPayloadBytes = std::size_t { 1024 } * 1024;

/// Where a report reads its hook payload: standard input, or a test's text.
class PayloadReader
{
  public:
    PayloadReader() = default;
    virtual ~PayloadReader() = default;
    PayloadReader(PayloadReader const&) = delete;
    PayloadReader& operator=(PayloadReader const&) = delete;
    PayloadReader(PayloadReader&&) = delete;
    PayloadReader& operator=(PayloadReader&&) = delete;

    /// Reads the whole payload, never waiting past @p deadline.
    /// @param maxBytes The most bytes to take.
    /// @param deadline When to give up.
    /// @return The payload; nothing when it is larger than @p maxBytes, its end did not come by @p deadline,
    ///         or it cannot be read.
    [[nodiscard]] virtual std::optional<std::string> read(std::size_t maxBytes,
                                                          core::platform::SteadyTimePoint deadline) = 0;
};

/// @param input The handle to read: core::platform::standardInput() for the real report.
/// @param clock What the deadline is measured on.
/// @return A reader of @p input that waits on it no longer than a read's deadline.
[[nodiscard]] std::unique_ptr<PayloadReader> makeHandlePayloadReader(core::platform::NativeHandle input,
                                                                     core::platform::IClock const& clock);

/// Opens the daemon's channel; called only once there is something to send, so that a shell outside any
/// Contour pane never dials.
using ChannelFactory = std::function<std::unique_ptr<ControlChannel>()>;

/// @param env Where `$CONTOUR_DAEMON` is read.
/// @return The factory of the real channel: `$CONTOUR_DAEMON`'s endpoint, or none when it cannot be resolved.
[[nodiscard]] ChannelFactory makeDaemonChannelFactory(core::Environment const& env);

/// Reports one hook payload, all of it within AgentReportTimeout.
/// @param integration The agent whose payload it is.
/// @param payload Where the payload's JSON text is read, at most MaxAgentPayloadBytes of it; not read at all
///                outside a Contour pane.
/// @param env Where `$CONTOUR_DAEMON` and `$CONTOUR_PANE` are read; without both nothing is sent.
/// @param openChannel Opens the channel to the daemon.
/// @param clock What the budget is measured on: the call to the daemon gets what reading left of it.
/// @return Always 0: the agent must not notice a failure.
[[nodiscard]] int runAgentReport(AgentIntegration const& integration,
                                 PayloadReader& payload,
                                 core::Environment const& env,
                                 ChannelFactory const& openChannel,
                                 core::platform::IClock const& clock);

} // namespace contour::cli
