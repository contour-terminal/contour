// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/ClientSupervisor.hpp>

#include <core/net/InterruptibleSleep.hpp>

#include <ranges>
#include <utility>

#include <vthost/Logging.hpp>
#include <vthost/proto/Negotiation.hpp>

namespace vthost::client
{

ClientSupervisor::ClientSupervisor(core::net::EventLoop& loop,
                                   std::shared_ptr<ITransportFactory> transport,
                                   ClientFactory makeClient,
                                   proto::AttachIntent firstIntent,
                                   BackoffSchedule backoff,
                                   StateHandler onState):
    _loop(loop),
    _transport(std::move(transport)),
    _makeClient(std::move(makeClient)),
    _firstIntent(std::move(firstIntent)),
    _backoff(backoff),
    _onState(std::move(onState))
{
}

void ClientSupervisor::enter(ConnectionState state, std::string const& reason)
{
    if (_onState)
        _onState(state, reason);
}

proto::AttachIntent ClientSupervisor::resumeIntent() const
{
    // A client that follows every window keeps doing so: Resume would narrow it to the windows it had, and a
    // window another client creates later would never reach it. The daemon's followedWindows still names
    // which of its old windows came back.
    if (_firstIntent.kind == proto::AttachIntentKind::AllWindows)
        return _firstIntent;
    return proto::AttachIntent { .kind = proto::AttachIntentKind::Resume, .windows = _followed };
}

void ClientSupervisor::stop()
{
    // The backoff wait parks on this token; requesting stop wakes it at once.
    _stop.request_stop();
    if (_current)
        _current->detach();
}

core::async::Task<ClientSupervisor::Outcome> ClientSupervisor::attempt(proto::AttachIntent intent)
{
    auto connection = co_await _transport->connect(_loop);
    if (!connection)
        co_return Outcome { .ending = Ending::TransportFailed, .error = std::move(connection.error()) };
    if (_stop.stop_requested())
        co_return Outcome {};

    _currentLifetime = (*connection)->lifetime;
    _current = _makeClient(std::move(*connection), std::move(intent));
    // Once the daemon accepted the hello: what it follows now is what a reconnect resumes.
    _current->setHandshakeHandler([this] { enter(ConnectionState::Connected); });
    co_await _current->run();

    auto outcome = Outcome {};
    if (_stop.stop_requested())
        outcome = Outcome {};
    else if (auto const& refusal = _current->refusal())
        outcome = Outcome { .ending = Ending::Refused,
                            .error = { .kind = TransportErrorKind::Refused,
                                       .message = proto::describe(*refusal, proto::PeerRole::Daemon) } };
    else if (_current->connected())
        outcome = Outcome { .ending = Ending::Lost,
                            .error = { .kind = TransportErrorKind::Unreachable,
                                       .message = _current->lossReason().value_or(
                                           "the connection to the daemon was lost") } };
    else
        outcome = Outcome { .ending = Ending::Unaccepted,
                            .error = { .kind = TransportErrorKind::Protocol,
                                       .message = "the connection closed before the daemon answered" } };
    if (_current->connected())
        _followed = _current->followedWindows();

    // The client first, holding the socket, then what carries it.
    _current.reset();
    _currentLifetime.reset();
    co_return outcome;
}

core::async::Task<ClientSupervisor::Outcome> ClientSupervisor::reconnect()
{
    for (auto const retry: std::views::iota(uint32_t { 1 }))
    {
        auto const wake = co_await core::net::interruptibleSleepUntil(
            &_loop, _stop.get_token(), _loop.clock().now() + backoffDelay(_backoff, retry));
        if (wake == core::net::WakeReason::Cancelled || _stop.stop_requested())
            co_return Outcome {};

        auto outcome = co_await attempt(resumeIntent());
        switch (outcome.ending)
        {
            case Ending::TransportFailed:
                if (outcome.error.kind == TransportErrorKind::Cancelled)
                    co_return Outcome {};
                if (retryOf(outcome.error.kind) == TransportRetry::Permanent)
                    co_return outcome;
                clientLog()(
                    "reconnect: {} (attempt {}): {}", _transport->describe(), retry, outcome.error.message);
                break;
            case Ending::Unaccepted:
                clientLog()(
                    "reconnect: {} (attempt {}): {}", _transport->describe(), retry, outcome.error.message);
                break;
            case Ending::Refused:
            case Ending::Lost:
            case Ending::Stopped: co_return outcome;
        }
    }
    co_return Outcome {};
}

core::async::Task<void> ClientSupervisor::run()
{
    enter(ConnectionState::Connecting);
    auto outcome = co_await attempt(_firstIntent);
    if (outcome.ending != Ending::Lost)
    {
        // The first connect is not retried: the caller decides what to fall back to.
        auto const cancelled = outcome.ending == Ending::Stopped
                               || (outcome.ending == Ending::TransportFailed
                                   && outcome.error.kind == TransportErrorKind::Cancelled);
        if (cancelled)
            enter(ConnectionState::Stopped);
        else
            enter(ConnectionState::Failed, outcome.error.message);
        co_return;
    }

    while (outcome.ending == Ending::Lost)
    {
        clientLog()("reconnect: {}: {}", _transport->describe(), outcome.error.message);
        enter(ConnectionState::Reconnecting, outcome.error.message);
        outcome = co_await reconnect();
    }
    if (outcome.ending == Ending::Stopped)
        enter(ConnectionState::Stopped);
    else
        enter(ConnectionState::Refused, outcome.error.message);
}

} // namespace vthost::client
