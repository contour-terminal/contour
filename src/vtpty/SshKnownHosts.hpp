// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/SshSession.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include <libssh2.h>

namespace vtpty
{

/// @return The known_hosts key type of the session host key type @p type (a LIBSSH2_HOSTKEY_TYPE_*).
[[nodiscard]] int hostkeyTypeToKnownHostType(int type) noexcept;

/// The known_hosts file of one SSH session, as libssh2 holds it.
///
/// The file itself is read and written here, through its path as it is: libssh2's own
/// libssh2_knownhost_readfile() and libssh2_knownhost_writefile() open it with fopen(), which takes a
/// narrow name, and Windows reads that in the ANSI code page.
class SshKnownHosts
{
  public:
    /// What checkHostkey() found.
    struct Check
    {
        int status;      ///< A LIBSSH2_KNOWNHOST_CHECK_*.
        std::string key; ///< The key known for the host, base64; empty where none is.
    };

    /// A line of a known_hosts file that libssh2 cannot parse, which readFromFile() passed over.
    struct SkippedLine
    {
        std::size_t number; ///< Its line number, counting from 1.
        std::string reason; ///< Why libssh2 refused it, in its own words.
    };

    /// @param session The session whose hosts these are; it must outlive this object.
    explicit SshKnownHosts(LIBSSH2_SESSION* session);
    ~SshKnownHosts();

    SshKnownHosts(SshKnownHosts const&) = delete;
    SshKnownHosts& operator=(SshKnownHosts const&) = delete;
    SshKnownHosts(SshKnownHosts&&) = delete;
    SshKnownHosts& operator=(SshKnownHosts&&) = delete;

    /// Adds every host the OpenSSH known_hosts file @p path names. A line libssh2 cannot parse is passed
    /// over, so the hosts on the lines after it are still known. A file that is not there names no host,
    /// as OpenSSH reads one.
    /// @return The lines passed over, or why a file that is there could not be read.
    [[nodiscard]] std::expected<std::vector<SkippedLine>, std::string> readFromFile(
        std::filesystem::path const& path);

    /// Looks @p hostkey up for @p host among the hosts read.
    [[nodiscard]] Check checkHostkey(SshHostConfig const& host, SshHostkey const& hostkey) const;

    /// Adds @p hostkey for @p host, and appends its line to @p host's knownHostsFile, as OpenSSH does: the
    /// file is never rewritten, so every line it had is kept, those libssh2 cannot parse among them. Where
    /// the file is not there it is made, the user's alone (0600), and so is the directory it is in (0700)
    /// where that is not there either -- that one, and none above it, as OpenSSH makes ~/.ssh.
    /// @return Nothing once the line is written, or why it was not.
    [[nodiscard]] std::expected<void, std::string> appendToFile(SshHostConfig const& host,
                                                                SshHostkey const& hostkey);

  private:
    /// @return What libssh2 last reported going wrong in the session.
    [[nodiscard]] std::string lastError() const;

    LIBSSH2_SESSION* _session;
    LIBSSH2_KNOWNHOSTS* _handle;
};

} // namespace vtpty
