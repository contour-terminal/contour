// SPDX-License-Identifier: Apache-2.0
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <utility>

#include <testing/CaseTrace.hpp>

using contour::testing::CaseTraceEvent;
using contour::testing::caseTraceLine;

TEST_CASE("the case trace is on only for a set, non-zero variable", "[testing][trace]")
{
    using contour::testing::caseTraceEnabled;
    auto const tracing = [](std::string value) {
        return caseTraceEnabled(core::testing::FakeEnvironment {
            { { std::string { contour::testing::CaseTraceVariable }, std::move(value) } } });
    };
    CHECK_FALSE(caseTraceEnabled(core::testing::FakeEnvironment {}));
    CHECK_FALSE(tracing(""));
    CHECK_FALSE(tracing("0"));
    CHECK(tracing("1"));
}

TEST_CASE("the case trace names each case and section as it starts", "[testing][trace]")
{
    using namespace std::chrono_literals;
    CHECK(caseTraceLine(CaseTraceEvent::CaseStarting, "a case", 0, 0ms) == "[trace] case: a case");
    CHECK(caseTraceLine(CaseTraceEvent::SectionStarting, "outer", 1, 0ms) == "[trace]   section: outer");
    CHECK(caseTraceLine(CaseTraceEvent::SectionStarting, "inner", 2, 0ms) == "[trace]     section: inner");
    CHECK(caseTraceLine(CaseTraceEvent::CaseEnded, "a case", 0, 42ms) == "[trace] ended: a case (42 ms)");
}
