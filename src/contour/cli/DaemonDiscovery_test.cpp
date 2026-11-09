// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/DaemonDiscovery.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <string>
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
