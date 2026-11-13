// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/ControlClient.hpp>

#include <core/Utf8Path.hpp>
#include <core/async/AsyncQueue.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <utility>
#include <variant>

#include <vtcontrol/Json.hpp>
#include <vthost/DaemonSpawn.hpp>
#include <vthost/KeyAuth.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/Negotiation.hpp>

namespace vthost::client
{

namespace
{
    /// The bytes a control connection may queue before it is severed: requests are small, and a client
    /// waits for each reply before it sends the next.
    constexpr auto ControlQueueBytes = std::size_t { 1024 } * 1024;

    /// Once this much of the read buffer is decoded, it is dropped.
    constexpr auto CompactThreshold = std::size_t { 65536 };

    [[nodiscard]] std::unexpected<ControlClientError> fail(ControlFailure failure, std::string message)
    {
        return std::unexpected(
            ControlClientError { .failure = failure, .message = std::move(message), .remote = std::nullopt });
    }

    /// @param spec What names an endpoint.
    /// @return Whether it names a saved host rather than a path: a URL-like `scheme://`, or a bare word.
    [[nodiscard]] bool namesSavedHost(std::string_view spec) noexcept
    {
        return spec.contains("://") || spec.find_first_of("/\\:") == std::string_view::npos;
    }

    /// @param kind Why a transport could not connect.
    /// @return How a control client reports it: a daemon that is not (yet) trusted is Untrusted, so no
    ///         request goes to it, a question the user declined is Declined, a transport that is set up
    ///         wrongly is Misconfigured, the rest Unreachable.
    [[nodiscard]] constexpr ControlFailure controlFailureOf(TransportErrorKind kind) noexcept
    {
        switch (kind)
        {
            case TransportErrorKind::HostKeyUnknown:
            case TransportErrorKind::HostKeyChanged: return ControlFailure::Untrusted;
            case TransportErrorKind::Cancelled: return ControlFailure::Declined;
            case TransportErrorKind::Misconfigured: return ControlFailure::Misconfigured;
            case TransportErrorKind::Unreachable:
            case TransportErrorKind::Refused:
            case TransportErrorKind::AuthenticationRequired:
            case TransportErrorKind::RemoteProgramMissing:
            case TransportErrorKind::ProgramNotFound:
            case TransportErrorKind::Protocol:
            case TransportErrorKind::DaemonAbsent: return ControlFailure::Unreachable;
        }
        return ControlFailure::Unreachable;
    }

    /// @param transport What the call goes to.
    /// @param method The method called.
    /// @param timeout How long it was given.
    /// @return The failure of a call that did not answer in time.
    [[nodiscard]] ControlClientError timedOut(ITransportFactory const& transport,
                                              std::string_view method,
                                              std::chrono::milliseconds timeout)
    {
        return ControlClientError {
            .failure = ControlFailure::TimedOut,
            .message = std::format("{} did not answer '{}' within {}", transport.describe(), method, timeout),
            .remote = std::nullopt
        };
    }

    /// Connects through @p transport.
    /// @return The client, not yet handshaken, or why there is none.
    core::async::Task<std::expected<std::unique_ptr<ControlClient>, ControlClientError>> connectClient(
        core::net::EventLoop* loop, ITransportFactory* transport)
    {
        auto connection = co_await transport->connect(*loop);
        if (!connection)
            co_return fail(controlFailureOf(connection.error().kind), std::move(connection.error().message));
        co_return std::make_unique<ControlClient>(*loop, std::move(*connection));
    }

    /// Connects through @p transport and handshakes.
    /// @return The client, or why there is none.
    core::async::Task<std::expected<std::unique_ptr<ControlClient>, ControlClientError>> connectControl(
        core::net::EventLoop* loop, ITransportFactory* transport)
    {
        auto client = co_await connectClient(loop, transport);
        if (!client)
            co_return std::unexpected(std::move(client.error()));
        if (auto handshaken = co_await (*client)->handshake(); !handshaken)
            co_return std::unexpected(std::move(handshaken.error()));
        co_return std::move(client);
    }

    /// Handshakes and calls one method.
    core::async::Task<std::expected<std::string, ControlClientError>> exchange(ControlClient* client,
                                                                               std::string method,
                                                                               std::string params,
                                                                               std::string context)
    {
        if (auto handshaken = co_await client->handshake(); !handshaken)
            co_return std::unexpected(std::move(handshaken.error()));
        co_return co_await client->call(std::move(method), std::move(params), std::move(context));
    }

