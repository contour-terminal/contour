// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <vector>

#include <vthost/latency/HeadlessBench.hpp>

using namespace vthost::latency;
using namespace std::chrono_literals;

namespace
{
/// @return The numeric columns after "@p metric daemon" in the printed table @p table: the count, then the
///         figures after it; empty without such a row.
std::vector<double> daemonRowOf(std::string const& table, std::string const& metric)
{
    auto lines = std::istringstream { table };
    for (auto line = std::string {}; std::getline(lines, line);)
    {
        auto fields = std::istringstream { line };
        auto name = std::string {};
        auto mode = std::string {};
        if (!(fields >> name >> mode) || name != metric || mode != "daemon")
            continue;
        auto columns = std::vector<double> {};
        for (auto value = double {}; fields >> value;)
            columns.push_back(value);
        return columns;
    }
    return {};
}
} // namespace

TEST_CASE("mixed mode reports keystrokes under a flood", "[vthost][latency]")
{
    auto const options = HeadlessOptions {
        .runs = 1,
        .keys = 20,
        .workload = {},
        .keystrokeLoad = KeystrokeLoad::FloodedPane,
        .floodDuration = 3s,
        .executable = std::filesystem::path { BENCH_LATENCY_EXECUTABLE },
        .record = std::nullopt,
    };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};

    auto const exitCode = runHeadlessBench(options, out, err);
    INFO(err.str());
    INFO(out.str());
    REQUIRE(exitCode == EXIT_SUCCESS);

    auto const keystrokes = daemonRowOf(out.str(), "keystroke");
    REQUIRE_FALSE(keystrokes.empty());
    CHECK(keystrokes.front() >= 20);
    auto const flood = daemonRowOf(out.str(), "flood-mibps");
    REQUIRE(flood.size() >= 2);
    CHECK(flood[1] > 0.0);
}

TEST_CASE("tabs mode times each tab opened during a flood", "[vthost][latency]")
{
    auto const options = HeadlessOptions {
        .runs = 1,
        .keys = 20,
        .workload = {},
        .keystrokeLoad = KeystrokeLoad::Idle,
        .floodDuration = 3s,
        .tabs = TabLoad::EverySecond,
        .executable = std::filesystem::path { BENCH_LATENCY_EXECUTABLE },
        .record = std::nullopt,
    };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};

    auto const exitCode = runHeadlessBench(options, out, err);
    INFO(err.str());
    INFO(out.str());
    REQUIRE(exitCode == EXIT_SUCCESS);

    // A tab a second over a three-second flood: two at least, whatever the flood's tail.
    auto const tabs = daemonRowOf(out.str(), "tab-startup");
    REQUIRE_FALSE(tabs.empty());
    CHECK(tabs.front() >= 2);
    auto const flood = daemonRowOf(out.str(), "flood-mibps");
    REQUIRE(flood.size() >= 2);
    CHECK(flood[1] > 0.0);
}

TEST_CASE("mixed mode refuses keys that do not fit in the flood", "[vthost][latency]")
{
    auto const options = HeadlessOptions {
        .runs = 1,
        .keys = 100,
        .workload = {},
        .keystrokeLoad = KeystrokeLoad::FloodedPane,
        .floodDuration = 3s,
        .executable = std::filesystem::path { BENCH_LATENCY_EXECUTABLE },
        .record = std::nullopt,
    };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};

    CHECK(runHeadlessBench(options, out, err) == EXIT_FAILURE);
    CHECK(err.str().contains("does not fit in `seconds 3`"));
}
