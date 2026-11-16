// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/DaemonDiscovery.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <latch>
#include <map>
#include <memory>
#include <random>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

using contour::cli::DiscoveredDaemon;
using contour::cli::filterDiscovered;
using contour::cli::makeLocalDaemonDiscovery;
using vthost::DaemonPresence;

namespace
{
namespace fs = std::filesystem;

/// Answers a probe from a table, and remembers what it was asked.
class FakeProber final: public vthost::DaemonSpawner
{
  public:
    [[nodiscard]] std::expected<std::unique_ptr<vthost::DaemonSpawnLock>, vthost::SpawnLockError>
    tryLockSpawn(std::filesystem::path const&) override
    {
        return std::make_unique<vthost::DaemonSpawnLock>(); // no other client starts one
    }

    explicit FakeProber(std::map<fs::path, DaemonPresence> answers): _answers(std::move(answers)) {}

    [[nodiscard]] std::expected<void, std::string> spawn(std::span<std::string const> /*argv*/,
                                                         vtpty::ProcessPlacement& /*placement*/) override
    {
        return std::unexpected { std::string { "discovery never spawns" } };
    }

    [[nodiscard]] DaemonPresence probe(fs::path const& socketPath) override
    {
        probed.push_back(socketPath);
        auto const found = _answers.find(socketPath);
        return found == _answers.end() ? DaemonPresence::Absent : found->second;
    }

    std::vector<fs::path> probed;

  private:
    std::map<fs::path, DaemonPresence> _answers;
};

[[nodiscard]] std::vector<std::string> labelsOf(std::vector<DiscoveredDaemon> const& daemons)
{
    auto labels = std::vector<std::string> {};
    for (auto const& daemon: daemons)
        labels.push_back(daemon.label);
    return labels;
}

[[nodiscard]] std::optional<std::size_t> twoWorkspaces(fs::path const& /*socket*/)
{
    return 2;
}
} // namespace

TEST_CASE("DaemonDiscovery.filterDiscovered drops a stale socket and orders default first",
          "[contour][discovery]")
{
    auto const found = filterDiscovered({ { "/run/contour/work", DaemonPresence::Serving },
                                          { "/run/contour/admin", DaemonPresence::Absent },
                                          { "/run/contour/default", DaemonPresence::Serving } },
                                        twoWorkspaces);
    CHECK(labelsOf(found) == std::vector<std::string> { "default", "work" });
    CHECK(found[0].socket == "/run/contour/default");
    CHECK(found[1].workspaces == 2);
}

TEST_CASE("DaemonDiscovery.filterDiscovered sorts the rest by label and skips sibling files",
          "[contour][discovery]")
{
    auto const found = filterDiscovered({ { "/r/zed", DaemonPresence::Serving },
                                          { "/r/default-native", DaemonPresence::Serving },
                                          { "/r/default.log", DaemonPresence::Serving },
                                          { "/r/alpha", DaemonPresence::Serving },
                                          { "/r/alpha-tmux", DaemonPresence::Serving } },
                                        [](auto const&) { return std::nullopt; });
    CHECK(labelsOf(found) == std::vector<std::string> { "alpha", "zed" });
    CHECK_FALSE(found[0].workspaces.has_value());
}

TEST_CASE("DaemonDiscovery.a fake directory is listed and each control socket probed once",
          "[contour][discovery]")
{
    auto prober = FakeProber { { { "/run/contour/default", DaemonPresence::Serving },
                                 { "/run/contour/work", DaemonPresence::Serving },
                                 { "/run/contour/admin", DaemonPresence::Absent } } };
    auto listed = fs::path {};
    auto const discovery =
        makeLocalDaemonDiscovery("/run/contour", prober, twoWorkspaces, [&](fs::path const& dir) {
            listed = dir;
            return std::vector<fs::path> { "/run/contour/work",
                                           "/run/contour/default-native",
                                           "/run/contour/admin",
                                           "/run/contour/default" };
        });
    auto const found = discovery->discover();
    CHECK(listed == "/run/contour");
    CHECK(labelsOf(found) == std::vector<std::string> { "default", "work" });
    // The sibling endpoint is not probed.
    CHECK(prober.probed.size() == 3);
}

