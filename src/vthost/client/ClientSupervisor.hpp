// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ClientSupervisor` — keeps a native client attached across a lost connection: it connects through a
/// transport factory, runs one `NativeClient` per connection, and after a loss reconnects with backoff,
/// resuming the windows the lost connection followed.

#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vthost/Transport.hpp>
#include <vthost/client/ConnectionState.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost::client
{

/// How long to wait before each reconnect attempt.
struct BackoffSchedule
{
    std::chrono::milliseconds first { 250 };    ///< The wait before the first retry.
    std::chrono::milliseconds ceiling { 5000 }; ///< The longest wait; the doubling stops here.
};

/// @param schedule The schedule.
/// @param attempt 0 for the first connect, n >= 1 for the n-th retry.
/// @return No wait for attempt 0; `first * 2^(n-1)` for attempt n, at most `ceiling`, without overflowing.
[[nodiscard]] constexpr std::chrono::milliseconds backoffDelay(BackoffSchedule schedule,
                                                               uint32_t attempt) noexcept
{
    if (attempt == 0 || schedule.first.count() <= 0)
        return std::chrono::milliseconds { 0 };
    auto const first = std::min(schedule.first, schedule.ceiling).count();
    auto const ceiling = schedule.ceiling.count();
    auto const doublings = attempt - 1;
    // first * 2^doublings <= ceiling exactly when first <= ceiling / 2^doublings: compared that way round,
    // nothing is multiplied that could overflow.
    if (doublings >= std::numeric_limits<std::chrono::milliseconds::rep>::digits
        || first > (ceiling >> doublings))
        return schedule.ceiling;
    return std::chrono::milliseconds { first << doublings };
}

/// Builds the client for one connection. The client keeps @p connection's socket for as long as it runs and
/// states its token in the hello; the supervisor keeps the connection's lifetime until the client is gone.
using ClientFactory = std::function<std::unique_ptr<NativeClient>(std::unique_ptr<Connection> connection,
                                                                  proto::AttachIntent intent)>;

/// Told every state change, on the loop's thread. @p reason says why, for Reconnecting, Failed and Refused.
using StateHandler = std::function<void(ConnectionState state, std::string const& reason)>;

/// Runs one native connection after another: the first with the caller's intent, every later one resuming
/// the windows the connection before it followed -- or, for a client that attached with AllWindows, following
/// every window again.
class ClientSupervisor
{
  public:
    /// @param loop The loop everything runs on.
    /// @param transport Opens each connection.
    /// @param makeClient Builds the client of each connection.
    /// @param firstIntent What the first connection asks to follow.
    /// @param backoff How long to wait before each reconnect attempt.
    /// @param onState Told every state change.
    ClientSupervisor(core::net::EventLoop& loop,
                     std::shared_ptr<ITransportFactory> transport,
                     ClientFactory makeClient,
                     proto::AttachIntent firstIntent,
                     BackoffSchedule backoff,
                     StateHandler onState);

    /// Connects, serves, and reconnects until a connection fails for good or stop() is called.
    [[nodiscard]] core::async::Task<void> run();

    /// Ends run(): detaches the current client, or cancels a pending backoff wait or connect at once -- a
    /// daemon start the transport has under way included, which is not waited out. Call on the loop's
    /// thread, where the client lives.
    void stop();

    /// @return The client of the connection under way; null between connections.
    [[nodiscard]] NativeClient* current() noexcept { return _current.get(); }

  private:
    /// How one connection ended.
    enum class Ending : uint8_t
    {
        TransportFailed = 0, ///< No connection was opened.
        Refused = 1,         ///< The daemon refused the handshake.
        /// The connection ended before the daemon answered the handshake, or the daemon did not answer it
        /// within LivenessPolicy::handshakeDeadline.
        Unaccepted = 2,
        Lost = 3,    ///< The connection ended after the daemon accepted it.
        Stopped = 4, ///< stop() ended it.
    };

    /// How one connection ended, and why.
    struct Outcome
    {
        Ending ending = Ending::Stopped;
        /// Why, in words for the user; the kind matters for Ending::TransportFailed only.
        TransportError error = {};
    };

    /// What opening one connection gave.
    using ConnectResult = std::expected<std::unique_ptr<Connection>, TransportError>;

    /// Opens a connection through the transport.
    /// @param result Where to put what it gave; left empty when the open is cancelled.
    [[nodiscard]] core::async::Task<void> connectInto(std::optional<ConnectResult>* result);

    /// Returns once stop() was called; what an open connection is raced against.
    [[nodiscard]] core::async::Task<void> untilStopped();

    /// Opens one connection and serves it to its end.
    /// @param intent What this connection asks to follow.
    /// @return How it ended.
    [[nodiscard]] core::async::Task<Outcome> attempt(proto::AttachIntent intent);

    /// Retries with backoff until a connection is accepted and lost again, or retrying ends.
    /// @return Ending::Lost to start retrying afresh; any other outcome ends the supervisor.
    [[nodiscard]] core::async::Task<Outcome> reconnect();

    /// @return What a reconnect asks to follow: every window again for a client that followed every window,
    ///         else Resume of the windows the lost connection followed.
    [[nodiscard]] proto::AttachIntent resumeIntent() const;

    /// Tells the handler of a state change.
    void enter(ConnectionState state, std::string const& reason = {});

    core::net::EventLoop& _loop;
    std::shared_ptr<ITransportFactory> _transport;
    ClientFactory _makeClient;
    proto::AttachIntent _firstIntent;
    BackoffSchedule _backoff;
    StateHandler _onState;
    core::async::StopSource _stop;
    /// The windows the last accepted connection followed: what the next one resumes.
    std::vector<uint64_t> _followed;
    /// The current connection's lifetime. Declared before @ref _current so that it outlives the client
    /// holding the socket it carries.
    std::shared_ptr<void> _currentLifetime;
    std::unique_ptr<NativeClient> _current;
};

} // namespace vthost::client
