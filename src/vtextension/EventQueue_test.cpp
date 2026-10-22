// SPDX-License-Identifier: Apache-2.0
//
// The queue between the terminal's threads and an extension runner: it coalesces, bounds, and never
// blocks a producer.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <ranges>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <vtextension/EventQueue.hpp>

using namespace vtextension;

namespace
{

/// @return A queue's limits: @p capacity, and room for four user-variable names.
[[nodiscard]] EventQueueLimits limits(std::size_t capacity)
{
    return EventQueueLimits { .capacity = capacity, .userVarNames = 4 };
}

} // namespace

TEST_CASE("EventQueue.coalesce", "[vtextension]")
{
    auto queue = EventQueue(limits(4));
    CHECK(queue.tryPush(CwdChanged { .cwd = "/a" }) == PushResult::Queued);
    CHECK(queue.tryPush(CwdChanged { .cwd = "/b" }) == PushResult::Coalesced);
    CHECK(queue.tryPush(CwdChanged { .cwd = "/c" }) == PushResult::Coalesced);

    auto const events = queue.drain();
    REQUIRE(events.size() == 1);
    CHECK(events.front() == Event { CwdChanged { .cwd = "/c" } });
    CHECK(queue.drain().empty());
    CHECK(queue.dropCount() == 0);
}

TEST_CASE("EventQueue.order", "[vtextension]")
{
    // Oldest first, by when each was last pushed: a coalesced event moves to where its newest value went.
    auto queue = EventQueue(limits(4));
    REQUIRE(queue.tryPush(TitleChanged { .title = "one" }) == PushResult::Queued);
    REQUIRE(queue.tryPush(ReplyBytes { .bytes = { 1 } }) == PushResult::Queued);
    REQUIRE(queue.tryPush(CwdChanged { .cwd = "/" }) == PushResult::Queued);
    REQUIRE(queue.tryPush(TitleChanged { .title = "two" }) == PushResult::Coalesced);
    REQUIRE(queue.tryPush(ReplyBytes { .bytes = { 2 } }) == PushResult::Queued);

    CHECK(queue.drain()
          == std::vector<Event> { ReplyBytes { .bytes = { 1 } },
                                  CwdChanged { .cwd = "/" },
                                  TitleChanged { .title = "two" },
                                  ReplyBytes { .bytes = { 2 } } });
}

TEST_CASE("EventQueue.full", "[vtextension]")
{
    auto queue = EventQueue(limits(2));
    CHECK(queue.tryPush(ReplyBytes { .bytes = { 1 } }) == PushResult::Queued);
    CHECK(queue.tryPush(ReplyBytes { .bytes = { 2 } }) == PushResult::Queued);
    CHECK(queue.tryPush(ReplyBytes { .bytes = { 3 } }) == PushResult::Dropped);
    CHECK(queue.dropCount() == 1);

    // A full queue still takes the latest state: a coalescing kind never drops.
    CHECK(queue.tryPush(CwdChanged { .cwd = "/latest" }) == PushResult::Queued);
    CHECK(queue.drain().size() == 3);

    // Draining makes room again.
    CHECK(queue.tryPush(ReplyBytes { .bytes = { 4 } }) == PushResult::Queued);
    CHECK(queue.dropCount() == 1);
}

TEST_CASE("EventQueue.userVarPerKey", "[vtextension]")
{
    auto queue = EventQueue(limits(8));
    CHECK(queue.tryPush(UserVarChanged { .name = "a", .value = "1" }) == PushResult::Queued);
    CHECK(queue.tryPush(UserVarChanged { .name = "b", .value = "2" }) == PushResult::Queued);
    CHECK(queue.tryPush(UserVarChanged { .name = "a", .value = "3" }) == PushResult::Coalesced);

    CHECK(queue.drain()
          == std::vector<Event> { UserVarChanged { .name = "b", .value = "2" },
                                  UserVarChanged { .name = "a", .value = "3" } });
}

TEST_CASE("EventQueue.userVarFlood", "[vtextension]")
{
    // One name updated far more often than the queue holds: every update coalesces, none is dropped, and
    // the latest value is what is delivered.
    constexpr auto Capacity = std::size_t { 4 };
    auto queue = EventQueue(limits(Capacity));
    for (auto const index: std::views::iota(std::size_t { 0 }, 2 * Capacity))
        CHECK(queue.tryPush(UserVarChanged { .name = "x", .value = std::to_string(index) })
              == (index == 0 ? PushResult::Queued : PushResult::Coalesced));
    CHECK(queue.dropCount() == 0);
    CHECK(queue.drain()
          == std::vector<Event> {
              UserVarChanged { .name = "x", .value = std::to_string((2 * Capacity) - 1) } });
}

