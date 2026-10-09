// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SystemdScopePlacement.hpp>
#include <vtpty/test/GateTesting.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using namespace std::chrono_literals;
using vtpty::ScopeError;
using vtpty::testing::fakeChild;
using vtpty::testing::FakeChild;
using vtpty::testing::released;

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

/// Hands @p child to @p placement. @return The end to wait on for its release.
vtpty::OwnedFd place(vtpty::ProcessPlacement& placement, FakeChild child)
{
    placement.placeThenRelease(std::move(child.parked));
    return std::move(child.waitingEnd);
}

/// One placement on a fake bus and a hand-moved clock.
struct Rig
{
    std::shared_ptr<BusScript> script = std::make_shared<BusScript>();
    core::platform::ManualClock clock {};
    vtpty::SystemdScopeConfig config {};

    /// @return A placement over this rig's script, configuration and clock.
    [[nodiscard]] vtpty::SystemdScopePlacement placement() const
    {
        return vtpty::SystemdScopePlacement { config, connectFake(script), clock };
    }

    /// Moves the clock past the breaker's cooldown.
    void outwaitCooldown() { clock.advance(config.breakerCooldown + std::chrono::seconds { 1 }); }
};

/// @return A descriptor to stand in for a pidfd.
vtpty::OwnedFd somePidFd()
{
    return vtpty::OwnedFd { ::open("/dev/null", O_RDONLY | O_CLOEXEC) };
}

} // namespace

TEST_CASE("SystemdScopePlacement.connectsOnlyWhenFirstUsed", "[placement]")
{
    auto const rig = Rig {};
    auto placement = rig.placement();
    CHECK(rig.script->connectsSoFar() == 0);

    CHECK(released(place(placement, fakeChild(4242))));
    CHECK(rig.script->connectsSoFar() == 1);
}

TEST_CASE("SystemdScopePlacement.request", "[placement]")
{
    auto rig = Rig {};
    rig.config.memoryLimit = vtpty::MemoryLimit { .maxBytes = 64 << 20, .swapMaxBytes = 0 };
    auto placement = rig.placement();

    CHECK(released(place(placement, fakeChild(4242, somePidFd()))));

    auto const requests = rig.script->requestsSoFar();
    REQUIRE(requests.size() == 1);
    auto const& request = requests.front();
    CHECK(request.unitName.starts_with(std::format("contour-session-{}-4242-", ::getpid())));
    CHECK(request.unitName.ends_with(".scope"));
    CHECK(request.slice == "app.slice");
    CHECK(request.protocol == vtpty::ScopeProtocol::Current);
    CHECK(request.pid == 4242);
    CHECK(request.pidfd >= 0);
    REQUIRE(request.memoryLimit.has_value());
    CHECK(request.memoryLimit->maxBytes == 64 << 20);
}

TEST_CASE("SystemdScopePlacement.unitNamesAreUnique", "[placement]")
{
    auto const rig = Rig {};
    auto placement = rig.placement();
    CHECK(released(place(placement, fakeChild(4242))));
    CHECK(released(place(placement, fakeChild(4242))));
    auto const requests = rig.script->requestsSoFar();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].unitName != requests[1].unitName);
}

TEST_CASE("SystemdScopePlacement.systemd before 253: falls back to the legacy protocol, and stays there",
          "[placement]")
{
    auto const rig = Rig {};
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::Unsupported));
    auto placement = rig.placement();

    CHECK(released(place(placement, fakeChild(1, somePidFd()))));
    CHECK(released(place(placement, fakeChild(2, somePidFd()))));

    auto const requests = rig.script->requestsSoFar();
    REQUIRE(requests.size() == 3);
    CHECK(requests[0].protocol == vtpty::ScopeProtocol::Current);
    CHECK(requests[1].protocol == vtpty::ScopeProtocol::Legacy);
    CHECK(requests[1].pid == 1);
    CHECK(requests[2].protocol == vtpty::ScopeProtocol::Legacy); // one request per spawn from now on
}

TEST_CASE("SystemdScopePlacement.a refusal is not taken for an old systemd", "[placement]")
{
    auto const rig = Rig {};
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::Refused));
    auto placement = rig.placement();

    CHECK(released(place(placement, fakeChild(1, somePidFd()))));
    CHECK(released(place(placement, fakeChild(2, somePidFd()))));

    auto const requests = rig.script->requestsSoFar();
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].protocol == vtpty::ScopeProtocol::Current);
}

#ifdef __linux__
TEST_CASE("SystemdScopePlacement.a child that already exited is not asked about", "[placement]")
{
    // Its tab closed while it waited in the queue, and it was reaped: its pid may name another
    // process by now. Asking systemd would move that one, or report a fault over a closed tab.
    auto const rig = Rig {};
    auto placement = rig.placement();
    CHECK(released(place(placement, fakeChild(4242, vtpty::testing::exitedPidFd()))));
    CHECK(rig.script->requestsSoFar().empty());
}

