// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SystemdScopePlacement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

using namespace std::chrono_literals;
using vtpty::ScopeError;

namespace
{

using Outcome = std::expected<void, ScopeError>;

/// What a FakeScopeBus answers, and what it was asked. Shared with the test, because the bus itself
/// is owned -- and destroyed -- by the placement.
struct BusScript
{
    std::mutex mutex;
    std::deque<Outcome> connects; ///< One per connect attempt, in order; success once empty.
    std::deque<Outcome> outcomes; ///< One per request, in order; success once empty.
    std::vector<vtpty::ScopeRequest> requests;
    int connectCount = 0;

    [[nodiscard]] std::vector<vtpty::ScopeRequest> requestsSoFar()
    {
        auto const _ = std::lock_guard { mutex };
        return requests;
    }

    [[nodiscard]] int connectsSoFar()
    {
        auto const _ = std::lock_guard { mutex };
        return connectCount;
    }
};

class FakeScopeBus final: public vtpty::ScopeBus
{
  public:
    explicit FakeScopeBus(std::shared_ptr<BusScript> script): _script { std::move(script) } {}

    [[nodiscard]] Outcome startScope(vtpty::ScopeRequest const& request,
                                     std::chrono::milliseconds /*deadline*/) override
    {
        auto const _ = std::lock_guard { _script->mutex };
        _script->requests.push_back(request);
        if (_script->outcomes.empty())
            return {};
        auto const outcome = _script->outcomes.front();
        _script->outcomes.pop_front();
        return outcome;
    }

  private:
    std::shared_ptr<BusScript> _script;
};

/// @return A factory connecting FakeScopeBus instances that follow @p script.
vtpty::ScopeBusFactory connectFake(std::shared_ptr<BusScript> const& script)
{
    return [script]() -> std::expected<std::unique_ptr<vtpty::ScopeBus>, ScopeError> {
        auto const _ = std::lock_guard { script->mutex };
        ++script->connectCount;
        if (!script->connects.empty())
        {
            auto const outcome = script->connects.front();
            script->connects.pop_front();
            if (!outcome)
                return std::unexpected(outcome.error());
        }
        return std::make_unique<FakeScopeBus>(script);
    };
}

/// A clock the test moves by hand.
struct ManualClock
{
    std::shared_ptr<std::atomic<std::chrono::steady_clock::rep>> ticks =
        std::make_shared<std::atomic<std::chrono::steady_clock::rep>>(0);

    [[nodiscard]] std::function<std::chrono::steady_clock::time_point()> function() const
    {
        return [ticks = ticks] {
            return std::chrono::steady_clock::time_point { std::chrono::steady_clock::duration {
                ticks->load() } };
        };
    }

    void advance(std::chrono::steady_clock::duration by) const { ticks->fetch_add(by.count()); }
};

/// @return The default configuration, on @p clock.
vtpty::SystemdScopeConfig configOn(ManualClock const& clock)
{
    auto config = vtpty::SystemdScopeConfig {};
    config.now = clock.function();
    return config;
}

/// A ParkedChild over a real gate, and the end a forked child would be waiting on.
struct FakeChild
{
    vtpty::ParkedChild parked;
    vtpty::OwnedFd waitingEnd;
};

/// @param pid   The pid the child claims; never touched by the fake bus.
/// @param pidfd The pidfd it carries, or none.
FakeChild fakeChild(int pid, vtpty::OwnedFd pidfd = vtpty::OwnedFd {})
{
    auto gate = vtpty::makeGate();
    REQUIRE(gate.has_value());
    return FakeChild { .parked = vtpty::ParkedChild { pid, std::move(pidfd), std::move(gate->parentEnd) },
                       .waitingEnd = std::move(gate->childEnd) };
}

/// Hands @p child to @p placement. @return The end to wait on for its release.
vtpty::OwnedFd place(vtpty::ProcessPlacement& placement, FakeChild child)
{
    placement.placeThenRelease(std::move(child.parked));
    return std::move(child.waitingEnd);
}

/// Waits, for at most ten seconds, for the child waiting on @p waitingEnd to be let go.
/// @return Whether it was.
bool released(vtpty::OwnedFd const& waitingEnd)
{
    auto pollFd = pollfd { .fd = waitingEnd.get(), .events = POLLIN, .revents = 0 };
    return ::poll(&pollFd, 1, 10'000) == 1;
}

/// @return A descriptor to stand in for a pidfd.
vtpty::OwnedFd somePidFd()
{
    return vtpty::OwnedFd { ::open("/dev/null", O_RDONLY | O_CLOEXEC) };
}

} // namespace

TEST_CASE("SystemdScopePlacement.connectsOnlyWhenFirstUsed", "[placement]")
{
    auto const script = std::make_shared<BusScript>();
    auto const clock = ManualClock {};
    auto placement = vtpty::SystemdScopePlacement { configOn(clock), connectFake(script) };
    CHECK(script->connectsSoFar() == 0);

    CHECK(released(place(placement, fakeChild(4242))));
    CHECK(script->connectsSoFar() == 1);
}

TEST_CASE("SystemdScopePlacement.request", "[placement]")
{
    auto const script = std::make_shared<BusScript>();
    auto const clock = ManualClock {};
    auto config = configOn(clock);
    config.memoryLimit = vtpty::MemoryLimit { .maxBytes = 64 << 20, .swapMaxBytes = 0 };
    auto placement = vtpty::SystemdScopePlacement { std::move(config), connectFake(script) };

    CHECK(released(place(placement, fakeChild(4242, somePidFd()))));

    auto const requests = script->requestsSoFar();
    REQUIRE(requests.size() == 1);
    auto const& request = requests.front();
    CHECK(request.unitName.starts_with(std::format("contour-session-{}-4242-", ::getpid())));
    CHECK(request.unitName.ends_with(".scope"));
    CHECK(request.slice == "app.slice");
    CHECK(request.reference == vtpty::ProcessReference::PidFd);
    CHECK(request.pid == 4242);
    CHECK(request.pidfd >= 0);
    REQUIRE(request.memoryLimit.has_value());
    CHECK(request.memoryLimit->maxBytes == 64 << 20);
}

TEST_CASE("SystemdScopePlacement.unitNamesAreUnique", "[placement]")
{
    auto const script = std::make_shared<BusScript>();
    auto const clock = ManualClock {};
    auto placement = vtpty::SystemdScopePlacement { configOn(clock), connectFake(script) };
    CHECK(released(place(placement, fakeChild(4242))));
    CHECK(released(place(placement, fakeChild(4242))));
    auto const requests = script->requestsSoFar();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].unitName != requests[1].unitName);
}

