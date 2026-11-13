// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A saved or ad-hoc remote host (`hosts:`, `contour client --host`) mapped onto the transport that reaches
/// its daemon. Qt-free, so a headless verb resolves a saved host exactly as the GUI's `contour client` does.

#include <contour/config/HostUrl.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <vthost/AttachBudget.hpp>

namespace core::platform
{
class FileSystem;
}

namespace vthost
{
class IAskpassBroker;
class IStdioSpawner;
class IPinPrompt;
class ITransportFactory;
class KnownDaemons;
class SshVersionProbe;
} // namespace vthost

namespace contour::cli
{

/// How long a TLS host's attach may take: a TCP connect, the TLS handshake and the hello.
inline constexpr auto TlsHostConnectTimeout = std::chrono::milliseconds { 5000 };

/// How much longer an ssh attach waits than its transport's own spawn-to-preamble bound. Without it the two
/// expire together and the attach's generic timeout wins, as its clock starts first; with it, a remote that
/// never answers is reported by the transport, in words naming the host.
inline constexpr auto SshAttachMargin = std::chrono::milliseconds { 5000 };

/// Whether an ssh attach's prompts are answered by Contour.
enum class SshPrompts : std::uint8_t
{
    Batch = 0, ///< No: a login that needs a prompt fails.
    Askpass,   ///< Yes: the transport's own deadline stands still while a prompt is open.
};

/// @param transportBound The ssh transport's own spawn-to-preamble bound.
/// @param prompts Whether Contour answers the host's prompts.
/// @return How long an attach over that transport may take. When Contour answers the prompts there is no
///         outer clock for the connect, as the transport's own deadline pauses while a prompt is open and
///         is the one that counts; the handshake after the preamble keeps @p transportBound.
[[nodiscard]] constexpr vthost::AttachBudget sshAttachBudget(std::chrono::milliseconds transportBound,
                                                             SshPrompts prompts) noexcept
{
    if (prompts == SshPrompts::Askpass)
        return vthost::AttachBudget::promptsOpen(transportBound);
    return vthost::AttachBudget { transportBound + SshAttachMargin };
}

/// What an ssh host needs to have its prompts answered: both are shared by every host of the process.
struct SshPrompting
{
    std::shared_ptr<vthost::IAskpassBroker> broker; ///< Answers the prompts; nullptr: batch mode only.
    std::shared_ptr<vthost::SshVersionProbe> probe; ///< The process's one `ssh -V` probe.
};

/// What a `tls://` host is trusted and signed in with beyond its own entry: shared by every host of the
/// process.
struct TlsTrust
{
    /// Where pinned daemons are kept and looked up; borrowed, and must outlive the transports. Null refuses
    /// a `tls://` host, which could then be verified by nothing.
    vthost::KnownDaemons* knownDaemons = nullptr;
    /// Asks the user to pin a daemon met for the first time; null where there is no one to ask, and a first
    /// contact then needs @ref daemonFingerprint.
    std::shared_ptr<vthost::IPinPrompt> pinPrompt = nullptr;
    /// `--daemon-fingerprint`: what a daemon without a CA must present on first contact; empty for none.
    std::string daemonFingerprint {};
    /// Where the user's SSH agent listens (core::net::defaultSshAgentEndpoint()); empty for no agent.
    std::string sshAgentEndpoint {};
};

/// How to reach one remote host.
struct HostTransport
{
    std::shared_ptr<vthost::ITransportFactory>
        factory;       ///< Opens each connection, the first and every reconnect.
    std::string label; ///< The host's name, or the URL for an ad-hoc host.
    /// How long the attach may take: sshAttachBudget(SshConnectTimeout, ...) for ssh, TlsHostConnectTimeout
    /// for tls.
    vthost::AttachBudget attach;
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
/// @param prompting Who answers an ssh host's prompts, and the shared version probe.
/// @param tls How a tls host's daemon is trusted, and what signs in to it.
/// @return The transport, or why the host cannot be reached as configured.
[[nodiscard]] std::expected<HostTransport, std::string> makeHostTransport(
    std::string label,
    config::HostEntry const& host,
    core::platform::FileSystem const& fileSystem,
    std::shared_ptr<vthost::IStdioSpawner> spawner,
    SshPrompting prompting,
    TlsTrust tls);

} // namespace contour::cli
