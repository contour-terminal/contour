// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/DaemonStart.hpp>

#include <vtpty/PtyAdoption.hpp>

#include <core/Utf8Path.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

using contour::cli::DaemonStartFailures;
using contour::cli::loadDaemonConfig;
using contour::cli::resolveDaemonProfile;
using vthost::StartContinuity;

namespace
{

/// A configuration file of its own, removed with this.
struct ConfigFile
{
    std::filesystem::path directory = std::filesystem::temp_directory_path()
                                      / std::format("contour-daemon-start-{}", std::random_device {}());
    std::filesystem::path path = directory / "contour.yml";

    explicit ConfigFile(std::string_view contents)
    {
        std::filesystem::create_directories(directory);
        std::ofstream { path, std::ios::binary | std::ios::trunc } << contents;
    }
    ConfigFile(ConfigFile const&) = delete;
    ConfigFile& operator=(ConfigFile const&) = delete;
    ConfigFile(ConfigFile&&) = delete;
    ConfigFile& operator=(ConfigFile&&) = delete;
    ~ConfigFile()
    {
        auto ignored = std::error_code {};
        std::filesystem::remove_all(directory, ignored);
    }

    [[nodiscard]] std::string name() const { return core::pathToUtf8(path); }
};

/// Not YAML at all.
constexpr auto Unparsable = std::string_view { "profiles: [unterminated\n  : : :\n" };

/// A configuration whose only profile is "work", and whose default names it.
constexpr auto OnlyWork = std::string_view { "default_profile: work\nprofiles:\n  work: {}\n" };

} // namespace

TEST_CASE("a fresh daemon refuses a configuration that does not load", "[contour][cli][daemon]")
{
    auto const file = ConfigFile { Unparsable };
    auto failures = DaemonStartFailures { StartContinuity::Fresh };
    auto const loaded = loadDaemonConfig(file.name(), failures);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().contains("could not be loaded"));
    CHECK(failures.take().empty());
}

TEST_CASE("a handed-over daemon runs on the built-in configuration, watching the same file",
          "[contour][cli][daemon]")
{
    auto const file = ConfigFile { Unparsable };
    auto failures = DaemonStartFailures { StartContinuity::HandedOver };
    auto const loaded = loadDaemonConfig(file.name(), failures);
    REQUIRE(loaded.has_value());
    CHECK(loaded->configFile == file.path);
    CHECK(loaded->profiles.value().contains("main"));
    auto const told = failures.take();
    REQUIRE(told.size() == 1);
    CHECK(told.front().contains("built-in configuration"));
}

TEST_CASE("a fresh daemon refuses a profile the configuration does not name", "[contour][cli][daemon]")
{
    auto const file = ConfigFile { OnlyWork };
    auto failures = DaemonStartFailures { StartContinuity::Fresh };
    auto config = loadDaemonConfig(file.name(), failures);
    REQUIRE(config.has_value());
    auto const profile = resolveDaemonProfile(*config, "gone", failures);
    REQUIRE_FALSE(profile.has_value());
    CHECK(profile.error().contains("gone"));
}

TEST_CASE("a handed-over daemon runs the default profile in place of one the configuration lost",
          "[contour][cli][daemon]")
{
    auto const file = ConfigFile { OnlyWork };
    auto failures = DaemonStartFailures { StartContinuity::HandedOver };
    auto config = loadDaemonConfig(file.name(), failures);
    REQUIRE(config.has_value());
    auto const profile = resolveDaemonProfile(*config, "gone", failures);
    REQUIRE(profile.has_value());
    CHECK(profile->name.empty());
    CHECK(profile->resolved.configFile == file.path);
    CHECK(config->profiles.value().contains("work"));
    auto const told = failures.take();
    REQUIRE(told.size() == 1);
    CHECK(told.front().contains("gone"));
}

TEST_CASE("a handed-over daemon runs the built-in profile when not even the default one resolves",
          "[contour][cli][daemon]")
{
    auto const file = ConfigFile { "default_profile: missing\nprofiles:\n  work: {}\n" };
    auto failures = DaemonStartFailures { StartContinuity::HandedOver };
    auto config = loadDaemonConfig(file.name(), failures);
    REQUIRE(config.has_value());
    auto const profile = resolveDaemonProfile(*config, "gone", failures);
    REQUIRE(profile.has_value());
    CHECK(config->profiles.value().contains("main"));
    CHECK(config->configFile == file.path);
    CHECK(failures.take().size() == 1);
}

TEST_CASE("a daemon its keeper restarted runs on the built-in configuration, as a handed-over one does",
          "[contour][cli][daemon]")
{
    auto const file = ConfigFile { Unparsable };
    auto failures = DaemonStartFailures { StartContinuity::Recovered };
    auto config = loadDaemonConfig(file.name(), failures);
    REQUIRE(config.has_value());
    CHECK(config->configFile == file.path);
    auto const profile = resolveDaemonProfile(*config, "gone", failures);
    REQUIRE(profile.has_value());
    // Both are told, neither ends the daemon: ending would hang up the shells its keeper kept.
    CHECK(failures.take().size() == 2);
}

TEST_CASE("handover-info reports whether the daemon's configuration loads and names its profile",
          "[contour][cli][daemon][upgrade]")
{
    using contour::cli::handoverReadiness;
    // Asked about no configuration, it reads none.
    auto const plain = handoverReadiness("1.2.3", {}, {});
    CHECK(plain
          == vthost::handover::ownReadiness(
              "1.2.3", vthost::handover::reportedAdoption(vtpty::pseudoconsoleAdoptability())));
    CHECK_FALSE(plain.configProblem.has_value());

    auto const good = ConfigFile { OnlyWork };
    CHECK_FALSE(handoverReadiness("1.2.3", good.name(), "work").configProblem.has_value());
    CHECK_FALSE(handoverReadiness("1.2.3", good.name(), {}).configProblem.has_value());

    auto const lost = handoverReadiness("1.2.3", good.name(), "play");
    REQUIRE(lost.configProblem.has_value());
    CHECK(lost.release == "1.2.3");
    CHECK(vthost::handover::checkConfigured(lost)
          == std::unexpected(vthost::handover::UpgradeRefusal::ConfigUnreadable));

    auto const broken = ConfigFile { Unparsable };
    CHECK(handoverReadiness("1.2.3", broken.name(), {}).configProblem.has_value());
}
