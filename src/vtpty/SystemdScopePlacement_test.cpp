// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessCgroup.hpp>
#include <vtpty/SystemdScopePlacement.hpp>
#include <vtpty/test/GateTesting.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#ifdef __linux__
    #include <sys/syscall.h>
    #include <sys/wait.h>
#endif

using namespace std::chrono_literals;
using vtpty::ScopeError;
using vtpty::testing::fakeChild;
using vtpty::testing::FakeChild;
using vtpty::testing::released;

namespace
{

using Outcome = std::expected<void, ScopeError>;

/// What systemd did about a request whose answer did not arrive in time.
enum class AfterTimeout : std::uint8_t
{
    NothingYet,   ///< The child is where it was.
    PlacedAnyway, ///< The child is in its scope: the answer came late.
};

/// Where the kernel says a child runs that no scope took.
constexpr auto ContoursOwnCgroup =
    std::string_view { "0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                       "app-org.contourterminal.Contour@test.service\n" };

/// What a FakeScopeBus answers, and what it was asked; and what the kernel says about each child.
/// Shared with the test, because the bus itself is owned -- and destroyed -- by the placement.
struct BusScript
{
    std::mutex mutex;
    std::deque<Outcome> connects; ///< One per connect attempt, in order; success once empty.
    std::deque<Outcome> outcomes; ///< One per request, in order; success once empty.
    AfterTimeout afterTimeout = AfterTimeout::NothingYet;
    std::vector<vtpty::ScopeRequest> requests;
    std::vector<std::thread::id> requestThreads; ///< The thread each request was made on.
    std::condition_variable requested;           ///< Told about each request.
    std::map<int, std::string> cgroups; ///< By pid: what /proc/<pid>/cgroup reads once a scope took it.
    std::vector<int> cgroupReads;       ///< The pids the placement asked the kernel about.
    int connectCount = 0;
    std::optional<std::size_t>
        throwsOnRequest; ///< The request, counted from 1, that throws instead of answering.

    [[nodiscard]] std::vector<vtpty::ScopeRequest> requestsSoFar()
    {
        auto const _ = std::lock_guard { mutex };
        return requests;
    }

    /// @return The pids requested so far, in order.
    [[nodiscard]] std::vector<int> requestedPids()
    {
        auto const _ = std::lock_guard { mutex };
        auto pids = std::vector<int> {};
        for (auto const& request: requests)
            pids.push_back(request.pid);
        return pids;
    }

    /// @return Whether @p count requests have been made, waiting up to ten seconds for them.
    [[nodiscard]] bool awaitRequests(std::size_t count)
    {
        auto lock = std::unique_lock { mutex };
        return requested.wait_for(
            lock, std::chrono::seconds { 10 }, [&] { return requests.size() >= count; });
    }

    [[nodiscard]] int connectsSoFar()
    {
        auto const _ = std::lock_guard { mutex };
        return connectCount;
    }

    [[nodiscard]] std::vector<int> cgroupReadsSoFar()
    {
        auto const _ = std::lock_guard { mutex };
        return cgroupReads;
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
        _script->requestThreads.push_back(std::this_thread::get_id());
        _script->requested.notify_all();
        if (_script->throwsOnRequest == _script->requests.size())
            throw std::runtime_error { "the bus failed" };
        auto const outcome = _script->outcomes.empty() ? Outcome {} : _script->outcomes.front();
        if (!_script->outcomes.empty())
            _script->outcomes.pop_front();
        if (outcome
            || (outcome.error() == ScopeError::TimedOut
                && _script->afterTimeout == AfterTimeout::PlacedAnyway))
            _script->cgroups[request.pid] = std::format("0::/app.slice/{}\n", request.unitName);
        return outcome;
    }

  private:
    std::shared_ptr<BusScript> _script;
};

/// @return What the kernel says about each child, as @p script has it.
vtpty::ProcessCgroupReader readCgroupFrom(std::shared_ptr<BusScript> const& script)
{
    return [script](int pid) -> std::expected<std::string, std::error_code> {
        auto const _ = std::lock_guard { script->mutex };
        script->cgroupReads.push_back(pid);
        auto const placed = script->cgroups.find(pid);
        return placed != script->cgroups.end() ? placed->second : std::string { ContoursOwnCgroup };
    };
}

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

/// What a placement told, in order. Shared with the test, because the placement tells it on its worker.
struct ReportLog
{
    std::mutex mutex;
    std::vector<vtpty::PlacementReport> reports;

    [[nodiscard]] std::vector<vtpty::PlacementReport> soFar()
    {
        auto const _ = std::lock_guard { mutex };
        return reports;
    }
};

/// @return A reporter that notes every report in @p log.
vtpty::PlacementReporter recordTo(std::shared_ptr<ReportLog> const& log)
{
    return [log](vtpty::PlacementReport const& report) {
        auto const _ = std::lock_guard { log->mutex };
        log->reports.push_back(report);
    };
}

/// One placement on a fake bus and a hand-moved clock.
struct Rig
{
    std::shared_ptr<BusScript> script = std::make_shared<BusScript>();
    std::shared_ptr<ReportLog> reports = std::make_shared<ReportLog>();
    core::platform::ManualClock clock {};
    vtpty::SystemdScopeConfig config {};

    /// @return A placement over this rig's script, configuration and clock.
    [[nodiscard]] vtpty::SystemdScopePlacement placement() const
    {
        return vtpty::SystemdScopePlacement {
            config, connectFake(script), readCgroupFrom(script), recordTo(reports), clock
        };
    }

    /// Moves the clock past the breaker's cooldown.
    void outwaitCooldown() { clock.advance(config.breakerCooldown + std::chrono::seconds { 1 }); }
};

/// A cooldown for tests of late placement. While children wait for one, the worker waits the cooldown
/// out in real time before it looks at the hand-moved clock again; a short one has it notice soon.
constexpr auto ShortCooldown = std::chrono::milliseconds { 10 };

/// @return A descriptor to stand in for a pidfd: one that says its process is alive -- this process's own,
///         where pidfds exist. (Not /dev/null, which is always readable: a pidfd that is has exited.)
vtpty::OwnedFd somePidFd()
{
#ifdef __linux__
    return vtpty::OwnedFd { static_cast<int>(::syscall(SYS_pidfd_open, ::getpid(), 0)) };
#else
    return vtpty::OwnedFd { ::open("/dev/null", O_RDONLY | O_CLOEXEC) };
#endif
}

/// @return A report of @p status for @p pid.
vtpty::PlacementReport told(int pid,
                            vtpty::PlacementStatus status,
                            std::optional<ScopeError> reason = std::nullopt)
{
    return vtpty::PlacementReport { .pid = pid, .status = status, .reason = reason };
}

#ifdef __linux__
/// A process of this test's own, waiting to be ended: a session process whose tab closes later.
class StandIn
{
  public:
    StandIn(): _pid { ::fork() }
    {
        REQUIRE(_pid >= 0);
        if (_pid == 0)
        {
            ::pause();
            ::_exit(0);
        }
    }
    ~StandIn() { end(); }
    StandIn(StandIn const&) = delete;
    StandIn& operator=(StandIn const&) = delete;
    StandIn(StandIn&&) = delete;
    StandIn& operator=(StandIn&&) = delete;

    [[nodiscard]] int pid() const noexcept { return _pid; }

    /// @return A fresh pidfd for it.
    [[nodiscard]] vtpty::OwnedFd pidfd() const
    {
        return vtpty::OwnedFd { static_cast<int>(::syscall(SYS_pidfd_open, _pid, 0)) };
    }

    /// Kills it, and waits until it has exited, without reaping it: a zombie, as a session's shell is
    /// until the Process owning it waits for it.
    void exitUnreaped() const
    {
        REQUIRE(::kill(_pid, SIGKILL) == 0);
        auto info = siginfo_t {};
        REQUIRE(::waitid(P_PID, static_cast<id_t>(_pid), &info, WEXITED | WNOWAIT) == 0);
    }

    /// Kills and reaps it, so that its pid is free for reuse.
    void end() noexcept
    {
        if (_pid <= 0)
            return;
        ::kill(_pid, SIGKILL);
        ::waitpid(_pid, nullptr, 0);
        _pid = -1;
    }

  private:
    int _pid;
};
#endif

} // namespace

/// A report as its log line reads, so that a failed comparison says which reports differ.
template <>
struct Catch::StringMaker<vtpty::PlacementReport>
{
    static std::string convert(vtpty::PlacementReport const& report) { return vtpty::describe(report).text; }
};

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

TEST_CASE("SystemdScopePlacement.a late answer places the child and holds requests off as a silence does",
          "[placement]")
{
    // systemd placed the child, but its answer missed the deadline. The kernel says where the child runs:
    // in its scope, so it was placed. Yet the request waited out its whole deadline: answers that went
    // astray for good would cost every tab that deadline, so the breaker holds the next request off, and
    // the late answer is told where a user sees it.
    using enum vtpty::PlacementStatus;
    auto rig = Rig {};
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    rig.script->afterTimeout = AfterTimeout::PlacedAnyway;
    {
        auto placement = rig.placement();
        CHECK(released(place(placement, fakeChild(1))));
        CHECK(released(place(placement, fakeChild(2))));
    }
    CHECK(rig.script->requestedPids() == std::vector { 1 });
    CHECK(rig.script->cgroupReadsSoFar() == std::vector { 1 });
    CHECK(rig.reports->soFar()
          == std::vector { told(1, PlacedUnheard, ScopeError::TimedOut),
                           told(2, Unplaced, ScopeError::TimedOut) });
}

TEST_CASE("SystemdScopePlacement.a timeout stands while the kernel shows the child where it was",
          "[placement]")
{
    auto rig = Rig {};
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    auto placement = rig.placement();

    CHECK(released(place(placement, fakeChild(1))));
    CHECK(released(place(placement, fakeChild(2)))); // skipped: the bus fell silent
    CHECK(rig.script->requestsSoFar().size() == 1);
}

TEST_CASE("SystemdScopePlacement.asks the kernel only about a child whose answer did not come", "[placement]")
{
    auto rig = Rig {};
    rig.script->outcomes.emplace_back(); // placed
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::Refused));
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    auto placement = rig.placement();

    for (auto const pid: { 1, 2, 3 })
        CHECK(released(place(placement, fakeChild(pid))));
    CHECK(rig.script->cgroupReadsSoFar() == std::vector { 3 });
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