TEST_CASE("DaemonDiscovery.an empty or unreadable directory finds nothing", "[contour][discovery]")
{
    auto prober = FakeProber { {} };
    auto const discovery = makeLocalDaemonDiscovery(
        "/nowhere", prober, twoWorkspaces, [](fs::path const&) { return std::vector<fs::path> {}; });
    CHECK(discovery->discover().empty());
    CHECK(prober.probed.empty());
}

TEST_CASE("DaemonDiscovery.two discoveries run at once through the native prober",
          "[contour][discovery][threading]")
{
    // The switcher and the switch directory each discover off the GUI thread, through the one discovery
    // the application holds. Both reach the operating system's prober at once, so its probes must not
    // share an event loop: one driven from two threads aborts (core::net's G1) or races.
    constexpr auto ProbesPerDiscovery = 200;
    auto const directory = fs::temp_directory_path() / "contour-discovery-test-no-daemons";
    auto const prober = vthost::makeNativeDaemonSpawner();
    auto bothListing = std::latch { 2 };
    auto const discovery =
        makeLocalDaemonDiscovery(directory, *prober, twoWorkspaces, [&](fs::path const& dir) {
            // Neither starts probing until both are about to: their probes overlap from the first.
            bothListing.arrive_and_wait();
            auto sockets = std::vector<fs::path> {};
            for (auto const index: std::views::iota(0, ProbesPerDiscovery))
                sockets.push_back(dir / std::format("absent-{}", index));
            return sockets;
        });

    auto first = std::vector<DiscoveredDaemon> {};
    auto second = std::vector<DiscoveredDaemon> {};
    {
        auto const one = std::jthread { [&] { first = discovery->discover(); } };
        auto const two = std::jthread { [&] { second = discovery->discover(); } };
    }
    // No daemon serves in a directory that does not exist.
    CHECK(first.empty());
    CHECK(second.empty());
}

TEST_CASE("DaemonDiscovery.saved hosts of a missing configuration are none, and create no file",
          "[contour][discovery]")
{
    auto const directory =
        fs::temp_directory_path() / std::format("contour-saved-hosts-{}", std::random_device {}());
    auto const configFile = directory / "contour.yml";
    REQUIRE_FALSE(fs::exists(configFile));

    auto const saved = contour::cli::savedHostsOf(configFile.string());
    REQUIRE(saved.contains);
    CHECK_FALSE(saved.contains("work"));
    CHECK_FALSE(fs::exists(configFile));
    CHECK_FALSE(fs::exists(directory));
}

TEST_CASE("DaemonDiscovery.--daemon names a host side file beside a missing configuration, creating no file",
          "[contour][discovery]")
{
    auto const directory =
        fs::temp_directory_path() / std::format("contour-saved-hosts-{}", std::random_device {}());
    fs::create_directories(directory / "hosts");
    std::ofstream(directory / "hosts" / "box.yml") << "url: ssh://box\n";
    auto const configFile = directory / "contour.yml";

    auto const saved = contour::cli::savedHostsOf(configFile.string());
    CHECK(saved.contains("box"));
    // A local daemon of that name too: a bare word that only a saved host names would fall to the saved hosts
    // anyway, so ambiguity is what tells the lookup found it.
    auto const ambiguous = vthost::client::resolveDaemonWord(
        "box", { .contains = [](std::string_view name) { return name == "box"; } }, saved);
    CHECK(ambiguous == std::unexpected { vthost::client::DaemonWordError::Ambiguous });
    auto const target = vthost::client::resolveDaemonWord("host:box", {}, saved);
    REQUIRE(target.has_value());
    CHECK(target->kind == vthost::client::DaemonWordKind::SavedHost);
    CHECK(target->text == "box");
    CHECK_FALSE(saved.contains("work"));
    CHECK_FALSE(fs::exists(configFile));

    fs::remove_all(directory);
}
