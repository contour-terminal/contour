// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/core/WorkingDirectory.hpp>
#include <vtbackend/grid/Line.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using vtbackend::AdoptMode;
using vtbackend::CommandBlockEnd;
using vtbackend::CommandBlockId;
using vtbackend::CommandBlockState;
using vtbackend::test::mirroredFinish;
using vtbackend::test::mirroredRecord;
using vtbackend::test::ranIn;

TEST_CASE("Terminal.adoptCommandBlock raises a finish only for live, reported news",
          "[terminal][semanticblocks]")
{
    // MockTerm keeps every finish event the terminal raises.
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(4) };
    auto& terminal = mock.terminal;

    SECTION("a record watched running and then finishing raises exactly one event")
    {
        terminal.adoptCommandBlock(mirroredRecord(5, CommandBlockState::Running), AdoptMode::Live);
        CHECK(mock.finishedCommandBlocks.empty());

        terminal.adoptCommandBlock(mirroredFinish(5, 2, CommandBlockEnd::Reported), AdoptMode::Live);
        REQUIRE(mock.finishedCommandBlocks.size() == 1);
        CHECK(mock.finishedCommandBlocks.front().id == CommandBlockId { 5 });
        CHECK(mock.finishedCommandBlocks.front().exitCode == 2);
        CHECK(mock.finishedCommandBlocks.front().commandLine.raw() == "make");

        // Hearing about the same finished record again is not a second finish.
        terminal.adoptCommandBlock(mirroredFinish(5, 2, CommandBlockEnd::Reported), AdoptMode::Live);
        CHECK(mock.finishedCommandBlocks.size() == 1);
        REQUIRE(terminal.commandBlocks().find(CommandBlockId { 5 }) != nullptr);
        CHECK(terminal.commandBlocks().find(CommandBlockId { 5 })->state == CommandBlockState::Finished);

        // Nor is learning afterwards how it ended: the host sends that when an OSC 3008 `end=` enriches a
        // command its OSC 133;D already finished. The record changes; the finish was announced already.
        auto const signalled = vtbackend::ContextOutcome { .exit = vtbackend::ContextExit::Crash,
                                                           .signal = vtbackend::ContextSignal::Segv,
                                                           .status = 0 };
        auto enriched = mirroredFinish(5, 2, CommandBlockEnd::Reported);
        enriched.outcome = signalled;
        terminal.adoptCommandBlock(std::move(enriched), AdoptMode::Live);
        CHECK(mock.finishedCommandBlocks.size() == 1);
        REQUIRE(terminal.commandBlocks().find(CommandBlockId { 5 }) != nullptr);
        CHECK(terminal.commandBlocks().find(CommandBlockId { 5 })->outcome == signalled);
    }

    SECTION("a command that ran and finished between two adoptions still raises one")
    {
        terminal.adoptCommandBlock(mirroredFinish(6, 0, CommandBlockEnd::Reported), AdoptMode::Live);
        CHECK(mock.finishedCommandBlocks.size() == 1);
    }

    SECTION("a snapshot adoption is history and never raises")
    {
        terminal.adoptCommandBlock(mirroredFinish(7, 1, CommandBlockEnd::Reported), AdoptMode::Snapshot);
        CHECK(mock.finishedCommandBlocks.empty());
        CHECK(terminal.commandBlocks().find(CommandBlockId { 7 }) != nullptr);
    }

    SECTION("an implicit close (a nested shell's prompt) never raises")
    {
        terminal.adoptCommandBlock(mirroredRecord(8, CommandBlockState::Running), AdoptMode::Live);
        terminal.adoptCommandBlock(mirroredFinish(8, std::nullopt, CommandBlockEnd::Implicit),
                                   AdoptMode::Live);
        CHECK(mock.finishedCommandBlocks.empty());
    }
}

