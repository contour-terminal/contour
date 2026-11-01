// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <vthost/NodeMove.hpp>
#include <vtworkspace/ModelFixture.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using vthost::applyMoveNode;
using vthost::NodeMoveError;
using vthost::proto::MoveNode;
using vtworkspace::MoveSourceKind;
using vtworkspace::MoveTargetKind;
using vtworkspace::SplitSide;

namespace
{

/// Two windows. The first holds a tab of sessions 1 and 2, and a tab of session 3; the second a tab of
/// session 4.
struct TwoWindows
{
    vtworkspace::test::RecordingEvents events;
    uint64_t nextSession = 1;
    vtworkspace::SessionModel model { events, [this] { return vtworkspace::SessionId { nextSession++ }; } };
    /// A new window as the model alone makes it, and takes it back.
    vthost::MoveWindowHooks hooks {
        .create = [this] { return model.createWindow()->id(); },
        .discard = [this](vtworkspace::WindowId window) { model.removeWindow(window); },
    };
    vtworkspace::WindowId first = model.createWindow()->id();
    vtworkspace::Tab* pair = model.createTab(first);
    vtworkspace::Pane* second = model.splitActivePane(pair->id(), vtworkspace::SplitState::Horizontal);
    vtworkspace::Tab* single = model.createTab(first);
    vtworkspace::WindowId other = model.createWindow()->id();
    vtworkspace::Tab* lone = model.createTab(other);

    TwoWindows() { events.log.clear(); }

    [[nodiscard]] vtworkspace::Window* windowOf(uint64_t session) const
    {
        return std::get<0>(model.findSessionLeaf(vtworkspace::SessionId { session }));
    }
};

[[nodiscard]] constexpr uint8_t raw(auto value) noexcept
{
    return static_cast<uint8_t>(std::to_underlying(value));
}

} // namespace

TEST_CASE("an edge move grafts the pane beside the target", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto const moved = applyMoveNode(h.model,
                                     MoveNode { .sourceKind = raw(MoveSourceKind::Pane),
                                                .sourceSessions = { 3 },
                                                .targetKind = raw(MoveTargetKind::Edge),
                                                .targetSession = 4,
                                                .side = raw(SplitSide::Right) },
                                     h.hooks);
    REQUIRE(moved.has_value());
    CHECK(moved->focused.value == 3);
    CHECK(h.windowOf(3) == h.windowOf(4));
    CHECK(h.lone->paneCount() == 2);
    CHECK_FALSE(h.events.log.empty());
}

TEST_CASE("an edge move onto the only sibling re-splits the tab along that edge", "[vthost][move]")
{
    // The pair is 1 above 2. Session 2 dropped on session 1's right edge keeps the leaf order and changes
    // only the axis: 1 | 2.
    auto h = TwoWindows {};
    auto const moved = applyMoveNode(h.model,
                                     MoveNode { .sourceKind = raw(MoveSourceKind::Pane),
                                                .sourceSessions = { 2 },
                                                .targetKind = raw(MoveTargetKind::Edge),
                                                .targetSession = 1,
                                                .side = raw(SplitSide::Right) },
                                     h.hooks);
    REQUIRE(moved.has_value());
    CHECK(moved->focused.value == 2);
    auto const* const root = h.pair->rootPane();
    CHECK(root->splitState() == vtworkspace::SplitState::Vertical);
    REQUIRE(h.pair->paneCount() == 2);
    CHECK(root->first()->session().value == 1);
    CHECK(root->second()->session().value == 2);
    CHECK(h.pair->activePane()->session().value == 2);
}

TEST_CASE("a swap trades the sessions of two panes", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto const moved = applyMoveNode(h.model,
                                     MoveNode { .sourceKind = raw(MoveSourceKind::Pane),
                                                .sourceSessions = { 1 },
                                                .targetKind = raw(MoveTargetKind::Swap),
                                                .targetSession = 4 },
                                     h.hooks);
    REQUIRE(moved.has_value());
    CHECK(h.windowOf(1) == h.model.window(h.other));
    CHECK(h.windowOf(4) == h.model.window(h.first));
}

TEST_CASE("a whole tab moves into another window's tab well, the index clamped", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto const moved = applyMoveNode(h.model,
                                     MoveNode { .sourceKind = raw(MoveSourceKind::Tab),
                                                .sourceSessions = { 1, 2 },
                                                .targetKind = raw(MoveTargetKind::TabWell),
                                                .targetSession = 4,
                                                .index = std::numeric_limits<uint64_t>::max() },
                                     h.hooks);
    REQUIRE(moved.has_value());
    CHECK(moved->window == h.other);
    CHECK(h.windowOf(1) == h.model.window(h.other));
    CHECK(h.windowOf(2) == h.model.window(h.other));
    CHECK(h.model.window(h.other)->tabCount() == 2);
}

