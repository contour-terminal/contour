// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The client side of the control API: one Control-role connection that offers Capability::ControlApi,
/// calls methods by name with JSON params, and reads the events it subscribed to. `ControlClient` is the
/// connection; `resolveControlEndpoint` turns what a user names into a transport, and `runControlCall` and
/// `runEventStream` are the synchronous facades the CLI runs on a loop of their own.

#include <core/Environment.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/WriteQueue.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtcontrol/ControlError.hpp>
#include <vthost/Transport.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost::client
{

/// Why a control call did not get its reply.
enum class ControlFailure : uint8_t
{
    Unreachable = 0, ///< No connection to the daemon could be opened, or no endpoint resolved.
    Refused,         ///< The daemon refused the handshake: a version it does not speak, or a wrong token.
    Unsupported,     ///< The daemon does not offer the control API, or answered a request Unsupported.
    Untrusted,       ///< The transport does not verify the daemon, so no request is sent over it.
    Disconnected,    ///< The connection ended, or broke, before the answer arrived.
    TimedOut,        ///< The daemon did not answer in time.
    Remote,          ///< The daemon answered with an error: `ControlClientError::remote`.
};

/// A failed control call.
struct ControlClientError
{
    ControlFailure failure = ControlFailure::Unreachable; ///< What a caller acts on.
    std::string message;                                  ///< For the user, shown verbatim.
    std::optional<vtcontrol::ControlError> remote;        ///< The daemon's error, for ControlFailure::Remote.
};

/// One Control-role connection to a daemon's control API.
///
/// Every member runs on the loop it was constructed with; at most one of call() and nextEvent() is in
/// flight at a time. Events that arrive while a call waits for its reply are kept for nextEvent().
class ControlClient
{
  public:
    /// @param loop The loop the connection runs on (not owned; outlives the client).
    /// @param connection The open connection, and the token its ClientHello carries.
    ControlClient(core::net::EventLoop& loop, std::unique_ptr<Connection> connection);
    ControlClient(ControlClient const&) = delete;
    ControlClient(ControlClient&&) = delete;
    ControlClient& operator=(ControlClient const&) = delete;
    ControlClient& operator=(ControlClient&&) = delete;
    ~ControlClient();

    /// Sends the Control hello offering the control API and reads the daemon's answer. A daemon that does
    /// not share Capability::ControlApi is ControlFailure::Unsupported, and the connection is closed at
    /// once rather than waited on.
    /// @return Nothing once the API is agreed, or why not.
    [[nodiscard]] core::async::Task<std::expected<void, ControlClientError>> handshake();

    /// Calls one method and waits for its reply.
    /// @param method The method name, e.g. `pane.capture`.
    /// @param params The request JSON; empty reads as `{}`.
    /// @param context The caller's pane id (`$CONTOUR_PANE`), or empty.
    /// @return The reply JSON, or why there is none; an error the daemon answered is ControlFailure::Remote.
    [[nodiscard]] core::async::Task<std::expected<std::string, ControlClientError>> call(std::string method,
                                                                                         std::string params,
                                                                                         std::string context);

    /// @return The next ControlEvent's JSON (as `vtcontrol::encodeEvent` writes it), or why the stream
    ///         ended.
    [[nodiscard]] core::async::Task<std::expected<std::string, ControlClientError>> nextEvent();

    /// Ends the connection: a pending call() or nextEvent() completes ControlFailure::Disconnected.
    void close() noexcept;

  private:
    /// @return The next frame off the connection, or why there is none.
    [[nodiscard]] core::async::Task<std::expected<proto::DecodedFrame, ControlClientError>> readFrame();

    /// Queues @p pdu under the next serial.
    /// @return The serial, or why the queue refused it.
    [[nodiscard]] std::expected<uint64_t, ControlClientError> send(proto::DecodedPdu const& pdu);

    core::net::EventLoop& _loop;
    std::unique_ptr<Connection> _connection;
    core::net::WriteQueue _writer;
    std::vector<std::byte> _buffer; ///< Bytes read and not yet decoded, from _consumed on.
    std::size_t _consumed = 0;      ///< How much of _buffer is decoded already.
    uint64_t _nextSerial = 1;       ///< The serial the next PDU is sent under.
    uint64_t _nextRequestId = 1;    ///< The id the next call carries; ids increase from 1.
    /// Events read while a call waited, oldest first. Unbounded on purpose: what the daemon sends is bounded
    /// by its own drop-past-half-the-queue rule only while this client reads, so a long call (a `block.wait`)
    /// beside a busy subscription holds every event it read until nextEvent() takes them. A caller that
    /// subscribes and also makes long calls on one connection takes events between the calls.
    std::deque<std::string> _events {};
};

/// Resolves a saved host's name to its transport; phase 5's saved hosts, which `vthost` cannot read.
using SavedHostResolver =
    std::function<std::expected<std::shared_ptr<ITransportFactory>, std::string>(std::string_view name)>;

/// What a control client is told about where its daemon is.
struct ControlEndpointOptions
{
    std::string daemon {};   ///< `--daemon`: a socket path, `HOST:PORT`, or a saved host; empty for none.
    std::string token {};    ///< The token a TCP endpoint is sent.
    std::string tlsCaPem {}; ///< The CA certificate (PEM) a TCP endpoint is verified against; empty for none.
    /// Resolves a spec that names a saved host (one holding `://`, or a bare word); empty for no saved hosts,
    /// every such spec then being a path.
    SavedHostResolver resolveSavedHost {};
};

/// Picks the daemon a control client talks to.
///
/// The spec is the first non-empty of `options.daemon`, `$CONTOUR_DAEMON`, and the default socket. A
/// spec starting with `/` or `.` is a socket path even with a colon in it; a `HOST:PORT` spec is TCP+TLS with
/// the options' token and CA; one holding `://`, or a bare word, goes to the saved-host resolver when there
/// is one; anything else is a socket path. A socket path never starts a daemon.
/// @param options What the user said.
/// @param env Where `$CONTOUR_DAEMON` (and the default socket's inputs) are read from.
/// @return The transport, or ControlFailure::Unreachable with the resolver's message.
[[nodiscard]] std::expected<std::shared_ptr<ITransportFactory>, ControlClientError> resolveControlEndpoint(
    ControlEndpointOptions const& options, core::Environment const& env);

/// Connects, handshakes, calls one method and returns its reply, on a loop of its own. Refuses an
/// Unverified transport before connecting (ControlFailure::Untrusted).
/// @param transport How to reach the daemon.
/// @param method The method name.
/// @param params The request JSON.
/// @param context The caller's pane id, or empty.
/// @param timeout How long the whole call may take; none waits for as long as the daemon takes.
/// @return The reply JSON, or why there is none.
[[nodiscard]] std::expected<std::string, ControlClientError> runControlCall(
    ITransportFactory& transport,
    std::string_view method,
    std::string_view params,
    std::string_view context,
    std::optional<std::chrono::milliseconds> timeout = std::nullopt);

/// What an event stream does after an event.
enum class StreamStep : uint8_t
{
    Continue = 0, ///< Read the next one.
    Stop,         ///< End the stream.
};

/// Connects, handshakes, subscribes with @p subscribeParams, and hands every event to @p onEvent until it
/// says Stop, @p stop is requested, or the connection ends; on a loop of its own. Refuses an Unverified
/// transport before connecting (ControlFailure::Untrusted).
/// @param transport How to reach the daemon.
/// @param subscribeParams The `events.subscribe` request JSON.
/// @param context The caller's pane id, or empty.
/// @param onEvent Called with each event's JSON, on the calling thread.
/// @param stop Ends the stream when requested, from any thread.
/// @return Nothing when @p onEvent or @p stop ended it, or why it ended otherwise.
[[nodiscard]] std::expected<void, ControlClientError> runEventStream(
    ITransportFactory& transport,
    std::string_view subscribeParams,
    std::string_view context,
    std::function<StreamStep(std::string_view eventJson)> onEvent,
    core::async::StopToken const& stop);

} // namespace vthost::client
