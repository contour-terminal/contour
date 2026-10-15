// SPDX-License-Identifier: Apache-2.0
#include <core/net/AcceptLoopHealth.hpp>
#include <core/net/AcceptPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using core::net::AcceptLoopEvent;
using core::net::AcceptLoopEventKind;
using core::net::AcceptLoopHealth;

namespace
{

/// @param surface What the loop serves.
/// @param kind What happened.
/// @return A report of it.
[[nodiscard]] AcceptLoopEvent eventOf(std::string surface, AcceptLoopEventKind kind)
{
    auto line = surface + ": " + std::string { kind == AcceptLoopEventKind::GaveUp ? "gave up" : "degraded" };
    return AcceptLoopEvent {
        .surface = std::move(surface), .line = std::move(line), .error = {}, .kind = kind
    };
}

/// @param health A registry.
/// @return Its conditions, in words, in order.
[[nodiscard]] std::string conditionsOf(AcceptLoopHealth const& health)
{
    auto text = std::string {};
    for (auto const& condition: health.snapshot())
        text += condition.surface
                + (condition.kind == AcceptLoopEventKind::GaveUp ? " gave up; " : " degraded; ");
    return text;
}

} // namespace

TEST_CASE("A degraded surface is recorded until it recovers, and one that gave up for good",
          "[net][accept-policy]")
{
    auto health = AcceptLoopHealth {};
    CHECK(health.snapshot().empty());

    auto told = std::vector<AcceptLoopEventKind> {};
    health.subscribe([&told](AcceptLoopEvent const& event) { told.push_back(event.kind); });

    health.record(eventOf("http", AcceptLoopEventKind::Degraded));
    health.record(eventOf("admin", AcceptLoopEventKind::GaveUp));
    CHECK(conditionsOf(health) == "http degraded; admin gave up; ");

    // A warning is not a condition, and a second degraded report replaces the first.
    health.record(eventOf("http", AcceptLoopEventKind::Warning));
    health.record(eventOf("http", AcceptLoopEventKind::Degraded));
    CHECK(conditionsOf(health) == "admin gave up; http degraded; ");

    health.record(eventOf("http", AcceptLoopEventKind::Recovered));
    CHECK(conditionsOf(health) == "admin gave up; ");

    // A degraded loop that stops -- closed, or cancelled -- is cleared too.
    health.record(eventOf("gossip", AcceptLoopEventKind::Degraded));
    health.record(eventOf("gossip", AcceptLoopEventKind::Stopped));
    CHECK(conditionsOf(health) == "admin gave up; ");
    CHECK(told
          == std::vector { AcceptLoopEventKind::Degraded,
                           AcceptLoopEventKind::GaveUp,
                           AcceptLoopEventKind::Degraded,
                           AcceptLoopEventKind::Recovered,
                           AcceptLoopEventKind::Degraded,
                           AcceptLoopEventKind::Stopped });
}

TEST_CASE("A forwarded registry passes on its conditions, the earlier ones included", "[net][accept-policy]")
{
    // A component with a loop and a registry of its own; the process still reads one. A report
    // recorded before the forward was set up must not be lost to it.
    auto process = AcceptLoopHealth {};
    auto component = AcceptLoopHealth {};
    component.record(eventOf("raft", AcceptLoopEventKind::GaveUp));
    component.forward(process);
    component.record(eventOf("gossip", AcceptLoopEventKind::Degraded));
    component.record(eventOf("gossip", AcceptLoopEventKind::Recovered));

    CHECK(conditionsOf(process) == "raft gave up; ");
    CHECK(conditionsOf(component) == "raft gave up; ");
}

TEST_CASE("A listener may read the registry back without deadlocking", "[net][accept-policy]")
{
    // The listener runs outside the lock, which is what lets a probe answered from inside one see
    // the report it was just told about. Run on a thread of its own and waited for within a bound:
    // a listener called under the lock would block that thread for good (re-locking a held mutex is
    // undefined behaviour, so a hang is the likely shape rather than a promise), and the case says
    // so rather than hanging. The thread is then detached, leaking what it holds, as a failed case may.
    // Shared, so a detached thread still blocked in it keeps everything it touches alive.
    auto const health = std::make_shared<AcceptLoopHealth>();
    auto const seen = std::make_shared<std::size_t>(0);
    health->subscribe(
        [registry = health.get(), seen](AcceptLoopEvent const&) { *seen = registry->snapshot().size(); });
    auto const done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    auto worker = std::thread { [health, done] {
        health->record(eventOf("admin", AcceptLoopEventKind::GaveUp));
        done->set_value();
    } };
    if (finished.wait_for(std::chrono::seconds { 10 }) != std::future_status::ready)
    {
        worker.detach();
        FAIL("recording a report did not return within 10 s: the listener was called under the lock");
    }
    worker.join();
    CHECK(*seen == 1);
}
