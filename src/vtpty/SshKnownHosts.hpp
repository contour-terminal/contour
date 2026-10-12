// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/SshSession.hpp>

#include <filesystem>
#include <string>

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

    /// @param session The session whose hosts these are; it must outlive this object.
    explicit SshKnownHosts(LIBSSH2_SESSION* session);
    ~SshKnownHosts();

    SshKnownHosts(SshKnownHosts const&) = delete;
    SshKnownHosts& operator=(SshKnownHosts const&) = delete;
    SshKnownHosts(SshKnownHosts&&) = delete;
    SshKnownHosts& operator=(SshKnownHosts&&) = delete;

    /// Adds every host the OpenSSH known_hosts file @p path names.
    /// @return LIBSSH2_ERROR_NONE; LIBSSH2_ERROR_FILE where the file cannot be read, or libssh2's error for
    ///         the first line it cannot read.
    [[nodiscard]] int readFromFile(std::filesystem::path const& path);

    /// Looks @p hostkey up for @p host among the hosts read.
    [[nodiscard]] Check checkHostkey(SshHostConfig const& host, SshHostkey const& hostkey) const;

    /// Adds @p hostkey for @p host, then writes every host known to @p host's knownHostsFile.
    /// @return LIBSSH2_ERROR_NONE; LIBSSH2_ERROR_FILE where the file cannot be written, or libssh2's error.
    [[nodiscard]] int addToFile(SshHostConfig const& host, SshHostkey const& hostkey) const;

  private:
    LIBSSH2_KNOWNHOSTS* _handle;
};

} // namespace vtpty