TEST_CASE("Terminal.setContextChain moves the ancestry but relabels no row", "[terminal][semanticblocks]")
{
    // A mirror's rows carry the contexts the host gave them, so the chain it replicates is for the ancestry
    // and those who follow it, never for stamping rows: neither the row the cursor stands on nor one it
    // reaches next may take the context here alone.
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(4) };
    auto& terminal = mock.terminal;
    auto box = vtbackend::TerminalContext {};
    box.id = terminal.contexts().nextAdoptedId();
    box.identifier = "box";
    box.type = vtbackend::ContextType::Container;
    terminal.adoptContext(box);

    auto const chain = std::array { box.id };
    terminal.setContextChain(chain);
    REQUIRE(terminal.contexts().active() != nullptr);
    CHECK(terminal.contexts().active()->identifier == "box");
    CHECK(mock.contextChanges == std::vector { box.id });

    auto const& grid = terminal.primaryScreen().grid();
    CHECK(grid.lineAt(vtbackend::LineOffset(0)).contextId() == vtbackend::ContextId {});
    mock.writeToScreen("\r\n");
    CHECK(grid.lineAt(vtbackend::LineOffset(1)).contextId() == vtbackend::ContextId {});
}

namespace
{

/// This machine, as the cases below know it: the identity a context's `machineid=` is held to.
[[nodiscard]] vtbackend::HostIdentity thisMachine()
{
    return vtbackend::HostIdentity { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "zeta" };
}

} // namespace

TEST_CASE("Terminal.bindReplica of another machine holds every directory foreign",
          "[terminal][semanticblocks]")
{
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(4) };
    mock.identity = thisMachine();
    auto& terminal = mock.terminal;
    // A context this machine's identity vouches for: local, on any terminal of this machine's own.
    mock.writeToScreen(
        "\033]3008;start=shell-1;type=shell;machineid=3deb5353d3ba43d08201c136a47ead7b;cwd=/srv\033\\");

    SECTION("a terminal of its own, or a replica on the daemon's machine, judges as it always has")
    {
        for (auto const role: { vtbackend::ReplicaRole::None, vtbackend::ReplicaRole::SameMachine })
        {
            terminal.bindReplica(role, thisMachine());
            auto const resolved = terminal.resolveWorkingDirectory(vtbackend::CwdPurpose::OpenLocally);
            REQUIRE(resolved.has_value());
            CHECK(resolved->path == "/srv");
            CHECK(resolved->locality == vtbackend::ContextLocality::Local);
        }
        terminal.adoptCommandBlock(ranIn(1, "/home/u/proj", vtbackend::ContextLocality::Local),
                                   AdoptMode::Live);
        REQUIRE(terminal.commandBlocks().find(CommandBlockId { 1 }) != nullptr);
        CHECK(terminal.commandBlocks().find(CommandBlockId { 1 })->workingDirectory.locality
              == vtbackend::ContextLocality::Local);
    }

    SECTION("a replica on the daemon's machine judges contexts against the identity it was bound with")
    {
        // The daemon names machines by derived ids, which this machine's own identity never matches: the
        // binding states the one that does. Bound with another, this machine's context is another's.
        terminal.bindReplica(
            vtbackend::ReplicaRole::SameMachine,
            vtbackend::HostIdentity { .machineId = "77aa5353d3ba43d08201c136a47ead7b", .hostname = "zeta" });
        auto const resolved = terminal.resolveWorkingDirectory(vtbackend::CwdPurpose::Display);
        REQUIRE(resolved.has_value());
        CHECK(resolved->locality == vtbackend::ContextLocality::Foreign);
    }

    SECTION("a replica of another machine's terminal opens and spawns into nothing")
    {
        terminal.bindReplica(vtbackend::ReplicaRole::OtherMachine, thisMachine());
        CHECK_FALSE(terminal.resolveWorkingDirectory(vtbackend::CwdPurpose::OpenLocally).has_value());
        CHECK_FALSE(terminal.resolveWorkingDirectory(vtbackend::CwdPurpose::Spawn).has_value());
        // Where the user is still shows, as somewhere else.
        auto const shown = terminal.resolveWorkingDirectory(vtbackend::CwdPurpose::Display);
        REQUIRE(shown.has_value());
        CHECK(shown->path == "/srv");
        CHECK(shown->locality == vtbackend::ContextLocality::Foreign);

        // Every record naming a directory holds it foreign, whatever the daemon judged; one naming none
        // keeps saying so.
        terminal.adoptCommandBlock(ranIn(1, "/home/u/proj", vtbackend::ContextLocality::Local),
                                   AdoptMode::Live);
        terminal.adoptCommandBlock(ranIn(2, "/home/u/proj", vtbackend::ContextLocality::Unknown),
                                   AdoptMode::Live);
        terminal.adoptCommandBlock(ranIn(3, "", vtbackend::ContextLocality::Unknown), AdoptMode::Live);
        auto const localityOf = [&](uint32_t id) {
            auto const* record = terminal.commandBlocks().find(CommandBlockId { id });
            REQUIRE(record != nullptr);
            return record->workingDirectory.locality;
        };
        CHECK(localityOf(1) == vtbackend::ContextLocality::Foreign);
        CHECK(localityOf(2) == vtbackend::ContextLocality::Foreign);
        CHECK(localityOf(3) == vtbackend::ContextLocality::Unknown);

        // The directory a command left the shell in, too.
        auto leftIn = ranIn(4, "/home/u/proj", vtbackend::ContextLocality::Local);
        leftIn.endWorkingDirectory =
            vtbackend::WorkingDirectorySnapshot { .path = "/home/u/proj/build",
                                                  .locality = vtbackend::ContextLocality::Local };
        terminal.adoptCommandBlock(std::move(leftIn), AdoptMode::Live);
        REQUIRE(terminal.commandBlocks().find(CommandBlockId { 4 }) != nullptr);
        CHECK(terminal.commandBlocks().find(CommandBlockId { 4 })->endWorkingDirectory.locality
              == vtbackend::ContextLocality::Foreign);
    }
}

