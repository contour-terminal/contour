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
    auto workspace = DirectoryWorkspace { .id = 1, .name = {}, .tabs = {} };
    for (auto& pane: panes)
        workspace.tabs.push_back(
            DirectoryTab { .id = pane.key.value, .title = {}, .name = {}, .panes = { pane } });
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

TEST_CASE("a daemon pane's urgency ignores the window ledger", "[attention][daemon-attention]")
{
    auto pane = paneOf(1, { recordOf(ProgramState::Done, 3) });
    pane.flagSource = FlagSource::Daemon;
    pane.attention = AttentionKnowledge::Stated;
    auto const sawEverything = SessionAttention { .activity = TabActivity::None,
                                                  .finished = FinishedBlockBadge::None,
                                                  .bell = BellMark::None,
                                                  .seenStatusGeneration = 3,
                                                  .seenActivitySerial = 9,
                                                  .seenBellSerial = 9,
                                                  .seenFinishedSerial = 9 };
    auto const markedEverything = SessionAttention { .activity = TabActivity::UnseenOutput,
                                                     .finished = FinishedBlockBadge::Failed,
                                                     .bell = BellMark::Rang,
                                                     .seenStatusGeneration = 0,
                                                     .seenActivitySerial = 0,
                                                     .seenBellSerial = 0,
                                                     .seenFinishedSerial = 0 };

    // The Done record is unseen at the daemon: what this window's ledger saw changes nothing.
    CHECK(paneUrgency(pane, {}) == Urgency::DoneUnseen);
    CHECK(paneUrgency(pane, sawEverything) == Urgency::DoneUnseen);
    pane.seenStatusGeneration = 3;
    CHECK(paneUrgency(pane, {}) == Urgency::Idle);
    // Nor do the ledger's own marks: the daemon flags nothing.
    CHECK(paneUrgency(pane, markedEverything) == Urgency::Idle);

    // A bell the daemon flags asks, however far the ledger's seen serial is.
    pane.bell = vthost::proto::PaneBell::Rang;
    pane.bellSerial = 5;
    CHECK(paneUrgency(pane, sawEverything) == Urgency::NeedsYou);

    // The same pane kept by the window reads the ledger, as before.
    pane.flagSource = FlagSource::Window;
    CHECK(paneUrgency(pane, sawEverything) == Urgency::Idle);
    CHECK(paneUrgency(pane, markedEverything) == Urgency::NeedsYou);
}

TEST_CASE("a daemon pane whose flags were not stated yet shows nothing unseen",
          "[attention][daemon-attention]")
{
    // Listed with a Done the window has not seen, a round trip before its daemon states what was seen of it:
    // not known is not unseen.
    auto pane = paneOf(1, { recordOf(ProgramState::Done, 3) });
    pane.flagSource = FlagSource::Daemon;
    REQUIRE(pane.attention == AttentionKnowledge::Unstated);
    CHECK_FALSE(isUnseen(pane));
    CHECK(paneUrgency(pane, {}) == Urgency::Idle);
    CHECK(daemonMarksOf(pane) == SessionAttention { .seenStatusGeneration = AllStatusGenerationsSeen });

    // Stated, and nothing of it seen: the Done asks.
    pane.attention = AttentionKnowledge::Stated;
    CHECK(isUnseen(pane));
    CHECK(paneUrgency(pane, {}) == Urgency::DoneUnseen);

    // A Blocked status asks whatever is known.
    auto blocked = paneOf(2, { recordOf(ProgramState::Blocked, 1) });
    blocked.flagSource = FlagSource::Daemon;
    CHECK(paneUrgency(blocked, {}) == Urgency::NeedsYou);
}

TEST_CASE("a daemon pane is seen at the daemon, with everything its entry states",
          "[attention][daemon-attention]")
{
    auto pane = paneOf(1, { recordOf(ProgramState::Done, 7) });
    pane.flagSource = FlagSource::Daemon;
    pane.attention = AttentionKnowledge::Stated;
    pane.seenStatusGeneration = 7;
    CHECK_FALSE(isUnseen(pane));
    pane.activity = vthost::proto::OutputActivity::Output;
    pane.activitySerial = 4;
    pane.bellSerial = 2; // a bell seen before, still carrying its serial
    CHECK(isUnseen(pane));
    auto const seen = paneSeenOf(pane);
    CHECK(seen.serials.activity == 4);
    CHECK(seen.serials.bell == 2);
    CHECK(seen.serials.finished == 0);
    CHECK(seen.statusGeneration == 7);
    pane.activity = vthost::proto::OutputActivity::None;
    pane.status.push_back(recordOf(ProgramState::Error, 8));
    CHECK(isUnseen(pane));
}

