// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/ControlClient.hpp>

#include <core/Utf8Path.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/WithTimeout.hpp>

#include <format>
#include <span>
#include <utility>
#include <variant>

#include <vtcontrol/Json.hpp>
#include <vthost/DaemonSpawn.hpp>
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

    /// @param transport The transport a request would travel over.
    /// @return The refusal of a transport that does not verify its peer, or nothing for one that does.
    [[nodiscard]] std::expected<void, ControlClientError> requireVerified(ITransportFactory const& transport)
    {
        if (transport.peerTrust() != PeerTrust::Unverified)
            return {};
        return fail(ControlFailure::Untrusted,
                    std::format("refusing to send control requests to {} without a verified certificate: "
                                "pass --tls-ca FILE, or set tls_ca on the saved host",
                                transport.describe()));
    }

    /// Connects through @p transport and handshakes.
    /// @return The client, or why there is none.
    core::async::Task<std::expected<std::unique_ptr<ControlClient>, ControlClientError>> connectControl(
        core::net::EventLoop* loop, ITransportFactory* transport)
    {
        auto connection = co_await transport->connect(*loop);
        if (!connection)
            co_return fail(connection.error().kind == TransportErrorKind::Misconfigured
                               ? ControlFailure::Misconfigured
                               : ControlFailure::Unreachable,
                           std::move(connection.error().message));
        auto client = std::make_unique<ControlClient>(*loop, std::move(*connection));
        if (auto handshaken = co_await client->handshake(); !handshaken)
            co_return std::unexpected(std::move(handshaken.error()));
        co_return std::move(client);
    }

    core::async::Task<std::expected<std::string, ControlClientError>> callOnce(core::net::EventLoop* loop,
                                                                               ITransportFactory* transport,
                                                                               std::string method,
                                                                               std::string params,
                                                                               std::string context)
    {
        auto client = co_await connectControl(loop, transport);
        if (!client)
            co_return std::unexpected(std::move(client.error()));
        co_return co_await (*client)->call(std::move(method), std::move(params), std::move(context));
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
    if (auto const sent =
            send(proto::DecodedPdu { proto::ClientHello { .versions = ours,
                                                          .capabilities = offered,
                                                          .token = _connection->token,
                                                          .role = proto::ConnectionRole::Control } });
        !sent)
        co_return std::unexpected(sent.error());

    while (true)
    {
        auto frame = co_await readFrame();
        if (!frame)
            co_return std::unexpected(std::move(frame.error()));
        auto const* hello = std::get_if<proto::ServerHello>(&frame->pdu);
        if (hello == nullptr)
            continue;
        auto const judged = judgeServerHello(ours, offered, *hello);
        if (!judged)
        {
            close();
            co_return fail(ControlFailure::Refused, proto::describe(judged.error(), proto::PeerRole::Daemon));
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
        return std::make_shared<TlsTransportFactory>(TcpEndpoint { .host = hostPort->first,
                                                                   .port = hostPort->second,
                                                                   .token = options.token,
                                                                   .caPem = options.tlsCaPem });
    if (options.resolveSavedHost && namesSavedHost(spec))
        return options.resolveSavedHost(spec).transform_error([](std::string message) {
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
    std::optional<std::chrono::milliseconds> timeout)
{
    if (auto const verified = requireVerified(transport); !verified)
        return std::unexpected(verified.error());

    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto work =
        callOnce(&loop, &transport, std::string { method }, std::string { params }, std::string { context });
    if (!timeout)
        return loop.blockOn(std::move(work));
    auto bounded = loop.blockOn(core::net::withTimeout(&loop, std::move(work), *timeout));
    if (!bounded)
        return fail(ControlFailure::TimedOut,
                    std::format("{} did not answer '{}' within {}", transport.describe(), method, *timeout));
    return std::move(*bounded);
}

std::expected<void, ControlClientError> runEventStream(
    ITransportFactory& transport,
    std::string_view subscribeParams,
    std::string_view context,
    std::function<StreamStep(std::string_view eventJson)> onEvent,
    core::async::StopToken const& stop)
{
    if (auto const verified = requireVerified(transport); !verified)
        return std::unexpected(verified.error());

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
