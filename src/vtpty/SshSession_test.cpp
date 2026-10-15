// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshSession.hpp>
#include <vtpty/test/NonAsciiSample.hpp>

#include <core/Utf8Path.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <system_error>

namespace
{

/// Writes @p contents to @p path, as it is.
void writeFile(std::filesystem::path const& path, std::string_view contents)
{
    auto out = std::ofstream { path, std::ios::binary };
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

} // namespace

TEST_CASE("readSshKeyFiles reads a key pair under a directory beyond ASCII", "[ssh]")
{
    // Read through the paths as they are, for libssh2 to take in memory: its own fopen() reads a narrow
    // name, which Windows spells in the ANSI code page.
    auto const temp = core::testing::ScopedTempDir { "contour-ssh-keys" };
    auto const directory = temp.path() / vtpty::testing::NonAsciiName;
    std::filesystem::create_directory(directory);
    writeFile(directory / "id_ed25519", "private\r\nkey\n");
    writeFile(directory / "id_ed25519.pub", "ssh-ed25519 public\n");

    auto config = vtpty::SshHostConfig {};
    config.privateKeyFile = directory / "id_ed25519";

    SECTION("the private key alone")
    {
        auto const keys = vtpty::readSshKeyFiles(config);
        REQUIRE(keys.has_value());
        CHECK(keys->privateKey == "private\r\nkey\n");
        CHECK(keys->publicKey.empty());
    }

    SECTION("with its public key")
    {
        config.publicKeyFile = directory / "id_ed25519.pub";
        auto const keys = vtpty::readSshKeyFiles(config);
        REQUIRE(keys.has_value());
        CHECK(keys->privateKey == "private\r\nkey\n");
        CHECK(keys->publicKey == "ssh-ed25519 public\n");
    }

    SECTION("a key file that is not there")
    {
        config.publicKeyFile = directory / "missing.pub";
        auto const keys = vtpty::readSshKeyFiles(config);
        REQUIRE_FALSE(keys.has_value());
        CHECK(keys.error() == std::errc::no_such_file_or_directory);
    }
}

TEST_CASE("SshHostConfig::toString names each key file under its own label", "[ssh]")
{
    auto config = vtpty::SshHostConfig {};
    config.hostname = "example.invalid";
    config.username = "user";
    config.privateKeyFile = "id_ed25519";
    config.publicKeyFile = "id_ed25519.pub";
    config.knownHostsFile = "known_hosts";
    CHECK(config.toString()
          == "user@example.invalid:22, private key: id_ed25519, public key: id_ed25519.pub, "
             "known hosts: known_hosts, ForwardAgent: No");
}

TEST_CASE("SshHostConfig spells its files in UTF-8", "[ssh]")
{
    // path::string() spelled them in the ANSI code page on Windows, and threw for a name beyond it.
    auto const spelled = std::format("/home/{}/.ssh", vtpty::testing::NonAsciiNameUtf8);
    auto config = vtpty::SshHostConfig {};
    config.hostname = "example.invalid";
    config.privateKeyFile = core::pathFromTrustedUtf8(spelled + "/id_ed25519");
    config.knownHostsFile = core::pathFromTrustedUtf8(spelled + "/known_hosts");

    CHECK(config.toString().contains(std::format("private key: {}/id_ed25519", spelled)));
    CHECK(config.toString().contains(std::format("known hosts: {}/known_hosts", spelled)));
    CHECK(config.toConfigString().contains(std::format("IdentityFile {}/id_ed25519\n", spelled)));
    CHECK(config.toConfigString().contains(std::format("KnownHostsFile {}/known_hosts\n", spelled)));
}

TEST_CASE("loadSshConfig reads an IdentityFile beyond ASCII as UTF-8", "[ssh]")
{
    // The file is UTF-8. Assigned to a path as a narrow string, Windows read it in the ANSI code page.
    auto const temp = core::testing::ScopedTempDir { "contour-ssh-config" };
    auto const identity = std::format("/home/{}/.ssh/id_ed25519", vtpty::testing::NonAsciiNameUtf8);
    writeFile(temp / "config",
              std::format("Host work\n    HostName example.invalid\n    IdentityFile {}\n", identity));

    auto const configs = vtpty::loadSshConfig(temp / "config");
    REQUIRE(configs.has_value());
    REQUIRE(configs->contains("work"));
    CHECK(configs->at("work").privateKeyFile == core::pathFromTrustedUtf8(identity));
}