    /// Ends when @p client has spent @p timeout waiting for the daemon since this began: the time it spends
    /// signing the daemon's challenges is not counted, up to PreAuthTimeout -- by then the daemon has closed
    /// the connection, and a signature that never comes (a pinentry on another terminal, a dead agent) must
    /// not hold the call for ever.
    core::async::Task<void> expireAfter(core::net::EventLoop* loop,
                                        ControlClient const* client,
                                        std::chrono::milliseconds timeout)
    {
        auto const start = loop->clock().now();
        while (true)
        {
            // While a signature is being made the due time moves with the clock, and this sleeps on.
            auto const due =
                start + timeout
                + std::min(client->signingTime(),
                           std::chrono::duration_cast<std::chrono::milliseconds>(PreAuthTimeout));
            auto const now = loop->clock().now();
            if (now >= due)
                co_return;
            co_await loop->delay(std::chrono::ceil<std::chrono::milliseconds>(due - now));
        }
    }

    /// Connects, then handshakes and calls.
    ///
    /// @p timeout bounds the handshake and the call together, one deadline for both, and never the connect:
    /// the connect may be waiting for the user to trust a daemon (the transport bounds the dialling itself),
    /// and a clock that runs through a question would end the call under the user's hand. For the same
    /// reason it does not run while a challenge is signed: a passphrase is the user's to type.
    core::async::Task<std::expected<std::string, ControlClientError>> callOnce(
        core::net::EventLoop* loop,
        ITransportFactory* transport,
        std::string method,
        std::string params,
        std::string context,
        std::optional<std::chrono::milliseconds> timeout)
    {
        auto client = co_await connectClient(loop, transport);
        if (!client)
            co_return std::unexpected(std::move(client.error()));
        if (!timeout)
            co_return co_await exchange(
                (*client).get(), std::move(method), std::move(params), std::move(context));
        auto const name = method;
        auto answered = std::optional<std::expected<std::string, ControlClientError>> {};
        auto exchanging = [](ControlClient* control,
                             std::string method,
                             std::string params,
                             std::string context,
                             std::optional<std::expected<std::string, ControlClientError>>* out)
            -> core::async::Task<void> {
            *out = co_await exchange(control, std::move(method), std::move(params), std::move(context));
        }((*client).get(), std::move(method), std::move(params), std::move(context), &answered);
        std::ignore = co_await core::async::whenAny(std::move(exchanging),
                                                    expireAfter(loop, (*client).get(), *timeout));
        if (!answered)
            co_return std::unexpected(timedOut(*transport, name, *timeout));
        co_return std::move(*answered);
    }

    /// What a stop request reaches the stream through, from any thread, by way of the loop.
    struct StreamState
    {
        ControlClient* client = nullptr; ///< The stream's client while it is open; touched on the loop only.
    };

    core::async::Task<std::expected<void, ControlClientError>> streamEvents(
        core::net::EventLoop* loop,
        ITransportFactory* transport,
        std::string subscribeParams,
        std::string context,
        std::function<StreamStep(std::string_view)> onEvent,
        core::async::StopToken stop,
        std::shared_ptr<StreamState> state)
    {
        auto client = co_await connectControl(loop, transport);
        if (!client)
            co_return std::unexpected(std::move(client.error()));
        // A stop asked for while connecting is honoured before anything more is asked of the daemon.
        if (stop.stop_requested())
            co_return std::expected<void, ControlClientError> {};
        auto& control = **client;
        auto subscribed =
            co_await control.call("events.subscribe", std::move(subscribeParams), std::move(context));
        if (!subscribed)
            co_return std::unexpected(std::move(subscribed.error()));
        if (stop.stop_requested())
            co_return std::expected<void, ControlClientError> {};

        state->client = &control;
        auto outcome = std::expected<void, ControlClientError> {};
        while (!stop.stop_requested())
        {
            auto event = co_await control.nextEvent();
            if (!event)
            {
                if (!stop.stop_requested())
                    outcome = std::unexpected(std::move(event.error()));
                break;
            }
            if (onEvent(*event) == StreamStep::Stop)
                break;
        }
        state->client = nullptr;
        co_return outcome;
    }
} // namespace

ControlClient::ControlClient(core::net::EventLoop& loop, std::unique_ptr<Connection> connection):
    _loop(loop),
    _connection(std::move(connection)),
    _writer(_loop, _connection->socket.get(), ControlQueueBytes)
{
}

ControlClient::~ControlClient()
{
    _writer.close();
}

void ControlClient::close() noexcept
{
    _writer.close();
    _connection->socket->close();
}

std::expected<uint64_t, ControlClientError> ControlClient::send(proto::DecodedPdu const& pdu)
{
    auto sink = proto::Writer {};
    auto const serial = _nextSerial++;
    proto::encodePdu(sink, serial, pdu);
    auto const bytes = sink.view();
    if (!_writer.enqueue(std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() }))
        return fail(ControlFailure::Disconnected, _writer.describeRefusal());
    return serial;
}

