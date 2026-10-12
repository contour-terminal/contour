// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshKnownHosts.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

namespace vtpty
{

int hostkeyTypeToKnownHostType(int type) noexcept
{
    switch (type)
    {
#ifdef LIBSSH2_HOSTKEY_TYPE_RSA
        case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_DSS
        case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_384
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_521
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
        case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
#endif
        default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

SshKnownHosts::SshKnownHosts(LIBSSH2_SESSION* session): _handle { libssh2_knownhost_init(session) }
{
}

SshKnownHosts::~SshKnownHosts()
{
    if (_handle)
        libssh2_knownhost_free(_handle);
}

int SshKnownHosts::readFromFile(std::filesystem::path const& path)
{
    // Text, as libssh2 reads it, and a line at a time, as libssh2 does.
    auto file = std::ifstream { path };
    if (!file)
        return LIBSSH2_ERROR_FILE;
    auto line = std::string {};
    while (std::getline(file, line))
        if (auto const rc =
                libssh2_knownhost_readline(_handle, line.data(), line.size(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
            rc < 0)
            return rc;
    return LIBSSH2_ERROR_NONE;
}

SshKnownHosts::Check SshKnownHosts::checkHostkey(SshHostConfig const& host, SshHostkey const& hostkey) const
{
    libssh2_knownhost* knownHost = nullptr;
    auto const knownhostType = hostkeyTypeToKnownHostType(hostkey.type);
    auto const rc =
        libssh2_knownhost_checkp(_handle,
                                 host.hostname.c_str(),
                                 host.port,
                                 hostkey.data.data(),
                                 hostkey.data.size(),
                                 LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownhostType,
                                 &knownHost);
    switch (rc)
    {
        case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        case LIBSSH2_KNOWNHOST_CHECK_MISMATCH: return { .status = rc, .key = knownHost->key };
        default: return { .status = rc, .key = {} };
    }
}

int SshKnownHosts::addToFile(SshHostConfig const& host, SshHostkey const& hostkey) const
{
    auto const knownhostType = hostkeyTypeToKnownHostType(hostkey.type);
    auto const comment = host.toKnownhostComment();
    auto const typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownhostType;
    libssh2_knownhost_addc(_handle,
                           host.hostname.c_str(),
                           nullptr /* salt */,
                           hostkey.data.data(),
                           hostkey.data.size(),
                           comment.data(),
                           comment.size(),
                           typeMask,
                           nullptr /* store */);

    // Text, as libssh2 writes it, each host a line of the size libssh2 allows one.
    auto file = std::ofstream { host.knownHostsFile };
    if (!file)
        return LIBSSH2_ERROR_FILE;
    auto line = std::array<char, 4096> {};
    libssh2_knownhost* previous = nullptr;
    libssh2_knownhost* entry = nullptr;
    while (true)
    {
        auto const found = libssh2_knownhost_get(_handle, &entry, previous);
        if (found < 0)
            return found;
        if (found > 0) // past the last host
            break;
        auto written = std::size_t {};
        if (auto const rc = libssh2_knownhost_writeline(
                _handle, entry, line.data(), line.size(), &written, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
            rc < 0)
            return rc;
        file.write(line.data(), static_cast<std::streamsize>(written));
        previous = entry;
    }
    file.flush();
    return file.good() ? LIBSSH2_ERROR_NONE : LIBSSH2_ERROR_FILE;
}

} // namespace vtpty
