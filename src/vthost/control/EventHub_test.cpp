// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <format>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <vtcontrol/ObjectRef.hpp>
#include <vtcontrol/testing/FakeControlApi.hpp>
#include <vthost/control/BlockWaiters.hpp>
#include <vthost/control/EventHub.hpp>

using vtcontrol::testing::RecordingEventSink;
using vthost::control::BlockWaiters;
using vthost::control::EventFilter;
using vthost::control::EventHub;

namespace
{
[[nodiscard]] vtcontrol::Uuid uuidOf(unsigned n)
{
    return *vtcontrol::parseUuid(std::format("00000000-0000-7000-8000-{:012x}", n));
}

[[nodiscard]] vtcontrol::Event bellIn(vtcontrol::Uuid pane)
{
    return vtcontrol::Event { .origin = { .window = {},
                                          .tab = {},
                                          .pane = vtcontrol::toString(vtcontrol::ObjectRef {
                                              .kind = vtcontrol::ObjectKind::Pane, .uuid = pane }) },
                              .payload = vtcontrol::BellEvent {} };
}

[[nodiscard]] vtcontrol::BlockInfo blockOf(vtcontrol::Uuid pane, uint64_t serial)
{
    auto info = vtcontrol::BlockInfo {};
    info.id = vtcontrol::toString(vtcontrol::BlockRef { .pane = pane, .serial = serial });
    info.state = vtcontrol::BlockState::Finished;
    return info;
}
} // namespace

TEST_CASE("EventHub delivers to the subscriptions whose filter takes an event", "[vthost][control][events]")
{
    auto hub = EventHub { 64 };
    auto sink = RecordingEventSink {};
    auto const all = *hub.subscribe(sink, EventFilter {});
    auto const bells = *hub.subscribe(sink, EventFilter { .kinds = { "pane.bell" }, .pane = std::nullopt });
    auto const titles =
        *hub.subscribe(sink, EventFilter { .kinds = { "pane.titleChanged" }, .pane = std::nullopt });
    auto const one = *hub.subscribe(sink, EventFilter { .kinds = {}, .pane = uuidOf(2) });
    CHECK(all != bells);

    hub.publish(bellIn(uuidOf(1)));
    REQUIRE(sink.deliveries.size() == 2);
    CHECK(sink.deliveries[0].subscription == all);
    CHECK(sink.deliveries[1].subscription == bells);

    sink.deliveries.clear();
    hub.publish(bellIn(uuidOf(2)));
    CHECK(sink.deliveries.size() == 3);
    CHECK(sink.deliveries.back().subscription == one);
    std::ignore = titles;
}

TEST_CASE("EventHub unsubscribes only a sink's own subscription", "[vthost][control][events]")
{
    auto hub = EventHub { 64 };
    auto mine = RecordingEventSink {};
    auto theirs = RecordingEventSink {};
    auto const id = *hub.subscribe(mine, EventFilter {});

    auto const foreign = hub.unsubscribe(theirs, id);
    REQUIRE_FALSE(foreign.has_value());
    CHECK(foreign.error().code == vtcontrol::ControlErrorCode::NotFound);
    CHECK(hub.unsubscribe(mine, id).has_value());
    CHECK(mine.ended == std::vector<uint32_t> { id });
    CHECK(theirs.ended.empty());
    CHECK_FALSE(hub.unsubscribe(mine, id).has_value());
    CHECK(hub.size() == 0);

    std::ignore = *hub.subscribe(mine, EventFilter {});
    std::ignore = *hub.subscribe(theirs, EventFilter {});
    hub.forget(mine);
    hub.publish(bellIn(uuidOf(1)));
    CHECK(mine.deliveries.empty());
    CHECK(theirs.deliveries.size() == 1);
}

