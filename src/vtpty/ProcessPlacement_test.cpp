// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/GateTesting.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <optional>
#include <utility>

#include <fcntl.h>

using namespace std::chrono_literals;
using vtpty::testing::fakeChild;
using vtpty::testing::GateState;
using vtpty::testing::observe;

TEST_CASE("ParkedChild.release sends one byte", "[placement]")
{
    auto child = fakeChild(4242);
    CHECK(observe(child.waitingEnd, 0ms) == GateState::Parked);
    child.parked.release();
    CHECK(observe(child.waitingEnd, 0ms) == GateState::Released);
}

TEST_CASE("ParkedChild releases on destruction", "[placement]")
{
    auto waitingEnd = vtpty::OwnedFd {};
    {
        auto child = fakeChild(4242);
        waitingEnd = std::move(child.waitingEnd);
        CHECK(observe(waitingEnd, 0ms) == GateState::Parked);
    }
    CHECK(observe(waitingEnd, 0ms) == GateState::Released);
}

TEST_CASE("ParkedChild release is idempotent and a moved-from child is inert", "[placement]")
{
    auto child = fakeChild(4242);
    auto moved = std::optional<vtpty::ParkedChild> {};
    {
        auto original = std::move(child.parked);
        moved.emplace(std::move(original));
    } // the moved-from original is destroyed here, and must not release
    CHECK(observe(child.waitingEnd, 0ms) == GateState::Parked);
    moved->release();
    moved->release();
    CHECK(observe(child.waitingEnd, 0ms) == GateState::Released);
    CHECK(observe(child.waitingEnd, 0ms) == GateState::Closed); // the second release wrote nothing more
}

TEST_CASE("ParkedChild release after the reader is gone does not raise SIGPIPE", "[placement]")
{
    // Contour does not ignore SIGPIPE. A shell killed while parked (a tab closed at once) leaves
    // nobody on the child's end; releasing it must not take this process down.
    auto* const previous = std::signal(SIGPIPE, SIG_DFL);
    auto child = fakeChild(4242);
    child.waitingEnd.reset();
    child.parked.release();
    std::signal(SIGPIPE, previous);
    SUCCEED("still alive");
}

TEST_CASE("Gate ends are close-on-exec", "[placement]")
{
    auto const gate = vtpty::makeGate();
    REQUIRE(gate.has_value());
    CHECK((::fcntl(gate->parentEnd.get(), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(gate->childEnd.get(), F_GETFD) & FD_CLOEXEC) != 0);
}

TEST_CASE("NoPlacement releases at once", "[placement]")
{
    auto child = fakeChild(4242);
    auto placement = vtpty::NoPlacement {};
    placement.placeThenRelease(std::move(child.parked));
    CHECK(observe(child.waitingEnd, 0ms) == GateState::Released);
}
