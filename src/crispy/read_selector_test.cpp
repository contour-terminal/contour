// SPDX-License-Identifier: Apache-2.0
#include <crispy/read_selector.h>

#include <catch2/catch_test_macros.hpp>

#if !defined(_WIN32)
    #include <array>
    #include <atomic>
    #include <chrono>
    #include <thread>

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

#endif // !_WIN32