TEST_CASE("SystemdScopePlacement.no systemd instance: children run unplaced and none is kept for later",
          "[placement]")
{
    // No systemd user instance is a valid configuration, not an outage: nobody is to wait for it to come
    // back, and a worker that kept every session's pidfd would retry for each of them every cooldown.
    using enum vtpty::PlacementStatus;
    auto rig = Rig {};
    rig.script->connects.emplace_back(std::unexpected(ScopeError::Unavailable));
    {
        auto placement = rig.placement();
        CHECK(released(place(placement, fakeChild(1, somePidFd()))));
        CHECK(released(place(placement, fakeChild(2, somePidFd())))); // while the breaker holds off
    }
    CHECK(rig.script->requestsSoFar().empty());
    CHECK(rig.reports->soFar()
          == std::vector { told(1, Unplaced, ScopeError::Unavailable),
                           told(2, Unplaced, ScopeError::Unavailable) });
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

TEST_CASE("SystemdScopePlacement.tells what became of every child", "[placement]")
{
    using enum vtpty::PlacementStatus;
    auto const rig = Rig {};
    rig.script->outcomes.emplace_back(); // placed
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::Refused));
    {
        auto placement = rig.placement();
        CHECK(released(place(placement, fakeChild(1, somePidFd()))));
        CHECK(released(place(placement, fakeChild(2, somePidFd()))));
    } // the worker is done telling once the placement is gone
    CHECK(rig.reports->soFar() == std::vector { told(1, Placed), told(2, Unplaced, ScopeError::Refused) });
}

