// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <vtconformance/Runner.hpp>
#include <vtconformance/Suite.hpp>

using namespace vtconformance;

TEST_CASE("Runner.programOf.isTheOverrideWhenGiven", "[vtconformance]")
{
    // The program a run executes is the one the availability check asks about, so that --skip-if-missing
    // skips exactly when the run could not start.
    auto const& suite = vtTestSuite();
    auto options = RunOptions {};
    CHECK(programOf(suite, options) == "vttest");

    options.program = "/opt/vttest/bin/vttest";
    CHECK(programOf(suite, options) == "/opt/vttest/bin/vttest");
}

TEST_CASE("Runner.isSuiteAvailable.asksAboutTheProgramTheRunExecutes", "[vtconformance]")
{
    // --skip-if-missing skips exactly when the run could not start: the program asked about is the one the
    // run executes, the override when given. vttest has no entry point, so nothing else is looked for.
    auto const& suite = vtTestSuite();
    auto asked = std::vector<std::string> {};
    auto const probe = [&](std::string const& program) {
        asked.push_back(program);
        return true;
    };

    auto options = RunOptions {};
    CHECK(isSuiteAvailable(suite, options, probe));
    options.program = "/opt/vttest/bin/vttest";
    CHECK(isSuiteAvailable(suite, options, probe));
    CHECK(asked == std::vector<std::string> { "vttest", "/opt/vttest/bin/vttest" });

    CHECK_FALSE(isSuiteAvailable(suite, options, [](std::string const&) { return false; }));
}
