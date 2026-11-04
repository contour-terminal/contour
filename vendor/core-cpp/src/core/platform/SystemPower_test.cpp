// SPDX-License-Identifier: Apache-2.0
#include <core/platform/SystemPower.hpp>
#include <core/platform/detail/LogindPlan.hpp>
#include <core/platform/detail/PowerBroadcastPlan.hpp>
#include <core/platform/detail/PowerMessagePlan.hpp>
#include <core/platform/testing/FakeSystemPowerMonitor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <latch>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

using core::platform::PowerSubscription;
using core::platform::SystemPowerEvent;
using core::platform::detail::PowerHandlerRegistry;
using core::platform::testing::FakeSystemPowerMonitor;

// The exception policy: a handler that throws terminates, on every backend and in the fake, because
// the dispatch it runs in is the noexcept boundary.
static_assert(noexcept(std::declval<PowerHandlerRegistry&>().dispatch(SystemPowerEvent::DidWake)),
              "a throwing handler must terminate, not unwind into the operating system's callback");
static_assert(noexcept(std::declval<FakeSystemPowerMonitor&>().deliver(SystemPowerEvent::DidWake)),
              "the fake must apply the same exception policy as the native monitors");

namespace
{
/// Joins its thread on every path out of the scope (std::jthread is missing from older AppleClang).
class ScopedThread
{
  public:
    explicit ScopedThread(std::function<void()> body): _thread { std::move(body) } {}
    ScopedThread(ScopedThread const&) = delete;
    ScopedThread& operator=(ScopedThread const&) = delete;
    ~ScopedThread() { _thread.join(); }

  private:
    std::thread _thread;
};
} // namespace

TEST_CASE("a subscriber receives each power event", "[platform][power]")
{
    auto monitor = FakeSystemPowerMonitor {};
    auto received = std::vector<SystemPowerEvent> {};
    auto const subscription = monitor.subscribe([&](SystemPowerEvent event) { received.push_back(event); });

    monitor.deliver(SystemPowerEvent::WillSleep);
    monitor.deliver(SystemPowerEvent::DidWake);

    CHECK(received == std::vector { SystemPowerEvent::WillSleep, SystemPowerEvent::DidWake });
}

TEST_CASE("a destroyed subscription receives nothing", "[platform][power]")
{
    auto monitor = FakeSystemPowerMonitor {};
    auto received = std::vector<SystemPowerEvent> {};
    auto subscription = monitor.subscribe([&](SystemPowerEvent event) { received.push_back(event); });
    REQUIRE(monitor.subscriberCount() == 1);

    subscription = PowerSubscription {};
    monitor.deliver(SystemPowerEvent::DidWake);

    CHECK(received.empty());
    CHECK(monitor.subscriberCount() == 0);
}

TEST_CASE("a moved subscription stays subscribed", "[platform][power]")
{
    auto monitor = FakeSystemPowerMonitor {};
    auto received = std::vector<SystemPowerEvent> {};
    auto first = monitor.subscribe([&](SystemPowerEvent event) { received.push_back(event); });
    auto const second = PowerSubscription { std::move(first) };

    monitor.deliver(SystemPowerEvent::DidWake);

    CHECK(received.size() == 1);
    CHECK(monitor.subscriberCount() == 1);
}

TEST_CASE("unsubscribing waits out a running handler", "[platform][power]")
{
    auto registry = PowerHandlerRegistry {};
    auto sequence = std::atomic<int> { 0 };
    auto handlerDone = std::atomic<int> { 0 };
    auto destructorDone = std::atomic<int> { 0 };
    auto entered = std::latch { 1 };
    auto release = std::latch { 1 };

    auto subscription = registry.subscribe([&](SystemPowerEvent) {
        entered.count_down();
        release.wait();
        handlerDone = ++sequence;
    });

    {
        auto dispatcher = ScopedThread { [&] { registry.dispatch(SystemPowerEvent::DidWake); } };
        entered.wait();
        auto destroyer = ScopedThread { [&] {
            subscription = PowerSubscription {};
            destructorDone = ++sequence;
        } };

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(destructorDone == 0);
        release.count_down();
    }

    CHECK(handlerDone != 0);
    CHECK(handlerDone < destructorDone);
}

TEST_CASE("a handler may drop its own subscription", "[platform][power]")
{
    auto monitor = FakeSystemPowerMonitor {};
    auto calls = 0;
    auto subscription = PowerSubscription {};
    subscription = monitor.subscribe([&](SystemPowerEvent) {
        ++calls;
        subscription = PowerSubscription {};
    });

    monitor.deliver(SystemPowerEvent::DidWake);
    monitor.deliver(SystemPowerEvent::DidWake);

    CHECK(calls == 1);
    CHECK(monitor.subscriberCount() == 0);
}

