// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/config/ConfigReader.hpp>
#include <contour/config/DaemonExtensionConfig.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <yaml-cpp/yaml.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/SessionSettings.hpp>

using contour::config::DaemonExtensionAutostart;
using contour::config::DaemonExtensionProgram;
using contour::config::DaemonMode;
using contour::config::YamlToJsonError;
using contour::test::loadConfigFromYaml;

TEST_CASE("daemon section defaults", "[config][daemon]")
{
    auto const config = contour::config::Config {};
    CHECK(config.daemon.value().mode == DaemonMode::Auto);
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
    CHECK(config.daemon.value().mode == DaemonMode::Auto);
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

TEST_CASE("daemon.default_cell_pixel_size defaults to the VT340 text cell", "[config][daemon]")
{
    auto const config = contour::config::Config {};
    CHECK(config.daemon.value().defaultCellPixelSize
          == vtbackend::ImageSize { vtbackend::Width(10), vtbackend::Height(20) });
    CHECK(loadConfigFromYaml("daemon:\n  mode: auto\n").daemon.value().defaultCellPixelSize
          == vtbackend::ImageSize { vtbackend::Width(10), vtbackend::Height(20) });
}

TEST_CASE("daemon.default_cell_pixel_size reads width and height", "[config][daemon]")
{
    auto const config = loadConfigFromYaml("daemon:\n  default_cell_pixel_size: { width: 9, height: 18 }\n");
    CHECK(config.daemon.value().defaultCellPixelSize
          == vtbackend::ImageSize { vtbackend::Width(9), vtbackend::Height(18) });
}

TEST_CASE("an invalid daemon.default_cell_pixel_size keeps the default", "[config][daemon]")
{
    auto const* const text = GENERATE(as<char const*>(),
                                      "{ width: 0, height: 18 }",
                                      "{ width: -1, height: 18 }",
                                      "{ width: 9 }",
                                      "{ height: 18 }",
                                      "{ width: 1025, height: 18 }",
                                      "{ width: nine, height: 18 }",
                                      "\"9x18\"");
    CAPTURE(text);
    auto const config = loadConfigFromYaml(std::format("daemon:\n  default_cell_pixel_size: {}\n", text));
    CHECK(config.daemon.value().defaultCellPixelSize
          == vtbackend::ImageSize { vtbackend::Width(10), vtbackend::Height(20) });
}

TEST_CASE("a null daemon.default_cell_pixel_size keeps the default", "[config][daemon]")
{
    auto const* const text = GENERATE(as<char const*>(), "~", "null", "");
    CAPTURE(text);
    auto const config = loadConfigFromYaml(std::format("daemon:\n  default_cell_pixel_size: {}\n", text));
    CHECK(config.daemon.value().defaultCellPixelSize
          == vtbackend::ImageSize { vtbackend::Width(10), vtbackend::Height(20) });
}

TEST_CASE("daemon.keeper.restart_hung is on unless turned off", "[config][daemon]")
{
    CHECK(contour::config::Config {}.daemon.value().keeper.restartHung);
    CHECK(
        loadConfigFromYaml("daemon:\n  keeper:\n    restart_hung: true\n").daemon.value().keeper.restartHung);
    CHECK_FALSE(loadConfigFromYaml("daemon:\n  keeper:\n    restart_hung: false\n")
                    .daemon.value()
                    .keeper.restartHung);
}

TEST_CASE("daemon.keeper's crash-loop numbers are read in whole seconds", "[config][daemon]")
{
    auto const defaults = vthost::keeper::RestartPolicyConfig {};
    auto const config = loadConfigFromYaml("daemon:\n"
                                           "  keeper:\n"
                                           "    give_up_crashes: 3\n"
                                           "    give_up_window: 60\n"
                                           "    backoff_start: 2\n"
                                           "    backoff_cap: 10\n"
                                           "    startup_timeout: 45\n"
                                           "    stop_timeout: 20\n");
    auto const& policy = config.daemon.value().keeper.restartPolicy;
    CHECK(policy.giveUpCrashes == 3);
    CHECK(policy.giveUpWindow == std::chrono::seconds { 60 });
    CHECK(policy.backoffStart == std::chrono::seconds { 2 });
    CHECK(policy.backoffCap == std::chrono::seconds { 10 });
    CHECK(policy.restartStartupBound == std::chrono::seconds { 45 });
    CHECK(policy.daemonExitBound == std::chrono::seconds { 20 });
    // What the keys do not name keeps its default.
    CHECK(policy.quickRepeat == defaults.quickRepeat);
    CHECK(policy.grace == defaults.grace);
}

TEST_CASE("daemon.keeper's crash-loop numbers keep their defaults where unreadable", "[config][daemon]")
{
    auto const defaults = vthost::keeper::RestartPolicyConfig {};
    CHECK(contour::config::Config {}.daemon.value().keeper.restartPolicy.giveUpCrashes
          == defaults.giveUpCrashes);
    auto const config = loadConfigFromYaml("daemon:\n"
                                           "  keeper:\n"
                                           "    give_up_crashes: 0\n"
                                           "    give_up_window: soon\n"
                                           "    startup_timeout: -5\n");
    auto const& policy = config.daemon.value().keeper.restartPolicy;
    CHECK(policy.giveUpCrashes == defaults.giveUpCrashes);
    CHECK(policy.giveUpWindow == defaults.giveUpWindow);
    CHECK(policy.restartStartupBound == defaults.restartStartupBound);
}

TEST_CASE("daemon.keeper.backoff_cap is never below backoff_start", "[config][daemon]")
{
    auto const config = loadConfigFromYaml("daemon:\n  keeper:\n    backoff_start: 40\n    backoff_cap: 5\n");
    auto const& policy = config.daemon.value().keeper.restartPolicy;
    CHECK(policy.backoffStart == std::chrono::seconds { 40 });
    CHECK(policy.backoffCap == std::chrono::seconds { 40 });
}

namespace
{

/// The orchestrator's configuration, as the user writes it.
constexpr auto OrchestratorYaml = R"(daemon:
  extensions:
    orchestrator:
      autostart: true
      config:
        worktree_root: ~/worktrees
        models: [opus, sonnet, haiku]
        max_agents: 4
        max_agents_per_repo: 2
        poll_every: 2m
        status_board_after: 15m
        finished_retention: 30d
        repos:
          - path: ~/repo/contour
            forge: github
            remote: origin            # default
            base: master              # optional; default: the forge's default branch
            issues:
              - { source: forge, query: "assignee:@me" }
        tasks:                        # one row per task; a new task is a new row
          - { id: fix_ci,         key: f, label: "fix CI",         applies_to: pr,    model: sonnet,
              prompt: "/contour-workflows:fix-ci {url}" }
          - { id: address_review, key: r, label: "address review", applies_to: pr,    model: opus,
              prompt: "/contour-workflows:address-review {url}" }
          - { id: issue,          key: w, label: "work on issue",  applies_to: issue, model: opus,
              prompt: "/contour-workflows:work-issue {url}" }
)";

/// What the orchestrator is loaded with.
constexpr auto OrchestratorJson =
    R"({"worktree_root":"~/worktrees","models":["opus","sonnet","haiku"],"max_agents":4,"max_agents_per_repo":2,"poll_every":"2m","status_board_after":"15m","finished_retention":"30d","repos":[{"path":"~/repo/contour","forge":"github","remote":"origin","base":"master","issues":[{"source":"forge","query":"assignee:@me"}]}],"tasks":[{"id":"fix_ci","key":"f","label":"fix CI","applies_to":"pr","model":"sonnet","prompt":"/contour-workflows:fix-ci {url}"},{"id":"address_review","key":"r","label":"address review","applies_to":"pr","model":"opus","prompt":"/contour-workflows:address-review {url}"},{"id":"issue","key":"w","label":"work on issue","applies_to":"issue","model":"opus","prompt":"/contour-workflows:work-issue {url}"}]})";

} // namespace

