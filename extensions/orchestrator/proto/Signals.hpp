// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>

#include <orchestrator/proto/Spawn.hpp>

namespace orchestrator::proto
{

/// Whether a signal asked the driver to stop.
enum class StopRequest : uint8_t
{
    None,     ///< Only SIGCHLD arrived: a child changed state.
    Received, ///< SIGINT or SIGTERM arrived.
};

/// SIGINT, SIGTERM and SIGCHLD as a readable descriptor (the self-pipe trick), so that the single-threaded
/// poll(2) loop wakes for them: to stop cleanly, and to notice at once a child that exited while a process it
/// left behind keeps its output open. One per process.
class SignalPipe
{
  public:
    /// Installs the handlers.
    /// @return The pipe; or errno when it could not be set up.
    [[nodiscard]] static std::expected<SignalPipe, int> install();

    /// @return The descriptor to poll for input.
    [[nodiscard]] int fd() const noexcept { return _read.get(); }

    /// Reads what the handlers wrote.
    /// @return Whether a stop was asked for.
    [[nodiscard]] StopRequest drain() const noexcept;

  private:
    SignalPipe(Descriptor read, Descriptor write) noexcept:
        _read { std::move(read) }, _write { std::move(write) }
    {
    }

    Descriptor _read;
    Descriptor _write; ///< Kept open for the handlers, which write to it.
};

} // namespace orchestrator::proto