TEST_CASE("Terminal.bindReplica of another machine holds an OSC 7 directory foreign",
          "[terminal][semanticblocks]")
{
    // An OSC 7 URL with no host names the machine whose program wrote it: on a replica, the daemon's.
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(4) };
    mock.identity = thisMachine();
    auto& terminal = mock.terminal;
    mock.writeToScreen("\033]7;file:///home/u/proj\033\\");
    REQUIRE(terminal.resolveWorkingDirectory(vtbackend::CwdPurpose::OpenLocally).has_value());

    terminal.bindReplica(vtbackend::ReplicaRole::OtherMachine, thisMachine());
    CHECK_FALSE(terminal.resolveWorkingDirectory(vtbackend::CwdPurpose::OpenLocally).has_value());
    auto const shown = terminal.resolveWorkingDirectory(vtbackend::CwdPurpose::Display);
    REQUIRE(shown.has_value());
    CHECK(shown->path == "/home/u/proj");
    CHECK(shown->locality == vtbackend::ContextLocality::Foreign);
}

TEST_CASE("Terminal.bindReplica leaves whether a block heard of a foreign directory to its host",
          "[terminal][semanticblocks]")
{
    // The host's record says whether its shell reported a directory elsewhere while the block was open; the
    // directory a mirror is handed afterwards is no news about the block.
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(4) };
    auto& terminal = mock.terminal;
    terminal.bindReplica(vtbackend::ReplicaRole::SameMachine, thisMachine());
    terminal.adoptCommandBlock(ranIn(1, "/home/u/proj", vtbackend::ContextLocality::Local), AdoptMode::Live);

    terminal.setCurrentWorkingDirectory("file://otherhost.invalid/srv/app");

    auto const shown = terminal.resolveWorkingDirectory(vtbackend::CwdPurpose::Display);
    REQUIRE(shown.has_value());
    REQUIRE(shown->locality == vtbackend::ContextLocality::Foreign);
    REQUIRE(terminal.commandBlocks().find(CommandBlockId { 1 }) != nullptr);
    CHECK(terminal.commandBlocks().find(CommandBlockId { 1 })->foreignDirectory
          == vtbackend::ForeignDirectory::Unreported);
}

