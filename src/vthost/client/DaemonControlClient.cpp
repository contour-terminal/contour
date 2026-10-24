// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/DaemonControlClient.hpp>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/WriteQueue.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <vthost/DaemonControlWire.hpp>
#include <vthost/PduPump.hpp>
#include <vthost/WireEnum.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/Negotiation.hpp>

namespace vthost::client
{

namespace
{
    /// How long to wait between two attempts to reach a daemon that is going away.
    constexpr auto ReconnectInterval = std::chrono::milliseconds { 20 };

    /// The bytes a Control connection may queue before it is severed: a hello and one request.
    constexpr auto ControlQueueBytes = std::size_t { 64 } * 1024;

    /// What each failure says, in front of its detail. Single-sourced so a new code is a row here.
    struct ControlErrorText
    {
        ControlErrorCode code;
        std::string_view lead;
    };

    constexpr auto ControlErrorTexts = std::to_array<ControlErrorText>({
        { ControlErrorCode::NotRunning, "no daemon is running" },
        { ControlErrorCode::Refused, "" }, // the refusal text stands on its own
        { ControlErrorCode::Unsupported, "the daemon does not support this request" },
        { ControlErrorCode::Timeout, "the daemon did not answer in time" },
        { ControlErrorCode::Transport, "the connection to the daemon failed" },
    });

    /// What a Control conversation is for.
    enum class Purpose : uint8_t
    {
        Status = 0,
        Stop,
    };

    /// How far a Control conversation has come.
    enum class Phase : uint8_t
    {
        AwaitHello = 0,
        AwaitAnswer,
        Done,
    };

    /// Everything a conversation learns, filled in by its frame handler.
    struct Conversation
    {
        Phase phase = Phase::AwaitHello;
        std::optional<ControlError> failure;
        std::optional<proto::DaemonStatusReply> reply;
        proto::CapabilitySet offered; ///< What the daemon's ServerHello offered.
    };

    /// What a Control conversation came to.
    struct Answer
    {
        std::optional<proto::DaemonStatusReply> reply; ///< The status, for Purpose::Status.
        proto::CapabilitySet offered;                  ///< What the daemon's ServerHello offered.
    };

    [[nodiscard]] ControlError fail(ControlErrorCode code, std::string detail)
    {
        return ControlError { .code = code, .detail = std::move(detail) };
    }

    /// Encodes @p pdu under @p serial and queues it.
    /// @return Whether the queue took it.
    [[nodiscard]] bool enqueue(core::net::WriteQueue& writer, uint64_t serial, proto::DecodedPdu const& pdu)
    {
        auto sink = proto::Writer {};
        proto::encodePdu(sink, serial, pdu);
        auto const bytes = sink.view();
        return writer.enqueue(std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() });
    }

