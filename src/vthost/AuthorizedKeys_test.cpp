// SPDX-License-Identifier: Apache-2.0
#include <core/net/SshKeys.hpp>
#include <core/net/testing/SoftwareSecurityKey.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>

#include <vthost/AuthorizedKeys.hpp>

using vtcontrol::ControlGrants;
using vtcontrol::Permission;

namespace
{

constexpr auto Ed25519Line = std::string_view {
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPsS66RFjVCGYtA7Zmou5+V/Rdlp8o7aNAPp/upaw5UU ed@test"
};
constexpr auto EcdsaLine =
    std::string_view { "ecdsa-sha2-nistp256 "
                       "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBFoLtnyGel5FeSlwlf+vcX0mYFog+R/"
                       "955LF08wQIWeVJSlup01Wh/Y/M4JdNO9gKTWDfe4SvaQNxWKFk7daGuE= ec@test" };
constexpr auto RsaLine = std::string_view { "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQC7 rsa@test" };

/// @return A security key's `.pub` line.
std::string securityKeyLine(core::net::SshKeyType type)
{
    auto const key = core::net::testing::SoftwareSecurityKey::generate(type);
    REQUIRE(key.has_value());
    return (*key)->publicKeyLine("sk@test");
}

} // namespace

TEST_CASE("authorized_keys: one usable key per supported type", "[vthost][authorized-keys]")
{
    auto const skEd = securityKeyLine(core::net::SshKeyType::SkEd25519);
    auto const skEc = securityKeyLine(core::net::SshKeyType::SkEcdsaP256);
    auto const file =
        vthost::parseAuthorizedKeys(std::format("{}\n{}\n{}\n{}\n", Ed25519Line, EcdsaLine, skEd, skEc));
    CHECK(file.skipped.empty());
    REQUIRE(file.keys.size() == 4);
    CHECK(file.keys[0].key.type == core::net::SshKeyType::Ed25519);
    CHECK(file.keys[1].key.type == core::net::SshKeyType::EcdsaP256);
    CHECK(file.keys[2].key.type == core::net::SshKeyType::SkEd25519);
    CHECK(file.keys[3].key.type == core::net::SshKeyType::SkEcdsaP256);
    // No option states no narrowing.
    for (auto const& key: file.keys)
        CHECK(key.grants == ControlGrants::all());
}

TEST_CASE("authorized_keys: lines that are not used are skipped with their number and reason",
          "[vthost][authorized-keys]")
{
    SECTION("a quoted grants option narrows the key")
    {
        auto const file =
            vthost::parseAuthorizedKeys(std::format("grants=\"read,screen\" {}\n", Ed25519Line));
        REQUIRE(file.keys.size() == 1);
        CHECK(file.skipped.empty());
        CHECK(file.keys[0].grants == ControlGrants::of({ Permission::Read, Permission::Screen }));
    }
    SECTION("an unquoted list is split at its comma, and its tail is an unknown option: skipped")
    {
        auto const file = vthost::parseAuthorizedKeys(std::format("grants=read,screen {}\n", Ed25519Line));
        CHECK(file.keys.empty());
        REQUIRE(file.skipped.size() == 1);
        CHECK(file.skipped[0].reason.contains("screen"));
    }
    SECTION("an unknown grant is named")
    {
        auto const file = vthost::parseAuthorizedKeys(std::format("grants=\"fly\" {}\n", Ed25519Line));
        CHECK(file.keys.empty());
        REQUIRE(file.skipped.size() == 1);
        CHECK(file.skipped[0].reason.contains("fly"));
    }
    SECTION("any other option skips the line, naming it, rather than widening the grant")
    {
        auto const file =
            vthost::parseAuthorizedKeys(std::format("# keys\n\nno-pty {}\nfrom=\"10.0.0.1\" {}\n"
                                                    "command=\"ls\",grants=\"read\" {}\n",
                                                    Ed25519Line,
                                                    EcdsaLine,
                                                    Ed25519Line));
        CHECK(file.keys.empty());
        REQUIRE(file.skipped.size() == 3);
        CHECK(file.skipped[0].lineNumber == 3);
        CHECK(file.skipped[0].reason.contains("no-pty"));
        CHECK(file.skipped[1].lineNumber == 4);
        CHECK(file.skipped[1].reason.contains("from"));
        CHECK(file.skipped[2].lineNumber == 5);
        CHECK(file.skipped[2].reason.contains("command"));
    }
    SECTION("an rsa key is skipped as unsupported")
    {
        auto const file = vthost::parseAuthorizedKeys(std::format("{}\n{}\n", RsaLine, Ed25519Line));
        REQUIRE(file.keys.size() == 1);
        REQUIRE(file.skipped.size() == 1);
        CHECK(file.skipped[0].lineNumber == 1);
        CHECK(file.skipped[0].reason.contains("not supported"));
    }
    SECTION("garbage is skipped")
    {
        auto const file = vthost::parseAuthorizedKeys("ssh-ed25519 AAAA!\n");
        CHECK(file.keys.empty());
        CHECK(file.skipped.size() == 1);
    }
    SECTION("comments and blank lines are neither keys nor skips")
    {
        auto const file = vthost::parseAuthorizedKeys("# nothing\n\n   \n");
        CHECK(file.keys.empty());
        CHECK(file.skipped.empty());
    }
    SECTION("a key repeated under another comment is skipped, so the first line's grants stand")
    {
        auto const file =
            vthost::parseAuthorizedKeys(std::format("grants=\"read\" {}\n{}x\n", Ed25519Line, Ed25519Line));
        REQUIRE(file.keys.size() == 1);
        REQUIRE(file.skipped.size() == 1);
        CHECK(file.skipped[0].lineNumber == 2);
        CHECK(file.skipped[0].reason.contains("line 1"));
    }
}

