// SPDX-License-Identifier: Apache-2.0
//
// What the POSIX accept loop does with each error `accept(2)` can leave: park, take the next
// connection at once, or report -- and, when it reports, in which category. The loop used to retry
// only EINTR and ECONNABORTED and hand every other errno over as `SystemError`, which a caller must
// read as exhaustion and back off on: so a packet filter's `EPERM` for ONE connection, or any of the
// pending-connection errors Linux's accept(2) says to treat like EAGAIN, cost a listener its backoff.
// Nothing in the suite can provoke most of these from a real kernel, so the decision is asserted
// row by row, together with the table that classifies what it reports.
#include <core/async/Task.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/AcceptPolicy.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/HttpServer.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/NetError.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/posix/AcceptLoop.hpp>
#include <core/net/testing/ScriptedBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using core::net::AcceptAttempt;
using core::net::EventLoop;
using core::net::IAcceptCall;
using core::net::NetErrorCode;
using core::net::detail::acceptFailureOf;
using core::net::detail::AcceptStep;

namespace
{

/// One `errno` a failed `accept(2)` can leave, what the loop does, and -- when it reports -- what
/// the caller is told.
struct Row
{
    int systemCode;    ///< The `errno` value.
    AcceptStep step;   ///< What the accept loop does.
    NetErrorCode code; ///< The category reported; `Ok` where the loop does not report.
};

constexpr auto Rows = std::array {
    // Nothing pending.
    Row { .systemCode = EAGAIN, .step = AcceptStep::Park, .code = NetErrorCode::Ok },
    Row { .systemCode = EWOULDBLOCK, .step = AcceptStep::Park, .code = NetErrorCode::Ok },
    // Taken again at once: an interrupted call, and the pending-connection errors no category names.
    Row { .systemCode = EINTR, .step = AcceptStep::Retry, .code = NetErrorCode::Ok },
    Row { .systemCode = ECONNABORTED, .step = AcceptStep::Retry, .code = NetErrorCode::Ok },
    Row { .systemCode = EPROTO, .step = AcceptStep::Retry, .code = NetErrorCode::Ok },
    Row { .systemCode = ENOPROTOOPT, .step = AcceptStep::Retry, .code = NetErrorCode::Ok },
    // Per-connection answers that HAVE a category, reported as it, never as SystemError.
    Row { .systemCode = EPERM, .step = AcceptStep::Report, .code = NetErrorCode::PermissionDenied },
    Row { .systemCode = EHOSTUNREACH, .step = AcceptStep::Report, .code = NetErrorCode::HostUnreach },
    Row { .systemCode = ENETUNREACH, .step = AcceptStep::Report, .code = NetErrorCode::HostUnreach },
    Row { .systemCode = EHOSTDOWN, .step = AcceptStep::Report, .code = NetErrorCode::HostUnreach },
    Row { .systemCode = ENETDOWN, .step = AcceptStep::Report, .code = NetErrorCode::HostUnreach },
#ifdef ENONET
    Row { .systemCode = ENONET, .step = AcceptStep::Report, .code = NetErrorCode::HostUnreach },
#endif
    // Also what a socket that is not a stream answers on every call, so reported rather than retried.
    Row { .systemCode = EOPNOTSUPP, .step = AcceptStep::Report, .code = NetErrorCode::Unsupported },
    // Exhaustion is ResourceExhausted, which is what tells a caller to back off: never SystemError,
    // which may be permanent and must not be backed off on forever (`AcceptPolicy.hpp`).
    Row { .systemCode = EMFILE, .step = AcceptStep::Report, .code = NetErrorCode::ResourceExhausted },
    Row { .systemCode = ENFILE, .step = AcceptStep::Report, .code = NetErrorCode::ResourceExhausted },
    Row { .systemCode = ENOBUFS, .step = AcceptStep::Report, .code = NetErrorCode::ResourceExhausted },
    Row { .systemCode = ENOMEM, .step = AcceptStep::Report, .code = NetErrorCode::ResourceExhausted },
    // A listener that is not one, whichever errno says so: `EINVAL` is a socket that is not listening,
    // accept's own row, and must not read as exhaustion (see `AcceptOwnClassifications`).
    Row { .systemCode = EBADF, .step = AcceptStep::Report, .code = NetErrorCode::BadHandle },
    Row { .systemCode = ENOTSOCK, .step = AcceptStep::Report, .code = NetErrorCode::BadHandle },
    Row { .systemCode = EINVAL, .step = AcceptStep::Report, .code = NetErrorCode::BadHandle },
    // A pending timeout on one new connection is that connection's failure, not the loop's poll
    // ticking: accept's own row, because the shared table answers `ETIMEDOUT` as a deadline.
    Row { .systemCode = ETIMEDOUT, .step = AcceptStep::Report, .code = NetErrorCode::HostUnreach },
#ifdef ENOSR
    Row { .systemCode = ENOSR, .step = AcceptStep::Report, .code = NetErrorCode::ResourceExhausted },
#endif
};

} // namespace

