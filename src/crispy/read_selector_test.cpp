// SPDX-License-Identifier: Apache-2.0
#include <crispy/read_selector.h>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
    #include <array>
    #include <atomic>
    #include <chrono>
    #include <ranges>
    #include <thread>
    #include <tuple>

    #include <unistd.h>

using crispy::read_selector;
using namespace std::chrono_literals;

namespace
{

/// RAII pair of pipe file descriptors used to feed real, kernel-backed fds into a read_selector.
///
/// The read-end is what gets registered with the selector; writing to the write-end makes the
/// read-end readable, which is how the tests drive wait_one() deterministically.
struct pipe_fds
{
    int reader = -1;
    int writer = -1;

    pipe_fds()
    {
        std::array<int, 2> fds {};
        REQUIRE(::pipe(fds.data()) == 0);
        reader = fds[0];
        writer = fds[1];
    }

    pipe_fds(pipe_fds const&) = delete;
    pipe_fds& operator=(pipe_fds const&) = delete;
    pipe_fds(pipe_fds&&) = delete;
    pipe_fds& operator=(pipe_fds&&) = delete;

    ~pipe_fds()
    {
        if (reader != -1)
            ::close(reader);
        if (writer != -1)
            ::close(writer);
    }

    /// Makes the read-end readable by writing a single byte to the write-end.
    void make_readable() const { REQUIRE(::write(writer, "x", 1) == 1); }
};

} // namespace

TEST_CASE("read_selector.size_accounting")
{
    // Guards the _size bookkeeping that underflowed to 0 in the GUI-tab-close crash: want_read must
    // grow the count and cancel_read must shrink it back, including all the way down to empty.
    auto selector = read_selector {};
    auto a = pipe_fds {};
    auto b = pipe_fds {};

    REQUIRE(selector.size() == 0);

    selector.want_read(a.reader);
    REQUIRE(selector.size() == 1);

    selector.want_read(b.reader);
    REQUIRE(selector.size() == 2);

    selector.cancel_read(a.reader);
    REQUIRE(selector.size() == 1);

    selector.cancel_read(b.reader);
    REQUIRE(selector.size() == 0);
}

TEST_CASE("read_selector.wait_one_returns_ready_fd")
{
    // A readable fd is returned by wait_one(); this is the happy path the read loop relies on.
    auto selector = read_selector {};
    auto a = pipe_fds {};
    selector.want_read(a.reader);
    a.make_readable();

    auto const fd = selector.wait_one(1000ms);
    REQUIRE(fd.has_value());
    REQUIRE(*fd == a.reader);
}

TEST_CASE("read_selector.cancel_during_wait_wakes_via_wakeup")
{
    // Reproduces the close()-during-read() interleaving that crashed: a thread is blocked in
    // wait_one() on a selector whose fds are NOT readable, while another thread cancels one fd and
    // wakes the selector. The waiter must unblock (woken by the independent eventfd/break-pipe, not by
    // any registered fd becoming readable), the size accounting must end correct, and -- crucially --
    // the process must not abort.
    auto selector = read_selector {};
    auto a = pipe_fds {};
    auto b = pipe_fds {};
    selector.want_read(a.reader);
    selector.want_read(b.reader);
    REQUIRE(selector.size() == 2);

    std::atomic<bool> waiterReturned { false };

    // The waiter blocks: neither pipe is readable, so only a wakeup() (or the timeout) can return it.
    auto waiter = std::thread([&] {
        std::ignore = selector.wait_one(5000ms);
        waiterReturned.store(true);
    });

    // Give the waiter a moment to actually enter the blocking epoll_wait/select before mutating.
    std::this_thread::sleep_for(100ms);

    selector.cancel_read(a.reader);
    selector.wakeup();

    waiter.join();

    REQUIRE(waiterReturned.load());
    REQUIRE(selector.size() == 1);

    // The selector is still usable afterwards: the surviving fd can still be waited on and returned.
    b.make_readable();
    auto const fd = selector.wait_one(1000ms);
    REQUIRE(fd.has_value());
    REQUIRE(*fd == b.reader);
}

