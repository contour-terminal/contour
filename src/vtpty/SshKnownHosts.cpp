// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshKnownHosts.hpp>

#include <core/Utf8Path.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;

namespace vtpty
{

namespace
{
    /// @return Whether the file at @p path ends part-way through a line, its last byte no line feed. One
    ///         that is empty, or not there, does not.
    [[nodiscard]] bool endsMidLine(std::filesystem::path const& path)
    {
        auto file = std::ifstream { path, std::ios::binary | std::ios::ate };
        if (!file || file.tellg() <= 0)
            return false;
        file.seekg(-1, std::ios::end);
        return file.get() != '\n';
    }
} // namespace

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

SshKnownHosts::SshKnownHosts(LIBSSH2_SESSION* session):
    _session { session }, _handle { libssh2_knownhost_init(session) }
{
}

SshKnownHosts::~SshKnownHosts()
{
    if (_handle)
        libssh2_knownhost_free(_handle);
}

std::expected<std::vector<SshKnownHosts::SkippedLine>, std::string> SshKnownHosts::readFromFile(
    std::filesystem::path const& path)
{
    // Text, as libssh2 reads it, and a line at a time, as libssh2 does -- but on past a line it cannot
    // parse, where libssh2_knownhost_readfile() stops and forgets the hosts after it.
    auto file = std::ifstream { path };
    if (!file)
        return std::unexpected { std::format("Cannot open \"{}\".", core::pathToUtf8(path)) };
    auto skipped = std::vector<SkippedLine> {};
    auto line = std::string {};
    auto number = std::size_t { 0 };
    while (std::getline(file, line))
    {
        ++number;
        if (libssh2_knownhost_readline(_handle, line.data(), line.size(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) < 0)
            skipped.push_back(SkippedLine { .number = number, .reason = lastError() });
    }
    if (file.bad())
        return std::unexpected { std::format("Cannot read \"{}\".", core::pathToUtf8(path)) };
    return skipped;
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

std::expected<void, std::string> SshKnownHosts::appendToFile(SshHostConfig const& host,
                                                             SshHostkey const& hostkey)
{
    auto const knownhostType = hostkeyTypeToKnownHostType(hostkey.type);
    auto const comment = host.toKnownhostComment();
    auto const typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownhostType;
    libssh2_knownhost* entry = nullptr;
    if (libssh2_knownhost_addc(_handle,
                               host.hostname.c_str(),
                               nullptr /* salt */,
                               hostkey.data.data(),
                               hostkey.data.size(),
                               comment.data(),
                               comment.size(),
                               typeMask,
                               &entry)
        < 0)
        return std::unexpected { lastError() };

    // The host's line, of the size libssh2 allows one.
    auto line = std::array<char, 4096> {};
    auto written = std::size_t {};
    if (libssh2_knownhost_writeline(
            _handle, entry, line.data(), line.size(), &written, LIBSSH2_KNOWNHOST_FILE_OPENSSH)
        < 0)
        return std::unexpected { lastError() };

    // Appended as OpenSSH's add_host_to_hostfile() appends it: after a line feed where the file's last
    // line has none, so that the two do not run together. Text, as libssh2 writes it.
    auto const separator = endsMidLine(host.knownHostsFile) ? "\n"sv : ""sv;
    auto file = std::ofstream { host.knownHostsFile, std::ios::app };
    if (!file)
        return std::unexpected { std::format("Cannot open \"{}\".", core::pathToUtf8(host.knownHostsFile)) };
    file << separator;
    file.write(line.data(), static_cast<std::streamsize>(written));
    file.flush();
    if (!file)
        return std::unexpected { std::format("Cannot write to \"{}\".",
                                             core::pathToUtf8(host.knownHostsFile)) };
    return {};
}

std::string SshKnownHosts::lastError() const
{
    char* message = nullptr;
    auto length = 0;
    libssh2_session_last_error(_session, &message, &length, 0);
    return std::string { std::string_view { message, static_cast<std::size_t>(length) } };
}

} // namespace vtpty
