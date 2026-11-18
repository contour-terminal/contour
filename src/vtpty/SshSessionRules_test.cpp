// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshSessionRules.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>

using namespace vtpty;

TEST_CASE("remainingSshBudget", "[ssh]")
{
    using namespace std::chrono;
    auto const deadline = steady_clock::now() + seconds { 10 };
    CHECK(remainingSshBudget(deadline, deadline - milliseconds { 2500 }) == milliseconds { 2500 });
    CHECK(remainingSshBudget(deadline, deadline - microseconds { 100 }) == milliseconds { 1 });
    CHECK_FALSE(remainingSshBudget(deadline, deadline).has_value());
    CHECK_FALSE(remainingSshBudget(deadline, deadline + milliseconds { 5 }).has_value());
}

TEST_CASE("sshCallTimeout arms libssh2 with what is left, 0 for no limit", "[ssh]")
{
    using namespace std::chrono;
    auto const now = steady_clock::now();
    CHECK(sshCallTimeout(std::nullopt, now) == milliseconds { 0 });
    CHECK(sshCallTimeout(now + milliseconds { 1500 }, now) == milliseconds { 1500 });
    // Never 0 while a limit applies: libssh2 would read it as none.
    CHECK(sshCallTimeout(now + microseconds { 10 }, now) == milliseconds { 1 });
    CHECK_FALSE(sshCallTimeout(now, now).has_value());
    CHECK_FALSE(sshCallTimeout(now - seconds { 1 }, now).has_value());
}

TEST_CASE("the connect budget stands still while the user is asked", "[ssh]")
{
    using namespace std::chrono;
    auto const start = steady_clock::now();

    SECTION("no limit")
    {
        auto budget = SshConnectBudget { std::nullopt, start };
        budget.pause(start);
        budget.resume(start + seconds { 5 });
        CHECK_FALSE(budget.deadline().has_value());
    }

    SECTION("a limit runs from the start")
    {
        auto const budget = SshConnectBudget { seconds { 30 }, start };
        CHECK(budget.deadline() == start + seconds { 30 });
    }

    SECTION("a pause moves the deadline on by its length")
    {
        auto budget = SshConnectBudget { seconds { 30 }, start };
        budget.pause(start + seconds { 2 });
        budget.resume(start + seconds { 62 });
        CHECK(budget.deadline() == start + seconds { 90 });
    }

    SECTION("pausing twice keeps the first stop; resuming twice counts once")
    {
        auto budget = SshConnectBudget { seconds { 30 }, start };
        budget.pause(start + seconds { 1 });
        budget.pause(start + seconds { 4 });
        budget.resume(start + seconds { 11 });
        budget.resume(start + seconds { 20 });
        CHECK(budget.deadline() == start + seconds { 40 });
    }

    SECTION("resuming a running clock changes nothing")
    {
        auto budget = SshConnectBudget { seconds { 30 }, start };
        budget.resume(start + seconds { 10 });
        CHECK(budget.deadline() == start + seconds { 30 });
    }

    SECTION("two pauses add up")
    {
        auto budget = SshConnectBudget { seconds { 30 }, start };
        budget.pause(start + seconds { 1 });
        budget.resume(start + seconds { 3 });
        budget.pause(start + seconds { 5 });
        budget.resume(start + seconds { 10 });
        CHECK(budget.deadline() == start + seconds { 37 });
    }
}

TEST_CASE("an enormous connect limit saturates instead of overflowing", "[ssh]")
{
    using namespace std::chrono;
    auto const start = steady_clock::now();
    auto budget = SshConnectBudget { milliseconds::max(), start };
    CHECK(budget.deadline() == steady_clock::time_point::max());
    budget.pause(start);
    budget.resume(start + hours { 1 });
    CHECK(budget.deadline() == steady_clock::time_point::max());
    CHECK(sshCallTimeout(budget.deadline(), start).value() > hours { 24 });
}

