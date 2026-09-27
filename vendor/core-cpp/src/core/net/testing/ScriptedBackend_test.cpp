// SPDX-License-Identifier: Apache-2.0

/// @file
/// Cases over the test double itself.
///
/// A double that is not faithful about a rule every real backend keeps is worse than
/// no double: a case written against it passes here and fails on four kernels, and the
/// author reads the green as evidence. These pin the places where `ScriptedBackend` is
/// asked to behave like a backend rather than like a script.
///
/// They live in the portable test binary because the double is portable: it has no
/// kernel in it, so a fidelity rule it keeps is one every platform can check.

#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using core::net::Interest;
using core::net::ReadinessHandler;
using core::net::testing::HandlerId;
using core::net::testing::ScriptedBackend;

namespace
{

/// A registration that counts what it was told, so a case can ask what was dispatched
/// rather than only how many callbacks ran.
struct Probe
{
    ReadinessHandler handler {};
    int readable = 0;
    int writable = 0;
    int failed = 0;

    Probe() noexcept
    {
        handler = ReadinessHandler { .handle = core::platform::InvalidHandle,
                                     .kind = core::net::DefaultHandleKind,
                                     .owner = this,
                                     .onReadable = &Probe::onReadableCallback,
                                     .onWritable = &Probe::onWritableCallback,
                                     .onError = &Probe::onErrorCallback };
    }

    Probe(Probe const&) = delete;
    Probe& operator=(Probe const&) = delete;
    Probe(Probe&&) = delete;
    Probe& operator=(Probe&&) = delete;
    ~Probe() = default;

    /// @return How many callbacks of any kind this probe has had.
    [[nodiscard]] int total() const noexcept { return readable + writable + failed; }

    static void onReadableCallback(ReadinessHandler& handler) noexcept
    {
        ++static_cast<Probe*>(handler.owner)->readable;
    }

    static void onWritableCallback(ReadinessHandler& handler) noexcept
    {
        ++static_cast<Probe*>(handler.owner)->writable;
    }

    static void onErrorCallback(ReadinessHandler& handler) noexcept
    {
        ++static_cast<Probe*>(handler.owner)->failed;
    }
};

} // namespace

TEST_CASE("a muted registration is silent on the scripted backend too", "[net][backend][scripted]")
{
    // The rule this double was breaking. `Interest::None` means "mute the handle
    // without detaching it", and every real backend keeps it: poll clears the pollfd,
    // epoll drops the registration out of the set, kqueue deletes both filters. A case
    // that mutes a registration and scripts it readable therefore gets silence on three
    // kernels -- and, until this was fixed, a dispatch
    // here. That is the worst failure a double has: it makes a case that is wrong
    // everywhere look right where it is written.
    auto backend = ScriptedBackend {};
    auto probe = Probe {};

    REQUIRE(backend.attach(probe.handler).has_value());
    auto const id = backend.lastHandlerId();
    REQUIRE(backend.setInterest(probe.handler, Interest::Read).has_value());
    REQUIRE(backend.setInterest(probe.handler, Interest::None).has_value());

    backend.pushReadable(id);
    auto const result = backend.wait(std::nullopt);

    CHECK(result.dispatched == 0);
    CHECK(probe.total() == 0);

    // And muting is not a one-way door here either, or a case could mute but never
    // un-mute and the double would be faithful in one direction only.
    REQUIRE(backend.setInterest(probe.handler, Interest::Read).has_value());
    backend.pushReadable(id);
    CHECK(backend.wait(std::nullopt).dispatched == 1);
    CHECK(probe.readable == 1);
}

TEST_CASE("the scripted backend reports only the direction a registration watches",
          "[net][backend][scripted]")
{
    // A registration watching Read is not told about writability by any real backend:
    // poll never sets POLLOUT on a pollfd that did not ask for it, epoll and kqueue
    // were never given the filter. Scripting the other direction should therefore be
    // as silent as muting, or a B4 case could park a reader and be woken by a write.
    auto backend = ScriptedBackend {};
    auto probe = Probe {};

    REQUIRE(backend.attach(probe.handler).has_value());
    auto const id = backend.lastHandlerId();
    REQUIRE(backend.setInterest(probe.handler, Interest::Read).has_value());

    backend.pushWritable(id);
    CHECK(backend.wait(std::nullopt).dispatched == 0);
    CHECK(probe.total() == 0);

    // A failure is the exception, and deliberately: POLLERR, POLLHUP and POLLNVAL
    // arrive whether or not they were asked for, on every backend. A double that
    // filtered a failure by direction would make an error case unwritable.
    backend.pushFailure(id);
    CHECK(backend.wait(std::nullopt).dispatched == 1);
    CHECK(probe.failed == 1);
}

TEST_CASE("the scripted backend refuses a handler that is already attached", "[net][backend][scripted]")
{
    // What all four real backends answer. Without it the double kept the FIRST id in
    // `_byHandler` while `_live` gained a second, so the registration a case then
    // scripted by id was not the one the loop would detach -- a divergence that shows
    // up as a leak assertion failing for no visible reason.
    auto backend = ScriptedBackend {};
    auto probe = Probe {};

    REQUIRE(backend.attach(probe.handler).has_value());
    auto const first = backend.lastHandlerId();

    auto const again = backend.attach(probe.handler);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == core::net::NetErrorCode::BadHandle);

    // The first registration is untouched: a refused attach changes nothing.
    CHECK(backend.lastHandlerId() == first);
    CHECK(backend.attachedCount() == 1);
}
