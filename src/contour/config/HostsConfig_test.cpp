// SPDX-License-Identifier: Apache-2.0
//
// The saved hosts Settings writes: `hosts/<name>.yml` side files merged into Config::hosts with their
// provenance, and the side file's own bytes (HostSideFile).

#include <contour/config/Config.hpp>
#include <contour/config/HostSideFile.hpp>
#include <contour/config/HostUrl.hpp>

#include <vtpty/Process.hpp>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string_view>

using namespace std::string_view_literals;

namespace
{

/// Writes @p yaml as the contour.yml inside @p dir and returns its path.
[[nodiscard]] std::filesystem::path writeConfig(QTemporaryDir const& dir, std::string_view yaml)
{
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / "contour.yml";
    auto out = std::ofstream(path);
    out << yaml;
    return path;
}

/// Loads a config from inline YAML through the production file loader.
[[nodiscard]] contour::config::Config loadFromYaml(QTemporaryDir const& dir, std::string_view yaml)
{
    auto config = contour::config::Config {};
    contour::config::loadConfigFromFile(config, writeConfig(dir, yaml));
    return config;
}

/// Writes @p content to the side file at @p relativePath under @p dir, creating `hosts/` as needed.
void writeSideFile(QTemporaryDir const& dir,
                   std::filesystem::path const& relativePath,
                   std::string_view content)
{
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / relativePath;
    std::filesystem::create_directories(path.parent_path());
    auto out = std::ofstream(path);
    out << content;
}

} // namespace

TEST_CASE("Config: a hosts/<name>.yml side file merges as an editable saved host", "[config][hosts][gui]")
{
    QTemporaryDir const dir;
    writeSideFile(dir, "hosts/starfive.yml", "url: ssh://me@starfive\nprofile: remote\n");
    writeSideFile(dir, "hosts/build-box.yml", "url: tls://10.0.0.7:7297\ntoken_file: /tmp/build-box.token\n");
    auto const config = loadFromYaml(dir, R"(
hosts:
    home:
        url: "ssh://homepc"
)"sv);

    auto const& hosts = config.hosts.value();
    REQUIRE(hosts.size() == 3);
    CHECK(hosts.at("starfive").url.user == "me");
    CHECK(hosts.at("starfive").profile == "remote");
    CHECK(hosts.at("build-box").url.scheme == contour::config::HostScheme::Tls);
    CHECK(hosts.at("build-box").tokenFile == std::filesystem::path { "/tmp/build-box.token" });

    // contour.yml's host is read-only in Settings; the side files are Settings' own.
    CHECK(config.hostOrigins.at("home") == contour::config::SettingsOrigin::MainConfig);
    CHECK(config.hostOrigins.at("starfive") == contour::config::SettingsOrigin::SideFile);
    CHECK(config.hostOrigins.at("build-box") == contour::config::SettingsOrigin::SideFile);
    CHECK(config.loadedSideFiles.contains("hosts/starfive.yml"));
}

TEST_CASE("Config: a host side file the loader cannot use is skipped, the others load",
          "[config][hosts][gui]")
{
    QTemporaryDir const dir;
    writeSideFile(dir, "hosts/no-token.yml", "url: tls://studio:7297\n");
    writeSideFile(dir, "hosts/broken.yml", "url: [unterminated\n");
    writeSideFile(dir, "hosts/fine.yml", "url: ssh://fine\n");
    auto const config = loadFromYaml(dir, "default_profile: main\n"sv);

    CHECK(config.hosts.value().size() == 1);
    CHECK(config.hosts.value().contains("fine"));
    CHECK_FALSE(config.hostOrigins.contains("no-token"));
}

TEST_CASE("Config: an in-place reload drops a removed host side file", "[config][hosts][gui]")
{
    QTemporaryDir const dir;
    writeSideFile(dir, "hosts/starfive.yml", "url: ssh://starfive\n");
    auto const configPath = writeConfig(dir, "default_profile: main\n");

    contour::config::Config config;
    contour::config::loadConfigFromFile(config, configPath);
    REQUIRE(config.hosts.value().contains("starfive"));

    std::filesystem::remove(std::filesystem::path(dir.path().toStdU16String()) / "hosts" / "starfive.yml");
    contour::config::loadConfigFromFile(config, configPath);

    CHECK_FALSE(config.hosts.value().contains("starfive"));
    CHECK_FALSE(config.hostOrigins.contains("starfive"));
}

TEST_CASE("Config: a host round-trips through emitHostYaml, as written and as loaded", "[config][hosts][gui]")
{
    auto const draft = contour::config::HostDraft { .url = "tls://build-box:7297",
                                                    .tokenFile = "~/build-box.token",
                                                    .tlsCa = {},
                                                    .remoteCommand = {},
                                                    .profile = "remote" };
    auto const bytes = contour::config::emitHostYaml(draft);
    CHECK_FALSE(bytes.contains("tls_ca")); // an empty field is left out, as the loader defaults it
    CHECK(contour::config::parseHostDraftYaml(bytes) == draft);

    QTemporaryDir const dir;
    writeSideFile(dir, "hosts/build-box.yml", bytes);
    auto const config = loadFromYaml(dir, "default_profile: main\n"sv);
    REQUIRE(config.hosts.value().contains("build-box"));
    auto const& host = config.hosts.value().at("build-box");
    CHECK(host.tokenFile == vtpty::Process::homeDirectory() / "build-box.token");
    CHECK(host.profile == "remote");
}