TEST_CASE("Terminal.setCurrentWorkingDirectory reads the URL once, as OSC 7 does",
          "[terminal][semanticblocks]")
{
    // A mirror hands the terminal the host's URL directly, not through OSC 7: it is read the same way -- the
    // path percent-decoded, and the authority judged against this machine, here named `fedora`.
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(4) };
    mock.identity =
        vtbackend::HostIdentity { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "fedora" };
    auto& terminal = mock.terminal;
    auto const shown = [&terminal] {
        auto directory = terminal.resolveWorkingDirectory(vtbackend::CwdPurpose::Display);
        REQUIRE(directory.has_value());
        CHECK(directory->source == vtbackend::CwdSource::Osc7);
        return *directory;
    };

    terminal.setCurrentWorkingDirectory("file://fedora/srv/a%20b");
    CHECK(terminal.currentWorkingDirectory() == "file://fedora/srv/a%20b");
    CHECK(shown().path == "/srv/a b");
    CHECK(shown().locality == vtbackend::ContextLocality::Local);

    // A path holding a NUL names no directory, so the previous one stays.
    terminal.setCurrentWorkingDirectory("file://fedora/srv%00/etc");
    CHECK(terminal.currentWorkingDirectory() == "file://fedora/srv/a%20b");
    CHECK(shown().path == "/srv/a b");

    // Another machine's authority is read as one.
    terminal.setCurrentWorkingDirectory("file://zeta/srv/a%20b");
    CHECK(shown().locality == vtbackend::ContextLocality::Foreign);
}

TEST_CASE("Terminal.adoptSessionEpoch re-anchors line birth times", "[terminal][semanticblocks]")
{
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(4) };
    auto const hostEpoch = std::chrono::system_clock::time_point { 1'700'000'000s };

    mock.terminal.adoptSessionEpoch(hostEpoch);
    CHECK(mock.terminal.sessionEpoch() == hostEpoch);

    // A stamp copied from the host means "60 seconds into the HOST's session", stored plus one.
    auto line = vtbackend::Line { vtbackend::ColumnCount(20) };
    line.stampBornAt(61);
    CHECK(mock.terminal.lineBirthTime(line) == hostEpoch + 60s);

    // Held to the whole second the terminal's own epoch is: a stamp counts whole seconds, so an epoch
    // part-way through one would answer every row up to a second later than the host does.
    mock.terminal.adoptSessionEpoch(hostEpoch + 900ms);
    CHECK(mock.terminal.sessionEpoch() == hostEpoch);
    CHECK(mock.terminal.lineBirthTime(line) == hostEpoch + 60s);
}

TEST_CASE("Terminal.adoptSessionEpoch holds an epoch to the range a terminal computes with",
          "[terminal][semanticblocks]")
{
    // Whoever produced it -- a daemon, a tmux server, a restore -- the terminal floors the epoch, adds up to
    // Line::MaxBornAt seconds to it for a row's birth time and subtracts it from its own clock to stamp a
    // row. Each overflows near the limits of a nanosecond clock unless the epoch is held here.
    using std::chrono::system_clock;
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(4) };
    auto line = vtbackend::Line { vtbackend::ColumnCount(20) };
    line.stampBornAt(vtbackend::Line::MaxBornAt - 1); // the latest stamp that still names a time
    auto const latestBirth = std::chrono::seconds { vtbackend::Line::MaxBornAt - 2 };

    SECTION("the edges and an instant inside them stand, floored to the second")
    {
        mock.terminal.adoptSessionEpoch(vtbackend::EarliestHeldInstant);
        CHECK(mock.terminal.sessionEpoch() == vtbackend::EarliestHeldInstant);
        auto const latest = std::chrono::floor<std::chrono::seconds>(vtbackend::LatestHeldInstant);
        mock.terminal.adoptSessionEpoch(vtbackend::LatestHeldInstant);
        CHECK(mock.terminal.sessionEpoch() == latest);
        CHECK(mock.terminal.lineBirthTime(line) == latest + latestBirth);
    }

    SECTION("an epoch past the latest instant is the latest")
    {
        mock.terminal.adoptSessionEpoch(system_clock::time_point::max());
        auto const latest = std::chrono::floor<std::chrono::seconds>(vtbackend::LatestHeldInstant);
        CHECK(mock.terminal.sessionEpoch() == latest);
        CHECK(mock.terminal.lineBirthTime(line) == latest + latestBirth);
    }

    SECTION("an epoch before the earliest instant is the earliest")
    {
        mock.terminal.adoptSessionEpoch(system_clock::time_point::min());
        CHECK(mock.terminal.sessionEpoch() == vtbackend::EarliestHeldInstant);
        CHECK(mock.terminal.lineBirthTime(line) == vtbackend::EarliestHeldInstant + latestBirth);
    }
}