TEST_CASE("Every error accept(2) can leave is parked on, retried or reported as its category",
          "[net][errors]")
{
    for (auto const& row: Rows)
    {
        INFO("errno " << row.systemCode);
        auto const failure = acceptFailureOf(row.systemCode);
        CHECK(failure.step == row.step);
        CHECK(failure.code == row.code);
    }
}

namespace
{

/// A script entry that stands for "a connection arrives": the scripted call hands out a fresh
/// connected descriptor in its place. Any other entry is a failure with that `errno`.
constexpr int Connects = 0;

/// `IAcceptCall` answering from a script, one entry per call: a failure `errno`, or `Connects`.
/// A script that has run out answers `EBADF`, which the loop reports, so a loop that retries what
/// it should have reported ends instead of spinning -- and the call count says so.
class ScriptedAcceptCall final: public IAcceptCall
{
  public:
    explicit ScriptedAcceptCall(std::vector<int> script): _script { std::move(script) } {}

    ScriptedAcceptCall(ScriptedAcceptCall const&) = delete;
    ScriptedAcceptCall& operator=(ScriptedAcceptCall const&) = delete;
    ScriptedAcceptCall(ScriptedAcceptCall&&) = delete;
    ScriptedAcceptCall& operator=(ScriptedAcceptCall&&) = delete;

    ~ScriptedAcceptCall() override
    {
        for (auto const fd: _peers)
            ::close(fd);
    }

    [[nodiscard]] AcceptAttempt accept(int /*listenFd*/,
                                       sockaddr_storage& /*peer*/,
                                       socklen_t& /*peerLen*/) noexcept override
    {
        auto const entry = _calls < _script.size() ? _script[_calls] : EBADF;
        ++_calls;
        if (entry != Connects)
            return AcceptAttempt { .fd = -1, .error = entry };
        auto pair = std::array<int, 2> { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) != 0)
            return AcceptAttempt { .fd = -1, .error = errno };
        ::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK);
        _peers.push_back(pair[1]);
        return AcceptAttempt { .fd = pair[0], .error = 0 };
    }

    /// @return How many times the loop called accept.
    [[nodiscard]] std::size_t calls() const noexcept { return _calls; }

  private:
    std::vector<int> _script;
    std::vector<int> _peers;
    std::size_t _calls = 0;
};

/// One path through the accept loop: what accept(2) answers, call by call, and what the loop
/// returns and after how many calls.
struct LoopCase
{
    std::string_view name;             ///< What the case is.
    std::vector<int> script;           ///< The answers, in order.
    std::optional<NetErrorCode> error; ///< The reported category, or nothing for an accepted socket.
    std::size_t calls;                 ///< How many accepts the loop made.
};

} // namespace