TEST_CASE("daemon.extensions round-trips the orchestrator example", "[config][daemon]")
{
    auto const config = loadConfigFromYaml(OrchestratorYaml);
    REQUIRE(config.daemon.value().extensions.size() == 1);
    auto const& orchestrator = config.daemon.value().extensions.front();
    CHECK(orchestrator.name == "orchestrator");
    CHECK(orchestrator.path.empty());
    CHECK(orchestrator.autostart == DaemonExtensionAutostart::AtDaemonStart);
    CHECK(orchestrator.configJson == OrchestratorJson);

    SECTION("and through the writer")
    {
        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        auto const reread = loadConfigFromYaml(written);
        CHECK(reread.daemon.value().extensions == config.daemon.value().extensions);
    }
}

TEST_CASE("daemon.extensions: an entry's defaults", "[config][daemon]")
{
    auto const config =
        loadConfigFromYaml("daemon:\n  extensions:\n    orchestrator:\n    other:\n      path: x.wasm\n");
    REQUIRE(config.daemon.value().extensions.size() == 2);
    auto const& orchestrator = config.daemon.value().extensions[0];
    CHECK(orchestrator.autostart == DaemonExtensionAutostart::OnDemand);
    CHECK(orchestrator.configJson == "{}");
    CHECK(config.daemon.value().extensions[1].path.filename() == "x.wasm");
    CHECK(config.daemon.value().extensions[1].path.is_absolute());
}