    /// Runs one Control conversation over @p socket: hello, one request, then its answer (status) or the
    /// daemon's EOF (stop).
    /// @return The status for Purpose::Status, nothing for Purpose::Stop, with what the daemon offered; or
    ///         the failure.
    core::async::Task<std::expected<Answer, ControlError>> converse(
        core::net::EventLoop* loop,
        std::unique_ptr<core::net::ISocket> socket,
        Purpose purpose,
        proto::StopReason reason)
    {
        auto writer = core::net::WriteQueue { *loop, socket.get(), ControlQueueBytes };
        auto const offered = proto::CapabilitySet::of({ proto::Capability::DaemonControl });
        auto conversation = Conversation {};
        auto const ours = proto::SupportedVersions;

        auto const sendFailure = [&] {
            return fail(ControlErrorCode::Transport, writer.describeRefusal());
        };
        if (!enqueue(writer,
                     1,
                     proto::DecodedPdu { proto::ClientHello { .versions = ours,
                                                              .capabilities = offered,
                                                              .role = proto::ConnectionRole::Control } }))
        {
            writer.close();
            co_return std::unexpected { sendFailure() };
        }

        auto const onFrame = [&](proto::DecodedFrame const& frame) {
            if (conversation.phase == Phase::AwaitHello)
            {
                auto const* hello = std::get_if<proto::ServerHello>(&frame.pdu);
                if (hello == nullptr)
                    return true;
                auto const judged = judgeServerHello(ours, offered, *hello);
                if (!judged)
                {
                    conversation.failure = fail(ControlErrorCode::Refused,
                                                proto::describe(judged.error(), proto::PeerRole::Daemon));
                    return false;
                }
                conversation.offered = hello->capabilities;
                if (!hello->capabilities.contains(proto::Capability::DaemonControl))
                {
                    conversation.failure = fail(ControlErrorCode::Unsupported,
                                                hello->releaseVersion.empty()
                                                    ? std::string { "an earlier release" }
                                                    : std::format("release {}", hello->releaseVersion));
                    return false;
                }
                auto const request =
                    purpose == Purpose::Status
                        ? proto::DecodedPdu { proto::DaemonStatusRequest {} }
                        : proto::DecodedPdu { proto::StopDaemon { .reason = std::to_underlying(reason) } };
                if (!enqueue(writer, 2, request))
                {
                    conversation.failure = sendFailure();
                    return false;
                }
                conversation.phase = Phase::AwaitAnswer;
                return true;
            }
            if (std::holds_alternative<proto::Unsupported>(frame.pdu))
            {
                conversation.failure =
                    fail(ControlErrorCode::Unsupported, "the daemon answered the request Unsupported");
                return false;
            }
            if (purpose == Purpose::Status)
                if (auto const* reply = std::get_if<proto::DaemonStatusReply>(&frame.pdu))
                {
                    conversation.reply = *reply;
                    conversation.phase = Phase::Done;
                    return false;
                }
            return true; // anything else is not an answer to this request
        };

        auto const outcome = co_await pumpPdus(socket.get(), onFrame);
        writer.close();

        if (conversation.failure)
            co_return std::unexpected { *conversation.failure };
        if (conversation.reply)
            co_return Answer { .reply = conversation.reply, .offered = std::move(conversation.offered) };
        // The stop is done once the daemon hangs up after having been asked.
        if (purpose == Purpose::Stop && conversation.phase == Phase::AwaitAnswer
            && outcome.stop == PumpStop::PeerClosed)
            co_return Answer { .reply = std::nullopt, .offered = std::move(conversation.offered) };
        co_return std::unexpected { fail(
            ControlErrorCode::Transport,
            describe(outcome).value_or(std::string { "the daemon closed the connection" })) };
    }

    /// Connects, mapping "nobody listens" to NotRunning.
    core::async::Task<std::expected<std::unique_ptr<core::net::ISocket>, ControlError>> open(
        SocketDaemonControl::Connector const* connect)
    {
        auto socket = co_await (*connect)();
        if (!socket)
            co_return std::unexpected { std::move(socket.error()) };
        co_return std::move(*socket);
    }

    core::async::Task<std::expected<DaemonStatus, ControlError>> queryStatus(
        core::net::EventLoop* loop, SocketDaemonControl::Connector const* connect)
    {
        auto socket = co_await open(connect);
        if (!socket)
            co_return std::unexpected { socket.error() };
        auto answer = co_await converse(loop, std::move(*socket), Purpose::Status, proto::StopReason::User);
        if (!answer)
            co_return std::unexpected { answer.error() };
        auto status = fromWire(*answer->reply);
        if (!status)
            co_return std::unexpected { fail(
                ControlErrorCode::Transport,
                "the daemon's status names a lifecycle this client does not know") };
        status->capabilities = std::move(answer->offered);
        co_return *std::move(status);
    }

    core::async::Task<std::expected<void, ControlError>> requestStop(
        core::net::EventLoop* loop, SocketDaemonControl::Connector const* connect, proto::StopReason reason)
    {
        auto socket = co_await open(connect);
        if (!socket)
            co_return std::unexpected { socket.error() };
        auto stopped = co_await converse(loop, std::move(*socket), Purpose::Stop, reason);
        if (!stopped)
            co_return std::unexpected { stopped.error() };
        // The daemon hung up; it has stopped once nothing accepts a connection any more.
        while (true)
        {
            auto again = co_await open(connect);
            if (!again)
            {
                if (again.error().code == ControlErrorCode::NotRunning)
                    co_return std::expected<void, ControlError> {};
                co_return std::unexpected { again.error() };
            }
            (*again).reset(); // it still accepts: it is not gone yet
            co_await loop->delay(ReconnectInterval);
        }
    }