TEST_CASE("SystemdScopePlacement.every child the breaker let go unplaced is told, with its pid and why",
          "[placement]")
{
    // Not only the first: each is a session an out-of-memory kill can stop Contour with.
    using enum vtpty::PlacementStatus;
    auto const rig = Rig {};
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    {
        auto placement = rig.placement();
        for (auto const pid: { 1, 2, 3 })
            CHECK(released(place(placement, fakeChild(pid, somePidFd()))));
    }
    CHECK(rig.script->requestsSoFar().size() == 1);
    CHECK(rig.reports->soFar()
          == std::vector { told(1, Deferred, ScopeError::TimedOut),
                           told(2, Deferred, ScopeError::TimedOut),
                           told(3, Deferred, ScopeError::TimedOut) });
}

TEST_CASE("SystemdScopePlacement.a child without a pidfd is not kept for a late placement", "[placement]")
{
    // Once it may have exited, nothing would tell whether its pid still names it.
    using enum vtpty::PlacementStatus;
    auto rig = Rig {};
    rig.config.breakerCooldown = ShortCooldown;
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    {
        auto placement = rig.placement();
        CHECK(released(place(placement, fakeChild(1))));
        CHECK(released(place(placement, fakeChild(2))));
        rig.outwaitCooldown();
        CHECK(released(place(placement, fakeChild(3))));
    }
    CHECK(rig.script->requestedPids() == std::vector { 1, 3 });
    CHECK(rig.reports->soFar()
          == std::vector { told(1, Unplaced, ScopeError::TimedOut),
                           told(2, Unplaced, ScopeError::TimedOut),
                           told(3, Placed) });
}

