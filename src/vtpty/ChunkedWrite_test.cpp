// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ChunkedWrite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using vtpty::ChunkResult;
using vtpty::ChunkStart;
using vtpty::ChunkStatus;
using vtpty::PendingWait;
using vtpty::WriteEnd;

namespace
{

/// Whether the chunk started last is still waiting for the reader.
enum class Pending : std::uint8_t
{
    No = 0,
    Waiting,   ///< It waits.
    Cancelled, ///< It waited, and was asked to be cancelled.
};

/// A pipe whose reader takes what was written as the test says: how much room it reports and really has,
/// what ends a wait, and what a cancelled chunk is reported as.
struct FakePipe
{
    std::size_t reportedRoom = 0; ///< What room() says.
    std::size_t actualRoom = 0;   ///< What a chunk takes without waiting.
    /// What ends the wait for a chunk the pipe did not take: the reader taking it, or an interruption.
    PendingWait waitEndsBy = PendingWait::Completed;
    /// How a cancelled chunk ends.
    ChunkResult cancelledAs { .status = ChunkStatus::Cancelled, .transferred = 0 };
    /// Writes are interrupted once this many chunks were started.
    std::optional<std::size_t> interruptedAfter {};
    /// The chunk, counted from zero, that the pipe fails.
    std::optional<std::size_t> failsAt {};

    std::vector<std::size_t> chunks {}; ///< The size of every chunk started, in order.
    std::size_t roomQueries = 0;        ///< How often room() was asked.
    Pending pending = Pending::No;

    [[nodiscard]] bool isInterrupted() const
    {
        return interruptedAfter && chunks.size() >= *interruptedAfter;
    }

    [[nodiscard]] std::size_t room()
    {
        ++roomQueries;
        return reportedRoom;
    }

    [[nodiscard]] ChunkStart start(std::string_view chunk)
    {
        if (failsAt == chunks.size())
            return ChunkStart::Failed;
        chunks.push_back(chunk.size());
        if (chunk.size() <= actualRoom)
        {
            actualRoom -= chunk.size();
            return ChunkStart::Completed;
        }
        pending = Pending::Waiting;
        return ChunkStart::Pending;
    }

    [[nodiscard]] PendingWait awaitPending() const { return waitEndsBy; }

    void cancel() { pending = Pending::Cancelled; }

    [[nodiscard]] ChunkResult finish()
    {
        auto const wasPending = std::exchange(pending, Pending::No);
        if (wasPending == Pending::Cancelled)
            return cancelledAs;
        return { .status = ChunkStatus::Written, .transferred = chunks.back() };
    }
};

static_assert(vtpty::ChunkedPipe<FakePipe>);

} // namespace

TEST_CASE("ChunkedWrite.aWriteThePipeHasRoomForGoesInOneChunk", "[vtpty]")
{
    auto pipe = FakePipe { .reportedRoom = 100, .actualRoom = 100 };
    auto knownRoom = std::size_t { 0 };
    auto const outcome = vtpty::writeInChunks(pipe, std::string(10, 'x'), knownRoom);
    CHECK(outcome.written == 10);
    CHECK(outcome.end == WriteEnd::Complete);
    CHECK(pipe.chunks == std::vector<std::size_t> { 10 });
    CHECK(pipe.roomQueries == 1);
    CHECK(knownRoom == 90);
}

TEST_CASE("ChunkedWrite.aWriteWithinTheKnownRoomAsksThePipeNothing", "[vtpty]")
{
    auto pipe = FakePipe { .reportedRoom = 100, .actualRoom = 100 };
    auto knownRoom = std::size_t { 50 };
    CHECK(vtpty::writeInChunks(pipe, std::string(10, 'x'), knownRoom).written == 10);
    CHECK(pipe.roomQueries == 0);
    CHECK(knownRoom == 40);
}

TEST_CASE("ChunkedWrite.aKeystrokeAsksThePipeNothing", "[vtpty]")
{
    auto pipe = FakePipe { .reportedRoom = 100, .actualRoom = 100 };
    auto knownRoom = std::size_t { 0 };
    CHECK(vtpty::writeInChunks(pipe, "x", knownRoom).written == 1);
    CHECK(pipe.roomQueries == 0);
}

TEST_CASE("ChunkedWrite.aFullPipeIsWrittenOneByteAtATime", "[vtpty]")
{
    auto pipe = FakePipe {};
    auto knownRoom = std::size_t { 0 };
    auto const outcome = vtpty::writeInChunks(pipe, "abc", knownRoom);
    CHECK(outcome.written == 3);
    CHECK(outcome.end == WriteEnd::Complete);
    CHECK(pipe.chunks == std::vector<std::size_t> { 1, 1, 1 });
}

TEST_CASE("ChunkedWrite.anInterruptedWriteStartsNothing", "[vtpty]")
{
    auto pipe = FakePipe { .reportedRoom = 100, .actualRoom = 100, .interruptedAfter = 0 };
    auto knownRoom = std::size_t { 0 };
    auto const outcome = vtpty::writeInChunks(pipe, "abc", knownRoom);
    CHECK(outcome.written == 0);
    CHECK(outcome.end == WriteEnd::Interrupted);
    CHECK(pipe.chunks.empty());
}

