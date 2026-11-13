// SPDX-License-Identifier: Apache-2.0
#include <contour/window/AttentionJump.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <utility>
#include <vector>

using namespace contour::window;
using vtbackend::ProgramState;
using vtbackend::ProgramStatusRecord;

namespace
{
ProgramStatusRecord recordOf(ProgramState state, uint64_t generation)
{
    return ProgramStatusRecord { .id = {},
                                 .state = state,
                                 .kind = {},
                                 .progress = std::nullopt,
                                 .app = "app",
                                 .title = {},
                                 .message = {},
                                 .generation = generation };
}

DirectoryPane paneOf(uint64_t key, std::vector<ProgramStatusRecord> records = {})
{
    auto pane = DirectoryPane {};
    pane.key = PaneKey { key };
    pane.status = std::move(records);
    return pane;
}

/// One workspace per list, one tab per pane, in the order given.
WorkspaceDirectory directoryOf(std::vector<DirectoryPane> panes)
{
    auto workspace = DirectoryWorkspace { .id = 1, .label = {}, .tabs = {} };
    for (auto& pane: panes)
        workspace.tabs.push_back(
            DirectoryTab { .id = pane.key.value, .title = {}, .label = {}, .panes = { pane } });
    return WorkspaceDirectory { .workspaces = { std::move(workspace) }, .selected = std::nullopt };
}
} // namespace

TEST_CASE("jump order is blocked, then failed, then done-unseen, in sidebar order", "[attention]")
{
    auto const directory = directoryOf({ paneOf(1, { recordOf(ProgramState::Done, 1) }),
                                         paneOf(2, { recordOf(ProgramState::Error, 1) }),
                                         paneOf(3),
                                         paneOf(4, { recordOf(ProgramState::Blocked, 1) }),
                                         paneOf(5, { recordOf(ProgramState::Blocked, 1) }) });
    auto attention = TabAttention {};
    CHECK(nextAttention(directory, attention, std::nullopt) == PaneKey { 4 });
    CHECK(nextAttention(directory, attention, PaneKey { 4 }) == PaneKey { 5 });
    // Blocked panes are no longer there: the failed one comes next, then the done one.
    auto const failedFirst = directoryOf(
        { paneOf(1, { recordOf(ProgramState::Done, 1) }), paneOf(2, { recordOf(ProgramState::Error, 1) }) });
    CHECK(nextAttention(failedFirst, attention, std::nullopt) == PaneKey { 2 });
    auto const doneOnly = directoryOf({ paneOf(1, { recordOf(ProgramState::Done, 1) }) });
    CHECK(nextAttention(doneOnly, attention, std::nullopt) == PaneKey { 1 });
}

TEST_CASE("jump wraps after the last", "[attention]")
{
    auto const directory = directoryOf({ paneOf(1, { recordOf(ProgramState::Blocked, 1) }),
                                         paneOf(2),
                                         paneOf(3, { recordOf(ProgramState::Blocked, 1) }) });
    auto const attention = TabAttention {};
    CHECK(nextAttention(directory, attention, PaneKey { 3 }) == PaneKey { 1 });
}

TEST_CASE("nothing to jump to gives nullopt", "[attention]")
{
    auto const directory = directoryOf({ paneOf(1), paneOf(2, { recordOf(ProgramState::Working, 1) }) });
    auto const attention = TabAttention {};
    CHECK(nextAttention(directory, attention, std::nullopt) == std::nullopt);
    CHECK(nextAttention(WorkspaceDirectory {}, attention, std::nullopt) == std::nullopt);
}

TEST_CASE("seen never hides blocked", "[attention]")
{
    auto attention = TabAttention {};
    std::ignore = attention.noteSeen(PaneKey { 1 }, 99);
    auto const blocked = paneOf(1, { recordOf(ProgramState::Blocked, 1) });
    auto const done = paneOf(1, { recordOf(ProgramState::Done, 1) });
    CHECK(paneUrgency(blocked, attention.of(PaneKey { 1 })) == Urgency::NeedsYou);
    CHECK(paneUrgency(done, attention.of(PaneKey { 1 })) == Urgency::Idle);
}

TEST_CASE("a new generation after seen is unseen again", "[attention]")
{
    auto attention = TabAttention {};
    std::ignore = attention.noteSeen(PaneKey { 1 }, 3);
    CHECK(paneUrgency(paneOf(1, { recordOf(ProgramState::Error, 3) }), attention.of(PaneKey { 1 }))
          == Urgency::Idle);
    CHECK(paneUrgency(paneOf(1, { recordOf(ProgramState::Error, 4) }), attention.of(PaneKey { 1 }))
          == Urgency::Failed);
}

TEST_CASE("a reconnect re-delivering the same generation stays seen", "[attention]")
{
    auto attention = TabAttention {};
    std::ignore = attention.noteSeen(PaneKey { 1 }, 5);
    auto const redelivered = paneOf(1, { recordOf(ProgramState::Done, 5) });
    CHECK(paneUrgency(redelivered, attention.of(PaneKey { 1 })) == Urgency::Idle);
    CHECK(nextAttention(directoryOf({ redelivered }), attention, std::nullopt) == std::nullopt);
}