namespace
{

/// Adopts a record with host id @p id and no head here, as a daemon mirror does.
void adoptWithoutHead(vtbackend::Terminal& terminal, uint32_t id)
{
    auto record = mirroredRecord(id, CommandBlockState::Running);
    record.headIdGeneration = vtbackend::UnknownHeadGeneration;
    terminal.adoptCommandBlock(std::move(record), AdoptMode::Snapshot);
}

/// Labels the primary page's row @p row a row of block @p id carrying @p marks, as a mirror writes a host's.
void labelRow(vtbackend::Terminal& terminal, int row, uint32_t id, vtbackend::LineFlags marks = {})
{
    auto& line = terminal.primaryScreen().grid().lineAt(vtbackend::LineOffset(row));
    line.adoptBlock(CommandBlockId { id });
    line.setFlag(marks, true);
}

/// @return The page row block @p id is headed at, or nullopt when it has no head on a row of the grid.
[[nodiscard]] std::optional<int> headRowOf(vtbackend::Terminal& terminal, uint32_t id)
{
    auto const* const record = terminal.commandBlocks().find(CommandBlockId { id });
    if (record == nullptr)
        return std::nullopt;
    return vtbackend::headOffsetOf(terminal.primaryScreen().grid(), *record).transform([](auto head) {
        return unbox<int>(head);
    });
}

} // namespace

TEST_CASE("Terminal.placeCommandBlockHeads heads an adopted block by the rule a reflow heads it by",
          "[terminal][semanticblocks]")
{
    // No history, so the first row the grid holds is the page's top row.
    auto mock =
        vtbackend::MockTerm<> { vtbackend::PageSize { vtbackend::LineCount(6), vtbackend::ColumnCount(20) },
                                vtbackend::LineCount(0) };
    auto& terminal = mock.terminal;
    adoptWithoutHead(terminal, 5);
    auto block = std::array { CommandBlockId { 5 } };

    SECTION("at its lowest Marked row")
    {
        // Lowest: an erase under the cursor hands an older prompt's row, its mark kept, to the running block,
        // above that block's own prompt.
        labelRow(terminal, 1, 5, vtbackend::LineFlag::Marked);
        labelRow(terminal, 2, 5, vtbackend::LineFlag::Marked);
        labelRow(terminal, 3, 5, vtbackend::LineFlag::OutputStart);
        labelRow(terminal, 4, 5);
        terminal.placeCommandBlockHeads(block);
        CHECK(headRowOf(terminal, 5) == 2);
    }

    SECTION("at its lowest OutputStart row when none is Marked: a block a ;C began")
    {
        labelRow(terminal, 1, 5, vtbackend::LineFlag::OutputStart);
        labelRow(terminal, 2, 5, vtbackend::LineFlag::OutputStart);
        labelRow(terminal, 3, 5);
        terminal.placeCommandBlockHeads(block);
        CHECK(headRowOf(terminal, 5) == 2);
    }

    SECTION("at its topmost row when it carries neither mark: ED 1 blanked them")
    {
        labelRow(terminal, 2, 5);
        labelRow(terminal, 3, 5);
        terminal.placeCommandBlockHeads(block);
        CHECK(headRowOf(terminal, 5) == 2);
    }

    SECTION("but not at the first row the grid holds, where a head that scrolled out looks no different")
    {
        labelRow(terminal, 0, 5);
        labelRow(terminal, 1, 5);
        terminal.placeCommandBlockHeads(block);
        CHECK(terminal.commandBlocks().find(CommandBlockId { 5 })->headIdGeneration
              == vtbackend::UnknownHeadGeneration);
    }

    SECTION("nowhere while no row of it has arrived, and at the first that heads it once one has")
    {
        terminal.placeCommandBlockHeads(block);
        CHECK(terminal.commandBlocks().find(CommandBlockId { 5 })->headIdGeneration
              == vtbackend::UnknownHeadGeneration);

        labelRow(terminal, 3, 5, vtbackend::LineFlag::Marked);
        terminal.placeCommandBlockHeads(block);
        CHECK(headRowOf(terminal, 5) == 3);
    }
}

