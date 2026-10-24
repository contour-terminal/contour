// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <string>
#include <vector>

#include <vthost/SessionSettings.hpp>

using contour::config::DaemonMode;
using contour::test::loadConfigFromYaml;

TEST_CASE("daemon section defaults", "[config][daemon]")
{
    auto const config = contour::config::Config {};
    CHECK(config.daemon.value().mode == DaemonMode::InProcess);
    CHECK(config.daemon.value().socket.empty());
    CHECK(config.daemon.value().sizePolicy == vthost::ClientSizePolicy::Latest);
    CHECK_FALSE(config.daemon.value().historyLimit.has_value());
}

TEST_CASE("daemon section parses every key", "[config][daemon]")
{
    auto const config =
        loadConfigFromYaml("daemon:\n  mode: required\n  socket: /tmp/x\n  size_policy: smallest\n  "
                           "history_limit: -1\n");
    CHECK(config.daemon.value().mode == DaemonMode::Required);
    CHECK(config.daemon.value().socket == "/tmp/x");
    CHECK(config.daemon.value().sizePolicy == vthost::ClientSizePolicy::Smallest);
    CHECK(config.daemon.value().historyLimit == vtbackend::MaxHistoryLineCount { vtbackend::Infinite {} });
}

TEST_CASE("daemon.history_limit takes a line count", "[config][daemon]")
{
    auto const config = loadConfigFromYaml("daemon:\n  history_limit: 250\n");
    CHECK(config.daemon.value().historyLimit == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(250) });
}

TEST_CASE("an unknown daemon.mode keeps the default", "[config][daemon]")
{
    auto const config = loadConfigFromYaml("daemon:\n  mode: sometimes\n  size_policy: biggest\n");
    CHECK(config.daemon.value().mode == DaemonMode::InProcess);
    CHECK(config.daemon.value().sizePolicy == vthost::ClientSizePolicy::Latest);
}

TEST_CASE("every daemon mode name parses to its mode", "[config][daemon]")
{
    for (auto const& [name, mode]: contour::config::DaemonModeNames)
    {
        auto const config = loadConfigFromYaml(std::format("daemon:\n  mode: {}\n", name));
        CHECK(config.daemon.value().mode == mode);
    }
}

TEST_CASE("the daemon's default scrollback is the in-process default", "[config][daemon]")
{
    CHECK(contour::config::HistoryConfig {}.maxHistoryLineCount
          == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(vthost::DefaultSessionHistoryLineCount) });
}

TEST_CASE("Config: daemon.restore defaults", "[config][daemon]")
{
    auto const config = contour::config::Config {};
    auto const& c = config.daemon.value().restore;
    CHECK(c.scrollback);
    CHECK(c.scrollbackLines == 1000);
    CHECK(c.rerunPrograms.empty());
    CHECK(c.interval == std::chrono::seconds { 30 });
}

TEST_CASE("Config: daemon.restore reads every key", "[config][daemon]")
{
    auto const config =
        loadConfigFromYaml("daemon:\n  restore:\n    scrollback: false\n    scrollback_lines: 200\n"
                           "    rerun_programs: [vim, htop]\n    interval: 60\n");
    auto const& c = config.daemon.value().restore;
    CHECK_FALSE(c.scrollback);
    CHECK(c.scrollbackLines == 200);
    CHECK(c.rerunPrograms == std::vector<std::string> { "vim", "htop" });
    CHECK(c.interval == std::chrono::seconds { 60 });
}

TEST_CASE("Config: daemon.restore clamps out-of-range values", "[config][daemon]")
{
    auto const config =
        loadConfigFromYaml("daemon:\n  restore:\n    interval: 1\n    scrollback_lines: 999999\n");
    auto const& c = config.daemon.value().restore;
    CHECK(c.interval == contour::config::MinRestoreInterval);
    CHECK(c.scrollbackLines == contour::config::MaxRestoreScrollbackLines);
}

TEST_CASE("Config: a malformed daemon.restore value keeps its default and the keys after it",
          "[config][daemon]")
{
    auto const config =
        loadConfigFromYaml("daemon:\n  restore:\n    scrollback_lines: lots\n"
                           "    rerun_programs: vim\n    interval: soon\n  size_policy: smallest\n");
    auto const& c = config.daemon.value().restore;
    CHECK(c.scrollbackLines == 1000);
    CHECK(c.rerunPrograms.empty());
    CHECK(c.interval == std::chrono::seconds { 30 });
    CHECK(config.daemon.value().sizePolicy == vthost::ClientSizePolicy::Smallest);
}
