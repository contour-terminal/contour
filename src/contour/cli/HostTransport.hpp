// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A saved or ad-hoc remote host (`hosts:`, `contour client --host`) mapped onto the transport that reaches
/// its daemon. Qt-free, so a headless verb resolves a saved host exactly as the GUI's `contour client` does.

#include <contour/config/HostUrl.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace core::platform
{
class FileSystem;
}

namespace vthost
{
class IStdioSpawner;
class ITransportFactory;
} // namespace vthost

namespace contour::cli
{

/// How long a TLS host's attach may take: a TCP connect, the TLS handshake and the hello.
inline constexpr auto TlsHostConnectTimeout = std::chrono::milliseconds { 5000 };

/// How much longer an ssh attach waits than its transport's own spawn-to-preamble bound. Without it the two
/// expire together and the attach's generic timeout wins, as its clock starts first; with it, a remote that
/// never answers is reported by the transport, in words naming the host.
inline constexpr auto SshAttachMargin = std::chrono::milliseconds { 5000 };

/// @param transportBound The ssh transport's own spawn-to-preamble bound.
/// @return How long an attach over that transport may take.
[[nodiscard]] constexpr std::chrono::milliseconds sshAttachBudget(
    std::chrono::milliseconds transportBound) noexcept
{
    return transportBound + SshAttachMargin;
}

/// How to reach one remote host.
struct HostTransport
{
    std::shared_ptr<vthost::ITransportFactory>
        factory;       ///< Opens each connection, the first and every reconnect.
    std::string label; ///< The host's name, or the URL for an ad-hoc host.
    /// How long the attach may take: sshAttachBudget(SshConnectTimeout) for ssh, TlsHostConnectTimeout for
    /// tls.
    std::chrono::milliseconds connectTimeout;
};

/// Reads the PEM a TLS connection pins the daemon's certificate to.
/// An empty file is refused like a missing one: an empty CA means trust on first use to the TLS layer, and
/// would silently drop the pinning the user asked for.
/// @param fileSystem Where the file is read.
/// @param path The PEM file.
/// @param origin What named the file, for the refusal: `--tls-ca`, `tls_ca`.
/// @return The PEM, or `cannot read {origin} file '{path}'`.
[[nodiscard]] std::expected<std::string, std::string> readPinnedCa(
    core::platform::FileSystem const& fileSystem, std::filesystem::path const& path, std::string_view origin);

/// Maps a saved host onto its transport. Reads token_file and tls_ca through @p fileSystem.
/// @param label The host's name, or the URL of an ad-hoc host; what the window's chrome shows.
/// @param host The host.
/// @param fileSystem Where token_file and tls_ca are read.
/// @param spawner Starts ssh, for an ssh host.
/// @return The transport, or why the host cannot be reached as configured.
[[nodiscard]] std::expected<HostTransport, std::string> makeHostTransport(
    std::string label,
    config::HostEntry const& host,
    core::platform::FileSystem const& fileSystem,
    std::shared_ptr<vthost::IStdioSpawner> spawner);

} // namespace contour::cli