TEST_CASE("SystemdScopePlacement.a burst during an open breaker is placed once the cooldown passed",
          "[placement]")
{
    using enum vtpty::PlacementStatus;
    auto rig = Rig {};
    rig.config.breakerCooldown = ShortCooldown;
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    {
        auto placement = rig.placement();
        for (auto const pid: { 1, 2, 3, 4 })
            CHECK(released(place(placement, fakeChild(pid, somePidFd()))));
        CHECK(rig.script->requestedPids() == std::vector { 1 }); // the rest were held off

        // No new tab needed: the cooldown passing is enough.
        rig.outwaitCooldown();
        REQUIRE(rig.script->awaitRequests(5));
    }
    CHECK(rig.script->requestedPids() == std::vector { 1, 1, 2, 3, 4 });
    CHECK(std::ranges::none_of(rig.script->requestThreads,
                               [](auto const thread) { return thread == std::this_thread::get_id(); }));
    CHECK(rig.reports->soFar()
          == std::vector { told(1, Deferred, ScopeError::TimedOut),
                           told(2, Deferred, ScopeError::TimedOut),
                           told(3, Deferred, ScopeError::TimedOut),
                           told(4, Deferred, ScopeError::TimedOut),
                           told(1, PlacedLate, ScopeError::TimedOut),
                           told(2, PlacedLate, ScopeError::TimedOut),
                           told(3, PlacedLate, ScopeError::TimedOut),
                           told(4, PlacedLate, ScopeError::TimedOut) });
}

TEST_CASE("SystemdScopePlacement.a still silent bus leaves the deferred children told and waiting",
          "[placement]")
{
    using enum vtpty::PlacementStatus;
    auto rig = Rig {};
    rig.config.breakerCooldown = ShortCooldown;
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut)); // the trial
    {
        auto placement = rig.placement();
        CHECK(released(place(placement, fakeChild(1, somePidFd()))));
        CHECK(released(place(placement, fakeChild(2, somePidFd()))));
        rig.outwaitCooldown();
        REQUIRE(rig.script->awaitRequests(2));
    }
    // One trial per cooldown, and no word more about children told about already.
    CHECK(rig.script->requestedPids() == std::vector { 1, 1 });
    CHECK(
        rig.reports->soFar()
        == std::vector { told(1, Deferred, ScopeError::TimedOut), told(2, Deferred, ScopeError::TimedOut) });
}

