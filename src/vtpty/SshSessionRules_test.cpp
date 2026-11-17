// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshSessionRules.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
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

TEST_CASE("libssh2's own timeout is a timeout whatever the clock says", "[ssh]")
{
    using namespace std::chrono;
    using enum SshCallError;
    using enum SshStepFailure;
    auto const now = steady_clock::now();
    struct Row
    {
        SshCallError error;
        std::optional<steady_clock::time_point> deadline;
        SshStepFailure expected;
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

TEST_CASE("a read of an ended session asks for injected text before it reports the end", "[ssh]")
{
    STATIC_CHECK(sshClosedReadErrno(InjectedRead::Pending) == EINTR);
    STATIC_CHECK(sshClosedReadErrno(InjectedRead::Empty) == EPIPE);
}