TEST_CASE("read_selector.is_wanted_tracks_registration")
{
    // is_wanted() is the read-path's re-validation after wait_one() returned: it must report the CURRENT
    // registration so a concurrent cancel_read()/close() is detected and the caller does not ::read() a
    // fd that is no longer registered (and may have been closed/recycled). The old stale-handle guard it
    // replaced could never observe this.
    auto selector = read_selector {};
    auto a = pipe_fds {};
    auto b = pipe_fds {};

    REQUIRE_FALSE(selector.is_wanted(a.reader));

    selector.want_read(a.reader);
    REQUIRE(selector.is_wanted(a.reader));
    REQUIRE_FALSE(selector.is_wanted(b.reader));

    selector.cancel_read(a.reader);
    REQUIRE_FALSE(selector.is_wanted(a.reader));
}

TEST_CASE("read_selector.wait_on_empty_blocks_until_timeout")
{
    // The read loop calls wait_one() even when nothing is registered (master closed + fastpipe EOF). It
    // must BLOCK on the wakeup channel until the timeout instead of returning instantly: an instant
    // nullopt makes the caller's read loop busy-spin a CPU core on session teardown. A wakeup() must still
    // return it early so a concurrent close() is not delayed by the full timeout.
    auto selector = read_selector {};
    REQUIRE(selector.size() == 0);

    // Empty selector, short timeout: wait_one() blocks for ~the timeout, then returns nullopt.
    auto const beforeTimeout = std::chrono::steady_clock::now();
    auto const timedOut = selector.wait_one(120ms);
    auto const elapsed = std::chrono::steady_clock::now() - beforeTimeout;
    REQUIRE_FALSE(timedOut.has_value());
    REQUIRE(elapsed >= 80ms); // did not spin-return; allow slack below the 120ms request

    // wakeup() returns a blocked wait early even on an empty selector (the teardown wake path).
    std::atomic<bool> returned { false };
    auto waiter = std::thread([&] {
        std::ignore = selector.wait_one(5000ms);
        returned.store(true);
    });
    std::this_thread::sleep_for(50ms);
    REQUIRE_FALSE(returned.load()); // still blocked, not spinning
    selector.wakeup();
    waiter.join();
    REQUIRE(returned.load());
}

TEST_CASE("posix_read_selector.concurrent_cancel_during_wait_is_race_free")
{
    // Regression for the data race on posix_read_selector::_fds: wait_one() iterates _fds (building the
    // fd_set) while a concurrent cancel_read() erases from the same vector. On Linux read_selector aliases
    // epoll_read_selector (kernel-synchronized), so this race can ONLY be exercised by naming the posix
    // type explicitly -- which is what makes it reachable in CI. The internal _fdsMutex must serialize the
    // two so no torn read / dangling fd / crash occurs, and the accounting must stay consistent. Repeated
    // many times to widen the interleaving window (and to give ThreadSanitizer, if enabled, a shot at the
    // race). Runs on every platform because posix_read_selector is unconditionally defined.
    for (auto const _: std::views::iota(0, 200))
    {
        std::ignore = _;
        auto selector = crispy::posix_read_selector {};
        auto a = pipe_fds {};
        auto b = pipe_fds {};
        selector.want_read(a.reader);
        selector.want_read(b.reader);

        std::atomic<bool> ready { false };
        auto waiter = std::thread([&] {
            ready.store(true);
            // Neither pipe is readable: only wakeup()/timeout returns this. The timeout bounds the test
            // if the interleaving misses the wakeup.
            std::ignore = selector.wait_one(500ms);
        });

        // Spin until the waiter thread is live, then race a cancel_read()+wakeup() against wait_one()'s
        // _fds iteration.
        while (!ready.load())
            std::this_thread::yield();
        selector.cancel_read(a.reader);
        selector.wakeup();

        waiter.join();
        REQUIRE(selector.size() == 1);
        REQUIRE(selector.is_wanted(b.reader));
        REQUIRE_FALSE(selector.is_wanted(a.reader));
    }
}

#endif // !_WIN32
