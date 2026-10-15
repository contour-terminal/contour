// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/RowMove.hpp>
#include <vtbackend/shell/StableIdSet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <set>
#include <string_view>

using namespace vtbackend;

namespace
{

/// The set {2, 5, 6, 20}.
[[nodiscard]] StableIdSet sample()
{
    auto ids = StableIdSet {};
    for (auto const id: { 2, 5, 6, 20 })
        ids.insert(id);
    return ids;
}

/// One mutation applied to sample(), and what it leaves.
struct Case
{
    std::string_view name;
    std::function<void(StableIdSet&)> apply;
    std::set<int64_t> expected;
};

} // namespace

TEST_CASE("StableIdSet.theRevisionMovesOnlyWithTheIds", "[folding][scrollbar-marks]")
{
    // Ids 4..9 scrolled up by two inside margins: 6..9 moved to 4..7, and what 4 and 5 named went.
    auto const scroll = RowMove {
        .changed = StableIdSpan { .first = 4, .last = 9 },
        .moved = StableIdSpan { .first = 6, .last = 9 },
        .by = -2,
    };
    auto const cases = std::array {
        Case { .name = "insert a new id",
               .apply = [](auto& s) { s.insert(3); },
               .expected = { 2, 3, 5, 6, 20 } },
        Case {
            .name = "insert a held id", .apply = [](auto& s) { s.insert(5); }, .expected = { 2, 5, 6, 20 } },
        Case { .name = "erase a held id", .apply = [](auto& s) { s.erase(5); }, .expected = { 2, 6, 20 } },
        Case {
            .name = "erase a missing id", .apply = [](auto& s) { s.erase(3); }, .expected = { 2, 5, 6, 20 } },
        Case { .name = "prune below a held id", .apply = [](auto& s) { s.prune(6); }, .expected = { 6, 20 } },
        Case { .name = "prune below them all",
               .apply = [](auto& s) { s.prune(2); },
               .expected = { 2, 5, 6, 20 } },
        Case { .name = "prune above a held id",
               .apply = [](auto& s) { s.pruneAbove(5); },
               .expected = { 2, 5 } },
        Case { .name = "prune above them all",
               .apply = [](auto& s) { s.pruneAbove(20); },
               .expected = { 2, 5, 6, 20 } },
        Case { .name = "follow a scroll",
               .apply = [&](auto& s) { s.follow(scroll); },
               .expected = { 2, 4, 20 } },
        Case { .name = "follow a scroll elsewhere",
               .apply =
                   [](auto& s) {
                       s.follow(RowMove { .changed = { .first = 10, .last = 15 },
                                          .moved = { .first = 11, .last = 15 },
                                          .by = -1 });
                   },
               .expected = { 2, 5, 6, 20 } },
        Case { .name = "follow a move by nothing",
               .apply =
                   [](auto& s) {
                       s.follow(RowMove { .changed = { .first = 4, .last = 9 },
                                          .moved = { .first = 4, .last = 9 },
                                          .by = 0 });
                   },
               .expected = { 2, 5, 6, 20 } },
        Case { .name = "forget some",
               .apply = [](auto& s) { s.forgetIf([](int64_t id) { return id % 2 == 0; }); },
               .expected = { 5 } },
        Case { .name = "forget none",
               .apply = [](auto& s) { s.forgetIf([](int64_t) { return false; }); },
               .expected = { 2, 5, 6, 20 } },
        Case { .name = "clear", .apply = [](auto& s) { s.clear(); }, .expected = {} },
    };

    for (auto const& c: cases)
    {
        INFO(c.name);
        auto ids = sample();
        auto const revision = ids.revision();
        c.apply(ids);
        CHECK(ids.ids() == c.expected);
        CHECK((ids.revision() != revision) == (c.expected != sample().ids()));
    }

    SECTION("clearing an empty set changes nothing")
    {
        auto ids = StableIdSet {};
        ids.clear();
        CHECK(ids.revision() == 0);
    }
}