TEST_CASE("authorized_keys: an offered key is found by its blob or fingerprint, not by comment",
          "[vthost][authorized-keys]")
{
    auto const file = vthost::parseAuthorizedKeys(std::format("{}\n{}\n", Ed25519Line, EcdsaLine));
    REQUIRE(file.keys.size() == 2);

    // The same key as the file lists it, under another comment: not operator==, yet the same key.
    auto offered = file.keys[1].key;
    offered.comment = "somebody else's name for it";
    CHECK_FALSE(offered == file.keys[1].key);
    CHECK(vthost::findAuthorizedKeyByBlob(file.keys, offered.blob) == &file.keys[1]);
    CHECK(vthost::findAuthorizedKeyByFingerprint(file.keys, core::net::sshFingerprintOf(offered))
          == &file.keys[1]);

    auto const stranger = core::net::testing::SoftwareSecurityKey::generate(core::net::SshKeyType::SkEd25519);
    REQUIRE(stranger.has_value());
    auto const strangerKey = core::net::parseSshPublicKey((*stranger)->publicKeyLine("x"));
    REQUIRE(strangerKey.has_value());
    CHECK(vthost::findAuthorizedKeyByBlob(file.keys, strangerKey->blob) == nullptr);
    CHECK(vthost::findAuthorizedKeyByFingerprint(file.keys, core::net::sshFingerprintOf(*strangerKey))
          == nullptr);
    CHECK(vthost::findAuthorizedKeyByFingerprint(file.keys, "SHA256:nope") == nullptr);
}

TEST_CASE("authorized_keys: an absent file is an empty one", "[vthost][authorized-keys]")
{
    auto const files =
        core::platform::testing::InMemoryFileSystem { { .path = "/home/u/.config/contour/authorized_keys",
                                                        .content = std::string { Ed25519Line } + "\n" } };
    auto const present = vthost::readAuthorizedKeys(files, "/home/u/.config/contour/authorized_keys");
    REQUIRE(present);
    CHECK(present->keys.size() == 1);

    auto const absent = vthost::readAuthorizedKeys(files, "/home/u/.config/contour/nope");
    REQUIRE(absent);
    CHECK(absent->keys.empty());
    CHECK(absent->skipped.empty());
}

TEST_CASE("--auth names its choices", "[vthost][authorized-keys]")
{
    CHECK(vthost::parseAuthorizedKeys("").keys.empty());
    CHECK(vthost::parseDaemonAuth("").value() == vthost::DaemonAuth::Any);
    CHECK(vthost::parseDaemonAuth("any").value() == vthost::DaemonAuth::Any);
    CHECK(vthost::parseDaemonAuth("keys").value() == vthost::DaemonAuth::KeysOnly);
    auto const bad = vthost::parseDaemonAuth("key");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().contains("any, keys"));
}

TEST_CASE("the TCP listener's startup rules", "[vthost][authorized-keys]")
{
    using vthost::DaemonAuth;
    using vthost::ListenerCredentials;
    auto const check = [](ListenerCredentials credentials) {
        return vthost::checkListenerCredentials(credentials);
    };

    SECTION("--listen-tcp needs a token, an access-file entry or a usable key")
    {
        auto const none = check({});
        REQUIRE_FALSE(none);
        CHECK(none.error().contains("--token"));
        CHECK(none.error().contains("--authorized-keys"));
        CHECK(check({ .tokens = 1 }));
        CHECK(check({ .tokenOptionsGiven = 1, .tokens = 2 }));
        CHECK(check({ .authorizedKeys = 1 }));
    }
    SECTION("--auth keys needs a key")
    {
        auto const none = check({ .auth = DaemonAuth::KeysOnly });
        REQUIRE_FALSE(none);
        CHECK(none.error().contains("--auth keys"));
        CHECK(none.error().contains("authorized_keys"));
        CHECK(check({ .auth = DaemonAuth::KeysOnly, .authorizedKeys = 2 }));
    }
    SECTION("--auth keys refuses every kind of token")
    {
        auto const refused =
            check({ .auth = DaemonAuth::KeysOnly, .tokenOptionsGiven = 1, .tokens = 1, .authorizedKeys = 1 });
        REQUIRE_FALSE(refused);
        CHECK(refused.error().contains("--token"));
        CHECK(refused.error().contains("--access-file"));
        // An access file given counts, even one that holds no entry.
        CHECK_FALSE(check({ .auth = DaemonAuth::KeysOnly, .tokenOptionsGiven = 1, .authorizedKeys = 1 }));
    }
}
