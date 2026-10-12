// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshKnownHosts.hpp>
#include <vtpty/SshSession.hpp>
#include <vtpty/test/NonAsciiSample.hpp>

#include <core/Base64.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
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

} // namespace

TEST_CASE("SshKnownHosts reads and writes a known_hosts file under a directory beyond ASCII", "[ssh]")
{
    // libssh2's own readfile() and writefile() open the file with fopen(), which takes a narrow name that
    // Windows reads in the ANSI code page.
    auto const temp = core::testing::ScopedTempDir { "contour-known-hosts" };
    auto const directory = temp.path() / vtpty::testing::NonAsciiName;
    std::filesystem::create_directory(directory);

    auto const known = vtpty::SshHostkey { .type = LIBSSH2_HOSTKEY_TYPE_ED25519, .data = ed25519Key('k') };
    auto host = vtpty::SshHostConfig {};
    host.hostname = "known.example";
    host.knownHostsFile = directory / "known_hosts";
    std::ofstream { host.knownHostsFile }
        << "known.example ssh-ed25519 "
        << core::base64::encode(std::string { known.data.begin(), known.data.end() }) << '\n';

    auto const session = OfflineSession {};
    REQUIRE(session.get() != nullptr);

    SECTION("a host it names is known")
    {
        auto hosts = vtpty::SshKnownHosts { session.get() };
        REQUIRE(hosts.readFromFile(host.knownHostsFile) == LIBSSH2_ERROR_NONE);
        CHECK(hosts.checkHostkey(host, known).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
    }

    SECTION("a host added is written beside the ones it had")
    {
        auto added = host;
        added.hostname = "added.example";
        auto const key = vtpty::SshHostkey { .type = LIBSSH2_HOSTKEY_TYPE_ED25519, .data = ed25519Key('a') };
        {
            auto hosts = vtpty::SshKnownHosts { session.get() };
            REQUIRE(hosts.readFromFile(host.knownHostsFile) == LIBSSH2_ERROR_NONE);
            REQUIRE(hosts.addToFile(added, key) == LIBSSH2_ERROR_NONE);
        }
        auto reread = vtpty::SshKnownHosts { session.get() };
        REQUIRE(reread.readFromFile(host.knownHostsFile) == LIBSSH2_ERROR_NONE);
        CHECK(reread.checkHostkey(host, known).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
        CHECK(reread.checkHostkey(added, key).status == LIBSSH2_KNOWNHOST_CHECK_MATCH);
    }

    SECTION("a file that is not there")
    {
        auto hosts = vtpty::SshKnownHosts { session.get() };
        CHECK(hosts.readFromFile(directory / "missing") == LIBSSH2_ERROR_FILE);
    }
}