TEST_CASE("Terminal.placeCommandBlockHeads heads the blocks it names that have no head",
          "[terminal][semanticblocks]")
{
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(6) };
    auto& terminal = mock.terminal;
    adoptWithoutHead(terminal, 5);
    adoptWithoutHead(terminal, 6);
    labelRow(terminal, 1, 6, vtbackend::LineFlag::Marked);
    // Below block 6's head, so the walk, which goes bottom up, passes it first.
    labelRow(terminal, 4, 5, vtbackend::LineFlag::Marked);

    auto six = std::array { CommandBlockId { 6 } };
    terminal.placeCommandBlockHeads(six);
    CHECK(headRowOf(terminal, 6) == 1);
    CHECK_FALSE(headRowOf(terminal, 5).has_value()); // not named, so not looked for

    // A head this generation has is kept, as the host keeps the one OSC 133;A gave until an erase takes it.
    labelRow(terminal, 2, 6, vtbackend::LineFlag::Marked);
    // Repeated, out of order, and with an id no record has.
    auto named = std::array {
        CommandBlockId { 6 }, CommandBlockId { 99 }, CommandBlockId { 5 }, CommandBlockId { 5 }
    };
    terminal.placeCommandBlockHeads(named);
    CHECK(headRowOf(terminal, 6) == 1);
    CHECK(headRowOf(terminal, 5) == 4);
}

TEST_CASE("Terminal.placeCommandBlockHeads heads every block it names, in whatever order they come",
          "[terminal][semanticblocks]")
{
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(6) };
    auto& terminal = mock.terminal;
    for (auto const id: { 5U, 6U, 7U })
        adoptWithoutHead(terminal, id);
    labelRow(terminal, 1, 5, vtbackend::LineFlag::Marked);
    labelRow(terminal, 2, 6, vtbackend::LineFlag::Marked);
    labelRow(terminal, 3, 7, vtbackend::LineFlag::Marked);

    // Neither ascending nor descending: the order a mirror notes them in is the order it wrote rows in.
    auto named = std::array { CommandBlockId { 7 }, CommandBlockId { 5 }, CommandBlockId { 6 } };
    terminal.placeCommandBlockHeads(named);

    CHECK(headRowOf(terminal, 5) == 1);
    CHECK(headRowOf(terminal, 6) == 2);
    CHECK(headRowOf(terminal, 7) == 3);
}

TEST_CASE("Screen.noteBlockRowChange has the next read re-head a block a rewrite took the head row from",
          "[terminal][semanticblocks]")
{
    // What a daemon mirror does for each row it writes: the host's erase reaches it as a rewrite.
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(6) };
    auto& terminal = mock.terminal;
    auto& screen = terminal.primaryScreen();
    adoptWithoutHead(terminal, 5);
    labelRow(terminal, 1, 5, vtbackend::LineFlag::Marked);
    labelRow(terminal, 2, 5, vtbackend::LineFlag::OutputStart);
    labelRow(terminal, 3, 5);
    auto five = std::array { CommandBlockId { 5 } };
    terminal.placeCommandBlockHeads(five);
    REQUIRE(headRowOf(terminal, 5) == 1);
    auto const changes = screen.blockRowChanges();
    auto& head = screen.grid().lineAt(vtbackend::LineOffset(1));

    // A rewrite that leaves the row in its block with its head marks counts nothing: a prompt redrawn as the
    // user types. Nor does one of a row that belonged to no block.
    screen.noteBlockRowChange(head.blockId(), head.flags(), head);
    auto const& blank = screen.grid().lineAt(vtbackend::LineOffset(5));
    screen.noteBlockRowChange(CommandBlockId {}, blank.flags(), blank);
    CHECK(screen.blockRowChanges() == changes);

    // One that takes it out of the block counts, and the next read heads the block where the host's erase
    // re-headed it.
    auto const flags = head.flags();
    head.reset(vtbackend::LineFlags {}, vtbackend::GraphicsAttributes {});
    screen.noteBlockRowChange(CommandBlockId { 5 }, flags, head);
    CHECK(screen.blockRowChanges() == changes + 1);
    CHECK(headRowOf(terminal, 5) == 2);

    // So does one that only takes a head mark off it.
    auto& output = screen.grid().lineAt(vtbackend::LineOffset(2));
    output.setFlag(vtbackend::LineFlag::OutputStart, false);
    screen.noteBlockRowChange(CommandBlockId { 5 }, vtbackend::LineFlag::OutputStart, output);
    CHECK(screen.blockRowChanges() == changes + 2);
    CHECK(headRowOf(terminal, 5) == 2); // topmost row of the block now, which is still this one
}
