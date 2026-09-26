// SPDX-License-Identifier: Apache-2.0
#include <core/testing/ExitCode.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using core::testing::normalisedExitCode;
using core::testing::SkipExitCode;

namespace
{

/// The totals of a run whose test cases ended as given, one assertion each.
[[nodiscard]] Catch::Totals totalsOf(std::uint64_t passed, std::uint64_t failed, std::uint64_t skipped)
{
    auto totals = Catch::Totals {};
    totals.testCases.passed = passed;
    totals.testCases.failed = failed;
    totals.testCases.skipped = skipped;
    totals.assertions.passed = passed;
    totals.assertions.failed = failed;
    return totals;
}

} // namespace

TEST_CASE("normalisedExitCode: a run where everything passed is 0", "[testing]")
{
    CHECK(normalisedExitCode(totalsOf(3, 0, 0), 0) == 0);
}

TEST_CASE("normalisedExitCode: any failure is 1, whatever Catch2 returned", "[testing]")
{
    CHECK(normalisedExitCode(totalsOf(2, 1, 0), 42) == 1);
    CHECK(normalisedExitCode(totalsOf(0, 4, 0), 4) == 1);
}

TEST_CASE("normalisedExitCode: a failed test case without a failed assertion is 1", "[testing]")
{
    auto totals = totalsOf(0, 0, 0);
    totals.testCases.failed = 1;
    CHECK(normalisedExitCode(totals, 42) == 1);
}

TEST_CASE("normalisedExitCode: a failure among skips is 1", "[testing]")
{
    CHECK(normalisedExitCode(totalsOf(0, 1, 2), 42) == 1);
}

TEST_CASE("normalisedExitCode: every test case skipped is the skip code", "[testing]")
{
    CHECK(SkipExitCode == 77);
    CHECK(normalisedExitCode(totalsOf(0, 0, 3), 4) == SkipExitCode);
}

TEST_CASE("normalisedExitCode: some test cases skipped and the rest passed is 0", "[testing]")
{
    CHECK(normalisedExitCode(totalsOf(1, 0, 2), 0) == 0);
}

TEST_CASE("normalisedExitCode: nothing failed but Catch2 reported an error is 1", "[testing]")
{
    // -w UnmatchedTestSpec with a test spec part that matched nothing: Catch2 returns 3.
    CHECK(normalisedExitCode(totalsOf(1, 0, 0), 3) == 1);
    CHECK(normalisedExitCode(totalsOf(1, 0, 2), 3) == 1);
}

TEST_CASE("normalisedExitCode: no test case ran and Catch2 objected is 2", "[testing]")
{
    CHECK(normalisedExitCode(totalsOf(0, 0, 0), 2) == 2);
}

TEST_CASE("normalisedExitCode: no test case ran and Catch2 was content is 0", "[testing]")
{
    // --list-tests, --help and the like run no test case and succeed.
    CHECK(normalisedExitCode(totalsOf(0, 0, 0), 0) == 0);
}