namespace
{
/// The window's own daemon in the cross-daemon cases.
[[nodiscard]] DaemonRef ownRef()
{
    return hostDaemonRef("alpha");
}

/// A connected daemon's section listing @p directory.
DaemonSection sectionOf(DaemonRef daemon,
                        uint64_t instance,
                        WorkspaceDirectory directory,
                        DaemonLinkState state = DaemonLinkState::Connected)
{
    return DaemonSection { .daemon = std::move(daemon),
                           .instance = DaemonInstance { instance },
                           .name = {},
                           .locality = DaemonLocality::Remote,
                           .state = state,
                           .reason = {},
                           .directory = std::move(directory),
                           .shownElsewhere = {},
                           .feed = SectionFeed::Observer };
}

/// What a jump to pane @p key of @p section names (directoryOf() lists each pane in a tab of its key).
SidebarTarget targetIn(DaemonSection const& section, uint64_t key)
{
    return SidebarTarget { .daemon = section.daemon,
                           .instance = section.instance,
                           .workspace = 1,
                           .tab = key,
                           .pane = PaneKey { key } };
}
} // namespace

TEST_CASE("needs-you on another daemon before failed on this one", "[attention]")
{
    auto const sections = std::vector {
        sectionOf(ownRef(), 1, directoryOf({ paneOf(1, { recordOf(ProgramState::Error, 1) }) })),
        sectionOf(
            hostDaemonRef("beta"), 2, directoryOf({ paneOf(7, { recordOf(ProgramState::Blocked, 1) }) })),
    };
    CHECK(nextAttention(sections, ownRef(), TabAttention {}, std::nullopt) == targetIn(sections[1], 7));
}

TEST_CASE("wraps across daemons in tree order", "[attention]")
{
    // beta and the own daemon both hold a pane of key 3: a pane is named by its daemon too.
    auto const sections = std::vector {
        sectionOf(
            hostDaemonRef("beta"), 2, directoryOf({ paneOf(3, { recordOf(ProgramState::Blocked, 1) }) })),
        sectionOf(ownRef(), 1, directoryOf({ paneOf(3, { recordOf(ProgramState::Blocked, 1) }) })),
        sectionOf(hostDaemonRef("gamma"),
                  4,
                  directoryOf({ paneOf(5, { recordOf(ProgramState::Blocked, 1) }),
                                paneOf(6, { recordOf(ProgramState::Blocked, 1) }) })),
    };
    auto const attention = TabAttention {};
    CHECK(nextAttention(sections, ownRef(), attention, targetIn(sections[0], 3)) == targetIn(sections[1], 3));
    CHECK(nextAttention(sections, ownRef(), attention, targetIn(sections[1], 3)) == targetIn(sections[2], 5));
    // The last daemon's last candidate: the first daemon's comes next.
    CHECK(nextAttention(sections, ownRef(), attention, targetIn(sections[2], 6)) == targetIn(sections[0], 3));
}

TEST_CASE("a not-connected daemon contributes nothing", "[attention]")
{
    auto const sections = std::vector {
        sectionOf(ownRef(), 1, directoryOf({ paneOf(1, { recordOf(ProgramState::Done, 1) }) })),
        sectionOf(hostDaemonRef("beta"),
                  2,
                  directoryOf({ paneOf(7, { recordOf(ProgramState::Blocked, 1) }) }),
                  DaemonLinkState::NotConnected),
    };
    // Its last-known Blocked pane is skipped: the own Done one is what is left.
    CHECK(nextAttention(sections, ownRef(), TabAttention {}, std::nullopt) == targetIn(sections[0], 1));
    // The window's own section counts whatever its link's state.
    auto ownDown = sections;
    ownDown[0].state = DaemonLinkState::Reconnecting;
    CHECK(nextAttention(ownDown, ownRef(), TabAttention {}, std::nullopt) == targetIn(ownDown[0], 1));
}

TEST_CASE("the own section's window-kept flags count", "[attention]")
{
    // An in-process (or older daemon's) pane: its flags are the window's ledger's, the directory states none.
    auto pane = paneOf(2);
    pane.flagSource = FlagSource::Window;
    auto const sections = std::vector {
        sectionOf(ownRef(), 1, directoryOf({ paneOf(1), pane })),
        sectionOf(hostDaemonRef("beta"), 2, directoryOf({ paneOf(2) })),
    };
    auto attention = TabAttention {};
    CHECK(nextAttention(sections, ownRef(), attention, std::nullopt) == std::nullopt);
    REQUIRE(
        attention.noteBell(PaneKey { 2 }, contour::platform::SessionVisibility::Hidden, OutputOrigin::Live)
        == AttentionChange::Changed);
    CHECK(nextAttention(sections, ownRef(), attention, std::nullopt) == targetIn(sections[0], 2));
}

TEST_CASE("a pending target is where the next press starts", "[attention]")
{
    auto const sections = std::vector {
        sectionOf(ownRef(), 1, directoryOf({ paneOf(1) })),
        sectionOf(hostDaemonRef("beta"),
                  2,
                  directoryOf({ paneOf(3, { recordOf(ProgramState::Blocked, 1) }),
                                paneOf(4, { recordOf(ProgramState::Blocked, 1) }) })),
    };
    auto const attention = TabAttention {};
    // From the pane still shown, the first press goes to beta's first blocked pane ...
    auto const first = nextAttention(sections, ownRef(), attention, targetIn(sections[0], 1));
    REQUIRE(first == targetIn(sections[1], 3));
    // ... and a second press while that jump is pending goes on from it, not back to it.
    CHECK(nextAttention(sections, ownRef(), attention, first) == targetIn(sections[1], 4));
}
