// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/RowMove.hpp>
#include <vtbackend/shell/UserMarkIndex.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <set>

using namespace vtbackend;

TEST_CASE("UserMarkIndex.recordsTheMarksOfItsGeneration", "[scrollbar-marks]")
{
    auto index = UserMarkIndex {};
    REQUIRE(index.isCurrentFor(0)); // a fresh grid starts at generation 0, with no marks
    REQUIRE(index.ids().empty());

    index.record(7, 0, UserMarkChange::Added);
    index.record(3, 0, UserMarkChange::Added);
    CHECK(index.ids() == std::set<int64_t> { 3, 7 });

    // Adding what is already there changes nothing either.
    auto const added = index.revision();
    index.record(3, 0, UserMarkChange::Added);
    CHECK(index.revision() == added);

    auto const revision = index.revision();
    index.record(7, 0, UserMarkChange::Removed);
    CHECK(index.ids() == std::set<int64_t> { 3 });
    CHECK(index.revision() > revision);

    // Removing what is not there changes nothing, and says so by leaving the revision alone.
    auto const settled = index.revision();
    index.record(7, 0, UserMarkChange::Removed);
    CHECK(index.revision() == settled);
}

TEST_CASE("UserMarkIndex.anIdFromAnotherGenerationWaitsForTheRescan", "[scrollbar-marks]")
{
    auto index = UserMarkIndex {};
    index.record(5, 0, UserMarkChange::Added);

    // A reflow renamed every row: generation 1's ids must never mix with generation 0's.
    index.record(9, 1, UserMarkChange::Added);
    CHECK_FALSE(index.isCurrentFor(1));
    CHECK(index.ids() == std::set<int64_t> { 5 });

    auto const revision = index.revision();
    auto const rescanned = std::array { int64_t { 12 }, int64_t { 20 } };
    index.rebuild(rescanned, 1);
    CHECK(index.isCurrentFor(1));
    CHECK(index.ids() == std::set<int64_t> { 12, 20 });
    CHECK(index.revision() > revision);
}

TEST_CASE("UserMarkIndex.pruneForgetsMarksThatLeftTheGrid", "[scrollbar-marks]")
{
    auto index = UserMarkIndex {};
    index.record(1, 0, UserMarkChange::Added);
    index.record(5, 0, UserMarkChange::Added);
    index.record(9, 0, UserMarkChange::Added);
    index.record(14, 0, UserMarkChange::Added);

    // 1 was evicted above the floor, 14 cut off below a page whose bottom row is now 12.
    auto const addressable = StableIdSpan { .first = 5, .last = 12 };
    auto const revision = index.revision();
    index.prune(addressable);
    CHECK(index.ids() == std::set<int64_t> { 5, 9 });
    CHECK(index.revision() > revision);

    // Nothing outside the range any more: a second prune is free and silent.
    auto const settled = index.revision();
    index.prune(addressable);
    CHECK(index.revision() == settled);
}

TEST_CASE("UserMarkIndex.followMovesAMarkWithItsLine", "[scrollbar-marks]")
{
    auto index = UserMarkIndex {};
    index.record(2, 0, UserMarkChange::Added);
    index.record(5, 0, UserMarkChange::Added);
    index.record(6, 0, UserMarkChange::Added);
    index.record(20, 0, UserMarkChange::Added);

    // Ids 4..9 scrolled up by two inside margins: 6..9 moved to 4..7, and what 4 and 5 named went.
    auto const move = RowMove {
        .changed = StableIdSpan { .first = 4, .last = 9 },
        .moved = StableIdSpan { .first = 6, .last = 9 },
        .by = -2,
    };
    auto const revision = index.revision();
    index.follow(move);
    CHECK(index.ids() == std::set<int64_t> { 2, 4, 20 });
    CHECK(index.revision() > revision);

    // A move that touches no mark changes nothing, and says so.
    auto const settled = index.revision();
    index.follow(RowMove {
        .changed = StableIdSpan { .first = 10, .last = 15 },
        .moved = StableIdSpan { .first = 11, .last = 15 },
        .by = -1,
    });
    CHECK(index.ids() == std::set<int64_t> { 2, 4, 20 });
    CHECK(index.revision() == settled);
}

TEST_CASE("UserMarkIndex.aRescanThatFindsTheSameMarksChangesNothing", "[scrollbar-marks]")
{
    auto index = UserMarkIndex {};
    index.record(4, 0, UserMarkChange::Added);
    index.record(8, 0, UserMarkChange::Added);

    // A history-limit change bumped the generation, yet every marked line kept its id: nothing to redraw.
    auto const revision = index.revision();
    auto const rescanned = std::array { int64_t { 8 }, int64_t { 4 } };
    index.rebuild(rescanned, 1);
    CHECK(index.isCurrentFor(1));
    CHECK(index.ids() == std::set<int64_t> { 4, 8 });
    CHECK(index.revision() == revision);
}
