// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/DaemonControlClient.hpp>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/NetError.hpp>
#include <core/net/WriteQueue.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <ranges>
#include <stdexcept>
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
        { ControlErrorCode::LoginNeeded, "the login needs an answer from you" },
    });

    /// What a Control conversation is for.
    enum class Purpose : uint8_t
    {
        Status = 0,
        Stop,
        Upgrade,
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
        std::optional<proto::UpgradeDaemonReply> upgradeReply;
        proto::CapabilitySet offered; ///< What the daemon's ServerHello offered.
    };

    /// What a Control conversation came to.
    struct Answer
    {
        std::optional<proto::DaemonStatusReply> reply;         ///< The status, for Purpose::Status.
        std::optional<proto::UpgradeDaemonReply> upgradeReply; ///< The outcome, for Purpose::Upgrade.
        proto::CapabilitySet offered;                          ///< What the daemon's ServerHello offered.
    };

    /// @return Whether a conversation ended with the daemon hanging up, cleanly or by a reset.
    [[nodiscard]] bool hungUp(PumpResult const& outcome) noexcept
    {
        return outcome.stop == PumpStop::PeerClosed
               || (outcome.stop == PumpStop::TransportError && outcome.transportError
                   && outcome.transportError->code == core::net::NetErrorCode::ConnReset);
    }

    /// @return What a transport's failure is to a control caller: no daemon, a login that wants an answer, or
    ///         any other failure of the connection.
    [[nodiscard]] constexpr ControlErrorCode controlErrorCodeOf(TransportErrorKind kind) noexcept
    {
        switch (kind)
        {
            case TransportErrorKind::DaemonAbsent: return ControlErrorCode::NotRunning;
            // The login would ask the user something a check may not: the host has answered.
            case TransportErrorKind::AuthenticationRequired:
            case TransportErrorKind::HostKeyUnknown: return ControlErrorCode::LoginNeeded;
            // A changed key is no question to answer: it stays a failure, with its words.
            case TransportErrorKind::HostKeyChanged:
            case TransportErrorKind::Unreachable:
            case TransportErrorKind::Refused:
            case TransportErrorKind::RemoteProgramMissing:
            case TransportErrorKind::ProgramNotFound:
            case TransportErrorKind::Protocol:
            case TransportErrorKind::Cancelled:
            case TransportErrorKind::Misconfigured: break;
        }
        return ControlErrorCode::Transport;
    }

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

    /// @param purpose What a conversation is for.
    /// @return The capabilities it needs both ends to share: an upgrade needs LiveUpgrade besides.
    [[nodiscard]] proto::CapabilitySet capabilitiesFor(Purpose purpose)
    {
        if (purpose == Purpose::Upgrade)
            return proto::CapabilitySet::of(
                { proto::Capability::DaemonControl, proto::Capability::LiveUpgrade });
        return proto::CapabilitySet::of({ proto::Capability::DaemonControl });
    }

    /// @param offered What the daemon's ServerHello offered.
    /// @param needed What the conversation needs it to.
    /// @return Whether it offered all of it.
    [[nodiscard]] bool offersAll(proto::CapabilitySet const& offered, proto::CapabilitySet const& needed)
    {
        return std::ranges::all_of(needed.idents, [&offered](auto const ident) {
            return std::ranges::contains(offered.idents, ident);
        });
    }

    /// Runs one Control conversation over @p socket: hello, one request, then its answer (status, upgrade) or
    /// the daemon's EOF (stop).
    /// @param request The request, sent once the daemon's ServerHello offered what @p purpose needs.
    /// @return The status for Purpose::Status, the reply for Purpose::Upgrade, nothing for Purpose::Stop,
    ///         with what the daemon offered; or the failure.
    core::async::Task<std::expected<Answer, ControlError>> converse(
        core::net::EventLoop* loop,
        std::unique_ptr<core::net::ISocket> socket,
        Purpose purpose,
        proto::DecodedPdu request)
    {
        auto writer = core::net::WriteQueue { *loop, socket.get(), ControlQueueBytes };
        auto const offered = capabilitiesFor(purpose);
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
                if (!offersAll(hello->capabilities, offered))
                {
                    conversation.failure = fail(ControlErrorCode::Unsupported,
                                                hello->releaseVersion.empty()
                                                    ? std::string { "an earlier release" }
                                                    : std::format("release {}", hello->releaseVersion));
                    return false;
                }
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
            if (purpose == Purpose::Upgrade)
                if (auto const* reply = std::get_if<proto::UpgradeDaemonReply>(&frame.pdu))
                {
                    conversation.upgradeReply = *reply;
                    conversation.phase = Phase::Done;
                    return false;
                }
            return true; // anything else is not an answer to this request
        };

        auto const outcome = co_await pumpPdus(socket.get(), onFrame);
        writer.close();

        if (conversation.failure)
            co_return std::unexpected { *conversation.failure };
        if (conversation.reply || conversation.upgradeReply)
            co_return Answer { .reply = conversation.reply,
                               .upgradeReply = std::move(conversation.upgradeReply),
                               .offered = std::move(conversation.offered) };
        // The stop is done once the daemon hangs up after having been asked. A reset counts as hanging up
        // here: Windows resets a connection whose process closes it with input unread, and whether the
        // daemon is gone is what the reconnects that follow find out.
        if (purpose == Purpose::Stop && conversation.phase == Phase::AwaitAnswer && hungUp(outcome))
            co_return Answer { .reply = std::nullopt,
                               .upgradeReply = std::nullopt,
                               .offered = std::move(conversation.offered) };
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
        auto answer =
            co_await converse(loop, std::move(*socket), Purpose::Status, proto::DaemonStatusRequest {});
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
        auto stopped = co_await converse(loop,
                                         std::move(*socket),
                                         Purpose::Stop,
                                         proto::StopDaemon { .reason = std::to_underlying(reason) });
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

    core::async::Task<std::expected<proto::UpgradeDaemonReply, ControlError>> requestUpgrade(
        core::net::EventLoop* loop, SocketDaemonControl::Connector const* connect, std::string imageId)
    {
        auto socket = co_await open(connect);
        if (!socket)
            co_return std::unexpected { socket.error() };
        auto answer = co_await converse(loop,
                                        std::move(*socket),
                                        Purpose::Upgrade,
                                        proto::UpgradeDaemon { .imageId = std::move(imageId) });
        if (!answer)
            co_return std::unexpected { answer.error() };
        co_return *std::move(answer->upgradeReply);
    }

    /// Runs @p work to completion on @p loop, bounded by @p budget.
    template <typename T>
    std::expected<T, ControlError> bounded(core::net::EventLoop& loop,
                                           core::async::Task<std::expected<T, ControlError>> work,
                                           AttachBudget const& budget,
                                           TransportReturnMark* transportReturned)
    {
        // Each call connects afresh: what an earlier one saw says nothing about this one.
        if (transportReturned != nullptr)
            transportReturned->reset();
        auto result = loop.blockOn(withAttachBudget(&loop, std::move(work), budget, transportReturned));
        if (!result)
            return std::unexpected(fail(ControlErrorCode::Timeout, ""));
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
            co_return std::unexpected { fail(controlErrorCodeOf(connection.error().kind),
                                             std::move(connection.error().message)) };
        co_return std::move((*connection)->socket);
    }

    /// The daemon control of a transport, with the loop it runs on.
    class OwnedLoopDaemonControl final: public DaemonControlPort
    {
      public:
        OwnedLoopDaemonControl(std::shared_ptr<ITransportFactory> transport, AttachBudget budget):
            _loop(*_backend),
            _transport(std::make_shared<ConnectObservingFactory>(
                std::move(transport), [this] { _transportReturned.note(_loop.clock().now()); })),
            _inner(
                _loop,
                [loop = &_loop, transport = _transport] { return connectControl(loop, transport.get()); },
                std::move(budget),
                &_transportReturned)
        {
        }

        [[nodiscard]] std::expected<DaemonStatus, ControlError> status() override { return _inner.status(); }

        [[nodiscard]] std::expected<void, ControlError> stop(proto::StopReason reason) override
        {
            return _inner.stop(reason);
        }

        [[nodiscard]] std::expected<proto::UpgradeDaemonReply, ControlError> upgrade(
            std::string imageId) override
        {
            return _inner.upgrade(std::move(imageId));
        }

      private:
        std::unique_ptr<core::net::IoBackend> _backend = core::net::makeDefaultBackend();
        core::net::EventLoop _loop;
        TransportReturnMark _transportReturned; ///< Set on the loop's thread.
        std::shared_ptr<ITransportFactory> _transport;
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
    return DaemonStatus {
        .releaseVersion = reply.releaseVersion,
        .processId = reply.processId,
        .uptime = std::chrono::seconds { static_cast<std::chrono::seconds::rep>(reply.uptimeSeconds) },
        .socketPath = std::filesystem::path { reply.socketPath },
        .executablePath = std::filesystem::path { reply.executablePath },
        .logFile = std::filesystem::path { reply.logFile },
        .sessions = reply.sessions,
        .clients = reply.clients,
        .lifecycle = *lifecycle,
        .capabilities = {},
        .upgrade = reply.upgrade,
        .relocation =
            reply.relocation ? enumUpTo(*reply.relocation, RelocationRequest::Refused) : std::nullopt,
        .workspaces = reply.workspaces.transform([](uint32_t count) { return std::size_t { count }; }),
        .tlsFingerprint = reply.tlsFingerprint,
    };
}

