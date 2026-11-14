// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ClientSupervisor` — keeps a native client attached across a lost connection: it connects through a
/// transport factory, runs one `NativeClient` per connection, and after a loss reconnects with backoff,
/// resuming the workspace the lost connection selected.

#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <vthost/Transport.hpp>
#include <vthost/client/ConnectionState.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/proto/ReconnectCeiling.hpp>

namespace vthost::client
{

/// How long to wait before each reconnect attempt.
struct BackoffSchedule
{
    std::chrono::milliseconds first { 250 }; ///< The wait before the first retry.
    std::chrono::milliseconds ceiling =
        proto::ReconnectCeiling; ///< The longest wait; the doubling stops here.
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

/// How long a client waits for a daemon that announced its upgrade (proto::DaemonUpgrading) to come back
/// before it may start one: the new image serves again within a few hundred milliseconds, and one that fails
/// rolls back and keeps serving. Meanwhile the transport starts no daemon
/// (ITransportFactory::holdDaemonStart) and an absent one is retried with the normal backoff.
inline constexpr auto UpgradeHold = std::chrono::seconds { 15 };

/// Builds the client for one connection. The client keeps @p connection's socket for as long as it runs and
/// states its token in the hello; the supervisor keeps the connection's lifetime until the client is gone.
using ClientFactory = std::function<std::unique_ptr<NativeClient>(std::unique_ptr<Connection> connection,
                                                                  proto::AttachIntent intent)>;

/// Told every state change, on the loop's thread. @p reason says why, for Reconnecting, Upgrading, Failed and
/// Refused.
using StateHandler = std::function<void(ConnectionState state, std::string const& reason)>;

/// Runs one native connection after another: the first with the caller's intent, every later one resuming
/// the workspace the connection before it selected.
class ClientSupervisor
{
  public:
    /// @param loop The loop everything runs on.
    /// @param transport Opens each connection.
    /// @param makeClient Builds the client of each connection.
    /// @param firstIntent What the first connection asks for.
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

    /// As stop(), for a window the user closed: the current client says Goodbye first
    /// (NativeClient::closeWindow), so the daemon frees the window's workspace at once. stop() is for every
    /// other end, a failed attach among them, which sends no word and so reads to the daemon as a drop.
    void closeWindow();

    /// Cuts the reconnect backoff short: the wait under way ends at once and the next attempt starts, or,
    /// while an attempt is under way, the wait after it is skipped. Does nothing outside a reconnect. Unlike
    /// stop() it ends nothing: a system wake calls it, since the network may be back. Thread-safe.
    void retryNow();

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
        /// The connection ended after the daemon accepted it and announced that it replaces itself in place.
        Upgrading = 5,
    };

    /// @param ending How a connection ended.
    /// @return Whether the daemon had accepted it before it was lost: what a reconnect follows up.
    [[nodiscard]] static constexpr bool acceptedThenLost(Ending ending) noexcept
    {
        return ending == Ending::Lost || ending == Ending::Upgrading;
    }

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
    /// @param intent What this connection asks for.
    /// @return How it ended.
    [[nodiscard]] core::async::Task<Outcome> attempt(proto::AttachIntent intent);

    /// Retries with backoff until a connection is accepted and lost again, or retrying ends.
    /// @return Ending::Lost or Ending::Upgrading to start retrying afresh; any other outcome ends the
    ///         supervisor.
    [[nodiscard]] core::async::Task<Outcome> reconnect();

    /// @return What a reconnect asks for: Resume of the workspace the lost connection selected.
    [[nodiscard]] proto::AttachIntent resumeIntent() const;

    /// Tells the handler of a state change.
    void enter(ConnectionState state, std::string const& reason = {});

    /// Waits until @p deadline, until stop(), or until retryNow(), whichever comes first; the caller tells
    /// a stop from the other two by asking @ref _stop.
    /// @param deadline When the backoff ends by itself.
    [[nodiscard]] core::async::Task<void> backoffUntil(core::platform::SteadyTimePoint deadline);

    /// @return A token of the retry source in force, stopped by retryNow() since it was installed.
    [[nodiscard]] core::async::StopToken retryToken();

    /// Installs a fresh retry source: a retryNow() before it no longer counts.
    void resetRetry();

    /// Starts the UpgradeHold: the transport starts no daemon until it ends.
    void beginUpgradeHold();

    /// Ends the UpgradeHold early, the daemon having answered again.
    void endUpgradeHold();

    /// Once the UpgradeHold ran out with the daemon still absent: ends it, and reports Reconnecting, as from
    /// now on the transport starts a daemon as it would after any loss.
    void expireUpgradeHold();

    core::net::EventLoop& _loop;
    std::shared_ptr<ITransportFactory> _transport;
    ClientFactory _makeClient;
    proto::AttachIntent _firstIntent;
    BackoffSchedule _backoff;
    StateHandler _onState;
    core::async::StopSource _stop;
    /// Guards @ref _retry, which retryNow() reaches from any thread.
    std::mutex _retryMutex;
    /// Stopped by retryNow(): what a backoff wait parks on beside @ref _stop. Replaced once spent.
    core::async::StopSource _retry;
    /// When the UpgradeHold under way ends, on the loop's clock; nullopt while none is.
    std::optional<core::platform::SteadyTimePoint> _upgradeHoldUntil;
    /// The workspace the last accepted connection selected, 0 for none: what the next one resumes.
    uint64_t _selected = 0;
    /// The current connection's lifetime. Declared before @ref _current so that it outlives the client
    /// holding the socket it carries.
    std::shared_ptr<void> _currentLifetime;
    std::unique_ptr<NativeClient> _current;
};

} // namespace vthost::client
