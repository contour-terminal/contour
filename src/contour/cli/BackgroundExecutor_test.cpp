// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/BackgroundExecutor.hpp>

#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
    #include <csignal>

    #include <pthread.h>

namespace
{
/// @return Whether @p signal is blocked on the calling thread.
[[nodiscard]] bool isBlockedHere(int signal)
{
    auto mask = sigset_t {};
    pthread_sigmask(SIG_BLOCK, nullptr, &mask);
    return sigismember(&mask, signal) == 1;
}
} // namespace

TEST_CASE("a background worker takes no shutdown signal, whatever thread made it", "[contour][cli][daemon]")
{
    // Made on a thread that blocks nothing, as `contour daemon` makes its pools before it blocks SIGTERM for
    // its sigwait thread: a worker that took the SIGTERM would end the process before its socket is removed.
    REQUIRE_FALSE(isBlockedHere(SIGTERM));
    auto pool = contour::cli::makeBackgroundExecutor(1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto const onWorker = loop.blockOn(
        [](core::async::IExecutor* worker, core::async::IExecutor* home) -> core::async::Task<bool> {
            co_await core::async::ResumeOn { *worker };
            auto const blocked = isBlockedHere(SIGTERM) && isBlockedHere(SIGINT) && !isBlockedHere(SIGSEGV);
            co_await core::async::ResumeOn { *home };
            co_return blocked;
        }(&pool, &loop));
    CHECK(onWorker);
    // The making thread's mask is as it was.
    CHECK_FALSE(isBlockedHere(SIGTERM));
}
#endif