TEST_CASE("a pane moves to a new window and keeps its session", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto const windows = h.model.windowCount();
    auto const moved = applyMoveNode(h.model,
                                     MoveNode { .sourceKind = raw(MoveSourceKind::Pane),
                                                .sourceSessions = { 2 },
                                                .targetKind = raw(MoveTargetKind::NewWindow) },
                                     h.hooks);
    REQUIRE(moved.has_value());
    CHECK(h.model.windowCount() == windows + 1);
    CHECK(moved->focused.value == 2);
    CHECK(h.windowOf(2)->id() == moved->window);
    CHECK(moved->window != h.first);
}

TEST_CASE("a source or target kind this host does not know is refused and changes nothing", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto const badSource = applyMoveNode(h.model,
                                         MoveNode { .sourceKind = 0xEE,
                                                    .sourceSessions = { 3 },
                                                    .targetKind = raw(MoveTargetKind::NewWindow) },
                                         h.hooks);
    REQUIRE_FALSE(badSource.has_value());
    CHECK(badSource.error().error == NodeMoveError::UnknownKind);
    auto const badTarget = applyMoveNode(
        h.model,
        MoveNode { .sourceKind = raw(MoveSourceKind::Pane), .sourceSessions = { 3 }, .targetKind = 0xEF },
        h.hooks);
    REQUIRE_FALSE(badTarget.has_value());
    CHECK(badTarget.error().error == NodeMoveError::UnknownKind);
    CHECK(h.events.log.empty());
}

TEST_CASE("a pane source not naming exactly one session is malformed", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto const moved = applyMoveNode(h.model,
                                     MoveNode { .sourceKind = raw(MoveSourceKind::Pane),
                                                .sourceSessions = { 1, 2 },
                                                .targetKind = raw(MoveTargetKind::NewWindow) },
                                     h.hooks);
    REQUIRE_FALSE(moved.has_value());
    CHECK(moved.error().error == NodeMoveError::UnknownKind);
}

TEST_CASE("a refused move to a new window gives the made window back", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto discarded = std::vector<vtworkspace::WindowId> {};
    // The model refuses nothing a validated request names, so a window the model does not know stands in
    // for a move that fails after its window was made.
    auto const ghost = vtworkspace::WindowId { 9999 };
    auto const hooks = vthost::MoveWindowHooks {
        .create = [&] { return ghost; },
        .discard = [&](vtworkspace::WindowId window) { discarded.push_back(window); }
    };
    auto const moved = applyMoveNode(h.model,
                                     MoveNode { .sourceKind = raw(MoveSourceKind::Pane),
                                                .sourceSessions = { 3 },
                                                .targetKind = raw(MoveTargetKind::NewWindow) },
                                     hooks);
    REQUIRE_FALSE(moved.has_value());
    CHECK(moved.error().error == NodeMoveError::Model);
    CHECK(discarded == std::vector { ghost });
    CHECK(h.windowOf(3) == h.model.window(h.first));
    CHECK(h.events.log.empty());
}

TEST_CASE("a side no enumerator names is refused", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto const moved = applyMoveNode(h.model,
                                     MoveNode { .sourceKind = raw(MoveSourceKind::Pane),
                                                .sourceSessions = { 3 },
                                                .targetKind = raw(MoveTargetKind::Edge),
                                                .targetSession = 4,
                                                .side = 0xEE },
                                     h.hooks);
    REQUIRE_FALSE(moved.has_value());
    CHECK(moved.error().error == NodeMoveError::UnknownSide);
    CHECK(h.events.log.empty());
}

TEST_CASE("a closed session is refused as unknown, as source and as target", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto const source = applyMoveNode(h.model,
                                      MoveNode { .sourceKind = raw(MoveSourceKind::Pane),
                                                 .sourceSessions = { 999 },
                                                 .targetKind = raw(MoveTargetKind::NewWindow) },
                                      h.hooks);
    REQUIRE_FALSE(source.has_value());
    CHECK(source.error().error == NodeMoveError::UnknownSession);
    for (auto const kind: { MoveTargetKind::Edge, MoveTargetKind::Swap, MoveTargetKind::TabWell })
    {
        auto const target = applyMoveNode(h.model,
                                          MoveNode { .sourceKind = raw(MoveSourceKind::Pane),
                                                     .sourceSessions = { 3 },
                                                     .targetKind = raw(kind),
                                                     .targetSession = 999,
                                                     .side = raw(SplitSide::Left) },
                                          h.hooks);
        REQUIRE_FALSE(target.has_value());
        CHECK(target.error().error == NodeMoveError::UnknownSession);
    }
    auto const none =
        applyMoveNode(h.model, MoveNode { .targetKind = raw(MoveTargetKind::NewWindow) }, h.hooks);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().error == NodeMoveError::UnknownSession);
    CHECK(h.events.log.empty());
}