TEST_CASE("SystemdScopePlacement.PIDFDs rejected: retried once with PIDs", "[placement]")
{
    auto const script = std::make_shared<BusScript>();
    script->outcomes.emplace_back(std::unexpected(ScopeError::UnknownProperty));
    auto const clock = ManualClock {};
    auto placement = vtpty::SystemdScopePlacement { configOn(clock), connectFake(script) };

    CHECK(released(place(placement, fakeChild(4242, somePidFd()))));

    auto const requests = script->requestsSoFar();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].reference == vtpty::ProcessReference::PidFd);
    CHECK(requests[1].reference == vtpty::ProcessReference::Pid);
    CHECK(requests[1].pid == 4242);
}

TEST_CASE("SystemdScopePlacement.everyOutcomeReleases", "[placement]")
{
    for (auto const outcome: { Outcome {},
                               Outcome { std::unexpected(ScopeError::Refused) },
                               Outcome { std::unexpected(ScopeError::UnknownProperty) },
                               Outcome { std::unexpected(ScopeError::TimedOut) },
                               Outcome { std::unexpected(ScopeError::Disconnected) } })
    {
        auto const script = std::make_shared<BusScript>();
        script->outcomes.push_back(outcome);
        auto const clock = ManualClock {};
        auto placement = vtpty::SystemdScopePlacement { configOn(clock), connectFake(script) };
        CHECK(released(place(placement, fakeChild(4242))));
    }
}

TEST_CASE("SystemdScopePlacement.a wedged bus is skipped until the cooldown", "[placement]")
{
    auto const script = std::make_shared<BusScript>();
    script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    auto const clock = ManualClock {};
    auto placement = vtpty::SystemdScopePlacement { configOn(clock), connectFake(script) };

    CHECK(released(place(placement, fakeChild(1))));
    CHECK(released(place(placement, fakeChild(2)))); // skipped: no second request
    CHECK(script->requestsSoFar().size() == 1);

    clock.advance(31s);
    CHECK(released(place(placement, fakeChild(3)))); // the trial request
    CHECK(script->requestsSoFar().size() == 2);
}

TEST_CASE("SystemdScopePlacement.no systemd instance: connects again after the cooldown", "[placement]")
{
    auto const script = std::make_shared<BusScript>();
    script->connects.emplace_back(std::unexpected(ScopeError::Unavailable));
    auto const clock = ManualClock {};
    auto placement = vtpty::SystemdScopePlacement { configOn(clock), connectFake(script) };

    CHECK(released(place(placement, fakeChild(1))));
    CHECK(released(place(placement, fakeChild(2))));
    CHECK(script->connectsSoFar() == 1);
    CHECK(script->requestsSoFar().empty());

    clock.advance(31s);
    CHECK(released(place(placement, fakeChild(3))));
    CHECK(script->connectsSoFar() == 2);
    CHECK(script->requestsSoFar().size() == 1);
}

TEST_CASE("SystemdScopePlacement.a broken connection is replaced", "[placement]")
{
    auto const script = std::make_shared<BusScript>();
    script->outcomes.emplace_back(std::unexpected(ScopeError::Disconnected));
    auto const clock = ManualClock {};
    auto placement = vtpty::SystemdScopePlacement { configOn(clock), connectFake(script) };

    CHECK(released(place(placement, fakeChild(1))));
    clock.advance(31s);
    CHECK(released(place(placement, fakeChild(2))));
    CHECK(script->connectsSoFar() == 2);
}

TEST_CASE("SystemdScopePlacement.destroying the placement releases queued children", "[placement]")
{
    auto const script = std::make_shared<BusScript>();
    auto const clock = ManualClock {};
    auto waiting = std::vector<vtpty::OwnedFd> {};
    {
        auto placement = vtpty::SystemdScopePlacement { configOn(clock), connectFake(script) };
        for (auto const pid: { 1, 2, 3, 4, 5 })
            waiting.push_back(place(placement, fakeChild(pid)));
    } // however far the worker got, every child must be let go
    for (auto const& end: waiting)
        CHECK(released(end));
}
