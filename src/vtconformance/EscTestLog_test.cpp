// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <vtconformance/EscTestLog.h>

using namespace vtconformance;

// The samples below are literal excerpts of what esctest wrote when run against a real xterm-406.

TEST_CASE("EscTestLog.reads_the_closing_verdict", "[vtconformance]")
{
    auto const summary = parseEscTestLog("Run test: EDTests.test_ED_0\n"
                                         "Send sequence: <ESC>[2J\n"
                                         "*** 27 tests passed, 1 known bug, 0 tests failed ***\n");

    CHECK(summary.valid());
    CHECK(summary.passed == 27);
    CHECK(summary.knownBugs == 1);
    CHECK(summary.failed == 0);
    CHECK(summary.total() == 28);
    CHECK(summary.failingTests.empty());
}

TEST_CASE("EscTestLog.reads_the_failing_test_names", "[vtconformance]")
{
    // esctest SHOUTS the failure count, so the words are not a reliable key -- only the numbers are.
    // And the name list is not terminated by a blank line: esctest goes straight on to log the
    // sequences it sends while tearing down.
    auto const summary = parseEscTestLog("*** 17 tests passed, 1 known bug, 10 TESTS FAILED ***\n"
                                         "Failing tests:\n"
                                         "DECSEDTests.test_DECSED_0_Protection\n"
                                         "EDTests.test_ED_respectsISOProtection\n"
                                         "Send sequence: <ESC>[1;1H\n"
                                         "Send sequence: <ESC>[999B\n");

    CHECK(summary.valid());
    CHECK(summary.passed == 17);
    CHECK(summary.knownBugs == 1);
    CHECK(summary.failed == 10);
    REQUIRE(summary.failingTests.size() == 2);
    CHECK(summary.failingTests[0] == "DECSEDTests.test_DECSED_0_Protection");
    CHECK(summary.failingTests[1] == "EDTests.test_ED_respectsISOProtection");
}

TEST_CASE("EscTestLog.a_log_without_a_verdict_is_an_error", "[vtconformance]")
{
    // esctest crashing and reporting nothing must not read as "nothing was wrong".
    auto const summary = parseEscTestLog("Run test: EDTests.test_ED_0\nTraceback (most recent call):\n");

    CHECK(!summary.valid());
    CHECK(!summary.parseError.empty());
    CHECK(summary.total() == 0);
}

TEST_CASE("EscTestLog.an_empty_log_is_an_error", "[vtconformance]")
{
    CHECK(!parseEscTestLog("").valid());
}

TEST_CASE("EscTestLog.the_last_verdict_wins", "[vtconformance]")
{
    // A log that was appended to twice must be read as the latest run, not the first.
    auto const summary = parseEscTestLog("*** 1 tests passed, 0 known bugs, 5 TESTS FAILED ***\n"
                                         "Failing tests:\n"
                                         "ATests.test_a\n"
                                         "*** 9 tests passed, 0 known bugs, 1 TESTS FAILED ***\n"
                                         "Failing tests:\n"
                                         "BTests.test_b\n");

    CHECK(summary.passed == 9);
    CHECK(summary.failed == 1);
    REQUIRE(summary.failingTests.size() == 1);
    CHECK(summary.failingTests[0] == "BTests.test_b");
}

TEST_CASE("EscTestLog.a_verdict_line_that_is_not_one_is_ignored", "[vtconformance]")
{
    // Only a line with three numbers in it is a verdict; esctest prints other `*** ... ***` banners.
    auto const summary = parseEscTestLog("*** starting ***\n"
                                         "*** 3 tests passed, 0 known bugs, 0 tests failed ***\n");

    CHECK(summary.valid());
    CHECK(summary.passed == 3);
}