core::async::Task<std::expected<proto::DecodedFrame, ControlClientError>> ControlClient::readFrame()
{
    while (true)
    {
        auto decoded = proto::decodePdu(std::span { _buffer }.subspan(_consumed));
        if (decoded)
        {
            _consumed += decoded->consumed;
            if (_consumed >= CompactThreshold || _consumed == _buffer.size())
            {
                _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(_consumed));
                _consumed = 0;
            }
            co_return std::move(*decoded);
        }
        if (decoded.error() != proto::DecodeError::NeedMoreData)
            co_return fail(ControlFailure::Disconnected,
                           std::format("the daemon sent a malformed frame ({})", decoded.error()));
        auto const appended = co_await core::net::appendReadChunk(_connection->socket.get(), &_buffer);
        if (!appended)
            co_return fail(ControlFailure::Disconnected,
                           std::format("reading from the daemon failed: {}", appended.error().toString()));
        if (*appended == 0)
            co_return fail(ControlFailure::Disconnected, "the daemon closed the connection");
    }
}

core::async::Task<std::expected<void, ControlClientError>> ControlClient::handshake()
{
    auto const ours = proto::SupportedVersions;
    auto const offered = proto::CapabilitySet::of({ proto::Capability::ControlApi });
    auto const sendHello = [&](std::optional<proto::KeyProof> proof) {
        return send(
            proto::DecodedPdu { proto::ClientHello { .versions = ours,
                                                     .capabilities = offered,
                                                     .token = proof ? std::string {} : _connection->token,
                                                     .role = proto::ConnectionRole::Control,
                                                     .keyProof = std::move(proof) } });
    };
    // A client with a key and no token lets the daemon speak first: its challenge is what the proof covers.
    auto keyAuth =
        _connection->keySource ? std::optional { KeyAuthenticator { _connection->keySource } } : std::nullopt;
    auto helloSent = !keyAuth || !_connection->token.empty();
    if (helloSent)
        if (auto const sent = sendHello(std::nullopt); !sent)
            co_return std::unexpected(sent.error());

    while (true)
    {
        auto frame = co_await readFrame();
        if (!frame)
            co_return std::unexpected(std::move(frame.error()));
        if (auto const* challenge = std::get_if<proto::AuthChallenge>(&frame->pdu))
        {
            if (!helloSent)
            {
                // The signature is the user's to give (a passphrase, an agent's confirmation), but not for
                // longer than the daemon waits for it: by PreAuthTimeout it has closed the connection, and a
                // signer that never answers must not hold the call -- timed or not -- for ever.
                auto const allowance =
                    std::max(std::chrono::duration_cast<std::chrono::milliseconds>(PreAuthTimeout) - _signed,
                             std::chrono::milliseconds::zero());
                _signingSince = _loop.clock().now();
                auto proof = std::optional<std::optional<proto::KeyProof>> {};
                auto signing =
                    [](KeyAuthenticator* authenticator,
                       proto::AuthChallenge challenge,
                       std::optional<std::array<std::byte, 32>> binding,
                       std::optional<std::optional<proto::KeyProof>>* out) -> core::async::Task<void> {
                    *out = co_await authenticator->proofFor(challenge, binding);
                }(&*keyAuth, *challenge, channelBindingOf(*_connection->socket), &proof);
                auto givingUp = [](core::net::EventLoop* loop,
                                   std::chrono::milliseconds wait) -> core::async::Task<void> {
                    co_await loop->delay(wait);
                }(&_loop, allowance);
                std::ignore = co_await core::async::whenAny(std::move(signing), std::move(givingUp));
                _signed = signingTime();
                _signingSince = std::nullopt;
                if (!proof)
                {
                    close();
                    co_return fail(ControlFailure::TimedOut,
                                   std::format("the SSH key did not sign within the daemon's {} to sign in",
                                               PreAuthTimeout));
                }
                if (auto const sent = sendHello(*std::move(proof)); !sent)
                    co_return std::unexpected(sent.error());
                // Still waiting: a refused proof is answered with a fresh challenge, on which the next key
                // is offered.
            }
            continue;
        }
        auto const* hello = std::get_if<proto::ServerHello>(&frame->pdu);
        if (hello == nullptr)
            continue;
        auto const judged = judgeServerHello(ours, offered, *hello);
        if (!judged)
        {
            close();
            auto const words = proto::describe(judged.error(), proto::PeerRole::Daemon);
            co_return fail(ControlFailure::Refused, keyAuth ? keyAuth->explainRefusal(words) : words);
        }
        if (!judged->shared.contains(proto::Capability::ControlApi))
        {
            // Named at once: waiting on a daemon that will never answer a ControlRequest is the hang this
            // check exists to rule out.
            close();
            auto const daemon = hello->hostname.empty() ? std::string { "the daemon" }
                                                        : std::format("the daemon on {}", hello->hostname);
            co_return fail(ControlFailure::Unsupported,
                           std::format("{} does not offer the control API (it predates it); restart the "
                                       "daemon on this version",
                                       daemon));
        }
        co_return std::expected<void, ControlClientError> {};
    }
}