TEST_CASE("The accept loop retries, reports and parks as the decision says, through the system call",
          "[net][errors]")
{
    auto const cases = std::array {
        // Retried: each dequeued a connection that had already failed; the next one is served.
        LoopCase { .name = "retried errors are taken again at once",
                   .script = { EPROTO, ECONNABORTED, ENOPROTOOPT, EINTR, Connects },
                   .error = std::nullopt,
                   .calls = 5 },
        // Reported at once, in its category, with no second call.
        LoopCase { .name = "a filtered connection is reported, not retried",
                   .script = { EPERM, Connects },
                   .error = NetErrorCode::PermissionDenied,
                   .calls = 1 },
        LoopCase { .name = "a pending network error with a category is reported",
                   .script = { ENETUNREACH, Connects },
                   .error = NetErrorCode::HostUnreach,
                   .calls = 1 },
        LoopCase { .name = "a socket that is not listening is a bad handle",
                   .script = { EINVAL, Connects },
                   .error = NetErrorCode::BadHandle,
                   .calls = 1 },
        LoopCase { .name = "exhaustion is reported for the caller to back off on",
                   .script = { EMFILE, Connects },
                   .error = NetErrorCode::ResourceExhausted,
                   .calls = 1 },
        // Parked: nothing pending; the listener is readable, so the park returns and accepts.
        LoopCase { .name = "nothing pending parks, then accepts",
                   .script = { EAGAIN, Connects },
                   .error = std::nullopt,
                   .calls = 2 },
    };

    for (auto const& loopCase: cases)
    {
        INFO(loopCase.name);
        auto source = core::net::makeBackend(core::net::preferredBackendKind());
        REQUIRE(source != nullptr);
        auto loop = EventLoop { *source };

        // The "listener": a descriptor the park can wait on, made readable up front so a park
        // returns at once rather than waiting for a connection nobody will make.
        auto listenPair = std::array<int, 2> { -1, -1 };
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, listenPair.data()) == 0);
        REQUIRE(::fcntl(listenPair[0], F_SETFL, ::fcntl(listenPair[0], F_GETFL, 0) | O_NONBLOCK) == 0);
        auto const byte = std::byte { 1 };
        REQUIRE(::write(listenPair[1], &byte, 1) == 1);

        auto scripted = ScriptedAcceptCall { loopCase.script };
        auto const lifetime = std::make_shared<int>(0);
        auto const closed = false;
        auto const listenFd = listenPair[0];
        auto result = loop.blockOn(core::net::acceptOne(
            &loop, &listenFd, &closed, std::weak_ptr<void const> { lifetime }, &scripted));

        auto const calls = scripted.calls();
        auto const accepted = result.has_value();
        auto const reported =
            accepted ? std::optional<NetErrorCode> {} : std::optional { result.error().code };
        ::close(listenPair[0]);
        ::close(listenPair[1]);

        CHECK(calls == loopCase.calls);
        CHECK(accepted == !loopCase.error.has_value());
        // `Ok` stands for "nothing reported" on both sides, so a mismatch prints two categories
        // rather than two unprintable optionals.
        CHECK(reported.value_or(NetErrorCode::Ok) == loopCase.error.value_or(NetErrorCode::Ok));
    }
}

namespace
{

/// A real backend that refuses ONE `attach`, the next one, with a chosen reason: how the kernel
/// answers a listener's registration under memory or descriptor pressure, which no case can provoke
/// on demand. Everything else reaches the real backend.
class RefusingOnceBackend final: public core::net::IoBackend
{
  public:
    /// @param inner The backend everything is forwarded to; must outlive this.
    explicit RefusingOnceBackend(core::net::IoBackend& inner) noexcept: _inner { inner } {}

    /// Makes the next @c attach refuse with @p reason.
    /// @param reason What the refusal says.
    void refuseNextAttach(core::net::NetError reason) { _refusal = std::move(reason); }

    /// @return How many refusals were answered.
    [[nodiscard]] int refused() const noexcept { return _refused; }

    [[nodiscard]] core::net::BackendKind kind() const noexcept override { return _inner.kind(); }