TEST_CASE("the agent allowance is the longer of its own and the connect limit", "[ssh]")
{
    using namespace std::chrono;
    STATIC_CHECK_FALSE(sshAgentAllowance(std::nullopt).has_value());
    STATIC_CHECK(sshAgentAllowance(seconds { 30 }) == seconds { 120 });
    STATIC_CHECK(sshAgentAllowance(seconds { 120 }) == seconds { 120 });
    STATIC_CHECK(sshAgentAllowance(seconds { 600 }) == seconds { 600 });
}

TEST_CASE("the agent step has its own allowance, and none without a connect limit", "[ssh]")
{
    using namespace std::chrono;
    auto const start = steady_clock::now();
    struct Row
    {
        std::optional<milliseconds> connectLimit;
        std::optional<steady_clock::time_point> expected;
    };
    auto const table = std::array {
        // No connect limit, no agent limit.
        Row { .connectLimit = std::nullopt, .expected = std::nullopt },
        // A connect limit shorter than the allowance gives the allowance.
        Row { .connectLimit = seconds { 1 }, .expected = start + seconds { 120 } },
        Row { .connectLimit = seconds { 30 }, .expected = start + SshAgentAuthAllowance },
        Row { .connectLimit = seconds { 120 }, .expected = start + seconds { 120 } },
        // A longer one gives itself.
        Row { .connectLimit = seconds { 300 }, .expected = start + seconds { 300 } },
        Row { .connectLimit = hours { 24 }, .expected = start + hours { 24 } },
    };
    for (auto const& row: table)
    {
        INFO("connect limit " << (row.connectLimit ? row.connectLimit->count() : -1) << " ms");
        CHECK(sshAgentDeadline(row.connectLimit, start) == row.expected);
    }
    CHECK(sshAgentDeadline(seconds { 30 }, steady_clock::time_point::max() - seconds { 1 })
          == steady_clock::time_point::max());
    CHECK(saturatingDeadline(start, milliseconds::max()) == steady_clock::time_point::max());
    CHECK(saturatingDeadline(start, milliseconds { 5 }) == start + milliseconds { 5 });
}

TEST_CASE("libssh2's timeout argument saturates at the largest long", "[ssh]")
{
    using namespace std::chrono;
    STATIC_CHECK(sshTimeoutArgument(milliseconds { 0 }) == 0);
    STATIC_CHECK(sshTimeoutArgument(milliseconds { 1500 }) == 1500);
    STATIC_CHECK(sshTimeoutArgument(MaxSshConnectTimeout) == 86'400'000);
    STATIC_CHECK(sshTimeoutArgument(milliseconds::max()) == std::numeric_limits<long>::max());
}

TEST_CASE("each resolved address gets its share of what is left", "[ssh]")
{
    using namespace std::chrono;
    struct Row
    {
        std::optional<milliseconds> remaining;
        std::size_t addressesLeft {};
        std::optional<milliseconds> expected;
    };
    auto const table = std::array {
        Row { .remaining = std::nullopt, .addressesLeft = 3, .expected = std::nullopt },
        Row { .remaining = milliseconds { 9000 }, .addressesLeft = 3, .expected = milliseconds { 3000 } },
        Row { .remaining = milliseconds { 9000 }, .addressesLeft = 2, .expected = milliseconds { 4500 } },
        // The last address gets everything that is left.
        Row { .remaining = milliseconds { 9000 }, .addressesLeft = 1, .expected = milliseconds { 9000 } },
        Row { .remaining = milliseconds { 9000 }, .addressesLeft = 0, .expected = milliseconds { 9000 } },
        // A share below 2 s is raised to 2 s ...
        Row { .remaining = milliseconds { 3000 }, .addressesLeft = 3, .expected = milliseconds { 2000 } },
        Row { .remaining = milliseconds { 9000 }, .addressesLeft = 10, .expected = milliseconds { 2000 } },
        // ... but never past what is left.
        Row { .remaining = milliseconds { 1500 }, .addressesLeft = 3, .expected = milliseconds { 1500 } },
        Row { .remaining = milliseconds { 2 }, .addressesLeft = 5, .expected = milliseconds { 2 } },
    };
    for (auto const& row: table)
    {
        INFO("remaining " << (row.remaining ? row.remaining->count() : -1) << " left " << row.addressesLeft);
        CHECK(sshAddressConnectLimit(row.remaining, row.addressesLeft) == row.expected);
    }
}