TEST_CASE("SystemdScopePlacement.the worker blocks process-directed signals", "[placement]")
{
    // The daemon waits for SIGTERM and SIGINT with sigwait() on a thread of its own. A worker that
    // left them unblocked could take one, and its default action would end the daemon.
    auto const rig = Rig {};
    auto placement = rig.placement();
    REQUIRE(released(place(placement, fakeChild(4242))));

    auto const readLine = [](std::filesystem::path const& file, std::string_view prefix) {
        auto in = std::ifstream { file };
        auto line = std::string {};
        while (std::getline(in, line))
            if (line.starts_with(prefix))
                return line.substr(prefix.size());
        return std::string {};
    };
    // This worker, by name: other threads -- the sanitizers' among them -- block signals of their own.
    auto const tasks = std::filesystem::directory_iterator { "/proc/self/task" };
    auto const worker = std::ranges::find_if(tasks, [&](auto const& entry) {
        return readLine(entry.path() / "comm", "") == vtpty::WorkerThreadName;
    });
    REQUIRE(worker != std::filesystem::directory_iterator {});

    auto const mask = std::stoull(readLine(worker->path() / "status", "SigBlk:"), nullptr, 16);
    auto const isBlocked = [mask](int signal) {
        return ((mask >> (signal - 1)) & 1U) != 0;
    };
    CHECK(isBlocked(SIGTERM));
    CHECK(isBlocked(SIGINT));
}
#endif

TEST_CASE("SystemdScopePlacement.supplies the configured oom_score_adj", "[placement]")
{
    auto rig = Rig {};
    CHECK_FALSE(rig.placement().childOomScoreAdjust().has_value());
    rig.config.childOomScoreAdjust = 300;
    CHECK(rig.placement().childOomScoreAdjust() == 300);
}

TEST_CASE("SystemdScopePlacement.everyOutcomeReleases", "[placement]")
{
    for (auto const outcome: { Outcome {},
                               Outcome { std::unexpected(ScopeError::Refused) },
                               Outcome { std::unexpected(ScopeError::Unsupported) },
                               Outcome { std::unexpected(ScopeError::TimedOut) },
                               Outcome { std::unexpected(ScopeError::Disconnected) } })
    {
        auto const rig = Rig {};
        rig.script->outcomes.push_back(outcome);
        auto placement = rig.placement();
        CHECK(released(place(placement, fakeChild(4242))));
    }
}

TEST_CASE("SystemdScopePlacement.a wedged bus is skipped until the cooldown", "[placement]")
{
    auto rig = Rig {};
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    auto placement = rig.placement();

    CHECK(released(place(placement, fakeChild(1))));
    CHECK(released(place(placement, fakeChild(2)))); // skipped: no second request
    CHECK(rig.script->requestsSoFar().size() == 1);

    rig.outwaitCooldown();
    CHECK(released(place(placement, fakeChild(3)))); // the trial request
    CHECK(rig.script->requestsSoFar().size() == 2);
}

TEST_CASE("SystemdScopePlacement.no systemd instance: connects again after the cooldown", "[placement]")
{
    auto rig = Rig {};
    rig.script->connects.emplace_back(std::unexpected(ScopeError::Unavailable));
    auto placement = rig.placement();

    CHECK(released(place(placement, fakeChild(1))));
    CHECK(released(place(placement, fakeChild(2))));
    CHECK(rig.script->connectsSoFar() == 1);
    CHECK(rig.script->requestsSoFar().empty());

    rig.outwaitCooldown();
    CHECK(released(place(placement, fakeChild(3))));
    CHECK(rig.script->connectsSoFar() == 2);
    CHECK(rig.script->requestsSoFar().size() == 1);
}

TEST_CASE("SystemdScopePlacement.a broken connection is replaced", "[placement]")
{
    auto rig = Rig {};
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::Disconnected));
    auto placement = rig.placement();

    CHECK(released(place(placement, fakeChild(1))));
    rig.outwaitCooldown();
    CHECK(released(place(placement, fakeChild(2))));
    CHECK(rig.script->connectsSoFar() == 2);
}

TEST_CASE("SystemdScopePlacement.destroying the placement releases queued children", "[placement]")
{
    auto const rig = Rig {};
    auto waiting = std::vector<vtpty::OwnedFd> {};
    {
        auto placement = rig.placement();
        for (auto const pid: { 1, 2, 3, 4, 5 })
            waiting.push_back(place(placement, fakeChild(pid)));
    } // however far the worker got, every child must be let go
    for (auto const& end: waiting)
        CHECK(released(end));
}
