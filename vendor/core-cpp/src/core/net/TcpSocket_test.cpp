// SPDX-License-Identifier: Apache-2.0
//
// `ITcpSocket`: an unconnected TCP socket that binds, then connects or listens, and the
// `SocketOption`s it, a connected `ISocket` and an `IListener` set and read back.
//
// **Every case runs over the platform's real sockets**, because each property is the kernel's: which
// port a bind got, whether a port in TIME_WAIT may be bound again, what a buffer size reads back as.
// A double could only agree with itself.
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IAsyncAddressResolver.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/ITcpSocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/SocketOption.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/SplitSocket.hpp>
#include <core/net/detail/SocketOptionAccess.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/RawSockets.hpp>
#include <core/net/testing/SocketDecorator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/interfaces/catch_interfaces_capture.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using core::async::Task;
using core::net::AddressFamily;
using core::net::EventLoop;
using core::net::IListener;
using core::net::IpSocketAddress;
using core::net::Ipv4Address;
using core::net::Ipv6Address;
using core::net::ISocket;
using core::net::ITcpSocket;
using core::net::NetError;
using core::net::NetErrorCode;
using core::net::OptionScale;
using core::net::ResolvedEndpoint;
using core::net::SocketOption;
using core::net::SocketResult;

namespace
{

/// Which handle a platform hands out next.
enum class HandleOrder : std::uint8_t
{
    Unordered,   ///< Any (Windows' socket handles).
    LowestFirst, ///< The lowest free one, as POSIX requires of a descriptor.
};

#ifdef _WIN32
constexpr auto NativeHandleOrder = HandleOrder::Unordered;
#else
constexpr auto NativeHandleOrder = HandleOrder::LowestFirst;
#endif

/// @return The loopback address of @p family, at @p port.
[[nodiscard]] ResolvedEndpoint loopback(AddressFamily family, std::uint16_t port)
{
    if (family == AddressFamily::Ipv6)
        return core::net::toEndpoint(
            IpSocketAddress { .address = Ipv6Address { { 0, 0, 0, 0, 0, 0, 0, 1 } }, .port = port });
    return core::net::toEndpoint(
        IpSocketAddress { .address = Ipv4Address { { 127, 0, 0, 1 } }, .port = port });
}

/// @return The IP form of @p endpoint, which the case requires to exist.
[[nodiscard]] IpSocketAddress ipOf(std::expected<ResolvedEndpoint, NetError> const& endpoint)
{
    REQUIRE(endpoint.has_value());
    auto address = core::net::toIpSocketAddress(*endpoint);
    REQUIRE(address.has_value());
    return *address;
}

/// @return Whether @p result failed with @p code.
template <typename T>
[[nodiscard]] bool failedWith(std::expected<T, NetError> const& result, NetErrorCode code)
{
    return !result.has_value() && result.error().code == code;
}

/// @return Whether @p result is the socket's own refusal of a consumed or closed handle:
///         @c BadHandle with no system code, which a call that reached the OS would carry.
template <typename T>
[[nodiscard]] bool refusedAsConsumed(std::expected<T, NetError> const& result)
{
    return failedWith(result, NetErrorCode::BadHandle) && result.error().systemCode == 0;
}

/// A loop over the platform's default backend, kept alive with it.
struct Loop
{
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    EventLoop loop { *backend };
};

/// Runs @p task on @p loop, and fails the case -- naming it -- if the task has not finished within
/// @p limit: every kernel wait in this file is bounded here, so a socket that never becomes ready
/// fails one case rather than hanging the binary into its TIMEOUT.
/// @param loop The loop.
/// @param task The flow to run; stopped if it runs out of time.
/// @param limit How long it may take.
void blockOnWithin(EventLoop& loop,
                   Task<void> task,
                   std::chrono::milliseconds limit = std::chrono::seconds { 5 })
{
    if (!core::net::testing::blockOnWithin(loop, std::move(task), limit))
        FAIL("a socket wait in \"" << Catch::getResultCapture().getCurrentTestName() << "\" took longer than "
                                   << limit.count() << " ms");
}

/// @return An open, unbound TCP socket of @p family, or a SKIP where the host has no such family.
[[nodiscard]] std::unique_ptr<ITcpSocket> openOrSkip(EventLoop& loop, AddressFamily family)
{
    auto opened = core::net::openTcpSocket(loop, family);
    if (!opened.has_value() && family == AddressFamily::Ipv6)
        SKIP("this host has no IPv6: " << opened.error().toString());
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

/// @return A listener on @p family's loopback at an ephemeral port, made through @c ITcpSocket,
///         or a SKIP where the host has no IPv6 loopback.
[[nodiscard]] std::unique_ptr<IListener> listenOnLoopback(EventLoop& loop, AddressFamily family)
{
    auto socket = openOrSkip(loop, family);
    auto const bound = socket->bind(loopback(family, 0));
    if (!bound.has_value() && family == AddressFamily::Ipv6)
        SKIP("this host has no IPv6 loopback: " << bound.error().toString());
    REQUIRE(bound.has_value());
    auto listener = socket->listen(16);
    REQUIRE(listener.has_value());
    return std::move(*listener);
}

/// Connects @p socket to @p remote, then accepts on @p listener: the connect completes out of the
/// kernel's backlog before anything accepts, so the two run in sequence on one loop.
Task<void> connectAndAccept(ITcpSocket* socket,
                            ResolvedEndpoint remote,
                            IListener* listener,
                            SocketResult* connected,
                            SocketResult* accepted)
{
    *connected = co_await socket->connect(remote);
    if (!connected->has_value())
        co_return;
    *accepted = co_await listener->accept();
}

/// The two ends of one loopback connection.
struct Connection
{
    std::unique_ptr<ISocket> client;   ///< The end an @c ITcpSocket connected.
    std::unique_ptr<ISocket> accepted; ///< The end the listener handed out.
};

/// @return A connection from a fresh @c ITcpSocket of @p family to @p listener.
[[nodiscard]] Connection connectTo(EventLoop& loop, AddressFamily family, IListener& listener)
{
    auto socket = openOrSkip(loop, family);
    auto connected = SocketResult {};
    auto accepted = SocketResult {};
    blockOnWithin(
        loop,
        connectAndAccept(
            socket.get(), loopback(family, listener.boundPort()), &listener, &connected, &accepted));
    REQUIRE(connected.has_value());
    REQUIRE(accepted.has_value());
    return Connection { .client = std::move(*connected), .accepted = std::move(*accepted) };
}

/// What one option is set to, and what it must read back as.
struct OptionCase
{
    SocketOption option;
    std::uint64_t value;
};

/// **One row per option, and the case below requires one for EVERY option**, so an option added to
/// the enum without a value to test it with fails here rather than going untested.
///
/// The values are ones every platform keeps exactly: buffer sizes well inside Linux's default
/// `rmem_max`/`wmem_max` and above every minimum, a hop limit no stack rounds, keepalive intervals
/// in whole seconds.
constexpr auto OptionCases = std::array {
    OptionCase { .option = SocketOption::ReceiveBufferSize, .value = 65'536 },
    OptionCase { .option = SocketOption::SendBufferSize, .value = 65'536 },
    OptionCase { .option = SocketOption::UnicastHopLimit, .value = 42 },
    OptionCase { .option = SocketOption::KeepAliveEnabled, .value = 1 },
    OptionCase { .option = SocketOption::KeepAliveIdleTime, .value = 30 },
    OptionCase { .option = SocketOption::KeepAliveInterval, .value = 7 },
    OptionCase { .option = SocketOption::KeepAliveCount, .value = 5 },
};

/// @return Every option the enum names, in order.
[[nodiscard]] auto allOptions()
{
    return std::views::iota(0, static_cast<int>(SocketOption::Last))
           | std::views::transform([](int index) { return static_cast<SocketOption>(index); });
}

/// Sets every option through @p set and reads it back through @p get, which are one object's.
template <typename Set, typename Get>
void requireEveryOptionReadsBack(Set set, Get get)
{
    for (auto const option: allOptions())
    {
        auto const row = std::ranges::find(OptionCases, option, &OptionCase::option);
        INFO("option " << static_cast<int>(option));
        REQUIRE(row != OptionCases.end());
        auto const written = set(option, row->value);
        INFO((written.has_value() ? std::string {} : written.error().toString()));
        REQUIRE(written.has_value());
        auto const read = get(option);
        INFO((read.has_value() ? std::string {} : read.error().toString()));
        REQUIRE(read.has_value());
        CHECK(*read == row->value);
    }
}

/// @return The family a case runs over, from a generator.
[[nodiscard]] AddressFamily generatedFamily()
{
    return GENERATE(AddressFamily::Ipv4, AddressFamily::Ipv6);
}

/// A socket that answers every option with a fixed value and records what it was asked to set:
/// the one way to tell a forwarded option from the base class's @c Unsupported default.
class OptionRecordingSocket final: public core::net::testing::SocketDecorator
{
  public:
    using SocketDecorator::SocketDecorator;

    [[nodiscard]] std::expected<std::uint64_t, NetError> option(SocketOption option) const override
    {
        return static_cast<std::uint64_t>(option) + 100;
    }

    [[nodiscard]] std::expected<void, NetError> setOption(SocketOption option, std::uint64_t value) override
    {
        lastSet = std::pair { option, value };
        return {};
    }

    std::optional<std::pair<SocketOption, std::uint64_t>> lastSet; ///< What @c setOption last saw.
};

/// Whether an arranged connect stays outstanding, and to where.
struct PendingTarget
{
    std::unique_ptr<IListener> listener; ///< A listener whose backlog is full, if that is the arrangement.
    std::vector<std::unique_ptr<ISocket>> fill; ///< The connections that filled it.
    std::optional<ResolvedEndpoint> endpoint;   ///< Where a connect stays outstanding; unset if nowhere.
};

/// Fills a backlog-1 listener with bounded dials until one runs out of time, which is the proof the
/// next connect to it stays outstanding. Bounded per dial, so the arrangement cannot become the hang
/// it is arranging; a stack that over-accepts leaves @c PendingTarget::endpoint unset.
Task<void> arrangePendingTarget(EventLoop* loop, PendingTarget* out)
{
    auto socket = core::net::openTcpSocket(*loop, AddressFamily::Ipv4);
    if (!socket.has_value() || !(*socket)->bind(loopback(AddressFamily::Ipv4, 0)).has_value())
        co_return;
    auto listener = (*socket)->listen(1);
    if (!listener.has_value())
        co_return;
    out->listener = std::move(*listener);
    auto const port = out->listener->boundPort();

    constexpr auto Fill = 32;
    auto resolver = core::net::InlineAddressResolver {};
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, Fill))
    {
        auto dialled = co_await core::net::connect(
            loop,
            "127.0.0.1",
            port,
            &resolver,
            core::net::DialOptions { .connectTimeout = std::chrono::milliseconds { 300 } });
        if (!dialled.has_value())
        {
            if (dialled.error().code == NetErrorCode::Timeout)
                out->endpoint = loopback(AddressFamily::Ipv4, port);
            co_return;
        }
        out->fill.push_back(std::move(*dialled));
    }
}

} // namespace

TEST_CASE("A TCP socket bound to port 0 reports the port the kernel chose", "[net][tcp-socket][loopback]")
{
    auto const family = generatedFamily();
    auto context = Loop {};
    auto socket = openOrSkip(context.loop, family);

    // Unbound, the answer is the socket's own, on every platform: no wildcard-at-port-0 here and an
    // error there.
    CHECK(refusedAsConsumed(socket->localEndpoint()));

    auto const bound = socket->bind(loopback(family, 0));
    if (!bound.has_value() && family == AddressFamily::Ipv6)
        SKIP("this host has no IPv6 loopback: " << bound.error().toString());
    REQUIRE(bound.has_value());

    // A second bind is refused before the kernel, which would answer an `EINVAL` that reads like a
    // bad address.
    CHECK(refusedAsConsumed(socket->bind(loopback(family, 0))));

    auto const local = ipOf(socket->localEndpoint());
    CHECK(local.family() == family);
    CHECK(local.port != 0);
    CHECK(local.address == ipOf(loopback(family, 0)).address);
}

TEST_CASE("A TCP socket bound to a chosen port connects from it", "[net][tcp-socket][loopback]")
{
    auto const family = generatedFamily();
    auto context = Loop {};
    auto listener = listenOnLoopback(context.loop, family);

    // A port the kernel just handed out, then gave back: free, and chosen by this case rather than
    // by the connect.
    auto const chosen = [&] {
        auto probe = openOrSkip(context.loop, family);
        REQUIRE(probe->bind(loopback(family, 0)).has_value());
        return ipOf(probe->localEndpoint()).port;
    }();
    REQUIRE(chosen != 0);

    auto socket = openOrSkip(context.loop, family);
    REQUIRE(socket->bind(loopback(family, chosen)).has_value());
    auto connected = SocketResult {};
    auto accepted = SocketResult {};
    blockOnWithin(
        context.loop,
        connectAndAccept(
            socket.get(), loopback(family, listener->boundPort()), listener.get(), &connected, &accepted));
    REQUIRE(connected.has_value());
    REQUIRE(accepted.has_value());

    // The server sees the port the client chose, and so does the client.
    CHECK(ipOf((*accepted)->remoteEndpoint()).port == chosen);
    CHECK(ipOf((*connected)->localEndpoint()).port == chosen);
    CHECK(ipOf((*connected)->remoteEndpoint()).port == listener->boundPort());
}

TEST_CASE("A TCP socket rebinds a port in TIME_WAIT, and not one a listener holds",
          "[net][tcp-socket][loopback]")
{
    auto context = Loop {};
    auto& loop = context.loop;
    auto listener = listenOnLoopback(loop, AddressFamily::Ipv4);
    auto const port = listener->boundPort();

    SECTION("a second bind while the listener is live is AddressInUse")
    {
        auto second = openOrSkip(loop, AddressFamily::Ipv4);
        CHECK(failedWith(second->bind(loopback(AddressFamily::Ipv4, port)), NetErrorCode::AddressInUse));
    }

    SECTION("a fresh socket binds and listens once the server closed first")
    {
        auto connection = connectTo(loop, AddressFamily::Ipv4, *listener);

        // The server closes first, which leaves ITS end in TIME_WAIT once the client's FIN arrives:
        // the client reads the EOF, then closes too.
        connection.accepted->close();
        auto drain = [](ISocket* client) -> Task<void> {
            auto buffer = std::array<std::byte, 16> {};
            std::ignore = co_await client->read(buffer);
        };
        blockOnWithin(loop, drain(connection.client.get()));
        connection.client->close();
        loop.blockOn(core::net::testing::sleepFor(&loop, std::chrono::milliseconds { 50 }));
        listener->close();
        listener.reset();
        connection = Connection {};

        auto fresh = openOrSkip(loop, AddressFamily::Ipv4);
        auto const rebound = fresh->bind(loopback(AddressFamily::Ipv4, port));
        INFO((rebound.has_value() ? std::string {} : rebound.error().toString()));
        REQUIRE(rebound.has_value());
        auto relistened = fresh->listen(16);
        REQUIRE(relistened.has_value());
        CHECK((*relistened)->boundPort() == port);
    }
}

TEST_CASE("Every socket option sets and reads back on an unconnected socket, a connection and a listener",
          "[net][tcp-socket][socket-option][loopback]")
{
    auto const family = generatedFamily();
    auto context = Loop {};

    SECTION("an unconnected socket")
    {
        auto socket = openOrSkip(context.loop, family);
        requireEveryOptionReadsBack(
            [&](SocketOption option, std::uint64_t value) { return socket->setOption(option, value); },
            [&](SocketOption option) { return socket->option(option); });
    }
    SECTION("a listener")
    {
        auto listener = listenOnLoopback(context.loop, family);
        requireEveryOptionReadsBack(
            [&](SocketOption option, std::uint64_t value) { return listener->setOption(option, value); },
            [&](SocketOption option) { return listener->option(option); });
    }
    SECTION("both ends of a connection")
    {
        auto listener = listenOnLoopback(context.loop, family);
        auto connection = connectTo(context.loop, family, *listener);
        for (auto* const end: { connection.client.get(), connection.accepted.get() })
            requireEveryOptionReadsBack(
                [end](SocketOption option, std::uint64_t value) { return end->setOption(option, value); },
                [end](SocketOption option) { return end->option(option); });
    }
}

TEST_CASE("Options set before a connect carry over to the connection",
          "[net][tcp-socket][socket-option][loopback]")
{
    auto const family = generatedFamily();
    auto context = Loop {};
    auto listener = listenOnLoopback(context.loop, family);
    auto socket = openOrSkip(context.loop, family);

    // Values no platform defaults to, so a connection that merely kept its defaults fails here. The
    // connection is asked through its own route -- the family from `getsockname` -- and the socket
    // through the family it was opened with: an IPv6 connection that read the IPv4 hop limit row
    // would answer `IP_TTL`'s default rather than this.
    auto const values = std::array {
        OptionCase { .option = SocketOption::UnicastHopLimit, .value = 23 },
        OptionCase { .option = SocketOption::KeepAliveEnabled, .value = 1 },
        OptionCase { .option = SocketOption::KeepAliveIdleTime, .value = 41 },
        OptionCase { .option = SocketOption::KeepAliveInterval, .value = 11 },
        OptionCase { .option = SocketOption::KeepAliveCount, .value = 6 },
    };
    for (auto const& [option, value]: values)
        REQUIRE(socket->setOption(option, value).has_value());

    auto connected = SocketResult {};
    auto accepted = SocketResult {};
    blockOnWithin(
        context.loop,
        connectAndAccept(
            socket.get(), loopback(family, listener->boundPort()), listener.get(), &connected, &accepted));
    REQUIRE(connected.has_value());
    for (auto const& [option, value]: values)
    {
        INFO("option " << static_cast<int>(option));
        CHECK((*connected)->option(option) == value);
    }
}

TEST_CASE("A keepalive flag reads back as 0 or 1, the hop limit as set, and every option clamps a huge value",
          "[net][tcp-socket][socket-option]")
{
    auto const family = generatedFamily();
    auto context = Loop {};
    auto socket = openOrSkip(context.loop, family);

    // A kernel that reports the flag's own bit (macOS answers `SO_KEEPALIVE`'s value, 8) still reads
    // back as 1: the value is a flag, not a bit pattern.
    REQUIRE(socket->setOption(SocketOption::KeepAliveEnabled, 7).has_value());
    CHECK(socket->option(SocketOption::KeepAliveEnabled) == std::uint64_t { 1 });
    REQUIRE(socket->setOption(SocketOption::KeepAliveEnabled, 0).has_value());
    CHECK(socket->option(SocketOption::KeepAliveEnabled) == std::uint64_t { 0 });

    REQUIRE(socket->setOption(SocketOption::UnicastHopLimit, 1).has_value());
    CHECK(socket->option(SocketOption::UnicastHopLimit) == std::uint64_t { 1 });
    REQUIRE(socket->setOption(SocketOption::UnicastHopLimit, 255).has_value());
    CHECK(socket->option(SocketOption::UnicastHopLimit) == std::uint64_t { 255 });

    // Every option takes any value: one above the row's maximum is clamped to it, never refused
    // and never truncated (2^64 - 1 would truncate to -1). Beyond the buffers, which the kernel caps
    // further on its own, it reads back as exactly the row's maximum.
    for (auto const& row: core::net::socketOptionTable())
    {
        if (row.family != family)
            continue;
        INFO("option " << static_cast<int>(row.option));
        auto const set = socket->setOption(row.option, std::numeric_limits<std::uint64_t>::max());
        INFO((set.has_value() ? std::string {} : set.error().toString()));
        REQUIRE(set.has_value());
        auto const read = socket->option(row.option);
        REQUIRE(read.has_value());
        if (row.option == SocketOption::ReceiveBufferSize || row.option == SocketOption::SendBufferSize)
            CHECK(*read > 0);
        else
            CHECK(*read == row.maximum);
    }
}

TEST_CASE("Kernel values convert by the row's scale, and values to the kernel clamp to the row",
          "[net][socket-option]")
{
    using core::net::detail::fromKernel;
    using core::net::detail::toKernel;

    struct FromCase
    {
        OptionScale scale;
        int kernel;
        std::uint64_t expected;
    };
    // macOS answers SO_KEEPALIVE with the option's own bit, 8; Linux doubles a buffer.
    constexpr auto FromCases = std::array {
        FromCase { .scale = OptionScale::Flag, .kernel = 8, .expected = 1 },
        FromCase { .scale = OptionScale::Flag, .kernel = 0, .expected = 0 },
        FromCase { .scale = OptionScale::KernelDoubles, .kernel = 4096, .expected = 2048 },
        FromCase { .scale = OptionScale::Identity, .kernel = 4096, .expected = 4096 },
        FromCase { .scale = OptionScale::Seconds, .kernel = 30, .expected = 30 },
        FromCase { .scale = OptionScale::Identity, .kernel = -1, .expected = 0 },
        FromCase { .scale = OptionScale::KernelDoubles, .kernel = -4096, .expected = 0 },
    };
    for (auto const& [scale, kernel, expected]: FromCases)
    {
        INFO("scale " << static_cast<int>(scale) << " kernel " << kernel);
        CHECK(fromKernel(scale, kernel) == expected);
    }

    auto const row = core::net::SocketOptionRow { .option = SocketOption::KeepAliveCount,
                                                  .family = AddressFamily::Ipv4,
                                                  .scale = OptionScale::Identity,
                                                  .maximum = 127 };
    CHECK(toKernel(row, 5) == 5);
    CHECK(toKernel(row, 127) == 127);
    CHECK(toKernel(row, 128) == 127);
    CHECK(toKernel(row, (std::uint64_t { 1 } << 32U) + 5U) == 127);
    auto const wide = core::net::SocketOptionRow { .maximum = std::numeric_limits<std::uint64_t>::max() };
    CHECK(toKernel(wide, std::numeric_limits<std::uint64_t>::max()) == std::numeric_limits<int>::max());
}

TEST_CASE("Option rows cover every option for both families, once", "[net][socket-option]")
{
    auto const table = core::net::socketOptionTable();
    for (auto const option: allOptions())
        for (auto const family: { AddressFamily::Ipv4, AddressFamily::Ipv6 })
        {
            INFO("option " << static_cast<int>(option) << " family " << static_cast<int>(family));
            CHECK(std::ranges::count_if(table,
                                        [&](core::net::SocketOptionRow const& row) {
                                            return row.option == option && row.family == family;
                                        })
                  == 1);
        }

    // The keepalive intervals are whole seconds on every platform, which is what a caller converts to;
    // every row has a maximum to clamp to, inside `int`; a flag's is 1.
    for (auto const& row: table)
    {
        INFO("option " << static_cast<int>(row.option));
        if (row.option == SocketOption::KeepAliveIdleTime || row.option == SocketOption::KeepAliveInterval)
            CHECK(row.scale == OptionScale::Seconds);
        CHECK(row.maximum > 0);
        CHECK(row.maximum <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
        if (row.scale == OptionScale::Flag)
            CHECK(row.maximum == 1);
    }
}

TEST_CASE("A connected TCP socket is consumed: every later call is BadHandle", "[net][tcp-socket][loopback]")
{
    auto context = Loop {};
    auto listener = listenOnLoopback(context.loop, AddressFamily::Ipv4);
    auto socket = openOrSkip(context.loop, AddressFamily::Ipv4);
    auto const remote = loopback(AddressFamily::Ipv4, listener->boundPort());

    auto connected = SocketResult {};
    auto accepted = SocketResult {};
    blockOnWithin(context.loop,
                  connectAndAccept(socket.get(), remote, listener.get(), &connected, &accepted));
    REQUIRE(connected.has_value());

    auto again = SocketResult {};
    auto connectAgain = [](ITcpSocket* tcp, ResolvedEndpoint to, SocketResult* out) -> Task<void> {
        *out = co_await tcp->connect(to);
    };
    blockOnWithin(context.loop, connectAgain(socket.get(), remote, &again));
    CHECK(refusedAsConsumed(again));
    CHECK(refusedAsConsumed(socket->listen(1)));
    CHECK(refusedAsConsumed(socket->bind(loopback(AddressFamily::Ipv4, 0))));
    CHECK(refusedAsConsumed(socket->localEndpoint()));
    CHECK(refusedAsConsumed(socket->option(SocketOption::KeepAliveEnabled)));
    CHECK(refusedAsConsumed(socket->setOption(SocketOption::KeepAliveEnabled, 1)));
}

TEST_CASE("A listening TCP socket is consumed too", "[net][tcp-socket][loopback]")
{
    auto context = Loop {};
    auto socket = openOrSkip(context.loop, AddressFamily::Ipv4);
    REQUIRE(socket->bind(loopback(AddressFamily::Ipv4, 0)).has_value());
    auto listener = socket->listen(4);
    REQUIRE(listener.has_value());

    auto again = SocketResult {};
    auto connectAfterListen = [](ITcpSocket* tcp, ResolvedEndpoint to, SocketResult* out) -> Task<void> {
        *out = co_await tcp->connect(to);
    };
    blockOnWithin(
        context.loop,
        connectAfterListen(socket.get(), loopback(AddressFamily::Ipv4, (*listener)->boundPort()), &again));
    CHECK(refusedAsConsumed(again));
    CHECK(refusedAsConsumed(socket->listen(4)));
}

TEST_CASE("A connect task that never runs still closes its socket", "[net][tcp-socket]")
{
    auto context = Loop {};

    // The handle the platform hands out next. Where it hands out the lowest free one (POSIX), a
    // socket that was not closed makes the next one differ; a platform that does not is not asked.
    // Windows is not asked even where the probe below happens to agree: its handle values follow no
    // order, and the loop's own threads open handles between two probes (CI: 0x238 after 0x1e4).
    auto const nextHandle = [] {
        return core::net::testing::RawSocket { core::net::testing::openRawTcpSocket() }.get();
    };
    auto const lowestFirst = [&] {
        auto first = core::net::testing::RawSocket { core::net::testing::openRawTcpSocket() };
        // Held open across the probe, so the freed handle is not merely the highest one.
        auto const second = core::net::testing::RawSocket { core::net::testing::openRawTcpSocket() };
        auto const held = first.get();
        core::net::testing::closeRawSocket(first.release());
        return NativeHandleOrder == HandleOrder::LowestFirst && held != core::platform::InvalidHandle
               && second.get() != held && nextHandle() == held;
    }();
    auto const before = nextHandle();

    auto socket = openOrSkip(context.loop, AddressFamily::Ipv4);
    {
        // Consumed at the call, not at the first resumption: the task holds the handle, and
        // dropping the task unstarted closes it.
        [[maybe_unused]] auto const unstarted = socket->connect(loopback(AddressFamily::Ipv4, 9));
        CHECK(refusedAsConsumed(socket->localEndpoint()));
    }
    CHECK(refusedAsConsumed(socket->listen(1)));
    if (lowestFirst)
        CHECK(nextHandle() == before);
}

TEST_CASE("Listening on an unbound TCP socket binds the wildcard address", "[net][tcp-socket][loopback]")
{
    auto const family = generatedFamily();
    auto context = Loop {};
    auto socket = openOrSkip(context.loop, family);
    auto listener = socket->listen(8);
    if (!listener.has_value() && family == AddressFamily::Ipv6)
        SKIP("this host cannot listen on IPv6: " << listener.error().toString());
    REQUIRE(listener.has_value());

    auto const local = ipOf((*listener)->localEndpoint());
    CHECK(local.family() == family);
    CHECK(local.port != 0);
    CHECK(local.port == (*listener)->boundPort());
    auto const wildcard = family == AddressFamily::Ipv6
                              ? std::variant<Ipv4Address, Ipv6Address> { Ipv6Address {} }
                              : std::variant<Ipv4Address, Ipv6Address> { Ipv4Address {} };
    CHECK(local.address == wildcard);

    // The wildcard bind is this library's, not the kernel's implicit one at `listen`: it set the
    // platform's reuse-address options first, as every bind here does.
    for (auto const option: core::net::detail::reuseAddressOptions())
    {
        auto const value = core::net::detail::getIntOption((*listener)->nativeHandle(), option);
        REQUIRE(value.has_value());
        CHECK(*value != 0); // macOS reports the option's own bit rather than 1
    }

    // Wildcard, so the loopback reaches it.
    auto connection = connectTo(context.loop, family, **listener);
    CHECK(ipOf(connection.accepted->localEndpoint()).port == local.port);
}

TEST_CASE("An IPv6 TCP socket is IPv6-only", "[net][tcp-socket][loopback]")
{
    auto context = Loop {};
    auto socket = openOrSkip(context.loop, AddressFamily::Ipv6);
    auto listener = socket->listen(8);
    if (!listener.has_value())
        SKIP("this host cannot listen on IPv6: " << listener.error().toString());
    REQUIRE(listener.has_value());

    // An IPv4 dial to the IPv6 wildcard's port finds nothing there: the socket does not take IPv4
    // on every platform alike, whatever the system's `bindv6only` default.
    auto client = openOrSkip(context.loop, AddressFamily::Ipv4);
    auto answer = SocketResult {};
    auto dial = [](ITcpSocket* tcp, ResolvedEndpoint to, SocketResult* out) -> Task<void> {
        *out = co_await tcp->connect(to);
    };
    blockOnWithin(context.loop,
                  dial(client.get(), loopback(AddressFamily::Ipv4, (*listener)->boundPort()), &answer));
    CHECK(failedWith(answer, NetErrorCode::ConnRefused));
}

TEST_CASE("A stop during a TCP connect throws, and closing a listener ends its accept with Cancelled",
          "[net][tcp-socket][stop][loopback]")
{
    auto context = Loop {};
    auto& loop = context.loop;

    SECTION("a stop of the flow unwinds a connect left outstanding")
    {
        auto target = PendingTarget {};
        blockOnWithin(loop, arrangePendingTarget(&loop, &target), std::chrono::seconds { 20 });
        if (!target.endpoint.has_value())
            SKIP("this stack completes a connect whose listener never accepts, so none could be left "
                 "outstanding to stop");

        auto socket = openOrSkip(loop, AddressFamily::Ipv4);
        auto answer = SocketResult {};
        auto threw = false;
        auto dial = [](ITcpSocket* tcp, ResolvedEndpoint to, SocketResult* out, bool* unwound) -> Task<void> {
            try
            {
                *out = co_await tcp->connect(to);
            }
            catch (core::async::OperationCancelled const&)
            {
                *unwound = true;
            }
        };
        // The loser of an `anyOf` is stopped through its token and nothing else, so only the
        // connect's own stop handling can end it; without it this case hangs into the binary's
        // TIMEOUT rather than passing.
        loop.blockOn(core::net::testing::anyOf(
            dial(socket.get(), *target.endpoint, &answer, &threw),
            core::net::testing::sleepFor(&loop, std::chrono::milliseconds { 100 })));
        CHECK(threw);
        CHECK(loop.parkedWaiterCount() == 0);
    }

    SECTION("closing the listener resolves a pending accept with Cancelled, as a value")
    {
        auto listener = listenOnLoopback(loop, AddressFamily::Ipv4);
        auto accepted = SocketResult {};
        auto accept = [](IListener* from, SocketResult* out) -> Task<void> {
            *out = co_await from->accept();
        };
        auto closeAfter = [](EventLoop* lp, IListener* target) -> Task<void> {
            co_await lp->delay(std::chrono::milliseconds { 50 });
            target->close();
        };
        blockOnWithin(
            loop,
            core::net::testing::allOf(accept(listener.get(), &accepted), closeAfter(&loop, listener.get())));
        CHECK(failedWith(accepted, NetErrorCode::Cancelled));
    }
}

TEST_CASE("Options default to Unsupported, and every decorator forwards them", "[net][socket-option]")
{
    auto pair = core::net::testing::InMemorySocketPair::create();
    auto& plain = *pair.client;

    CHECK(failedWith(plain.option(SocketOption::KeepAliveEnabled), NetErrorCode::Unsupported));
    CHECK(failedWith(plain.setOption(SocketOption::KeepAliveEnabled, 1), NetErrorCode::Unsupported));

    auto recording = OptionRecordingSocket { plain };

    SECTION("SocketDecorator")
    {
        // Named as an ISocket, or the braces would pick the decorator's deleted copy constructor.
        auto& inner = static_cast<ISocket&>(recording);
        auto decorator = core::net::testing::SocketDecorator { inner };
        CHECK(decorator.option(SocketOption::KeepAliveCount)
              == std::uint64_t { static_cast<std::uint64_t>(SocketOption::KeepAliveCount) + 100 });
        REQUIRE(decorator.setOption(SocketOption::SendBufferSize, 4096).has_value());
        CHECK(recording.lastSet == std::pair { SocketOption::SendBufferSize, std::uint64_t { 4096 } });
    }
    SECTION("SplitSocket, from its read half")
    {
        auto owned = std::make_unique<OptionRecordingSocket>(plain);
        auto* const readHalf = owned.get();
        auto split = core::net::SplitSocket { std::move(owned), std::move(pair.server) };
        CHECK(split.option(SocketOption::UnicastHopLimit)
              == std::uint64_t { static_cast<std::uint64_t>(SocketOption::UnicastHopLimit) + 100 });
        REQUIRE(split.setOption(SocketOption::KeepAliveIdleTime, 9).has_value());
        CHECK(readHalf->lastSet == std::pair { SocketOption::KeepAliveIdleTime, std::uint64_t { 9 } });
    }
    SECTION("a closed socket refuses without the OS")
    {
        auto context = Loop {};
        auto listener = listenOnLoopback(context.loop, AddressFamily::Ipv4);
        auto connection = connectTo(context.loop, AddressFamily::Ipv4, *listener);
        connection.client->close();
        listener->close();
        CHECK(refusedAsConsumed(connection.client->option(SocketOption::KeepAliveEnabled)));
        CHECK(refusedAsConsumed(connection.client->setOption(SocketOption::KeepAliveEnabled, 1)));
        CHECK(refusedAsConsumed(listener->option(SocketOption::KeepAliveEnabled)));
        CHECK(refusedAsConsumed(listener->setOption(SocketOption::KeepAliveEnabled, 1)));
    }
}