TEST_CASE("a request made of a stale view of the tab moves nothing", "[vthost][move]")
{
    auto h = TwoWindows {};
    // The client saw the tab as holding only session 1; it holds 1 and 2 now.
    auto const fewer = applyMoveNode(h.model,
                                     MoveNode { .sourceKind = raw(MoveSourceKind::Tab),
                                                .sourceSessions = { 1 },
                                                .targetKind = raw(MoveTargetKind::NewWindow) },
                                     h.hooks);
    REQUIRE_FALSE(fewer.has_value());
    CHECK(fewer.error().error == NodeMoveError::StaleTab);
    auto const more = applyMoveNode(h.model,
                                    MoveNode { .sourceKind = raw(MoveSourceKind::Tab),
                                               .sourceSessions = { 1, 2, 9 },
                                               .targetKind = raw(MoveTargetKind::NewWindow) },
                                    h.hooks);
    REQUIRE_FALSE(more.has_value());
    CHECK(more.error().error == NodeMoveError::StaleTab);
    CHECK(h.events.log.empty());
}

TEST_CASE("what the model refuses is carried as the model's reason", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto const onto = applyMoveNode(h.model,
                                    MoveNode { .sourceKind = raw(MoveSourceKind::Pane),
                                               .sourceSessions = { 3 },
                                               .targetKind = raw(MoveTargetKind::Swap),
                                               .targetSession = 3 },
                                    h.hooks);
    REQUIRE_FALSE(onto.has_value());
    CHECK(onto.error().error == NodeMoveError::Model);
    REQUIRE(onto.error().model.has_value());
    CHECK(*onto.error().model == vtworkspace::MoveError::SelfTarget);
    CHECK(h.events.log.empty());
}

TEST_CASE("a move taking a window's last tab removes the window, and only that one", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto discarded = std::vector<vtworkspace::WindowId> {};
    auto const hooks = vthost::MoveWindowHooks {
        .create = [&] { return h.model.createWindow()->id(); },
        .discard =
            [&](vtworkspace::WindowId window) {
                discarded.push_back(window);
                h.model.removeWindow(window);
            },
    };
    // The second window's only pane beside one of the first's: the second is left with no tab.
    auto const moved = applyMoveNode(h.model,
                                     MoveNode { .sourceKind = raw(MoveSourceKind::Pane),
                                                .sourceSessions = { 4 },
                                                .targetKind = raw(MoveTargetKind::Edge),
                                                .targetSession = 3,
                                                .side = raw(SplitSide::Right) },
                                     hooks);
    REQUIRE(moved.has_value());
    CHECK(discarded == std::vector { h.other });
    CHECK(h.model.window(h.other) == nullptr);

    // A tab of several leaving its window, which keeps another: nothing is removed.
    discarded.clear();
    REQUIRE(applyMoveNode(h.model,
                          MoveNode { .sourceKind = raw(MoveSourceKind::Tab),
                                     .sourceSessions = { 1, 2 },
                                     .targetKind = raw(MoveTargetKind::NewWindow) },
                          hooks));
    CHECK(discarded.empty());
}

// A window of its own for what already is a window's whole content would tear that window down and build it
// again for every client: refused as the model's NoOp, before a window is made.
TEST_CASE("a new window for a window's whole content is refused before any window is made", "[vthost][move]")
{
    auto h = TwoWindows {};
    auto made = 0;
    auto const hooks = vthost::MoveWindowHooks {
        .create =
            [&] {
                ++made;
                return h.model.createWindow()->id();
            },
        .discard = [&](vtworkspace::WindowId window) { h.model.removeWindow(window); },
    };
    auto const windows = h.model.windowCount();
    auto const kinds = std::vector<std::pair<MoveSourceKind, char const*>> {
        { MoveSourceKind::Pane, "the only pane of the only tab" },
        { MoveSourceKind::Tab, "the only tab" },
    };
    for (auto const& [kind, what]: kinds)
    {
        INFO(what);
        auto const moved = applyMoveNode(h.model,
                                         MoveNode { .sourceKind = raw(kind),
                                                    .sourceSessions = { 4 },
                                                    .targetKind = raw(MoveTargetKind::NewWindow) },
                                         hooks);
        REQUIRE_FALSE(moved.has_value());
        CHECK(moved.error().error == NodeMoveError::Model);
        CHECK(moved.error().model == vtworkspace::MoveError::NoOp);
    }
    CHECK(made == 0);
    CHECK(h.model.windowCount() == windows);
    CHECK(h.windowOf(4)->id() == h.other);
    CHECK(h.events.log.empty());
}
