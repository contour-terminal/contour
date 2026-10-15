// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <vthost/CommandBlockWire.hpp>

using namespace std::chrono_literals;
namespace proto = vthost::proto;

namespace
{

/// A finished record with every optional field present and every enum off its zero value.
[[nodiscard]] vtbackend::CommandBlockRecord fullRecord()
{
    auto record = vtbackend::CommandBlockRecord {};
    record.id = vtbackend::CommandBlockId { 42 };
    record.state = vtbackend::CommandBlockState::Finished;
    record.end = vtbackend::CommandBlockEnd::Implicit;
    record.commandLine = vtbackend::CommandLineText { "make test" };
    record.commandLineSource = vtbackend::CommandLineSource::Recovered;
    record.exitCode = 2;
    record.outcome.exit = vtbackend::ContextExit::Crash;
    record.outcome.signal = vtbackend::ContextSignal::Segv;
    record.outcome.status = 139;
    record.promptStartedAt = std::chrono::system_clock::time_point { 1'700'000'000s };
    record.commandStartedAt = *record.promptStartedAt + 2s;
    record.duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(3s);
    record.workingDirectory.path = "/home/user/src";
    record.workingDirectory.locality = vtbackend::ContextLocality::Local;
    record.endWorkingDirectory.path = "/home/user/src/build";
    record.endWorkingDirectory.locality = vtbackend::ContextLocality::Foreign;
    record.foreignDirectory = vtbackend::ForeignDirectory::Reported;
    record.commandContext = vtbackend::ContextId { 7 }; // the HOST's OSC 3008 context: never reaches the wire
    record.headStableId = 1234;                         // the HOST's row id: never reaches the wire
    record.headIdGeneration = 5;
    return record;
}

} // namespace

TEST_CASE("a record survives its wire form, minus what only the host can use", "[vthost][wire][blocks]")
{
    auto const record = fullRecord();
    auto const back =
        vthost::fromWireCommandBlock(vthost::toWireCommandBlock(record, vthost::HostHead::Held), nullptr);

    auto expected = record;
    expected.commandContext = {};
    expected.headStableId = 0;
    expected.headIdGeneration = vtbackend::UnknownHeadGeneration; // no grid reaches it: "not yet known"
    CHECK(back == expected);
}

TEST_CASE("a record the mirror already holds keeps the mirror's own head position", "[vthost][wire][blocks]")
{
    auto held = fullRecord();
    held.headStableId = 77;
    held.headIdGeneration = 3;
    auto changed = vthost::toWireCommandBlock(fullRecord(), vthost::HostHead::Held);
    changed.commandLine = "make install";

    auto const back = vthost::fromWireCommandBlock(changed, &held);
    CHECK(back.commandLine.raw() == "make install");
    CHECK(back.headStableId == 77);
    CHECK(back.headIdGeneration == 3);
    CHECK(back.commandContext == held.commandContext);
}

TEST_CASE("a resend of the record the mirror holds rebuilds it exactly", "[vthost][wire][blocks]")
{
    // What CommandBlockStore::adopt() recognizes as a resend, and so leaves alone: a record equal to
    // the one it holds in every field, the ones the wire does not carry included.
    auto held = fullRecord();
    held.headStableId = 77;
    held.headIdGeneration = 3;
    CHECK(vthost::fromWireCommandBlock(vthost::toWireCommandBlock(held, vthost::HostHead::Held), &held)
          == held);
}

TEST_CASE("a record no prompt opened arrives without a prompt time", "[vthost][wire][blocks]")
{
    // A block a shell's ;C opened, with no ;A before it, has no prompt time: what tells the mirror's sticky
    // header that such a block never had a command row, rather than one that left.
    auto record = fullRecord();
    record.promptStartedAt.reset();
    auto const wire = vthost::toWireCommandBlock(record, vthost::HostHead::Held);
    CHECK((wire.present & proto::CommandBlockHasPromptStart) == 0);

    auto const back = vthost::fromWireCommandBlock(wire, nullptr);
    CHECK_FALSE(back.promptStartedAt.has_value());
    CHECK(back.commandStartedAt == record.commandStartedAt);
}

TEST_CASE("a record says whether the host's head row is gone, and nothing more", "[vthost][wire][blocks]")
{
    // Only the verdict travels, never a row id; and it is the mirror's to apply to its own grid, so a record
    // rebuilt from it is the record a held head would rebuild, still recognized as a resend.
    auto held = fullRecord();
    held.headStableId = 77;
    held.headIdGeneration = 3;
    auto const evicted = vthost::toWireCommandBlock(held, vthost::HostHead::Evicted);
    CHECK(vthost::hostHeadOf(evicted) == vthost::HostHead::Evicted);
    CHECK(vthost::hostHeadOf(vthost::toWireCommandBlock(held, vthost::HostHead::Held))
          == vthost::HostHead::Held);
    CHECK(vthost::fromWireCommandBlock(evicted, &held) == held);
}

TEST_CASE("a record arrives with the locality the host judged", "[vthost][wire][blocks]")
{
    // Whether the host's verdict holds where the record is read is the reading terminal's to decide
    // (vtbackend::Terminal::bindReplica), not the wire's.
    for (auto const locality: { vtbackend::ContextLocality::Unknown,
                                vtbackend::ContextLocality::Local,
                                vtbackend::ContextLocality::Foreign })
    {
        auto record = fullRecord();
        record.workingDirectory.locality = locality;
        record.endWorkingDirectory.locality = locality;
        auto const back =
            vthost::fromWireCommandBlock(vthost::toWireCommandBlock(record, vthost::HostHead::Held), nullptr);
        CHECK(back.workingDirectory.locality == locality);
        CHECK(back.endWorkingDirectory.locality == locality);
    }
}

TEST_CASE("bytes no enumerator names fall to the zero value", "[vthost][wire][blocks]")
{
    // A peer's byte becomes an enumerator here or not at all: every switch downstream assumes it
    // holds one the enum has.
    auto wire = proto::WireCommandBlock {};
    wire.state = 9;
    wire.end = 7;
    wire.commandLineSource = 5;
    wire.locality = 9;
    wire.endLocality = 9;
    wire.foreignDirectory = 2;
    wire.outcomeExit = 99;
    wire.outcomeSignal = 200;

    auto const record = vthost::fromWireCommandBlock(wire, nullptr);
    CHECK(record.state == vtbackend::CommandBlockState::Prompting);
    CHECK(record.end == vtbackend::CommandBlockEnd::Reported);
    CHECK(record.commandLineSource == vtbackend::CommandLineSource::None);
    CHECK(record.workingDirectory.locality == vtbackend::ContextLocality::Unknown);
    CHECK(record.endWorkingDirectory.locality == vtbackend::ContextLocality::Unknown);
    CHECK(record.foreignDirectory == vtbackend::ForeignDirectory::Unreported);
    CHECK(record.outcome.exit == vtbackend::ContextExit::Unknown);
    CHECK(record.outcome.signal == vtbackend::ContextSignal::None);
}

TEST_CASE("the last enumerator of each enum arrives as itself", "[vthost][wire][blocks]")
{
    // The other edge of each validator's range: off by one there and the newest enumerator, the one a
    // change is most likely to have just added, is the one that reads as unknown.
    CHECK(vthost::commandBlockStateOf(2) == vtbackend::CommandBlockState::Finished);
    CHECK(vthost::commandBlockStateOf(3) == std::nullopt);
    CHECK(vthost::commandBlockEndOf(1) == vtbackend::CommandBlockEnd::Implicit);
    CHECK(vthost::commandBlockEndOf(2) == std::nullopt);
    CHECK(vthost::commandLineSourceOf(2) == vtbackend::CommandLineSource::Recovered);
    CHECK(vthost::commandLineSourceOf(3) == std::nullopt);
    CHECK(vthost::contextLocalityOf(2) == vtbackend::ContextLocality::Foreign);
    CHECK(vthost::contextLocalityOf(3) == std::nullopt);
    CHECK(vthost::foreignDirectoryOf(1) == vtbackend::ForeignDirectory::Reported);
    CHECK(vthost::foreignDirectoryOf(2) == std::nullopt);
}

TEST_CASE("an optional the wire marks absent, or cannot represent, is absent", "[vthost][wire][blocks]")
{
    auto wire = vthost::toWireCommandBlock(fullRecord(), vthost::HostHead::Held);
    wire.exitCode = std::numeric_limits<int64_t>::max(); // present, but no `int` holds it
    wire.present = static_cast<uint8_t>(wire.present & ~proto::CommandBlockHasDuration);

    auto const record = vthost::fromWireCommandBlock(wire, nullptr);
    CHECK_FALSE(record.exitCode.has_value());
    CHECK_FALSE(record.duration.has_value());
    CHECK(record.commandStartedAt.has_value());

    SECTION("an exit code at the edge of `int` is one, a step past it is not")
    {
        wire.exitCode = std::numeric_limits<int>::min();
        CHECK(vthost::fromWireCommandBlock(wire, nullptr).exitCode == std::numeric_limits<int>::min());
        wire.exitCode = int64_t { std::numeric_limits<int>::min() } - 1;
        CHECK_FALSE(vthost::fromWireCommandBlock(wire, nullptr).exitCode.has_value());
    }

    SECTION("a negative duration is no length, not a length of zero")
    {
        wire.present = static_cast<uint8_t>(wire.present | proto::CommandBlockHasDuration);
        wire.duration = 0;
        CHECK(vthost::fromWireCommandBlock(wire, nullptr).duration
              == std::chrono::steady_clock::duration::zero());
        wire.duration = -1;
        CHECK_FALSE(vthost::fromWireCommandBlock(wire, nullptr).duration.has_value());
    }
}