SocketDaemonControl::SocketDaemonControl(core::net::EventLoop& loop,
                                         Connector connect,
                                         std::chrono::milliseconds timeout):
    SocketDaemonControl(loop, std::move(connect), AttachBudget { timeout }, nullptr)
{
}

SocketDaemonControl::SocketDaemonControl(core::net::EventLoop& loop,
                                         Connector connect,
                                         AttachBudget budget,
                                         TransportReturnMark* transportReturned):
    _loop(loop),
    _connect(std::move(connect)),
    _budget(std::move(budget)),
    _transportReturned(transportReturned)
{
    if (!_budget.connect && _transportReturned == nullptr)
        throw std::invalid_argument("a control call with no connect bound needs to be told when the "
                                    "transport returned");
}

std::expected<DaemonStatus, ControlError> SocketDaemonControl::status()
{
    return bounded(_loop, queryStatus(&_loop, &_connect), _budget, _transportReturned);
}

std::expected<void, ControlError> SocketDaemonControl::stop(proto::StopReason reason)
{
    return bounded(_loop, requestStop(&_loop, &_connect, reason), _budget, _transportReturned);
}

std::expected<proto::UpgradeDaemonReply, ControlError> SocketDaemonControl::upgrade(std::string imageId)
{
    return bounded(_loop, requestUpgrade(&_loop, &_connect, std::move(imageId)), _budget, _transportReturned);
}

std::unique_ptr<DaemonControlPort> makeDaemonControl(std::shared_ptr<ITransportFactory> transport,
                                                     AttachBudget timeout)
{
    return std::make_unique<OwnedLoopDaemonControl>(std::move(transport), std::move(timeout));
}

} // namespace vthost::client
