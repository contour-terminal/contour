// SPDX-License-Identifier: Apache-2.0
///
/// The parts of the @c IoBackend contract that are pure: which callback a readiness
/// selects, what a ready batch does with an entry withdrawn mid-dispatch, and how a
/// timeout converts.
///
/// They are tested here, without a kernel, because each of them is where a real
/// defect lived and none of them needs one: a socket cannot be made to report
/// `EPOLLERR` on demand, and a race between a callback and a detach is not something
/// a case can schedule. `BackendParity_test` then proves every real backend agrees
/// with the rules pinned here.
#include <core/net/IoBackend.hpp>
#include <core/net/detail/ReadyBatch.hpp>
#include <core/net/detail/WaitTimeout.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

using core::net::Readiness;
using core::net::ReadinessHandler;
using core::net::selectReadinessCallback;

namespace
{

void onReadable(ReadinessHandler&) noexcept
{
}

void onWritable(ReadinessHandler&) noexcept
{
}

void onError(ReadinessHandler&) noexcept
{
}

} // namespace

TEST_CASE("a readable event selects the read callback", "[net][iobackend][readiness]")
{
    auto const handler = ReadinessHandler { .onReadable = &onReadable, .onWritable = &onWritable };

    CHECK(selectReadinessCallback(handler, Readiness::Readable) == &onReadable);
    CHECK(selectReadinessCallback(handler, Readiness::Writable) == &onWritable);
}

TEST_CASE("a failure reaches onError even when no direction is signalled", "[net][iobackend][readiness]")
{
    // The case `onError` exists for: a failed outbound connect arrives with the error
    // bits and NEITHER direction set. Before there was anywhere for it to go, it
    // matched no branch, and the registration being level-triggered meant it was
    // reported again immediately — a loop at 100% CPU that never told anyone.
    auto const handler =
        ReadinessHandler { .onReadable = &onReadable, .onWritable = &onWritable, .onError = &onError };

    CHECK(selectReadinessCallback(handler, Readiness::Failed) == &onError);
}

TEST_CASE("a watched direction outranks a failure that arrived with it", "[net][iobackend][readiness]")
{
    // Ruling R101, and it is data loss rather than a routing preference. A peer
    // hangup on a socket that still has UNREAD BYTES arrives as POLLIN|POLLHUP on
    // poll and epoll. This function returns exactly one callback, so the older order
    // — failure first — returned `onError` alone the moment a handler set the field,
    // the reader was never woken, and those bytes were never read. kqueue reports
    // the same hangup as readable and was always right.
    //
    // The portable idiom is the reason this is the correct order and not merely the
    // safer one: no platform lets you learn what went wrong from the readiness bits.
    // You are woken, you call read() or write(), and THAT reports the error. A reader
    // needs the wakeup so it can read 0; a dial needs the wakeup and then checks
    // SO_ERROR. Neither consults which callback fired.
    auto const handler =
        ReadinessHandler { .onReadable = &onReadable, .onWritable = &onWritable, .onError = &onError };

    CHECK(selectReadinessCallback(handler, Readiness::Readable | Readiness::Failed) == &onReadable);
    CHECK(selectReadinessCallback(handler, Readiness::Writable | Readiness::Failed) == &onWritable);

    // onError is not unreachable, it is a LAST RESORT: it takes the failure only when
    // the direction it accompanied is one this handler does not watch, so there is no
    // read or write for the caller to have the error reported through.
    auto const readerOnly = ReadinessHandler { .onReadable = &onReadable, .onError = &onError };
    CHECK(selectReadinessCallback(readerOnly, Readiness::Writable | Readiness::Failed) == &onError);
}

TEST_CASE("a failure with no onError falls back to a watched direction", "[net][iobackend][readiness]")
{
    // What every park the event loop makes relies on: it registers one direction and
    // no onError, and expects a hangup to wake it so it can look and report EOF.
    auto const reader = ReadinessHandler { .onReadable = &onReadable };
    auto const writer = ReadinessHandler { .onWritable = &onWritable };

    CHECK(selectReadinessCallback(reader, Readiness::Failed) == &onReadable);
    CHECK(selectReadinessCallback(writer, Readiness::Failed) == &onWritable);
}