TEST_CASE("a subscription may outlive its monitor", "[platform][power]")
{
    auto calls = 0;
    auto subscription = PowerSubscription {};
    {
        auto monitor = FakeSystemPowerMonitor {};
        subscription = monitor.subscribe([&](SystemPowerEvent) { ++calls; });
        monitor.deliver(SystemPowerEvent::DidWake);
    }
    subscription = PowerSubscription {}; // unsubscribes from the state the token kept alive

    CHECK(calls == 1);
}

TEST_CASE("a subscription may outlive its registry", "[platform][power]")
{
    auto registry = std::make_unique<PowerHandlerRegistry>();
    auto sentinel = std::make_shared<int>(0);
    auto const watch = std::weak_ptr { sentinel };
    auto subscription = registry->subscribe([captured = std::move(sentinel)](SystemPowerEvent) {});

    registry.reset();
    CHECK_FALSE(watch.expired()); // the token keeps the handler table, and the handler, alive

    subscription = PowerSubscription {};
    CHECK(watch.expired());
}

TEST_CASE("a handler subscribed during a dispatch sees the next event only", "[platform][power]")
{
    auto monitor = FakeSystemPowerMonitor {};
    auto late = std::vector<SystemPowerEvent> {};
    auto lateSubscription = PowerSubscription {};
    auto const subscription = monitor.subscribe([&](SystemPowerEvent) {
        if (monitor.subscriberCount() == 1)
            lateSubscription = monitor.subscribe([&](SystemPowerEvent event) { late.push_back(event); });
    });

    monitor.deliver(SystemPowerEvent::WillSleep);
    CHECK(late.empty());
    CHECK(monitor.subscriberCount() == 2);

    monitor.deliver(SystemPowerEvent::DidWake);
    CHECK(late == std::vector { SystemPowerEvent::DidWake });
}

TEST_CASE("a handler may destroy the fake", "[platform][power]")
{
    auto monitor = std::make_unique<FakeSystemPowerMonitor>();
    auto calls = 0;
    auto subscription = PowerSubscription {};
    subscription = monitor->subscribe([&](SystemPowerEvent) {
        ++calls;
        // With its own token gone too, the dispatch's keep-alive is the last reference to the registry's
        // state: without it, the dispatch would go on over freed memory once this returns.
        subscription = PowerSubscription {};
        monitor.reset();
    });

    monitor->deliver(SystemPowerEvent::DidWake);

    CHECK(monitor == nullptr);
    CHECK(calls == 1);
}

TEST_CASE("the native monitor constructs or says why not", "[platform][power]")
{
    auto monitor = core::platform::makeNativeSystemPowerMonitor();
#ifdef __APPLE__
    REQUIRE(monitor.has_value());
#endif
#ifdef _WIN32
    REQUIRE(monitor.has_value());
#endif
#ifdef CORE_CPP_HAS_SYSTEMD
    // A container without a system bus cannot register, and says so.
    if (!monitor.has_value())
        REQUIRE(monitor.error() == core::platform::PlatformError::NotificationRegistrationFailed);
#endif
    if (monitor.has_value())
    {
        auto const subscription = (*monitor)->subscribe([](SystemPowerEvent) {});
        SUCCEED();
    }
#if !defined(__APPLE__) && !defined(_WIN32) && !defined(CORE_CPP_HAS_SYSTEMD)
    else
        CHECK(monitor.error() == core::platform::PlatformError::NotImplemented);
#endif
}

TEST_CASE("logind's PrepareForSleep argument names the event", "[platform][power]")
{
    using core::platform::detail::eventForPrepareForSleep;
    using core::platform::detail::LogindSleepState;

    CHECK(eventForPrepareForSleep(LogindSleepState::Sleeping) == SystemPowerEvent::WillSleep);
    CHECK(eventForPrepareForSleep(LogindSleepState::Waking) == SystemPowerEvent::DidWake);
}

