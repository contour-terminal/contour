// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshKnownHosts.hpp>
#include <vtpty/SshSession.hpp>
#include <vtpty/test/NonAsciiSample.hpp>

#include <core/Base64.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <libssh2.h>

namespace
{

/// An ssh-ed25519 host key as a server sends it: its type, then 32 bytes of key, each @p fill.
[[nodiscard]] std::vector<char> ed25519Key(char fill)
{
    auto blob = std::string { "\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20", 19 };
    blob.append(32, fill);
    return { blob.begin(), blob.end() };
}

/// @return The line an OpenSSH known_hosts file names @p key for @p hostname with, its line feed included.
[[nodiscard]] std::string knownHostLine(std::string_view hostname, vtpty::SshHostkey const& key)
{
    return std::format("{} ssh-ed25519 {}\n",
                       hostname,
                       core::base64::encode(std::string { key.data.begin(), key.data.end() }));
}

/// @return What the file at @p path holds, read as text.
[[nodiscard]] std::string contentsOf(std::filesystem::path const& path)
{
    return (std::ostringstream {} << std::ifstream { path }.rdbuf()).str();
}

/// @return The permission bits of the file or directory at @p path.
[[nodiscard]] std::filesystem::perms permissionsOf(std::filesystem::path const& path)
{
    return std::filesystem::status(path).permissions() & std::filesystem::perms::all;
}

/// @return @p hostname, as a host whose known_hosts file is @p knownHostsFile.
[[nodiscard]] vtpty::SshHostConfig hostNamed(std::string hostname, std::filesystem::path knownHostsFile)
{
    auto host = vtpty::SshHostConfig {};
    host.hostname = std::move(hostname);
    host.knownHostsFile = std::move(knownHostsFile);
    return host;
}

/// A libssh2 session that connects nowhere: all known_hosts handling needs of one.
class OfflineSession
{
  public:
    OfflineSession(): _initialized { libssh2_init(0) == 0 }, _session { libssh2_session_init() } {}

    ~OfflineSession()
    {
        if (_session != nullptr)
            libssh2_session_free(_session);
        if (_initialized)
            libssh2_exit();
    }

    OfflineSession(OfflineSession const&) = delete;
    OfflineSession& operator=(OfflineSession const&) = delete;
    OfflineSession(OfflineSession&&) = delete;
    OfflineSession& operator=(OfflineSession&&) = delete;

    [[nodiscard]] LIBSSH2_SESSION* get() const noexcept { return _session; }

  private:
    bool _initialized;
    LIBSSH2_SESSION* _session;
};

/// A line of a known_hosts file that libssh2 cannot parse: a host with its key cut off, as an interrupted
/// write leaves one.
constexpr auto TruncatedLine = std::string_view { "truncated.example ssh-ed25519\n" };

} // namespace

