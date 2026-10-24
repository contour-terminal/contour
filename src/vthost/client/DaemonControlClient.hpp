// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The client side of daemon control: asking a running daemon about itself, and asking it to stop, over a
/// Control-role connection. `DaemonControlPort` is the seam the CLI and the GUI's drift handling talk to;
/// `SocketDaemonControl` is its implementation over any connected socket, and `makeDaemonControl` wires it
/// to the daemon's real endpoint.

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <vthost/Daemon.hpp>
#include <vthost/DaemonLifecycle.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost::client
{

/// Why a daemon-control request did not get its answer.
enum class ControlErrorCode : uint8_t
{
    NotRunning = 0, ///< No daemon listens at the endpoint.
    Refused,        ///< The daemon refused this client's handshake; the detail is the refusal text.
    Unsupported,    ///< The daemon does not offer daemon control (it predates it).
    Timeout,        ///< The daemon did not answer, or did not stop, in time.
    Transport,      ///< The connection failed or broke; the detail says how.
};

/// A failed daemon-control request.
struct ControlError
{
    ControlErrorCode code = ControlErrorCode::Transport; ///< What kind of failure.
    std::string detail;                                  ///< The specifics, in words for the user.

    /// @return The error as one sentence for the user.
    [[nodiscard]] std::string toString() const;
};

/// A daemon's report on itself, as the user-facing types spell it.
struct DaemonStatus
{
    std::string releaseVersion;                              ///< The daemon's release.
    std::uint64_t processId = 0;                             ///< Its process id.
    std::chrono::seconds uptime {};                          ///< How long it has been serving.
    std::filesystem::path socketPath;                        ///< The endpoint it listens on.
    std::filesystem::path executablePath;                    ///< The image it runs from.
    std::filesystem::path logFile;                           ///< Where it logs; empty when nowhere.
    std::size_t sessions = 0;                                ///< How many sessions it hosts.
    std::size_t clients = 0;                                 ///< How many clients are attached.
    DaemonLifecycle lifecycle = DaemonLifecycle::Persistent; ///< When it ends on its own.
};

/// @param reply A DaemonStatusReply as the wire spells it.
/// @return The status it states, or nullopt when its lifecycle byte names no lifecycle.
[[nodiscard]] std::optional<DaemonStatus> fromWire(proto::DaemonStatusReply const& reply);

/// What a caller asks of a daemon it does not attach to.
class DaemonControlPort
{
  public:
    virtual ~DaemonControlPort() = default;

    /// @return The daemon's status, or why it could not be had.
    [[nodiscard]] virtual std::expected<DaemonStatus, ControlError> status() = 0;

    /// Asks the daemon to stop and waits until it no longer accepts connections.
    /// @param reason Why the daemon is asked.
    /// @return Nothing once the daemon is gone, or why it is not known to be.
    [[nodiscard]] virtual std::expected<void, ControlError> stop(proto::StopReason reason) = 0;
};

/// Daemon control over Control-role connections, one per call. Runs a blocking call on @p loop, so it must
/// not be called from the loop's own thread.
class SocketDaemonControl final: public DaemonControlPort
{
  public:
    /// Opens a fresh connection to the daemon, or says why not: ControlErrorCode::NotRunning when nothing
    /// listens, ControlErrorCode::Transport for any other failure. Classified where the typed error is in
    /// hand, never from its text.
    using Connector =
        std::function<core::async::Task<std::expected<std::unique_ptr<core::net::ISocket>, ControlError>>()>;

    /// @param loop The loop the connections run on (not owned).
    /// @param connect Opens one connection per call.
    /// @param timeout How long a whole call may take.
    SocketDaemonControl(core::net::EventLoop& loop, Connector connect, std::chrono::milliseconds timeout);

    [[nodiscard]] std::expected<DaemonStatus, ControlError> status() override;
    [[nodiscard]] std::expected<void, ControlError> stop(proto::StopReason reason) override;

  private:
    core::net::EventLoop& _loop;
    Connector _connect;
    std::chrono::milliseconds _timeout;
};

/// The control timeout when a caller states none.
///
/// It bounds a whole call: for stop(), the connect, the StopDaemon, the daemon's hang-up and the
/// reconnects until nothing accepts any more. Enough while the shutdown only closes listeners; once
/// phase 6's beforeShutdown writes a session snapshot inside the funnel, a stop may legitimately take
/// longer — revisit it then.
inline constexpr auto DefaultControlTimeout = std::chrono::milliseconds { 2000 };

/// Daemon control over the daemon's real endpoint, owning the loop it runs on.
/// @param endpoint Where the daemon listens.
/// @param timeout How long a whole call may take.
/// @return The port; a missing socket or a refused connection reads as ControlErrorCode::NotRunning.
[[nodiscard]] std::unique_ptr<DaemonControlPort> makeDaemonControl(
    AttachEndpoint endpoint, std::chrono::milliseconds timeout = DefaultControlTimeout);

} // namespace vthost::client