TEST_CASE("a lost system bus is reconnected with a capped exponential backoff", "[platform][power]")
{
    using core::platform::detail::reconnectDelay;
    using std::chrono::milliseconds;

    CHECK(reconnectDelay(0) == milliseconds { 1'000 });
    CHECK(reconnectDelay(1) == milliseconds { 2'000 });
    CHECK(reconnectDelay(2) == milliseconds { 4'000 });
    CHECK(reconnectDelay(3) == milliseconds { 8'000 });
    CHECK(reconnectDelay(4) == milliseconds { 16'000 });
    CHECK(reconnectDelay(5) == milliseconds { 30'000 }); // 32 s, capped
    CHECK(reconnectDelay(6) == milliseconds { 30'000 });
    CHECK(reconnectDelay(31) == milliseconds { 30'000 }); // no shift overflow
    CHECK(reconnectDelay(std::numeric_limits<std::uint32_t>::max()) == milliseconds { 30'000 });
}

TEST_CASE("an absolute sd-bus deadline becomes a poll timeout", "[platform][power]")
{
    using core::platform::detail::pollTimeoutMilliseconds;
    constexpr auto Never = std::numeric_limits<std::uint64_t>::max();

    CHECK(pollTimeoutMilliseconds(Never, 1'000) == -1);
    CHECK(pollTimeoutMilliseconds(1'000, 1'000) == 0);
    CHECK(pollTimeoutMilliseconds(500, 1'000) == 0);
    CHECK(pollTimeoutMilliseconds(1'001, 1'000) == 1); // rounds up, never spins short
    CHECK(pollTimeoutMilliseconds(3'500, 1'000) == 3);
    CHECK(pollTimeoutMilliseconds(1'000 + 2'000'000, 1'000) == 2'000);
    CHECK(pollTimeoutMilliseconds(Never - 1, 0) == std::numeric_limits<int>::max());
}

namespace
{
/// Owns a second subscription, as an object captured by another handler would.
struct SubscriptionOwner
{
    PowerSubscription subscription;
};
} // namespace

TEST_CASE("destroying a handler may unsubscribe another token", "[platform][power]")
{
    auto monitor = FakeSystemPowerMonitor {};
    auto owner = std::make_shared<SubscriptionOwner>();
    owner->subscription = monitor.subscribe([](SystemPowerEvent) {});
    auto first = monitor.subscribe([captured = std::move(owner)](SystemPowerEvent) {});
    REQUIRE(monitor.subscriberCount() == 2);

    first = PowerSubscription {}; // destroys the handler, hence the owner, hence the second token

    CHECK(monitor.subscriberCount() == 0);
}

TEST_CASE("destroying an earlier handler may unsubscribe a later token", "[platform][power]")
{
    auto monitor = FakeSystemPowerMonitor {};
    auto owner = std::make_shared<SubscriptionOwner>();
    auto first = monitor.subscribe([captured = owner](SystemPowerEvent) {});
    owner->subscription = monitor.subscribe([](SystemPowerEvent) {});
    owner.reset();
    REQUIRE(monitor.subscriberCount() == 2);

    first = PowerSubscription {}; // the owner dies with the handler; its token sits after the erased entry

    CHECK(monitor.subscriberCount() == 0);
}

TEST_CASE("power messages map to events and acknowledgements", "[platform][power]")
{
    using core::platform::detail::planPowerMessage;
    using core::platform::detail::PowerAcknowledgement;
    using core::platform::detail::PowerMessage;
    using core::platform::detail::PowerMessageAction;

    CHECK(planPowerMessage(PowerMessage::CanSystemSleep)
          == PowerMessageAction { std::nullopt, PowerAcknowledgement::Yes });
    CHECK(planPowerMessage(PowerMessage::WillSleep)
          == PowerMessageAction { SystemPowerEvent::WillSleep, PowerAcknowledgement::Yes });
    CHECK(planPowerMessage(PowerMessage::WillNotSleep)
          == PowerMessageAction {}); // follows CanSystemSleep only, which reported nothing to take back
    CHECK(planPowerMessage(PowerMessage::HasPoweredOn)
          == PowerMessageAction { SystemPowerEvent::DidWake, PowerAcknowledgement::No });
    CHECK(planPowerMessage(PowerMessage::Other) == PowerMessageAction {});
}

TEST_CASE("Windows power broadcasts map to events", "[platform][power]")
{
    using core::platform::detail::eventForPowerBroadcast;
    using core::platform::detail::PowerBroadcastResumeAutomatic;
    using core::platform::detail::PowerBroadcastResumeSuspend;
    using core::platform::detail::PowerBroadcastSuspend;

    CHECK(eventForPowerBroadcast(PowerBroadcastSuspend) == SystemPowerEvent::WillSleep);
    CHECK(eventForPowerBroadcast(PowerBroadcastResumeAutomatic) == SystemPowerEvent::DidWake);
    CHECK_FALSE(eventForPowerBroadcast(PowerBroadcastResumeSuspend).has_value()); // follows the automatic one
    CHECK_FALSE(eventForPowerBroadcast(0).has_value());
    CHECK_FALSE(eventForPowerBroadcast(0x000A).has_value()); // PBT_APMPOWERSTATUSCHANGE
}