TEST_CASE("daemon.extensions: programs name the executables an extension runs", "[config][daemon]")
{
    auto const config = loadConfigFromYaml("daemon:\n"
                                           "  extensions:\n"
                                           "    orchestrator:\n"
                                           "      programs:\n"
                                           "        claude: /opt/claude/bin/claude\n"
                                           "        gh: ~/bin/gh\n");
    REQUIRE(config.daemon.value().extensions.size() == 1);
    auto const& programs = config.daemon.value().extensions.front().programs;
    REQUIRE(programs.size() == 2);
    CHECK(programs[0] == DaemonExtensionProgram { .name = "claude", .path = "/opt/claude/bin/claude" });
    CHECK(programs[1].name == "gh");
    // `~/` is the home directory: absolute, and no longer spelled with a tilde.
    CHECK(programs[1].path.is_absolute());
    CHECK(programs[1].path.filename() == "gh");

    SECTION("and through the writer")
    {
        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        auto const reread = loadConfigFromYaml(written);
        CHECK(reread.daemon.value().extensions == config.daemon.value().extensions);
    }
}

TEST_CASE("daemon.extensions: a program that names no executable costs only itself", "[config][daemon]")
{
    auto const config = loadConfigFromYaml("daemon:\n"
                                           "  extensions:\n"
                                           "    orchestrator:\n"
                                           "      autostart: true\n"
                                           "      programs:\n"
                                           "        bin/claude: /opt/claude\n" // a path, not a program name
                                           "        relative: bin/claude\n" // found on the PATH it bypasses
                                           "        empty: ''\n"
                                           "        listed: [/a, /b]\n"
                                           "        claude: /opt/claude\n"
                                           "        claude: /opt/other\n"); // the first one stands
    REQUIRE(config.daemon.value().extensions.size() == 1);
    auto const& orchestrator = config.daemon.value().extensions.front();
    CHECK(orchestrator.autostart == DaemonExtensionAutostart::AtDaemonStart);
    CHECK(orchestrator.programs
          == std::vector { DaemonExtensionProgram { .name = "claude", .path = "/opt/claude" } });

    SECTION("a programs that is no map is ignored, the entry kept")
    {
        auto const list =
            loadConfigFromYaml("daemon:\n  extensions:\n    orchestrator:\n      programs: [claude]\n");
        REQUIRE(list.daemon.value().extensions.size() == 1);
        CHECK(list.daemon.value().extensions.front().programs.empty());
    }
}

TEST_CASE("daemon.extensions: a config that has no JSON spelling costs the entry", "[config][daemon]")
{
    auto const config = loadConfigFromYaml(
        "daemon:\n  extensions:\n    bad:\n      config: { x: .inf }\n    good:\n      autostart: maybe\n");
    REQUIRE(config.daemon.value().extensions.size() == 1);
    CHECK(config.daemon.value().extensions[0].name == "good");
    CHECK(config.daemon.value().extensions[0].autostart == DaemonExtensionAutostart::OnDemand);
}

TEST_CASE("yamlToJson", "[config][daemon]")
{
    struct Row
    {
        std::string_view yaml;
        std::expected<std::string, YamlToJsonError> json;
    };
    for (auto const& row: std::array {
             Row { .yaml = R"("4")", .json = R"("4")" },
             Row { .yaml = "4", .json = "4" },
             Row { .yaml = "-4", .json = "-4" },
             Row { .yaml = "0x1f", .json = "31" },
             Row { .yaml = "1.5", .json = "1.5" },
             Row { .yaml = "-1.5", .json = "-1.5" },
             Row { .yaml = "1e3", .json = "1000" },
             Row { .yaml = ".5", .json = "0.5" },
             Row { .yaml = "1e999", .json = std::unexpected(YamlToJsonError::NotRepresentable) },
             Row { .yaml = "true", .json = "true" },
             Row { .yaml = "'true'", .json = R"("true")" },
             Row { .yaml = "~", .json = "null" },
             Row { .yaml = "~/w", .json = R"("~/w")" },
             Row { .yaml = "two words", .json = R"("two words")" },
             Row { .yaml = R"("a\"b\\c\nd")", .json = R"("a\"b\\c\nd")" },
             Row { .yaml = "[1, a, {b: c}]", .json = R"([1,"a",{"b":"c"}])" },
             Row { .yaml = "-.inf", .json = std::unexpected(YamlToJsonError::NotRepresentable) },
             Row { .yaml = "99999999999999999999",
                   .json = std::unexpected(YamlToJsonError::NotRepresentable) },
             Row { .yaml = "{a: 1, a: 2}", .json = std::unexpected(YamlToJsonError::NotRepresentable) },
             Row { .yaml = "{[a]: 1}", .json = std::unexpected(YamlToJsonError::NotRepresentable) },
         })
    {
        INFO(row.yaml);
        CHECK(contour::config::yamlToJson(YAML::Load(std::string(row.yaml))) == row.json);
    }

    auto const nested = [](int depth) {
        auto yaml = std::string { "1" };
        for ([[maybe_unused]] auto const _: std::views::iota(0, depth))
            yaml = std::format("{{a: {}}}", yaml);
        return YAML::Load(yaml);
    };
    CHECK(contour::config::yamlToJson(nested(16)).has_value());
    CHECK(contour::config::yamlToJson(nested(17)) == std::unexpected(YamlToJsonError::TooDeep));
}
