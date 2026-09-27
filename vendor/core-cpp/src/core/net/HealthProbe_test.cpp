// SPDX-License-Identifier: Apache-2.0
//
// The health probe against core-cpp's own `HttpServer`, a raw responder, a silent peer and nothing
// at all. The probe BLOCKS the calling thread, so the peer runs on a loop of its own on a second
// thread; every case below asserts what distinguishes rather than what a probe that answered
// `false` to everything would also satisfy.
//
// Origin: fastcached `src/FastCache/Net/HealthProbe_test.cpp`
// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`), rewritten against `core::net::serve` in place of
// fastcached's `AdminHttpServer`. Upstream's cases asserted only the probe's `bool`, and needed
// flags to prove a peer had answered at all (fastcached#1141); `probeHttpStatus` returns the
// status, so the peer having answered IS the assertion.
#include <core/async/Task.hpp>
#include <core/net/HealthProbe.hpp>
#include <core/net/HttpServer.hpp>
#include <core/net/IListener.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/Sockets.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

using namespace std::chrono_literals;
using core::async::Task;
using core::net::HttpRequest;
using core::net::HttpResponse;
using core::net::IListener;
using core::net::ISocket;
using core::net::NetErrorCode;

namespace
{

/// A loopback listener whose flows run on a loop on a thread of its own, torn down in the one safe
/// order: close and stop are asked FOR on the loop's thread, the thread is joined, and only then are
/// the listener and whatever a flow held destroyed -- with the loop no longer running.
class ServedLoop
{
  public:
    ServedLoop()
    {
        auto listener = core::net::listen(_loop, "127.0.0.1", 0);
        REQUIRE(listener.has_value());
        _listener = std::move(*listener);
    }

    ServedLoop(ServedLoop const&) = delete;
    ServedLoop& operator=(ServedLoop const&) = delete;
    ServedLoop(ServedLoop&&) = delete;
    ServedLoop& operator=(ServedLoop&&) = delete;

    ~ServedLoop()
    {
        if (!_thread.joinable())
            return;
        _loop.post([this] {
            _listener->close();
            _loop.stop();
        });
        _thread.join();
    }

    /// Spawns @p flow and starts the loop's thread. Spawned before the thread exists, so the loop is
    /// touched by one thread at a time.
    void run(Task<void> flow)
    {
        _loop.spawn(std::move(flow));
        _thread = std::thread { [this] { _loop.run(); } };
    }

    [[nodiscard]] IListener* listener() const noexcept { return _listener.get(); }
    [[nodiscard]] std::uint16_t port() const noexcept { return _listener->boundPort(); }

    /// Keeps a connection open until teardown. Called only on the loop's thread.
    void hold(std::unique_ptr<ISocket> socket) noexcept { _held = std::move(socket); }

  private:
    // Declared in the order that makes the implicit destruction safe: a held socket goes before the
    // loop it is pinned to. The thread is joined explicitly, in the destructor's body, before any
    // member goes: a `std::thread` rather than a `std::jthread`, because AppleClang's libc++ has no
    // `<stop_token>` and so no `jthread`.
    core::net::PlatformLoop _loop;
    std::unique_ptr<IListener> _listener;
    std::unique_ptr<ISocket> _held;
    std::thread _thread;
};

/// Answers `/healthz` with 200 and anything else with 404.
[[nodiscard]] HttpResponse healthz(HttpRequest const& request)
{
    if (request.path == "/healthz")
        return HttpResponse::ok("ok");
    return HttpResponse::withStatus(404, "no such path");
}

/// Accepts one connection, reads the request, and answers with @p reply verbatim.
Task<void> respondRaw(IListener* listener, std::string reply)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
        co_return;
    auto request = std::array<std::byte, 512> {};
    std::ignore = co_await (*accepted)->read(request);
    std::ignore = co_await (*accepted)->write(std::as_bytes(std::span { reply }));
    (*accepted)->close();
}

/// Accepts one connection and holds it open, saying nothing.
Task<void> acceptAndHold(IListener* listener, ServedLoop* served)
{
    auto accepted = co_await listener->accept();
    if (accepted.has_value())
        served->hold(std::move(*accepted));
}

} // namespace

TEST_CASE("The health probe reads a live server's status, and 200 is the only healthy one", "[net][health]")
{
    auto served = ServedLoop {};
    served.run(core::net::serve(served.listener(), &healthz));

    // The status, not a bool: the peer having answered is part of what is asserted, so a probe
    // that failed to connect cannot pass for one that read a refusal.
    auto const live = core::net::probeHttpStatus("127.0.0.1", served.port(), "/healthz");
    INFO("probe: " << (live.has_value() ? std::to_string(*live) : live.error().toString()));
    REQUIRE(live.has_value());
    CHECK(*live == 200);
    CHECK(core::net::httpHealthProbe("127.0.0.1", served.port(), "/healthz"));

    // The discrimination half: a path that 404s is not healthy.
    auto const missing = core::net::probeHttpStatus("127.0.0.1", served.port(), "/nope");
    REQUIRE(missing.has_value());
    CHECK(*missing == 404);
    CHECK_FALSE(core::net::httpHealthProbe("127.0.0.1", served.port(), "/nope"));
}

TEST_CASE("The health probe rejects a 500 whose body contains \" 200 \"", "[net][health]")
{
    // An implementation that searched the response for " 200 " declared this peer healthy. The
    // status line is parsed strictly.
    auto served = ServedLoop {};
    served.run(respondRaw(served.listener(),
                          "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 27\r\n\r\n"
                          "expected 200 OK got 5xx err"));

    auto const status = core::net::probeHttpStatus("127.0.0.1", served.port(), "/healthz");
    REQUIRE(status.has_value());
    CHECK(*status == 500);
}

TEST_CASE("The health probe refuses a status of four digits whose first three are 200", "[net][health]")
{
    // What follows the three digits must end them, or `2000` would read as `200`.
    auto served = ServedLoop {};
    served.run(respondRaw(served.listener(), "HTTP/1.1 2000 Odd\r\n\r\n"));

    auto const status = core::net::probeHttpStatus("127.0.0.1", served.port(), "/healthz");
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == NetErrorCode::Unsupported);
}

TEST_CASE("The health probe times out, rather than hangs, against a peer that accepts and says nothing",
          "[net][health]")
{
    constexpr auto Timeout = 500ms;
    auto served = ServedLoop {};
    served.run(acceptAndHold(served.listener(), &served));

    auto const started = std::chrono::steady_clock::now();
    auto const status = core::net::probeHttpStatus("127.0.0.1", served.port(), "/healthz", Timeout);
    auto const elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(status.has_value());
    CAPTURE(status.error().toString());
    CHECK(core::net::isDeadlineExpiry(status.error().code));
    // The FLOOR is what discriminates: a peer that accepted and hung up answers in well under a
    // millisecond and satisfies any ceiling. A steady-clock interval only grows on a slow host, so
    // a floor cannot be flaked by one.
    CHECK(elapsed >= Timeout / 2);
    CHECK(elapsed < 10s);
}

TEST_CASE("The health probe fails, quickly, when nothing is listening", "[net][health]")
{
    auto loop = core::net::PlatformLoop {};
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();
    (*listener)->close();

    constexpr auto Timeout = 500ms;
    auto const started = std::chrono::steady_clock::now();
    CHECK_FALSE(core::net::httpHealthProbe("127.0.0.1", port, "/healthz", Timeout));
    CHECK(std::chrono::steady_clock::now() - started < Timeout * 20);
}
