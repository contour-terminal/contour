// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/ConnectionId.hpp>
#include <vthost/ExtensionActions.hpp>
#include <vthost/ExtensionHosting.hpp>

using vthost::ConnectionId;
using vthost::DaemonExtensionState;
using vthost::ExtensionActions;
using vthost::InvocationEffect;
using vthost::Origin;
using vthost::ServedHint;

namespace
{

/// One expected effect, by state.
struct EffectRow
{
    DaemonExtensionState state;
    InvocationEffect effect;
};

constexpr auto EffectRows = std::array {
    EffectRow { .state = DaemonExtensionState::Running, .effect = InvocationEffect::Deliver },
    EffectRow { .state = DaemonExtensionState::Stopped, .effect = InvocationEffect::StartThenDeliver },
    EffectRow { .state = DaemonExtensionState::Pending, .effect = InvocationEffect::AskAgain },
    EffectRow { .state = DaemonExtensionState::Disabled, .effect = InvocationEffect::Restart },
    EffectRow { .state = DaemonExtensionState::Denied, .effect = InvocationEffect::Ignore },
    EffectRow { .state = DaemonExtensionState::Failed, .effect = InvocationEffect::Ignore },
};

/// @return The connection numbered @p index on the native endpoint.
[[nodiscard]] ConnectionId connection(uint64_t index)
{
    return ConnectionId { .endpoint = "native", .index = index, .peer = {} };
}

/// An ExtensionActions that records every hint it raises.
struct Recorder
{
    std::vector<std::pair<std::string, ServedHint>> raised;
    ExtensionActions actions { [this](std::string_view extension, ServedHint hint) {
        raised.emplace_back(std::string(extension), hint);
    } };
};

} // namespace

TEST_CASE("invocationEffectOf: one row per state", "[daemon][presence]")
{
    STATIC_CHECK(vthost::InvocationRows.size() == EffectRows.size());
    for (auto const& row: EffectRows)
    {
        CAPTURE(std::to_underlying(row.state));
        CHECK(vthost::invocationEffectOf(row.state) == row.effect);
    }
}

TEST_CASE("ExtensionActions: an origin is stable per connection, distinct across, never reused",
          "[daemon][presence]")
{
    auto recorder = Recorder {};
    auto& actions = recorder.actions;
    auto const first = actions.originOf(connection(1));
    auto const second = actions.originOf(connection(2));
    CHECK(actions.originOf(connection(1)) == first);
    CHECK(first != second);
    actions.deliver("orchestrator", "open", first);
    actions.deliver("orchestrator", "open", second);
    REQUIRE(actions.take("orchestrator").size() == 2);
    CHECK(actions.connectionOf("orchestrator", first) == connection(1));
    CHECK(actions.connectionOf("orchestrator", second) == connection(2));

    actions.forget(connection(1));
    CHECK(actions.connectionOf("orchestrator", first) == std::nullopt);
    // The same connection attaching again -- or another -- gets a fresh origin: the old one names nothing.
    auto const again = actions.originOf(connection(1));
    CHECK(again != first);
    CHECK(again != second);
    CHECK(actions.connectionOf("orchestrator", first) == std::nullopt);
    CHECK(actions.connectionOf("orchestrator", Origin { 999 }) == std::nullopt);
}

TEST_CASE("ExtensionActions: an origin acts only for the extension it was handed to", "[daemon][presence]")
{
    auto recorder = Recorder {};
    auto& actions = recorder.actions;
    auto const origin = actions.originOf(connection(1));
    // Not handed out yet: no extension may act through it.
    CHECK(actions.connectionOf("orchestrator", origin) == std::nullopt);
    actions.deliver("orchestrator", "open", origin);
    REQUIRE(actions.take("orchestrator").size() == 1);
    CHECK(actions.connectionOf("orchestrator", origin) == connection(1));
    // Another extension, which was never handed it, cannot borrow it.
    CHECK(actions.connectionOf("notes", origin) == std::nullopt);
    actions.deliver("notes", "open", origin);
    REQUIRE(actions.take("notes").size() == 1);
    CHECK(actions.connectionOf("notes", origin) == connection(1));
}

TEST_CASE("ExtensionActions: every delivery raises Actions", "[daemon][presence]")
{
    // The runner coalesces hints; raising only when a queue fills from empty would lose the wakeup of an
    // extension that faulted before it took what was queued.
    auto recorder = Recorder {};
    auto& actions = recorder.actions;
    auto const origin = actions.originOf(connection(1));
    CHECK_FALSE(actions.holdsActionsFor("orchestrator"));
    actions.deliver("orchestrator", "open", origin);
    actions.deliver("orchestrator", "refresh", origin);
    actions.deliver("notes", "open", origin);
    CHECK(recorder.raised
          == std::vector<std::pair<std::string, ServedHint>> { { "orchestrator", ServedHint::Actions },
                                                               { "orchestrator", ServedHint::Actions },
                                                               { "notes", ServedHint::Actions } });
    CHECK(actions.holdsActionsFor("orchestrator"));

    using Taken = std::vector<std::pair<std::string, Origin>>;
    CHECK(actions.take("orchestrator") == Taken { { "open", origin }, { "refresh", origin } });
    CHECK_FALSE(actions.holdsActionsFor("orchestrator"));
    CHECK(actions.take("orchestrator").empty());
    CHECK(actions.take("unknown").empty());

    actions.deliver("orchestrator", "open", origin);
    CHECK(recorder.raised.size() == 4);
}

TEST_CASE("ExtensionActions: beyond the bound the oldest is dropped", "[daemon][presence]")
{
    auto recorder = Recorder {};
    auto& actions = recorder.actions;
    auto const origin = actions.originOf(connection(1));
    for (auto const index: std::views::iota(std::size_t { 0 }, vthost::MaxQueuedActions + 1))
        actions.deliver("orchestrator", std::to_string(index), origin);

    auto const taken = actions.take("orchestrator");
    REQUIRE(taken.size() == vthost::MaxQueuedActions);
    CHECK(taken.front().first == "1");
    CHECK(taken.back().first == std::to_string(vthost::MaxQueuedActions));
    CHECK(recorder.raised.size() == vthost::MaxQueuedActions + 1);
}
