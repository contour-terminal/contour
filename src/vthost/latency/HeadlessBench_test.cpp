// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <vector>

#include <vthost/DaemonSpawn.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/latency/BenchSupport.hpp>
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

#ifndef _WIN32
TEST_CASE("serve with keeper runs the daemon under a keeper", "[vthost][latency]")
{
    auto const scratch = ScratchDirectory::create();
    REQUIRE(scratch.has_value());
    auto const socketPath = scratch->path() / "daemon";
    auto keeper = BenchProcess::start(std::filesystem::path { BENCH_LATENCY_EXECUTABLE },
                                      { "serve", "socket", socketPath.string(), "keeper" });
    REQUIRE(keeper.has_value());
    REQUIRE(vthost::awaitDaemon(*vthost::makeNativeDaemonSpawner(), socketPath, 10s)
            == vthost::DaemonPresence::Serving);

    auto const control = vthost::client::makeDaemonControl(
        std::make_shared<vthost::UnixTransportFactory>(socketPath, nullptr));
    auto const status = control->status();
    REQUIRE(status.has_value());
    // The process started is the keeper; the daemon serving is its child, a process of its own.
    auto const keeperProcessId = (*keeper)->processId();
    REQUIRE(keeperProcessId.has_value());
    REQUIRE(status->keeperProcessId.has_value());
    auto const started = static_cast<uint64_t>(*keeperProcessId);
    CHECK(*status->keeperProcessId == started);
    CHECK(status->processId != started);

    // Stopping the daemon ends its keeper too.
    REQUIRE(control->stop(vthost::proto::StopReason::User));
    CHECK((*keeper)->waitFor(10s).has_value());
}
#endif

TEST_CASE("headless passes keeper on to the daemon it serves", "[vthost][latency]")
{
    auto options = HeadlessOptions {
        .runs = 1,
        .keys = 20,
        .workload = {},
        .keystrokeLoad = KeystrokeLoad::Idle,
        .floodDuration = 3s,
        .keeping = DaemonKeeping::Plain,
        .executable = std::filesystem::path { BENCH_LATENCY_EXECUTABLE },
        .record = std::nullopt,
    };
    auto const socket = std::filesystem::path { "daemon" };
    auto const plain = daemonServeArguments(options, socket);
    CHECK(std::ranges::find(plain, "keeper") == plain.end());

    options.keeping = DaemonKeeping::Keeper;
    auto const arguments = daemonServeArguments(options, socket);
    CHECK(arguments.back() == "keeper");

#ifndef _WIN32
    // And a run behind the keeper measures as one without.
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    auto const exitCode = runHeadlessBench(options, out, err);
    INFO(err.str());
    INFO(out.str());
    REQUIRE(exitCode == EXIT_SUCCESS);
    auto const keystrokes = daemonRowOf(out.str(), "keystroke");
    REQUIRE_FALSE(keystrokes.empty());
    CHECK(keystrokes.front() >= 20);
#endif
}