TEST_CASE("ChunkedWrite.aSingleByteWaitingForTheReaderEndsWhenWritesAreInterrupted", "[vtpty]")
{
    auto pipe = FakePipe { .waitEndsBy = PendingWait::Interrupted };
    auto knownRoom = std::size_t { 0 };
    auto const outcome = vtpty::writeInChunks(pipe, "abc", knownRoom);
    CHECK(outcome.written == 0);
    CHECK(outcome.end == WriteEnd::Interrupted);
    CHECK(pipe.chunks == std::vector<std::size_t> { 1 });
}

// The hole this closes: a pipe that says it has room it does not have leaves a chunk of many bytes waiting
// for a reader that may never read. That wait too ends when writes are interrupted.
TEST_CASE("ChunkedWrite.aChunkTheRoomWasOverstatedForEndsWhenWritesAreInterrupted", "[vtpty]")
{
    auto pipe = FakePipe { .reportedRoom = 100, .waitEndsBy = PendingWait::Interrupted };
    auto knownRoom = std::size_t { 0 };
    auto const outcome = vtpty::writeInChunks(pipe, std::string(50, 'x'), knownRoom);
    CHECK(outcome.written == 0);
    CHECK(outcome.end == WriteEnd::Interrupted);
    CHECK(pipe.chunks == std::vector<std::size_t> { 50 });
}

TEST_CASE("ChunkedWrite.aCancelledChunkCountsWhatThePipeReportsTaken", "[vtpty]")
{
    auto pipe = FakePipe { .reportedRoom = 100,
                           .waitEndsBy = PendingWait::Interrupted,
                           .cancelledAs = { .status = ChunkStatus::Cancelled, .transferred = 7 } };
    auto knownRoom = std::size_t { 0 };
    auto const outcome = vtpty::writeInChunks(pipe, std::string(50, 'x'), knownRoom);
    CHECK(outcome.written == 7);
    CHECK(outcome.end == WriteEnd::Interrupted);
    CHECK(knownRoom == 0); // it waited: the room it was sized by was wrong
}

// The race: the reader takes the chunk as its cancellation lands. It was written, all of it, and the write
// stops before the next one.
TEST_CASE("ChunkedWrite.aChunkCompletingAsItIsCancelledCountsInFull", "[vtpty]")
{
    auto pipe = FakePipe { .reportedRoom = 50,
                           .waitEndsBy = PendingWait::Interrupted,
                           .cancelledAs = { .status = ChunkStatus::Written, .transferred = 50 },
                           .interruptedAfter = 1 };
    auto knownRoom = std::size_t { 0 };
    auto const outcome = vtpty::writeInChunks(pipe, std::string(80, 'x'), knownRoom);
    CHECK(outcome.written == 50);
    CHECK(outcome.end == WriteEnd::Interrupted);
    CHECK(pipe.chunks == std::vector<std::size_t> { 50 });
}

TEST_CASE("ChunkedWrite.aPipeThatFailsEndsTheWriteWithWhatWasWritten", "[vtpty]")
{
    SECTION("as a chunk is handed over")
    {
        auto pipe = FakePipe { .reportedRoom = 10, .actualRoom = 100, .failsAt = 1 };
        auto knownRoom = std::size_t { 0 };
        auto const outcome = vtpty::writeInChunks(pipe, std::string(30, 'x'), knownRoom);
        CHECK(outcome.written == 10);
        CHECK(outcome.end == WriteEnd::Failed);
    }

    SECTION("while a chunk waits")
    {
        auto pipe = FakePipe { .reportedRoom = 10,
                               .waitEndsBy = PendingWait::Interrupted,
                               .cancelledAs = { .status = ChunkStatus::Failed, .transferred = 0 } };
        auto knownRoom = std::size_t { 0 };
        auto const outcome = vtpty::writeInChunks(pipe, std::string(30, 'x'), knownRoom);
        CHECK(outcome.written == 0);
        CHECK(outcome.end == WriteEnd::Failed);
    }
}

// A chunk that waited proved the room it was sized by wrong. Once writes are allowed again, the next write
// asks the pipe rather than trusting it, and does not wait again.
TEST_CASE("ChunkedWrite.aChunkThatWaitedLeavesNoRoomKnown", "[vtpty]")
{
    auto pipe = FakePipe { .reportedRoom = 100, .waitEndsBy = PendingWait::Interrupted };
    auto knownRoom = std::size_t { 0 };
    auto const interrupted = vtpty::writeInChunks(pipe, std::string(50, 'x'), knownRoom);
    REQUIRE(interrupted.end == WriteEnd::Interrupted);
    CHECK(knownRoom == 0);

    // Allowed again; the pipe now tells its room right.
    pipe.reportedRoom = 0;
    pipe.waitEndsBy = PendingWait::Completed;
    auto const queriesBefore = pipe.roomQueries;
    auto const outcome = vtpty::writeInChunks(pipe, "ab", knownRoom);
    CHECK(outcome.written == 2);
    CHECK(outcome.end == WriteEnd::Complete);
    CHECK(pipe.roomQueries > queriesBefore);
    CHECK(pipe.chunks == std::vector<std::size_t> { 50, 1, 1 });
}