TEST_CASE("libssh2's own timeout is a timeout whatever the clock says", "[ssh]")
{
    using namespace std::chrono;
    using enum SshCallError;
    using enum SshStepFailure;
    auto const now = steady_clock::now();
    struct Row
    {
        SshCallError error {};
        std::optional<steady_clock::time_point> deadline;
        SshStepFailure expected {};
    };
    auto const table = std::array {
        // libssh2 counts whole seconds, so its timeout can fire with our deadline still ahead.
        Row { .error = Timeout, .deadline = now + seconds { 1 }, .expected = TimedOut },
        Row { .error = Timeout, .deadline = now, .expected = TimedOut },
        Row { .error = Timeout, .deadline = std::nullopt, .expected = TimedOut },
        // Any other failure is a timeout only once the deadline has passed.
        Row { .error = Other, .deadline = now + seconds { 1 }, .expected = Failed },
        Row { .error = Other, .deadline = now, .expected = TimedOut },
        Row { .error = Other, .deadline = now - seconds { 1 }, .expected = TimedOut },
        Row { .error = Other, .deadline = std::nullopt, .expected = Failed },
    };
    for (auto const& row: table)
    {
        INFO("error " << static_cast<int>(row.error) << " deadline "
                      << (row.deadline ? duration_cast<milliseconds>(*row.deadline - now).count() : -1));
        CHECK(classifySshStepFailure(row.error, row.deadline, now) == row.expected);
    }
}

TEST_CASE("a closed agent is told apart from a slow one", "[ssh]")
{
    using namespace std::chrono;
    using enum SshAgentIo;
    using enum SshCallError;
    auto const now = steady_clock::now();
    auto const ahead = std::optional { now + seconds { 60 } };
    auto const passed = std::optional { now - seconds { 1 } };
    auto const none = std::optional<steady_clock::time_point> {};
    struct Row
    {
        SshAgentIo io {};
        SshCallError error {};
        std::optional<steady_clock::time_point> deadline;
        SshAgentFailure expected {};
    };
    auto const table = std::array {
        // Gone is gone, whatever the clock or libssh2 says: no talk of a pending approval.
        Row { .io = Closed, .error = Other, .deadline = ahead, .expected = SshAgentFailure::AgentClosed },
        Row { .io = Closed, .error = Other, .deadline = passed, .expected = SshAgentFailure::AgentClosed },
        Row { .io = Closed, .error = Timeout, .deadline = passed, .expected = SshAgentFailure::AgentClosed },
        Row { .io = Closed, .error = Other, .deadline = none, .expected = SshAgentFailure::AgentClosed },
        // A wait on the agent that ran out, or a deadline passed, is a timeout.
        Row { .io = TimedOut, .error = Other, .deadline = ahead, .expected = SshAgentFailure::TimedOut },
        Row { .io = Fine, .error = Other, .deadline = passed, .expected = SshAgentFailure::TimedOut },
        Row { .io = Fine, .error = Timeout, .deadline = ahead, .expected = SshAgentFailure::TimedOut },
        // Anything else is a plain failure; without a limit nothing is a timeout.
        Row { .io = Fine, .error = Other, .deadline = ahead, .expected = SshAgentFailure::Failed },
        Row { .io = Fine, .error = Timeout, .deadline = none, .expected = SshAgentFailure::Failed },
        Row { .io = TimedOut, .error = Other, .deadline = none, .expected = SshAgentFailure::Failed },
    };
    for (auto const& row: table)
    {
        INFO("io " << static_cast<int>(row.io) << " error " << static_cast<int>(row.error) << " deadline "
                   << (row.deadline ? duration_cast<milliseconds>(*row.deadline - now).count() : 0));
        CHECK(classifySshAgentFailure(row.io, row.error, row.deadline, now) == row.expected);
    }
}

TEST_CASE("a read of an ended session asks for injected text before it reports the end", "[ssh]")
{
    STATIC_CHECK(sshClosedReadErrno(InjectedRead::Pending) == EINTR);
    STATIC_CHECK(sshClosedReadErrno(InjectedRead::Empty) == EPIPE);
}