TEST_CASE("SshKnownHosts reads and writes a known_hosts file under a directory beyond ASCII", "[ssh]")
{
    // libssh2's own readfile() and writefile() open the file with fopen(), which takes a narrow name that
    // Windows reads in the ANSI code page.
    auto const temp = core::testing::ScopedTempDir { "contour-known-hosts" };
    auto const directory = temp.path() / vtpty::testing::NonAsciiName;
    std::filesystem::create_directory(directory);

    auto const known = vtpty::SshHostkey { .type = LIBSSH2_HOSTKEY_TYPE_ED25519, .data = ed25519Key('k') };
    auto const host = hostNamed("known.example", directory / "known_hosts");
    std::ofstream { host.knownHostsFile } << knownHostLine(host.hostname, known);

    auto const session = OfflineSession {};
    REQUIRE(session.get() != nullptr);

    SECTION("a host it names is known")
    {
        auto hosts = vtpty::SshKnownHosts { session.get() };
        REQUIRE(hosts.readFromFile(host.knownHostsFile).has_value());
        CHECK(hosts.checkHostkey(host, known).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
    }

    SECTION("a host added is written beside the ones it had")
    {
        auto const added = hostNamed("added.example", host.knownHostsFile);
        auto const key = vtpty::SshHostkey { .type = LIBSSH2_HOSTKEY_TYPE_ED25519, .data = ed25519Key('a') };
        REQUIRE(vtpty::SshKnownHosts { session.get() }.appendToFile(added, key).has_value());

        auto reread = vtpty::SshKnownHosts { session.get() };
        REQUIRE(reread.readFromFile(host.knownHostsFile).has_value());
        CHECK(reread.checkHostkey(host, known).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
        CHECK(reread.checkHostkey(added, key).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
    }

    SECTION("a file that is not there knows no host yet")
    {
        // As OpenSSH reads one: the host is then unknown, and the user is asked whether to trust its key.
        auto hosts = vtpty::SshKnownHosts { session.get() };
        auto const read = hosts.readFromFile(directory / "missing");
        REQUIRE(read.has_value());
        CHECK(read->empty());
        CHECK(hosts.checkHostkey(host, known).status == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND);
    }

    SECTION("a file that is there but cannot be read")
    {
        auto hosts = vtpty::SshKnownHosts { session.get() };
        auto const read = hosts.readFromFile(directory);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().contains("Cannot"));
    }
}

TEST_CASE("SshKnownHosts reads on past a line libssh2 cannot parse", "[ssh]")
{
    // libssh2_knownhost_readfile() stops at such a line, and every host after it went unknown.
    auto const temp = core::testing::ScopedTempDir { "contour-known-hosts" };
    auto const first = vtpty::SshHostkey { .type = LIBSSH2_HOSTKEY_TYPE_ED25519, .data = ed25519Key('f') };
    auto const second = vtpty::SshHostkey { .type = LIBSSH2_HOSTKEY_TYPE_ED25519, .data = ed25519Key('s') };
    auto const firstHost = hostNamed("first.example", temp.path() / "known_hosts");
    auto const secondHost = hostNamed("second.example", firstHost.knownHostsFile);
    std::ofstream { firstHost.knownHostsFile } << knownHostLine(firstHost.hostname, first) << TruncatedLine
                                               << knownHostLine(secondHost.hostname, second);

    auto const session = OfflineSession {};
    REQUIRE(session.get() != nullptr);
    auto hosts = vtpty::SshKnownHosts { session.get() };
    auto const read = hosts.readFromFile(firstHost.knownHostsFile);

    REQUIRE(read.has_value());
    REQUIRE(read->size() == 1);
    CHECK(read->front().number == 2);
    CHECK_FALSE(read->front().reason.empty());
    CHECK(hosts.checkHostkey(firstHost, first).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
    CHECK(hosts.checkHostkey(secondHost, second).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
}

TEST_CASE("SshKnownHosts appends a host, and keeps every line the file had", "[ssh]")
{
    // Accepting a host key used to rewrite the file from the hosts libssh2 had read, which lost the line it
    // could not parse and, with the read stopped there, every line after it.
    auto const temp = core::testing::ScopedTempDir { "contour-known-hosts" };
    auto const first = vtpty::SshHostkey { .type = LIBSSH2_HOSTKEY_TYPE_ED25519, .data = ed25519Key('f') };
    auto const second = vtpty::SshHostkey { .type = LIBSSH2_HOSTKEY_TYPE_ED25519, .data = ed25519Key('s') };
    auto const key = vtpty::SshHostkey { .type = LIBSSH2_HOSTKEY_TYPE_ED25519, .data = ed25519Key('a') };
    auto const firstHost = hostNamed("first.example", temp.path() / "known_hosts");
    auto const secondHost = hostNamed("second.example", firstHost.knownHostsFile);
    auto const added = hostNamed("added.example", firstHost.knownHostsFile);

    auto const session = OfflineSession {};
    REQUIRE(session.get() != nullptr);

    SECTION("past a line libssh2 cannot parse")
    {
        auto const original = knownHostLine(firstHost.hostname, first) + std::string { TruncatedLine }
                              + knownHostLine(secondHost.hostname, second);
        std::ofstream { firstHost.knownHostsFile } << original;

        REQUIRE(vtpty::SshKnownHosts { session.get() }.appendToFile(added, key).has_value());

        auto const contents = contentsOf(firstHost.knownHostsFile);
        CHECK(contents.starts_with(original));
        CHECK(std::ranges::count(contents, '\n') == 4);
        auto reread = vtpty::SshKnownHosts { session.get() };
        REQUIRE(reread.readFromFile(firstHost.knownHostsFile).has_value());
        CHECK(reread.checkHostkey(firstHost, first).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
        CHECK(reread.checkHostkey(secondHost, second).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
        CHECK(reread.checkHostkey(added, key).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
    }

    SECTION("after a last line without a line feed")
    {
        // OpenSSH ends that line first, so that the two do not run together.
        auto original = knownHostLine(firstHost.hostname, first);
        original.pop_back();
        std::ofstream { firstHost.knownHostsFile } << original;

        REQUIRE(vtpty::SshKnownHosts { session.get() }.appendToFile(added, key).has_value());

        CHECK(contentsOf(firstHost.knownHostsFile).starts_with(original + '\n'));
        auto reread = vtpty::SshKnownHosts { session.get() };
        REQUIRE(reread.readFromFile(firstHost.knownHostsFile).has_value());
        CHECK(reread.checkHostkey(firstHost, first).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
        CHECK(reread.checkHostkey(added, key).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
    }

    SECTION("to a file that is not there")
    {
        REQUIRE(vtpty::SshKnownHosts { session.get() }.appendToFile(added, key).has_value());

        auto const contents = contentsOf(firstHost.knownHostsFile);
        CHECK(contents.starts_with("added.example ssh-ed25519 "));
        CHECK(std::ranges::count(contents, '\n') == 1);
    }

    SECTION("to a file whose directory is not there")
    {
        // As OpenSSH makes ~/.ssh before adding the first host: the directory, and the file in it, are the
        // user's alone.
        auto const sshDirectory = temp.path() / ".ssh";
        auto const newcomer = hostNamed("added.example", sshDirectory / "known_hosts");
        REQUIRE(vtpty::SshKnownHosts { session.get() }.appendToFile(newcomer, key).has_value());

        auto reread = vtpty::SshKnownHosts { session.get() };
        REQUIRE(reread.readFromFile(newcomer.knownHostsFile).has_value());
        CHECK(reread.checkHostkey(newcomer, key).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
#ifndef _WIN32
        CHECK(permissionsOf(sshDirectory) == std::filesystem::perms::owner_all);
        CHECK(permissionsOf(newcomer.knownHostsFile)
              == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
#endif
    }

#ifndef _WIN32
    SECTION("to a file that is there, whose permissions it keeps")
    {
        std::ofstream { firstHost.knownHostsFile } << knownHostLine(firstHost.hostname, first);
        auto const shared = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
                            | std::filesystem::perms::group_read | std::filesystem::perms::others_read;
        std::filesystem::permissions(firstHost.knownHostsFile, shared);

        REQUIRE(vtpty::SshKnownHosts { session.get() }.appendToFile(added, key).has_value());

        CHECK(permissionsOf(firstHost.knownHostsFile) == shared);
    }
#endif

    SECTION("to a file two directories below any that is there")
    {
        // OpenSSH makes ~/.ssh, and no directory above it.
        auto const nested = temp.path() / "no-such-directory" / ".ssh";
        auto const unwritable = hostNamed("added.example", nested / "known_hosts");
        auto const appended = vtpty::SshKnownHosts { session.get() }.appendToFile(unwritable, key);
        REQUIRE_FALSE(appended.has_value());
        CHECK(appended.error().contains("no-such-directory"));
        CHECK_FALSE(std::filesystem::exists(nested));
    }
}
