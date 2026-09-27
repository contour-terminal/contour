// SPDX-License-Identifier: Apache-2.0
//
// A flow parked through `EventLoop::waitReadable` on a descriptor that is announced closing is
// finished by ONE `runUntilIdle`.
//
// The POSIX half of `ClosedParkIdle_test.cpp`, and the one that can fail here: a `PosixSocket`
// read closed under it is settled inline by `close()` and never reaches the closed-park path, so
// that file's socket case is a control on POSIX. This one parks through the loop's own coroutine
// awaiter, which is what `AcceptLoop` and every caller watching a raw descriptor use, so the wake
// can only come from the parks `notifyHandleClosing` recorded -- queued by the next turn, which
// until the fix still called itself idle.
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/BackendMatrix.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>

#include <array>
#include <tuple>

#include <fcntl.h>
#include <unistd.h>

using core::async::OperationCancelled;
using core::async::Task;
using core::net::EventLoop;
using core::net::FdWakePolicy;
using core::net::testing::BackendMatrix;

namespace
{

/// How the parked wait ended.
struct Outcome
{
    bool resolved = false; ///< Whether it finished at all.
    bool threw = false;    ///< Whether it unwound through OperationCancelled.
};

/// Parks on @p fd's readability and records how that ended.
Task<void> waitOnce(EventLoop* loop, int fd, Outcome* out)
{
    try
    {
        co_await loop->waitReadable(fd);
        out->resolved = true;
    }
    catch (OperationCancelled const&)
    {
        out->resolved = true;
        out->threw = true;
    }
}

} // namespace

TEST_CASE("runUntilIdle does not return while a closed descriptor's waitReadable is still queued",
          "[net][loop][idle]")
{
    // Resume: an owner that closes its own descriptor and is still alive, so the flow resumes on
    // its normal path. Cancel: an owner being destroyed, so the flow unwinds. Both reach the flow
    // through the closed-park queue, and both must be through it when the drain returns.
    struct Policy
    {
        FdWakePolicy policy;
        bool throws;
        char const* name;
    };
    constexpr auto Policies = std::array {
        Policy { .policy = FdWakePolicy::Resume, .throws = false, .name = "resume" },
        Policy { .policy = FdWakePolicy::Cancel, .throws = true, .name = "cancel" },
    };
    for (auto const& backend: BackendMatrix)
    {
        for (auto const& policy: Policies)
        {
            auto source = core::net::makeBackend(backend.kind);
            if (!source)
                continue;
            DYNAMIC_SECTION("backend=" << backend.name << " policy=" << policy.name)
            {
                auto fds = std::array<int, 2> { -1, -1 };
                REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
                REQUIRE(::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK) == 0);

                auto loop = EventLoop { *source };
                auto outcome = Outcome {};
                loop.spawn(waitOnce(&loop, fds[0], &outcome));
                std::ignore = loop.runUntilIdle();
                // Parked: the peer never writes, so only the close can end it.
                REQUIRE_FALSE(outcome.resolved);

                loop.notifyHandleClosing(fds[0], policy.policy);
                ::close(fds[0]);
                std::ignore = loop.runUntilIdle();

                CHECK(outcome.resolved);
                CHECK(outcome.threw == policy.throws);
                ::close(fds[1]);
            }
        }
    }
}