TEST_CASE("a handler watching nothing selects no callback", "[net][iobackend][readiness]")
{
    auto const deaf = ReadinessHandler {};
    CHECK(selectReadinessCallback(deaf, Readiness::Readable | Readiness::Writable | Readiness::Failed)
          == nullptr);

    auto const reader = ReadinessHandler { .onReadable = &onReadable };
    CHECK(selectReadinessCallback(reader, Readiness::Writable) == nullptr);
    CHECK(selectReadinessCallback(reader, Readiness::None) == nullptr);
}

namespace
{

/// A handler that counts its own dispatches and can act on the batch it is being
/// dispatched from — which is what a real callback does when the coroutine it
/// enqueues goes on to close another socket.
struct CountingPeer
{
    ReadinessHandler handler {};
    int dispatched = 0;
    bool sawDispatchInFlight = false;
    core::net::detail::ReadyBatch* batch = nullptr;
    ReadinessHandler* withdraw = nullptr;

    static void onReady(ReadinessHandler& handler) noexcept
    {
        auto* const self = static_cast<CountingPeer*>(handler.owner);
        ++self->dispatched;
        self->sawDispatchInFlight = core::net::detail::readinessDispatchInFlight();
        if (self->batch != nullptr && self->withdraw != nullptr)
            self->batch->withdraw(*self->withdraw);
    }

    void arm() noexcept
    {
        handler.owner = this;
        handler.onReadable = &CountingPeer::onReady;
    }
};

} // namespace

TEST_CASE("a handler withdrawn earlier in the same batch is not dispatched", "[net][iobackend][batch]")
{
    // fastcached#475, in the form that needs no kernel. Dropping a kernel
    // registration stops FUTURE reports and does nothing about an entry the wait has
    // already written into the batch being walked; a callback that detaches another
    // handler and frees its owner would otherwise leave that entry dangling, and the
    // walk reads it. Under a sanitizer the real thing is a heap-use-after-free rather
    // than a failed assertion, which is why the rule is pinned here where it can fail
    // as an assertion instead.
    auto batch = core::net::detail::ReadyBatch {};
    auto first = CountingPeer {};
    auto second = CountingPeer {};
    first.arm();
    second.arm();
    first.batch = &batch;
    first.withdraw = &second.handler;

    batch.add(first.handler, Readiness::Readable);
    batch.add(second.handler, Readiness::Readable);
    REQUIRE(batch.size() == 2);

    CHECK(batch.dispatch() == 1);
    CHECK(first.dispatched == 1);
    CHECK(second.dispatched == 0); // withdrawn from inside the first callback
}

TEST_CASE("a handler reported twice in one batch is dispatched once", "[net][iobackend][batch]")
{
    // kqueue answers per (descriptor, filter), so a registration watching both
    // directions arrives as two events. Dispatching it twice would call back into an
    // object the first callback may have left ready to be freed, so the two reports
    // are merged into one entry — and "at most one callback per registration per
    // wait" then holds on every backend, including the ones whose kernel never
    // duplicates.
    auto batch = core::net::detail::ReadyBatch {};
    auto peer = CountingPeer {};
    peer.arm();
    peer.handler.onWritable = &CountingPeer::onReady;

    batch.add(peer.handler, Readiness::Readable);
    batch.add(peer.handler, Readiness::Writable);
    CHECK(batch.size() == 1);

    CHECK(batch.dispatch() == 1);
    CHECK(peer.dispatched == 1);
}