TEST_CASE("SystemdScopePlacement.a refused child is not asked about again", "[placement]")
{
    using enum vtpty::PlacementStatus;
    auto rig = Rig {};
    rig.config.breakerCooldown = ShortCooldown;
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::Refused));
    {
        auto placement = rig.placement();
        CHECK(released(place(placement, fakeChild(1, somePidFd()))));
        rig.outwaitCooldown();
        CHECK(released(place(placement, fakeChild(2, somePidFd()))));
    }
    CHECK(rig.script->requestedPids() == std::vector { 1, 2 });
}

#ifdef __linux__
TEST_CASE("SystemdScopePlacement.a deferred child that exited is not placed late", "[placement]")
{
    // Its pid may name another process by now.
    using enum vtpty::PlacementStatus;
    auto rig = Rig {};
    rig.config.breakerCooldown = ShortCooldown;
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    auto closedTab = StandIn {};
    auto openTab = StandIn {};
    auto const closed = closedTab.pid();
    auto const open = openTab.pid();
    {
        auto placement = rig.placement();
        CHECK(released(place(placement, fakeChild(closed, closedTab.pidfd()))));
        CHECK(released(place(placement, fakeChild(open, openTab.pidfd()))));
        closedTab.end();
        rig.outwaitCooldown();
        REQUIRE(rig.script->awaitRequests(2));
    }
    CHECK(rig.script->requestedPids() == std::vector { closed, open });
    CHECK(rig.reports->soFar()
          == std::vector { told(closed, Deferred, ScopeError::TimedOut),
                           told(open, Deferred, ScopeError::TimedOut),
                           told(closed, Ended),
                           told(open, PlacedLate, ScopeError::TimedOut) });
}
#endif

#ifdef __linux__
TEST_CASE("SystemdScopePlacement.a deferred child that exited but is not reaped yet is not placed late",
          "[placement]")
{
    // A zombie is gone as a session, though signal 0 still reaches it: asking systemd to move it would be
    // told as a placement, or as a fault, over a tab that has closed.
    using enum vtpty::PlacementStatus;
    auto rig = Rig {};
    rig.config.breakerCooldown = ShortCooldown;
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    auto closedTab = StandIn {};
    auto openTab = StandIn {};
    auto const closed = closedTab.pid();
    auto const open = openTab.pid();
    {
        auto placement = rig.placement();
        CHECK(released(place(placement, fakeChild(closed, closedTab.pidfd()))));
        CHECK(released(place(placement, fakeChild(open, openTab.pidfd()))));
        closedTab.exitUnreaped();
        rig.outwaitCooldown();
        REQUIRE(rig.script->awaitRequests(2));
    }
    CHECK(rig.script->requestedPids() == std::vector { closed, open });
    CHECK(rig.reports->soFar()
          == std::vector { told(closed, Deferred, ScopeError::TimedOut),
                           told(open, Deferred, ScopeError::TimedOut),
                           told(closed, Ended),
                           told(open, PlacedLate, ScopeError::TimedOut) });
}
#endif

TEST_CASE("SystemdScopePlacement.a reporter that throws ends neither the worker nor Contour", "[placement]")
{
    // The worker allocates and calls out -- the reporter, the bus, the kernel reader. Anything thrown there
    // would reach std::terminate and end Contour: what this placement exists to prevent.
    using enum vtpty::PlacementStatus;
    auto const rig = Rig {};
    auto reporter = [log = rig.reports,
                     throwsLeft = std::make_shared<int>(1)](vtpty::PlacementReport const& report) {
        if (std::exchange(*throwsLeft, 0) > 0) // called on the worker alone
            throw std::runtime_error { "the reporter failed" };
        recordTo(log)(report);
    };
    {
        auto placement = vtpty::SystemdScopePlacement {
            rig.config, connectFake(rig.script), readCgroupFrom(rig.script), reporter, rig.clock
        };
        CHECK(released(place(placement, fakeChild(1, somePidFd()))));
        CHECK(released(place(placement, fakeChild(2, somePidFd()))));
    }
    CHECK(rig.script->requestedPids() == std::vector { 1, 2 });
    CHECK(rig.reports->soFar() == std::vector { told(2, Placed) });
}

