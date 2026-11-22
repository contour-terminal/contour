// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

#include <orchestrator/core/FailureWords.hpp>

using namespace orchestrator;

TEST_CASE("a failure in words of its own is shown as it is", "[orchestrator][failure]")
{
    CHECK(plainFailure("fatal: couldn't find remote ref") == "fatal: couldn't find remote ref");
    CHECK(plainFailure("no reply came in time") == "no reply came in time");
    CHECK(plainFailure("limit") == "busy — try again");
    CHECK(plainFailure("escapes") == "outside the worktree");
    CHECK(plainFailure("too-many") == "too many agent panes — close one first");
    CHECK(plainFailure("program-missing") == "claude isn't installed or isn't on the daemon's PATH");
    CHECK(plainFailure("not-a-directory") == "the worktree path isn't a directory");
    // A missing git or gh is refused as bad-argument: no word of it points at claude.
    REQUIRE(failureWordsOf("bad-argument") != nullptr);
    CHECK(failureWordsOf("bad-argument")->detail.empty());
    CHECK(failureWordsOf("fatal: no remote") == nullptr);
}

TEST_CASE("every failure code has one row of plain words", "[orchestrator][failure]")
{
    auto codes = std::vector<std::string_view> {};
    for (auto const& row: failureWords())
    {
        INFO(row.code);
        CHECK(!row.plain.empty());
        CHECK(row.plain != row.code);
        codes.push_back(row.code);
    }
    std::ranges::sort(codes);
    CHECK(std::ranges::adjacent_find(codes) == codes.end());
}
