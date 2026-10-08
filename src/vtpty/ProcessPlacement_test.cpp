// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessPlacement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <optional>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace
{

/// What a parked child would find on its end of the gate.
enum class GateState : std::uint8_t
{
    Parked,   ///< Nothing to read yet: still parked.
    Released, ///< The release byte.
    Closed,   ///< End-of-file without a byte.
};

/// @return What the child's end of the gate reads within @p wait.
GateState observe(vtpty::OwnedFd const& childEnd, std::chrono::milliseconds wait = 0ms)
{
    auto pollFd = pollfd { .fd = childEnd.get(), .events = POLLIN, .revents = 0 };
    if (::poll(&pollFd, 1, static_cast<int>(wait.count())) != 1)
        return GateState::Parked;
    auto byte = char {};
    return ::read(childEnd.get(), &byte, 1) == 1 ? GateState::Released : GateState::Closed;
}

/// @return A fresh gate; fails the test when none can be made.
vtpty::Gate gate()
{
    auto made = vtpty::makeGate();
    REQUIRE(made.has_value());
    return std::move(*made);
}

} // namespace

TEST_CASE("ParkedChild.release sends one byte", "[placement]")
{
    auto ends = gate();
    auto child = vtpty::ParkedChild { 4242, vtpty::OwnedFd {}, std::move(ends.parentEnd) };
    CHECK(observe(ends.childEnd) == GateState::Parked);
    child.release();
    CHECK(observe(ends.childEnd) == GateState::Released);
}

TEST_CASE("ParkedChild releases on destruction", "[placement]")
{
    auto ends = gate();
    {
        auto const child = vtpty::ParkedChild { 4242, vtpty::OwnedFd {}, std::move(ends.parentEnd) };
        CHECK(observe(ends.childEnd) == GateState::Parked);
    }
    CHECK(observe(ends.childEnd) == GateState::Released);
}

TEST_CASE("ParkedChild release is idempotent and a moved-from child is inert", "[placement]")
{
    auto ends = gate();
    auto moved = std::optional<vtpty::ParkedChild> {};
    {
        auto original = vtpty::ParkedChild { 4242, vtpty::OwnedFd {}, std::move(ends.parentEnd) };
        moved.emplace(std::move(original));
    } // the moved-from original is destroyed here, and must not release
    CHECK(observe(ends.childEnd) == GateState::Parked);
    moved->release();
    moved->release();
    CHECK(observe(ends.childEnd) == GateState::Released);
    CHECK(observe(ends.childEnd) == GateState::Closed); // the second release wrote nothing more
}

TEST_CASE("ParkedChild release after the reader is gone does not raise SIGPIPE", "[placement]")
{
    // Contour does not ignore SIGPIPE. A shell killed while parked (a tab closed at once) leaves
    // nobody on the child's end; releasing it must not take this process down.
    auto* const previous = std::signal(SIGPIPE, SIG_DFL);
    auto ends = gate();
    auto child = vtpty::ParkedChild { 4242, vtpty::OwnedFd {}, std::move(ends.parentEnd) };
    ends.childEnd.reset();
    child.release();
    std::signal(SIGPIPE, previous);
    SUCCEED("still alive");
}

TEST_CASE("Gate ends are close-on-exec", "[placement]")
{
    auto const ends = gate();
    CHECK((::fcntl(ends.parentEnd.get(), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(ends.childEnd.get(), F_GETFD) & FD_CLOEXEC) != 0);
}

TEST_CASE("NoPlacement releases at once", "[placement]")
{
    auto ends = gate();
    auto placement = vtpty::NoPlacement {};
    placement.placeThenRelease(vtpty::ParkedChild { 4242, vtpty::OwnedFd {}, std::move(ends.parentEnd) });
    CHECK(observe(ends.childEnd) == GateState::Released);
}