    [[nodiscard]] std::expected<void, core::net::NetError> attach(
        core::net::ReadinessHandler& handler) override
    {
        if (_refusal.has_value())
        {
            ++_refused;
            auto reason = std::move(*_refusal);
            _refusal.reset();
            return std::unexpected { std::move(reason) };
        }
        return _inner.attach(handler);
    }

    [[nodiscard]] std::expected<void, core::net::NetError> setInterest(core::net::ReadinessHandler& handler,
                                                                       core::net::Interest interest) override
    {
        return _inner.setInterest(handler, interest);
    }

    void detach(core::net::ReadinessHandler& handler) noexcept override { _inner.detach(handler); }

    [[nodiscard]] core::net::WaitResult wait(std::optional<core::platform::SteadyDuration> timeout) override
    {
        return _inner.wait(timeout);
    }

    void wake() noexcept override { _inner.wake(); }

  private:
    core::net::IoBackend& _inner;
    std::optional<core::net::NetError> _refusal;
    int _refused = 0;
};

/// Drains @p socket to EOF into @p out.
core::async::Task<void> drainInto(core::net::ISocket* socket, std::string* out)
{
    auto chunk = std::array<std::byte, 4096> {};
    while (true)
    {
        auto const got = co_await socket->read(chunk);
        if (!got.has_value() || *got == 0)
            co_return;
        out->append(reinterpret_cast<char const*>(chunk.data()), *got);
    }
}

/// Waits a millisecond -- so `serve` has parked first, and met the refusal -- then makes one GET.
core::async::Task<void> getAfterServeParked(EventLoop* loop, std::uint16_t port, std::string* out)
{
    co_await loop->delay(std::chrono::milliseconds { 1 });
    auto connected = co_await core::net::connect(loop, "127.0.0.1", port);
    if (!connected.has_value())
        co_return;
    auto socket = std::move(*connected);
    auto const request = std::string_view { "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n" };
    std::ignore = co_await socket->write(std::as_bytes(std::span { request }));
    co_await drainInto(socket.get(), out);
}

} // namespace

TEST_CASE("A listener the loop refuses to watch is reported as the refusal says, never thrown",
          "[net][errors][accept-policy]")
{
    // `waitReadable` throws `FdRegistrationFailed` when the backend refuses the registration. Let
    // out of `accept()`, it ended an accept loop as an exception nobody reported. Reported, it
    // carries the refusal's own code -- already classified by the backend's socket-error table --
    // because a refusal for want of kernel memory or descriptors is exhaustion, which a loop backs
    // off on; only a refusal that says the descriptor is bad, or says nothing, is a dead listener.
    struct Row
    {
        core::net::NetErrorCode refusal;  ///< What the backend's refusal says.
        core::net::NetErrorCode reported; ///< What the accept must report.
    };
    auto wrong = std::string {};
    for (auto const row:
         { Row { .refusal = NetErrorCode::ResourceExhausted, .reported = NetErrorCode::ResourceExhausted },
           Row { .refusal = NetErrorCode::SystemError, .reported = NetErrorCode::SystemError },
           Row { .refusal = NetErrorCode::BadHandle, .reported = NetErrorCode::BadHandle },
           Row { .refusal = NetErrorCode::Ok, .reported = NetErrorCode::BadHandle },
           Row { .refusal = NetErrorCode::Cancelled, .reported = NetErrorCode::SystemError } })
    {
        auto source = core::net::testing::ScriptedBackend {};
        source.refuseNextAttach(core::net::makeNetError(row.refusal, 0, "refused"));
        auto loop = EventLoop { source };
        auto listenPair = std::array<int, 2> { -1, -1 };
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, listenPair.data()) == 0);
        REQUIRE(::fcntl(listenPair[0], F_SETFL, ::fcntl(listenPair[0], F_GETFL, 0) | O_NONBLOCK) == 0);

        // Nothing pending, so the accept parks -- and the park is what the backend refuses.
        auto scripted = ScriptedAcceptCall { { EAGAIN } };
        auto const lifetime = std::make_shared<int>(0);
        auto const closed = false;
        auto const listenFd = listenPair[0];
        auto const accept = [](EventLoop* l,
                               int const* fd,
                               bool const* isClosed,
                               std::weak_ptr<void const> alive,
                               IAcceptCall* call) -> core::async::Task<std::optional<NetErrorCode>> {
            try
            {
                auto result = co_await core::net::acceptOne(l, fd, isClosed, std::move(alive), call);
                co_return result.has_value() ? std::optional<NetErrorCode> {}
                                             : std::optional { result.error().code };
            }
            catch (core::net::FdRegistrationFailed const&)
            {
                co_return std::optional { NetErrorCode::Last }; // escaped: stands for "thrown"
            }
        };
        auto const reported = loop.blockOn(
            accept(&loop, &listenFd, &closed, std::weak_ptr<void const> { lifetime }, &scripted));
        ::close(listenPair[0]);
        ::close(listenPair[1]);

        auto const got = reported.has_value() ? std::string { core::net::toString(*reported) }
                                              : std::string { "accepted" };
        if (!reported.has_value() || *reported != row.reported)
            wrong += std::string { core::net::toString(row.refusal) } + " was reported as " + got + "; ";
    }
    INFO(wrong);
    CHECK(wrong.empty());
}