std::chrono::milliseconds ControlClient::signingTime() const
{
    if (!_signingSince)
        return _signed;
    return _signed + std::chrono::ceil<std::chrono::milliseconds>(_loop.clock().now() - *_signingSince);
}

core::async::Task<std::expected<std::string, ControlClientError>> ControlClient::call(std::string method,
                                                                                      std::string params,
                                                                                      std::string context)
{
    auto const requestId = _nextRequestId++;
    auto const serial = send(proto::DecodedPdu { proto::ControlRequest { .requestId = requestId,
                                                                         .method = method,
                                                                         .params = std::move(params),
                                                                         .context = std::move(context) } });
    if (!serial)
        co_return std::unexpected(serial.error());

    while (true)
    {
        auto frame = co_await readFrame();
        if (!frame)
            co_return std::unexpected(std::move(frame.error()));
        if (auto* event = std::get_if<proto::ControlEvent>(&frame->pdu))
        {
            _events.push_back(std::move(event->event));
            continue;
        }
        if (auto const* refusal = std::get_if<proto::Unsupported>(&frame->pdu);
            refusal != nullptr && refusal->refusedSerial == *serial)
            co_return fail(ControlFailure::Unsupported,
                           std::format("the daemon does not serve '{}'", method));
        auto* reply = std::get_if<proto::ControlReply>(&frame->pdu);
        // A reply to a request this client did not send, or no reply at all: not this call's answer.
        if (reply == nullptr || reply->requestId != requestId)
            continue;
        if (reply->outcome == proto::ControlOutcome::Ok)
            co_return std::move(reply->body);
        auto remote = vtcontrol::decodeJson<vtcontrol::ControlError>(reply->body);
        if (!remote)
            co_return fail(
                ControlFailure::Remote,
                std::format("the daemon answered '{}' with an error this client cannot read", method));
        auto message = remote->message;
        co_return std::unexpected(ControlClientError {
            .failure = ControlFailure::Remote, .message = std::move(message), .remote = std::move(*remote) });
    }
}

core::async::Task<std::expected<std::string, ControlClientError>> ControlClient::nextEvent()
{
    while (_events.empty())
    {
        auto frame = co_await readFrame();
        if (!frame)
            co_return std::unexpected(std::move(frame.error()));
        if (auto* event = std::get_if<proto::ControlEvent>(&frame->pdu))
            _events.push_back(std::move(event->event));
    }
    auto event = std::move(_events.front());
    _events.pop_front();
    co_return event;
}