TEST_CASE("EventQueue.userVarNamesOverflow", "[vtextension]")
{
    // More names than slots: the extra names queue (and drain-coalesce) like replies, within capacity.
    auto queue = EventQueue(EventQueueLimits { .capacity = 2, .userVarNames = 1 });
    CHECK(queue.tryPush(UserVarChanged { .name = "a", .value = "1" }) == PushResult::Queued);
    CHECK(queue.tryPush(UserVarChanged { .name = "b", .value = "1" }) == PushResult::Queued);
    CHECK(queue.tryPush(UserVarChanged { .name = "b", .value = "2" }) == PushResult::Queued);
    CHECK(queue.tryPush(UserVarChanged { .name = "c", .value = "1" }) == PushResult::Dropped);
    CHECK(queue.tryPush(UserVarChanged { .name = "a", .value = "2" }) == PushResult::Coalesced);
    CHECK(queue.drain()
          == std::vector<Event> { UserVarChanged { .name = "b", .value = "2" },
                                  UserVarChanged { .name = "a", .value = "2" } });
    // Slots stay with the names that claimed them: a later name keeps queueing uncoalesced, and is
    // coalesced by name when drained.
    CHECK(queue.tryPush(UserVarChanged { .name = "c", .value = "2" }) == PushResult::Queued);
    CHECK(queue.tryPush(UserVarChanged { .name = "c", .value = "3" }) == PushResult::Queued);
    CHECK(queue.tryPush(UserVarChanged { .name = "a", .value = "3" }) == PushResult::Queued);
    CHECK(queue.drain()
          == std::vector<Event> { UserVarChanged { .name = "c", .value = "3" },
                                  UserVarChanged { .name = "a", .value = "3" } });
}

TEST_CASE("EventQueue.userVar.concurrentDrain", "[vtextension][threading]")
{
    // A producer updates a few names many times while the consumer drains: whatever interleaving, the
    // last value drained for each name is the last value pushed for it. (Catch2's assertions are not
    // thread-safe: the threads record, the test thread checks.)
    constexpr auto Names = std::array { "a", "b", "c", "d" };
    constexpr auto Updates = 20'000;
    auto queue = EventQueue(EventQueueLimits { .capacity = 8, .userVarNames = 2 }); // two names overflow
    auto producing = std::atomic<bool> { true };
    auto lastDrained = std::map<std::string, std::string> {};

    auto const record = [&] {
        for (auto const& event: queue.drain())
            if (auto const* const userVar = std::get_if<UserVarChanged>(&event))
                lastDrained[userVar->name] = userVar->value;
    };
    auto consumer = std::thread([&] {
        while (producing.load())
            record();
    });
    auto producer = std::thread([&] {
        for (auto const update: std::views::iota(0, Updates))
            for (auto const* const name: Names)
            {
                // Retried while the overflow stack is full, so that every name's last value gets in.
                auto event = UserVarChanged { .name = name, .value = std::to_string(update) };
                while (queue.tryPush(event) == PushResult::Dropped)
                    std::this_thread::yield();
            }
        producing.store(false);
    });
    producer.join();
    consumer.join();
    record();

    for (auto const* const name: Names)
    {
        INFO(name);
        CHECK(lastDrained[name] == std::to_string(Updates - 1));
    }
}

TEST_CASE("EventQueue.repliesDoNotCoalesce", "[vtextension]")
{
    auto queue = EventQueue(limits(8));
    REQUIRE(queue.tryPush(ReplyBytes { .bytes = { 1 } }) == PushResult::Queued);
    REQUIRE(queue.tryPush(ReplyBytes { .bytes = { 1 } }) == PushResult::Queued);
    CHECK(queue.drain().size() == 2);
}

TEST_CASE("EventQueue.tryPush.neverBlocks", "[vtextension][threading]")
{
    // A consumer drains continuously while a producer pushes: every push returns, and every event is
    // either delivered or counted as dropped. (Catch2's assertions are not thread-safe: the threads
    // count, the test thread checks.)
    constexpr auto Pushes = std::size_t { 20'000 };
    auto queue = EventQueue(limits(16));
    auto producing = std::atomic<bool> { true };
    auto delivered = std::size_t { 0 };
    auto queued = std::size_t { 0 };

    auto consumer = std::thread([&] {
        while (producing.load(std::memory_order_acquire))
            delivered += queue.drain().size();
        delivered += queue.drain().size();
    });
    auto producer = std::thread([&] {
        for (auto const index: std::views::iota(std::size_t { 0 }, Pushes))
            if (queue.tryPush(ReplyBytes { .bytes = { static_cast<uint8_t>(index) } }) == PushResult::Queued)
                ++queued;
        producing.store(false, std::memory_order_release);
    });
    producer.join();
    consumer.join();

    CHECK(delivered == queued);
    CHECK(queued + queue.dropCount() == Pushes);
}