    /// Runs @p work to completion on @p loop, bounded by @p timeout.
    template <typename T>
    std::expected<T, ControlError> bounded(core::net::EventLoop& loop,
                                           core::async::Task<std::expected<T, ControlError>> work,
                                           std::chrono::milliseconds timeout)
    {
        auto result = loop.blockOn(core::net::withTimeout(&loop, std::move(work), timeout));
        if (!result)
            return std::unexpected { fail(ControlErrorCode::Timeout, "") };
        return std::move(*result);
    }

    /// Connects through @p transport, with "nobody listens" told from every other failure by the typed error.
    /// Only the socket is kept: a Control conversation is one request long, and the transports it runs over
    /// today carry no lifetime of their own.
    core::async::Task<std::expected<std::unique_ptr<core::net::ISocket>, ControlError>> connectControl(
        core::net::EventLoop* loop, ITransportFactory* transport)
    {
        auto connection = co_await transport->connect(*loop);
        if (!connection)
            co_return std::unexpected { fail(connection.error().kind == TransportErrorKind::DaemonAbsent
                                                 ? ControlErrorCode::NotRunning
                                                 : ControlErrorCode::Transport,
                                             std::move(connection.error().message)) };
        co_return std::move((*connection)->socket);
    }

    /// The daemon control of a transport, with the loop it runs on.
    class OwnedLoopDaemonControl final: public DaemonControlPort
    {
      public:
        OwnedLoopDaemonControl(std::shared_ptr<ITransportFactory> transport,
                               std::chrono::milliseconds timeout):
            _loop(*_backend),
            _inner(
                _loop,
                [loop = &_loop, transport = std::move(transport)] {
                    return connectControl(loop, transport.get());
                },
                timeout)
        {
        }

        [[nodiscard]] std::expected<DaemonStatus, ControlError> status() override { return _inner.status(); }

        [[nodiscard]] std::expected<void, ControlError> stop(proto::StopReason reason) override
        {
            return _inner.stop(reason);
        }

      private:
        std::unique_ptr<core::net::IoBackend> _backend = core::net::makeDefaultBackend();
        core::net::EventLoop _loop;
        SocketDaemonControl _inner;
    };
} // namespace

std::string ControlError::toString() const
{
    auto const row = std::ranges::find(ControlErrorTexts, code, &ControlErrorText::code);
    auto const lead = row != ControlErrorTexts.end() ? row->lead : std::string_view {};
    if (lead.empty())
        return detail;
    if (detail.empty())
        return std::string { lead };
    return std::format("{} ({})", lead, detail);
}

std::optional<DaemonStatus> fromWire(proto::DaemonStatusReply const& reply)
{
    auto const lifecycle = enumUpTo(reply.lifecycle, DaemonLifecycle::ExitWhenIdle);
    if (!lifecycle)
        return std::nullopt;
    return DaemonStatus { .releaseVersion = reply.releaseVersion,
                          .processId = reply.processId,
                          .uptime = std::chrono::seconds { static_cast<std::chrono::seconds::rep>(
                              reply.uptimeSeconds) },
                          .socketPath = std::filesystem::path { reply.socketPath },
                          .executablePath = std::filesystem::path { reply.executablePath },
                          .logFile = std::filesystem::path { reply.logFile },
                          .sessions = reply.sessions,
                          .clients = reply.clients,
                          .lifecycle = *lifecycle };
}

SocketDaemonControl::SocketDaemonControl(core::net::EventLoop& loop,
                                         Connector connect,
                                         std::chrono::milliseconds timeout):
    _loop(loop), _connect(std::move(connect)), _timeout(timeout)
{
}

std::expected<DaemonStatus, ControlError> SocketDaemonControl::status()
{
    return bounded(_loop, queryStatus(&_loop, &_connect), _timeout);
}

std::expected<void, ControlError> SocketDaemonControl::stop(proto::StopReason reason)
{
    return bounded(_loop, requestStop(&_loop, &_connect, reason), _timeout);
}

std::unique_ptr<DaemonControlPort> makeDaemonControl(std::shared_ptr<ITransportFactory> transport,
                                                     std::chrono::milliseconds timeout)
{
    return std::make_unique<OwnedLoopDaemonControl>(std::move(transport), timeout);
}

} // namespace vthost::client