std::expected<std::shared_ptr<ITransportFactory>, ControlClientError> resolveControlEndpoint(
    ControlEndpointOptions const& options, core::Environment const& env)
{
    // A label names a local daemon outright, exactly as `contour client --label` resolves it.
    if (!options.label.empty())
        return std::make_shared<UnixTransportFactory>(muxSocketPath(socketLabelOf(options.label), {}, env),
                                                      UnixTransportFactory::EnsureDaemon {});
    auto const spec =
        !options.daemon.empty() ? options.daemon : env.get("CONTOUR_DAEMON").value_or(std::string {});
    // Never a daemon started for a control call: an empty ensureDaemon.
    if (spec.empty())
        return std::make_shared<UnixTransportFactory>(muxSocketPath({}, {}, env),
                                                      UnixTransportFactory::EnsureDaemon {});
    // A spec that reads as a path is one, a colon in it notwithstanding (`/run/user/1000/x:1`).
    auto const socketAt = [](std::string const& path) {
        return std::make_shared<UnixTransportFactory>(
            core::pathFromUtf8(path).value_or(std::filesystem::path {}),
            UnixTransportFactory::EnsureDaemon {});
    };
    if (spec.starts_with('/') || spec.starts_with('.'))
        return socketAt(spec);
    if (auto const hostPort = parseHostPort(spec))
    {
        if (options.knownDaemons == nullptr)
            return fail(ControlFailure::Misconfigured, "no file to keep pinned daemons in is available");
        return std::make_shared<TlsTransportFactory>(
            TcpEndpoint { .host = hostPort->first,
                          .port = hostPort->second,
                          .token = options.token,
                          .caPem = options.tlsCaPem,
                          .expectedFingerprint = options.daemonFingerprint },
            *options.knownDaemons,
            options.pinPrompt,
            options.keySourceProvider);
    }
    if (options.resolveSavedHost && namesSavedHost(spec))
        return options.resolveSavedHost(spec, options.daemonFingerprint)
            .transform_error([](std::string message) {
                return ControlClientError { .failure = ControlFailure::Unreachable,
                                            .message = std::move(message),
                                            .remote = std::nullopt };
            });
    return socketAt(spec);
}

std::expected<std::string, ControlClientError> runControlCall(
    ITransportFactory& transport,
    std::string_view method,
    std::string_view params,
    std::string_view context,
    std::optional<std::chrono::milliseconds> timeout,
    core::async::StopToken const& stop)
{
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    // Closed when stop is requested, from whichever thread asks: what waits on it then ends the call, and
    // the call's frames unwind (a dial, a question to the user, a read) on the loop. Declared before the
    // callback, which is deregistered first.
    auto stopped = core::async::AsyncQueue<int> { loop, core::async::AsyncQueueOptions { .capacity = 1 } };
    auto const onStop = core::async::StopCallback { stop, [&stopped] { stopped.close(); } };
    auto answer = std::optional<std::expected<std::string, ControlClientError>> {};
    auto calling =
        [](core::net::EventLoop* loop,
           ITransportFactory* transport,
           std::string method,
           std::string params,
           std::string context,
           std::optional<std::chrono::milliseconds> timeout,
           std::optional<std::expected<std::string, ControlClientError>>* out) -> core::async::Task<void> {
        // The timeout bounds what the daemon does, not the connect, which may wait for the user (callOnce).
        *out = co_await callOnce(
            loop, transport, std::move(method), std::move(params), std::move(context), timeout);
    }(&loop,
                                                                               &transport,
                                                                               std::string { method },
                                                                               std::string { params },
                                                                               std::string { context },
                                                                               timeout,
                                                                               &answer);
    auto stopping = [](core::async::AsyncQueue<int>* stopped) -> core::async::Task<void> {
        std::ignore = co_await stopped->pop();
    }(&stopped);
    loop.blockOn(
        [](core::async::Task<void> calling, core::async::Task<void> stopping) -> core::async::Task<void> {
            std::ignore = co_await core::async::whenAny(std::move(calling), std::move(stopping));
        }(std::move(calling), std::move(stopping)));
    if (!answer)
        return std::unexpected(
            ControlClientError { .failure = ControlFailure::Declined,
                                 .message = std::format("the call to {} was given up", transport.describe()),
                                 .remote = std::nullopt });
    return *std::move(answer);
}

std::expected<void, ControlClientError> runEventStream(
    ITransportFactory& transport,
    std::string_view subscribeParams,
    std::string_view context,
    std::function<StreamStep(std::string_view eventJson)> onEvent,
    core::async::StopToken const& stop)
{
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto const state = std::make_shared<StreamState>();
    // A stop from another thread reaches the client on the loop, which closes it: the pending read ends.
    // Declared after the loop, so it is deregistered before the loop goes.
    auto const onStop = core::async::StopCallback { stop, [&loop, state] {
                                                       loop.post([state] {
                                                           if (state->client != nullptr)
                                                               state->client->close();
                                                       });
                                                   } };
    return loop.blockOn(streamEvents(&loop,
                                     &transport,
                                     std::string { subscribeParams },
                                     std::string { context },
                                     std::move(onEvent),
                                     stop,
                                     state));
}

} // namespace vthost::client
