// SPDX-License-Identifier: Apache-2.0
#include <core/net/ISocket.hpp>
#include <core/net/IoAwaitable.hpp>
#include <core/net/SocketDeadline.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>

using core::net::armSocketDeadline;
using core::net::ISocket;
using core::net::SocketDeadlineTarget;

namespace
{

/// A socket that records, at the moment it is closed, what the deadline's flag said.
///
/// That moment is the whole of the ordering promise: a caller resumed BY the close must find the
/// reason already recorded, so the flag has to be set before the close is issued rather than
/// after it returns. Reading the flag once the timer's callback has finished could not tell the
/// two orders apart.
class CloseRecorder final: public ISocket
{
  public:
    CloseRecorder(std::unique_ptr<ISocket> inner, SocketDeadlineTarget const* target) noexcept:
        _inner(std::move(inner)), _target(target)
    {
    }

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        return _inner->read(buffer);
    }

    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        return _inner->write(buffer);
    }

    void close() noexcept override
    {
        ++closes;
        expiredAtClose = _target->expired;
        _inner->close();
    }

    [[nodiscard]] bool isClosed() const noexcept override { return _inner->isClosed(); }

    std::size_t closes = 0;
    std::optional<bool> expiredAtClose;

  private:
    std::unique_ptr<ISocket> _inner;
    SocketDeadlineTarget const* _target;
};

} // namespace

TEST_CASE("a non-positive ceiling arms nothing", "[net]")
{
    // The decision this helper exists to hold in one place. The arithmetic says the opposite of
    // what the value means: a zero ceiling puts the deadline at `now()`, so the socket would die
    // on the loop's next turn — a knob documented as "no ceiling" that turns off the CONNECTION.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto pair = core::net::testing::InMemorySocketPair::create();
    auto target = SocketDeadlineTarget { .socket = pair.client.get() };

    CHECK_FALSE(armSocketDeadline(&loop, std::chrono::milliseconds { 0 }, &target).has_value());
    CHECK_FALSE(armSocketDeadline(&loop, std::chrono::milliseconds { -1 }, &target).has_value());
    CHECK_FALSE(armSocketDeadline(nullptr, std::chrono::milliseconds { 100 }, &target).has_value());
    CHECK(loop.pendingTimers() == 0);

    loop.drain();
    CHECK_FALSE(pair.client->isClosed());
    CHECK_FALSE(target.expired);

    // And a positive one does arm, so the three refusals above are refusals rather than a helper
    // that arms nothing at all.
    auto const armed = armSocketDeadline(&loop, std::chrono::milliseconds { 100 }, &target);
    CHECK(armed.has_value());
    CHECK(loop.pendingTimers() == 1);
}

TEST_CASE("an expired deadline records why before it closes the socket", "[net]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto pair = core::net::testing::InMemorySocketPair::create();

    auto target = SocketDeadlineTarget {};
    auto recorder = CloseRecorder { std::move(pair.client), &target };
    target.socket = &recorder;

    auto const timer = armSocketDeadline(&loop, std::chrono::milliseconds { 100 }, &target);
    REQUIRE(timer.has_value());

    clock.advance(std::chrono::milliseconds { 99 });
    loop.drain();
    CHECK(recorder.closes == 0);
    CHECK_FALSE(target.expired);

    clock.advance(std::chrono::milliseconds { 1 });
    loop.drain();
    CHECK(recorder.closes == 1);
    CHECK(target.expired);
    // The ordering promise itself: at the instant of the close, the reason was already there.
    CHECK(recorder.expiredAtClose == std::optional<bool> { true });
}

TEST_CASE("a deadline dropped before it expires closes nothing and records nothing", "[net]")
{
    // The exchange finished in time, so the caller drops the timer. What must not happen is a
    // close — or an `expired` — arriving afterwards and blaming a slow exchange for a peer that
    // went away for its own reasons.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto pair = core::net::testing::InMemorySocketPair::create();

    auto target = SocketDeadlineTarget {};
    auto recorder = CloseRecorder { std::move(pair.client), &target };
    target.socket = &recorder;

    {
        auto const timer = armSocketDeadline(&loop, std::chrono::milliseconds { 100 }, &target);
        REQUIRE(timer.has_value());
    }

    clock.advance(std::chrono::milliseconds { 500 });
    loop.drain();
    CHECK(recorder.closes == 0);
    CHECK_FALSE(target.expired);
}
