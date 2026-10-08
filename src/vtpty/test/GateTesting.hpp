// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ProcessPlacement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <utility>

#include <poll.h>
#include <unistd.h>

namespace vtpty::testing
{

/// What a parked child would find on its end of the gate.
enum class GateState : std::uint8_t
{
    Parked,   ///< Nothing to read: still parked.
    Released, ///< The release byte.
    Closed,   ///< End-of-file without a byte.
};

/// @param waitingEnd The end a parked child waits on.
/// @param wait       How long to wait for anything to arrive.
/// @return What the end reads within @p wait.
inline GateState observe(OwnedFd const& waitingEnd, std::chrono::milliseconds wait)
{
    auto pollFd = pollfd { .fd = waitingEnd.get(), .events = POLLIN, .revents = 0 };
    if (::poll(&pollFd, 1, static_cast<int>(wait.count())) != 1)
        return GateState::Parked;
    auto byte = char {};
    return ::read(waitingEnd.get(), &byte, 1) == 1 ? GateState::Released : GateState::Closed;
}

/// @return Whether the child waiting on @p waitingEnd is let go within ten seconds.
inline bool released(OwnedFd const& waitingEnd)
{
    return observe(waitingEnd, std::chrono::seconds { 10 }) == GateState::Released;
}

/// A ParkedChild over a real gate, and the end a forked child would be waiting on.
struct FakeChild
{
    ParkedChild parked;
    OwnedFd waitingEnd;
};

/// @param pid   The pid the child claims; nothing ever signals it.
/// @param pidfd The pidfd it carries, if any.
/// @return A parked child that no process stands behind.
inline FakeChild fakeChild(int pid, OwnedFd pidfd = OwnedFd {})
{
    auto gate = makeGate();
    REQUIRE(gate.has_value());
    return FakeChild { .parked = ParkedChild { pid, std::move(pidfd), std::move(gate->parentEnd) },
                       .waitingEnd = std::move(gate->childEnd) };
}

} // namespace vtpty::testing