TEST_CASE("BlockWaiters complete each waiter once", "[vthost][control][wait]")
{
    auto waiters = BlockWaiters { 256 };
    auto sink = RecordingEventSink {};
    auto results = std::vector<std::expected<vtcontrol::BlockInfo, vtcontrol::ControlError>> {};
    auto const record = [&](std::expected<vtcontrol::BlockInfo, vtcontrol::ControlError> result) {
        results.push_back(std::move(result));
    };

    REQUIRE(waiters.waitFor(sink, vtcontrol::BlockRef { .pane = uuidOf(1), .serial = 5 }, record));
    REQUIRE(waiters.waitForNext(sink, uuidOf(1), 5, record));
    REQUIRE(waiters.waitFor(sink, vtcontrol::BlockRef { .pane = uuidOf(2), .serial = 5 }, record));

    waiters.blockEnded(blockOf(uuidOf(1), 5));
    REQUIRE(results.size() == 1);
    CHECK(results[0]->id == blockOf(uuidOf(1), 5).id);

    waiters.blockEnded(blockOf(uuidOf(1), 7));
    REQUIRE(results.size() == 2);
    CHECK(results[1]->id == blockOf(uuidOf(1), 7).id);
    waiters.blockEnded(blockOf(uuidOf(1), 8));
    CHECK(results.size() == 2);

    waiters.paneClosed(uuidOf(2));
    REQUIRE(results.size() == 3);
    REQUIRE_FALSE(results[2].has_value());
    CHECK(results[2].error().code == vtcontrol::ControlErrorCode::NotFound);
    CHECK(results[2].error().message.contains("closed"));
    CHECK(waiters.size() == 0);

    REQUIRE(waiters.waitFor(sink, vtcontrol::BlockRef { .pane = uuidOf(3), .serial = 1 }, record));
    waiters.forget(sink);
    waiters.blockEnded(blockOf(uuidOf(3), 1));
    CHECK(results.size() == 3);
}

TEST_CASE("EventHub holds a sink's subscriptions up to its share", "[vthost][control][events][limits]")
{
    auto hub = EventHub { 2 };
    auto mine = RecordingEventSink {};
    auto theirs = RecordingEventSink {};
    auto const first = *hub.subscribe(mine, EventFilter {});
    std::ignore = *hub.subscribe(mine, EventFilter {});

    auto const third = hub.subscribe(mine, EventFilter {});
    REQUIRE_FALSE(third.has_value());
    CHECK(third.error().code == vtcontrol::ControlErrorCode::LimitReached);
    CHECK(hub.countOf(mine) == 2);
    // The share is per sink.
    CHECK(hub.subscribe(theirs, EventFilter {}).has_value());

    // Ending one frees its place; forgetting the sink frees them all.
    REQUIRE(hub.unsubscribe(mine, first).has_value());
    CHECK(hub.subscribe(mine, EventFilter {}).has_value());
    hub.forget(mine);
    CHECK(hub.countOf(mine) == 0);
    CHECK(hub.subscribe(mine, EventFilter {}).has_value());
    CHECK(hub.subscribe(mine, EventFilter {}).has_value());
    CHECK_FALSE(hub.subscribe(mine, EventFilter {}).has_value());
}

TEST_CASE("BlockWaiters hold a sink's waits up to its share", "[vthost][control][wait][limits]")
{
    auto waiters = BlockWaiters { 1 };
    auto sink = RecordingEventSink {};
    auto results = std::vector<std::expected<vtcontrol::BlockInfo, vtcontrol::ControlError>> {};
    auto const record = [&](std::expected<vtcontrol::BlockInfo, vtcontrol::ControlError> result) {
        results.push_back(std::move(result));
    };

    CHECK(waiters.waitFor(sink, vtcontrol::BlockRef { .pane = uuidOf(1), .serial = 5 }, record).has_value());
    auto const refused = waiters.waitForNext(sink, uuidOf(1), 5, record);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == vtcontrol::ControlErrorCode::LimitReached);
    // The refused wait is answered at once, with the same error.
    REQUIRE(results.size() == 1);
    REQUIRE_FALSE(results[0].has_value());
    CHECK(results[0].error().code == vtcontrol::ControlErrorCode::LimitReached);
    CHECK(waiters.countOf(sink) == 1);

    // A completed wait frees its place, and so does forgetting the sink.
    waiters.blockEnded(blockOf(uuidOf(1), 5));
    CHECK(waiters.countOf(sink) == 0);
    CHECK(waiters.waitForNext(sink, uuidOf(1), 5, record).has_value());
    waiters.forget(sink);
    CHECK(waiters.countOf(sink) == 0);
    CHECK(waiters.waitForNext(sink, uuidOf(1), 5, record).has_value());
}