TEST_CASE("a sole candidate wraps back to the current pane", "[attention]")
{
    auto const directory = directoryOf({ paneOf(1, { recordOf(ProgramState::Blocked, 1) }), paneOf(2) });
    auto const attention = TabAttention {};
    CHECK(nextAttention(directory, attention, PaneKey { 1 }) == PaneKey { 1 });
}

TEST_CASE("bells and unseen output count from the directory and from the ledger", "[attention]")
{
    auto pane = paneOf(1);
    CHECK(paneUrgency(pane, {}) == Urgency::Idle);
    pane.activity = vthost::proto::OutputActivity::Output;
    pane.activitySerial = 1;
    CHECK(paneUrgency(pane, {}) == Urgency::UnseenOutput);
    CHECK(paneUrgency(paneOf(1), { .activity = TabActivity::UnseenOutput }) == Urgency::UnseenOutput);
    pane.bell = vthost::proto::PaneBell::Rang;
    pane.bellSerial = 2;
    CHECK(paneUrgency(pane, {}) == Urgency::NeedsYou);
    CHECK(paneUrgency(paneOf(1), { .bell = BellMark::Rang }) == Urgency::NeedsYou);
    CHECK(paneUrgency(paneOf(1), { .finished = FinishedBlockBadge::Failed }) == Urgency::Failed);
    CHECK(paneUrgency(paneOf(1), { .finished = FinishedBlockBadge::Succeeded }) == Urgency::DoneUnseen);
}

TEST_CASE("mark seen in a hidden workspace clears output and bell in this window", "[attention]")
{
    auto pane = paneOf(1);
    pane.activity = vthost::proto::OutputActivity::Output;
    pane.activitySerial = 4;
    pane.bell = vthost::proto::PaneBell::Rang;
    pane.bellSerial = 5;
    auto attention = TabAttention {};
    REQUIRE(paneUrgency(pane, attention.of(pane.key)) == Urgency::NeedsYou);
    CHECK(noteDirectoryPaneSeen(attention, pane) == AttentionChange::Changed);
    CHECK(paneUrgency(pane, attention.of(pane.key)) == Urgency::Idle);
    // Another window's ledger knows nothing of it.
    CHECK(paneUrgency(pane, TabAttention {}.of(pane.key)) == Urgency::NeedsYou);
}

TEST_CASE("a new bell or new output after seen rings again", "[attention]")
{
    auto pane = paneOf(1);
    pane.bell = vthost::proto::PaneBell::Rang;
    pane.bellSerial = 5;
    auto attention = TabAttention {};
    std::ignore = noteDirectoryPaneSeen(attention, pane);
    CHECK(paneUrgency(pane, attention.of(pane.key)) == Urgency::Idle);
    pane.bellSerial = 6;
    CHECK(paneUrgency(pane, attention.of(pane.key)) == Urgency::NeedsYou);
    std::ignore = noteDirectoryPaneSeen(attention, pane);
    pane.activity = vthost::proto::OutputActivity::Output;
    pane.activitySerial = 7;
    CHECK(paneUrgency(pane, attention.of(pane.key)) == Urgency::UnseenOutput);
}

TEST_CASE("a visited bell pane leaves the jump, and two never cycle forever", "[attention]")
{
    auto first = paneOf(1);
    auto second = paneOf(2);
    first.bell = second.bell = vthost::proto::PaneBell::Rang;
    first.bellSerial = 1;
    second.bellSerial = 2;
    auto const directory = directoryOf({ first, second });
    auto attention = TabAttention {};
    auto const one = nextAttention(directory, attention, std::nullopt);
    REQUIRE(one == PaneKey { 1 });
    std::ignore = noteDirectoryPaneSeen(attention, first);
    auto const two = nextAttention(directory, attention, one);
    REQUIRE(two == PaneKey { 2 });
    std::ignore = noteDirectoryPaneSeen(attention, second);
    CHECK(nextAttention(directory, attention, two) == std::nullopt);
}

TEST_CASE("a finished command counts by its exit, once per run", "[attention]")
{
    auto pane = paneOf(1);
    auto attention = TabAttention {};
    pane.finished = vthost::proto::FinishedCommand { .exitCode = 1 };
    pane.finishedSerial = 1;
    CHECK(paneUrgency(pane, attention.of(pane.key)) == Urgency::Failed);
    pane.finished = vthost::proto::FinishedCommand { .exitCode = 0 };
    CHECK(paneUrgency(pane, attention.of(pane.key)) == Urgency::DoneUnseen);
    pane.finished = vthost::proto::FinishedCommand {};
    CHECK(paneUrgency(pane, attention.of(pane.key)) == Urgency::Idle);

    pane.finished = vthost::proto::FinishedCommand { .exitCode = 1 };
    std::ignore = noteDirectoryPaneSeen(attention, pane);
    CHECK(paneUrgency(pane, attention.of(pane.key)) == Urgency::Idle);
    pane.finished = vthost::proto::FinishedCommand { .exitCode = 0 };
    CHECK(paneUrgency(pane, attention.of(pane.key)) == Urgency::Idle);
    pane.finishedSerial = 2; // a new run
    CHECK(paneUrgency(pane, attention.of(pane.key)) == Urgency::DoneUnseen);
}
