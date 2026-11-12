// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/WorkspaceLister.hpp>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <QtCore/QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QTest>
#include <vthost/Transport.hpp>

using namespace std::chrono_literals;

namespace
{

/// What a transport saw of the one connect it was asked for.
struct ConnectTrace
{
    std::atomic<bool> started = false; ///< The connect began.
    std::atomic<bool> unwound = false; ///< Its frame went, cancelled, before it answered.
};

/// Marks @c ConnectTrace::unwound as it is destroyed: on a coroutine's frame, as the frame goes.
class UnwindMark
{
  public:
    explicit UnwindMark(ConnectTrace* trace): _trace(trace) {}
    ~UnwindMark() { _trace->unwound = true; }
    UnwindMark(UnwindMark const&) = delete;
    UnwindMark& operator=(UnwindMark const&) = delete;
    UnwindMark(UnwindMark&&) = delete;
    UnwindMark& operator=(UnwindMark&&) = delete;

  private:
    ConnectTrace* _trace;
};

/// A transport whose connect never answers by itself, as a dial to a daemon that never accepts, or one that
/// waits for the user to trust the daemon, does.
class NeverConnecting final: public vthost::ITransportFactory
{
  public:
    explicit NeverConnecting(ConnectTrace& trace): _trace(&trace) {}

    [[nodiscard]] core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connect(core::net::EventLoop& loop) override
    {
        return waitForever(&loop, _trace);
    }

    [[nodiscard]] std::string describe() const override { return "never:"; }

  private:
    static core::async::Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    waitForever(core::net::EventLoop* loop, ConnectTrace* trace)
    {
        auto const mark = UnwindMark { trace };
        trace->started = true;
        co_await loop->delay(std::chrono::hours { 1 });
        co_return std::unexpected(
            vthost::TransportError { .kind = vthost::TransportErrorKind::Unreachable, .message = "never" });
    }

    ConnectTrace* _trace;
};

/// A transport whose connection is open but never answered: the call waits on the daemon for ever.
class SilentDaemon final: public vthost::ITransportFactory
{
  public:
    /// @param heard Set once the daemon read anything from the client: its hello.
    explicit SilentDaemon(std::atomic<bool>& heard): _heard(&heard) {}

    [[nodiscard]] core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connect(core::net::EventLoop& loop) override
    {
        auto pair = *core::net::testing::makeSocketPair(loop);
        loop.spawn(holdOpen(std::move(pair.first), _heard));
        return settled(std::make_unique<vthost::Connection>(
            vthost::Connection { .lifetime = {}, .socket = std::move(pair.second), .token = {} }));
    }

    [[nodiscard]] std::string describe() const override { return "silent:"; }

  private:
    static core::async::Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    settled(std::unique_ptr<vthost::Connection> connection)
    {
        co_return std::move(connection);
    }

    /// Reads what the client sends, and answers nothing, until the client goes.
    static core::async::Task<void> holdOpen(std::unique_ptr<core::net::ISocket> socket,
                                            std::atomic<bool>* heard)
    {
        auto buffer = std::vector<std::byte>(4096);
        while (true)
        {
            auto const count = co_await socket->read(buffer);
            if (!count || *count == 0)
                co_return;
            *heard = true;
        }
    }

    std::atomic<bool>* _heard;
};

} // namespace

TEST_CASE("a lister destroyed while its dial waits ends the dial and returns at once",
          "[contour][remote][lister][threading]")
{
    auto trace = ConnectTrace {};
    auto const transport = std::make_shared<NeverConnecting>(trace);
    auto delivered = false;
    auto lister = contour::remote::makeControlWorkspaceLister(10s);
    lister->list(contour::remote::ListTarget { .address = {}, .transport = transport },
                 [&delivered](contour::remote::WorkspaceListing) { delivered = true; });
    REQUIRE(QTest::qWaitFor([&trace] { return trace.started.load(); }, 5000));

    auto const before = std::chrono::steady_clock::now();
    lister.reset();
    // The dial was given up, and its frame gone, before the destruction returned -- promptly: nothing it
    // borrowed (a pin store, a prompt router) is touched after its owner goes.
    CHECK(trace.unwound);
    CHECK(std::chrono::steady_clock::now() - before < 2s);
    QCoreApplication::processEvents();
    CHECK_FALSE(delivered);
}

TEST_CASE("a lister destroyed while its call waits on the daemon returns at once",
          "[contour][remote][lister][threading]")
{
    auto heard = std::atomic<bool> { false };
    auto const transport = std::make_shared<SilentDaemon>(heard);
    auto lister = contour::remote::makeControlWorkspaceLister(1h);
    lister->list(contour::remote::ListTarget { .address = {}, .transport = transport },
                 [](contour::remote::WorkspaceListing) {});
    // Connected, and the hello read by a daemon that answers nothing: the call waits on it.
    REQUIRE(QTest::qWaitFor([&heard] { return heard.load(); }, 5000));

    auto const before = std::chrono::steady_clock::now();
    lister.reset();
    CHECK(std::chrono::steady_clock::now() - before < 2s);
}