TEST_CASE("a batch dispatch publishes that it is in flight, and stops when it ends",
          "[net][iobackend][batch]")
{
    // The other half of Rule 1: the loop asserts this is false wherever it resumes a
    // coroutine, so a backend that resumed from inside its own walk fails with a
    // stack. That assertion is only worth anything if the flag is actually raised
    // while a callback runs, which is what this pins.
    auto batch = core::net::detail::ReadyBatch {};
    auto peer = CountingPeer {};
    peer.arm();

    CHECK_FALSE(core::net::detail::readinessDispatchInFlight());
    batch.add(peer.handler, Readiness::Readable);
    CHECK(batch.dispatch() == 1);
    CHECK(peer.sawDispatchInFlight);
    CHECK_FALSE(core::net::detail::readinessDispatchInFlight());
}

TEST_CASE("a dispatched batch is empty, so a later withdrawal scans nothing", "[net][iobackend][batch]")
{
    auto batch = core::net::detail::ReadyBatch {};
    auto peer = CountingPeer {};
    peer.arm();

    batch.add(peer.handler, Readiness::Readable);
    CHECK(batch.dispatch() == 1);
    CHECK(batch.size() == 0);

    // A detach arriving after the wait finished must be harmless, not a second walk
    // over entries whose handlers may already be gone.
    batch.withdraw(peer.handler);
    CHECK(batch.size() == 0);
    CHECK(peer.dispatched == 1);
}

TEST_CASE("a backend timeout converts to the millisecond count a native wait takes",
          "[net][iobackend][timeout]")
{
    using core::net::detail::toTimeoutMillis;
    using namespace std::chrono_literals;

    CHECK(toTimeoutMillis(std::nullopt) == -1); // block until something happens
    CHECK(toTimeoutMillis(core::platform::SteadyDuration::zero()) == 0);
    CHECK(toTimeoutMillis(std::chrono::duration_cast<core::platform::SteadyDuration>(250ms)) == 250);

    // The interesting one: a positive remainder under a millisecond truncates to 0,
    // and a wait of 0 is a poll — a loop whose next deadline is 400µs away would then
    // spin at 100% CPU until the deadline crossed.
    CHECK(toTimeoutMillis(std::chrono::duration_cast<core::platform::SteadyDuration>(400us)) == 1);
    CHECK(toTimeoutMillis(std::chrono::duration_cast<core::platform::SteadyDuration>(1ns)) == 1);

    // A negative duration is a deadline already past, which is a poll and not a block.
    CHECK(toTimeoutMillis(std::chrono::duration_cast<core::platform::SteadyDuration>(-5ms)) == 0);

    CHECK(toTimeoutMillis(std::chrono::duration_cast<core::platform::SteadyDuration>(24h * 365))
          == std::numeric_limits<int>::max());
}

TEST_CASE("every backend kind renders as words", "[net][iobackend]")
{
    using core::net::BackendKind;
    using core::net::toString;

    // Every kind, by the sentinel rather than by a restated list: a kind added without
    // a row in toString() fails to compile, and one added without a row here fails
    // this assertion instead of silently going unnamed in every section label.
    auto named = std::vector<std::string_view> {};
    for (auto const value: std::views::iota(std::size_t { 0 }, static_cast<std::size_t>(BackendKind::Last)))
    {
        auto const rendered = toString(static_cast<BackendKind>(value));
        CHECK(rendered != "unknown");
        named.push_back(rendered);
    }
    CHECK(named.size() == static_cast<std::size_t>(BackendKind::Last));

    CHECK(toString(BackendKind::Last) == "unknown");
    CHECK(toString(static_cast<BackendKind>(200)) == "unknown");
}

TEST_CASE("BackendKind names exactly the backends core::net has", "[net][iobackend][kind]")
{
    // Every kind up to `Last`, by the name `toString` gives it. The WFMO backend was removed once
    // IOCP had been the Windows default for a release (core-cpp#6), so a kind that still named it
    // would be a backend nothing can build and every matrix row would have to skip.
    auto names = std::vector<std::string_view> {};
    for (auto const value: std::views::iota(0, static_cast<int>(core::net::BackendKind::Last)))
        names.push_back(core::net::toString(static_cast<core::net::BackendKind>(value)));
    CHECK(names
          == std::vector<std::string_view> {
              "poll", "epoll", "kqueue", "iocp", "host-driven", "scripted", "null" });
}
