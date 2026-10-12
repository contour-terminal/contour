// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshSession.hpp>
#include <vtpty/test/NonAsciiSample.hpp>

#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
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