TEST_CASE("serve backs off on a registration refused for want of memory, and serves again with its port open",
          "[net][http][accept-policy]")
{
    // The refusal arrives at a real listener on a real loop: the next accept parks again, and is
    // served. Taken for a dead listener instead, the refusal would make `serve` close its own port
    // for good under kernel memory pressure -- the outage the accept policy exists to prevent --
    // where Windows, refusing a park, backs off.
    auto real = core::net::makeDefaultBackend();
    REQUIRE(real != nullptr);
    auto backend = RefusingOnceBackend { *real };
    auto loop = EventLoop { backend };
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();
    auto const closed = (*listener)->closeToken();
    backend.refuseNextAttach(core::net::makeNetError(NetErrorCode::ResourceExhausted, ENOMEM, "epoll_ctl"));

    auto events = std::vector<core::net::AcceptLoopEvent> {};
    auto reply = std::string {};
    auto handler = core::net::HttpHandler { [](core::net::HttpRequest const& request) {
        return core::net::HttpResponse::ok("served:" + request.path);
    } };
    auto reporting = core::net::AcceptLoopReporting {
        .surface = "http",
        .onEvent = [&events](core::net::AcceptLoopEvent const& event) { events.push_back(event); },
    };
    auto run = [](EventLoop* l,
                  core::net::IListener* lis,
                  core::net::HttpHandler h,
                  core::net::AcceptLoopReporting r,
                  std::uint16_t p,
                  std::string* out) -> core::async::Task<void> {
        static_cast<void>(co_await core::async::whenAny(
            core::net::serve(l, lis, std::move(h), {}, std::move(r)), getAfterServeParked(l, p, out)));
    };
    auto const ended = loop.blockOn(core::net::withTimeout(
        &loop,
        run(&loop, listener->get(), std::move(handler), std::move(reporting), port, &reply),
        std::chrono::seconds { 10 }));
    REQUIRE(ended);

    auto wrong = std::string {};
    if (backend.refused() != 1)
        wrong += "the backend refused " + std::to_string(backend.refused()) + " times; ";
    if (closed.stop_requested())
        wrong += "serve closed its own listener; ";
    if (!reply.ends_with("served:/hello"))
        wrong += "the client read \"" + reply + "\"; ";
    if (events.size() != 1 || events.front().kind != core::net::AcceptLoopEventKind::Warning
        || events.front().error.code != NetErrorCode::ResourceExhausted)
        wrong += std::to_string(events.size()) + " reports, the first "
                 + (events.empty() ? std::string { "none" } : events.front().line) + "; ";
    INFO(wrong);
    CHECK(wrong.empty());
}