TEST_CASE("SystemdScopePlacement.a late placement that throws lets its child go rather than ask again",
          "[placement]")
{
    // Asked again at once, a child whose placement throws every time would keep the worker spinning.
    using enum vtpty::PlacementStatus;
    auto rig = Rig {};
    rig.config.breakerCooldown = ShortCooldown;
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    rig.script->throwsOnRequest = 2; // the late placement of the first child
    {
        auto placement = rig.placement();
        CHECK(released(place(placement, fakeChild(1, somePidFd()))));
        rig.outwaitCooldown();
        REQUIRE(rig.script->awaitRequests(2));
        CHECK(released(place(placement, fakeChild(2, somePidFd()))));
    }
    CHECK(rig.script->requestedPids() == std::vector { 1, 1, 2 });
    CHECK(rig.reports->soFar() == std::vector { told(1, Deferred, ScopeError::TimedOut), told(2, Placed) });
}

#ifdef __linux__
TEST_CASE("SystemdScopePlacement.a late pass that throws telling of an ended child keeps the others",
          "[placement]")
{
    // The late pass first lets go of the deferred children that exited. Should telling of one throw, the
    // children still running are not the failure's to drop: each is still placed late.
    using enum vtpty::PlacementStatus;
    auto rig = Rig {};
    rig.config.breakerCooldown = ShortCooldown;
    rig.script->outcomes.emplace_back(std::unexpected(ScopeError::TimedOut));
    auto reporter = [log = rig.reports,
                     throwsLeft = std::make_shared<int>(1)](vtpty::PlacementReport const& report) {
        if (report.status == Ended && std::exchange(*throwsLeft, 0) > 0) // called on the worker alone
            throw std::runtime_error { "the reporter failed" };
        recordTo(log)(report);
    };
    auto closedTab = StandIn {};
    auto const closed = closedTab.pid();
    {
        auto placement = vtpty::SystemdScopePlacement {
            rig.config, connectFake(rig.script), readCgroupFrom(rig.script), reporter, rig.clock
        };
        CHECK(released(place(placement, fakeChild(closed, closedTab.pidfd()))));
        CHECK(released(place(placement, fakeChild(2, somePidFd()))));
        CHECK(released(place(placement, fakeChild(3, somePidFd()))));
        closedTab.end();
        rig.outwaitCooldown();
        REQUIRE(rig.script->awaitRequests(3));
    }
    CHECK(rig.script->requestedPids() == std::vector { closed, 2, 3 });
}
#endif

TEST_CASE("PlacementReport.describe", "[placement]")
{
    using enum vtpty::PlacementStatus;
    using vtpty::ScopeErrorSeverity;

    struct Row
    {
        vtpty::PlacementReport report;
        ScopeErrorSeverity severity;
        std::string_view mentions;
    };
    auto const rows = std::array {
        Row { told(4242, Placed), ScopeErrorSeverity::Expected, "" },
        Row { told(4242, PlacedUnheard, ScopeError::TimedOut),
              ScopeErrorSeverity::Fault,
              vtpty::traitsOf(ScopeError::TimedOut).description },
        Row { told(4242, Deferred, ScopeError::TimedOut),
              ScopeErrorSeverity::Fault,
              vtpty::traitsOf(ScopeError::TimedOut).description },
        Row { told(4242, Unplaced, ScopeError::Unavailable),
              ScopeErrorSeverity::Expected,
              vtpty::traitsOf(ScopeError::Unavailable).description },
        Row { told(4242, PlacedLate, ScopeError::TimedOut), ScopeErrorSeverity::Fault, "after all" },
        Row { told(4242, Unplaced, ScopeError::Refused),
              ScopeErrorSeverity::Fault,
              vtpty::traitsOf(ScopeError::Refused).description },
        Row { told(4242, Ended), ScopeErrorSeverity::Expected, "ended" },
    };
    for (auto const& row: rows)
    {
        auto const line = vtpty::describe(row.report);
        INFO(line.text);
        CHECK(line.severity == row.severity);
        CHECK(line.text.contains("4242"));
        CHECK(line.text.contains(row.mentions));
    }
}
