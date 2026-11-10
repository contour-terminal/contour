// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The seam between the ssh transport and whoever answers ssh's prompts. The transport knows nothing of
/// dialogs or sockets: it asks a broker for one session per attempt, runs ssh with the environment the
/// session names, and reads back whether the user cancelled and how long prompts held the attempt up.

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <string>

#include <vthost/SshTransport.hpp>
#include <vthost/StdioChild.hpp>

namespace vthost
{

/// One ssh attempt's side of askpass: alive from before ssh is spawned until its login is decided.
class IAskpassSession
{
  public:
    virtual ~IAskpassSession() = default;

    /// @return What ssh's environment needs so that it runs Contour as its askpass helper.
    [[nodiscard]] virtual SpawnEnvironment environment() const = 0;

    /// @return Whether the user dismissed a prompt of this attempt.
    [[nodiscard]] virtual bool wasCancelled() const noexcept = 0;

    /// @return The total time prompts of this attempt have been open so far; the connect deadline does
    ///         not run while the user is answering.
    [[nodiscard]] virtual std::chrono::milliseconds promptTime() const noexcept = 0;

    /// Has @p wake called as soon as the user dismisses a prompt of this attempt, so that the transport ends
    /// ssh at once: left to its own devices ssh would ask again, or go on waiting for a key. Called at once,
    /// from here, if one was dismissed already. One handler per session; a later one replaces the earlier.
    /// @param wake Called from any thread, once at most.
    virtual void onCancelled(std::function<void()> wake) = 0;
};

/// Opens an IAskpassSession for each ssh attempt.
class IAskpassBroker
{
  public:
    virtual ~IAskpassBroker() = default;

    /// @param target The host the attempt logs in to; what the prompts name.
    /// @return The session, or why it cannot be set up.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IAskpassSession>, std::string> open(
        SshTarget const& target) = 0;
};

} // namespace vthost
