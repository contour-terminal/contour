// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/NewDaemon.hpp>

#include <vtpty/ProcessPlacement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <vthost/DaemonBinary.hpp>

using contour::cli::NamedDaemonStart;
using vthost::DaemonPresence;

namespace
{

/// Records what it spawns; a probe answers what `answer` says.
class RecordingSpawner final: public vthost::DaemonSpawner
{
  public:
    [[nodiscard]] std::expected<void, std::string> spawn(std::span<std::string const> argv,
                                                         vtpty::ProcessPlacement& /*placement*/) override
    {
        spawned.emplace_back(argv.begin(), argv.end());
        return {};
    }

    [[nodiscard]] DaemonPresence probe(std::filesystem::path const& /*socketPath*/) override
    {
        return answer();
    }

    std::function<DaemonPresence()> answer = [] {
        return DaemonPresence::Absent;
    };
    std::vector<std::vector<std::string>> spawned;
};

} // namespace

TEST_CASE("startNamedDaemon starts a persistent daemon where none serves", "[cli][daemon]")
{
    auto spawner = RecordingSpawner {};
    spawner.answer = [&spawner] {
        return spawner.spawned.empty() ? DaemonPresence::Absent : DaemonPresence::Serving;
    };
    auto binary = vthost::InstalledDaemonBinary { "contour" };
    auto placement = vtpty::NoPlacement {};

    auto const started = contour::cli::startNamedDaemon("/run/contour/work2",
                                                        { .spawner = spawner,
                                                          .binary = binary,
                                                          .placement = placement,
                                                          .options = {},
                                                          .timeout = std::chrono::seconds { 1 } });

    REQUIRE(started == NamedDaemonStart::Started);
    REQUIRE(spawner.spawned.size() == 1);
    auto const& argv = spawner.spawned.front();
    CHECK(std::ranges::contains(argv, std::string { "--lifecycle=persistent" }));
    CHECK(std::ranges::contains(argv, std::string { "--socket=/run/contour/work2" }));
}

TEST_CASE("startNamedDaemon starts nothing where a daemon serves", "[cli][daemon]")
{
    auto spawner = RecordingSpawner {};
    spawner.answer = [] {
        return DaemonPresence::Serving;
    };
    auto binary = vthost::InstalledDaemonBinary { "contour" };
    auto placement = vtpty::NoPlacement {};

    auto const started = contour::cli::startNamedDaemon("/run/contour/admin",
                                                        { .spawner = spawner,
                                                          .binary = binary,
                                                          .placement = placement,
                                                          .options = {},
                                                          .timeout = std::chrono::seconds { 1 } });

    CHECK(started == NamedDaemonStart::AlreadyRunning);
    CHECK(spawner.spawned.empty());
}

TEST_CASE("startNamedDaemon says why when the daemon does not come up", "[cli][daemon]")
{
    auto spawner = RecordingSpawner {};
    auto binary = vthost::InstalledDaemonBinary { "contour" };
    auto placement = vtpty::NoPlacement {};

    auto const started = contour::cli::startNamedDaemon("/run/contour/slow",
                                                        { .spawner = spawner,
                                                          .binary = binary,
                                                          .placement = placement,
                                                          .options = {},
                                                          .timeout = std::chrono::seconds { 0 } });

    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().contains("/run/contour/slow"));
    CHECK(spawner.spawned.size() == 1);
}
