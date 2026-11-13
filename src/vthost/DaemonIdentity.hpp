// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon's persistent TLS identity: the self-signed certificate and key `--listen-tcp` presents. Kept
/// in a private file under the state directory so that its fingerprint survives a restart -- a client that
/// pinned it (`known_daemons`) goes on trusting the same daemon, and a changed one is refused, never
/// click-through accepted.

#include <core/platform/FileSystem.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <vthost/FilePrivacy.hpp>
#include <vthost/handover/HandoverImage.hpp>

namespace vthost
{

/// @param stateDirectory The daemon's state directory (`daemonStateDirectory(...)`, StatePath.hpp).
/// @param label The daemon's label.
/// @return `<stateDirectory>/tls/<label>.pem`; nullopt for a label that cannot name a file.
[[nodiscard]] std::optional<std::filesystem::path> daemonIdentityPath(
    std::filesystem::path const& stateDirectory, std::string_view label);

/// Reads the identity at @p file, or makes one and keeps it there: a self-signed certificate named
/// `contour-daemon`, written certificate then key to a file made private (empty, mode 0600) before the key
/// is put in it, and moved into place whole.
/// @param fileSystem What the file is made and written through.
/// @param privacy What makes it, and its directory, the user's alone, and reads an existing one only when it
///        is: owned by this user and closed to everyone else (IFilePrivacy::readPrivateFile()).
/// @param file Where the identity lives.
/// @return The identity, or why it cannot be had. A file that exists but holds no usable certificate and
///         key is an error and is never overwritten: replacing it would change the daemon's fingerprint
///         behind the back of every client that pinned it.
[[nodiscard]] std::expected<handover::TlsIdentity, std::string> loadOrCreateDaemonIdentity(
    core::platform::FileSystem const& fileSystem,
    IFilePrivacy const& privacy,
    std::filesystem::path const& file);

/// @param identity A certificate and key.
/// @return The certificate's fingerprint as OpenSSH spells one, `SHA256:<unpadded base64>`, or why the
///         certificate cannot be read.
[[nodiscard]] std::expected<std::string, std::string> daemonFingerprint(
    handover::TlsIdentity const& identity);

} // namespace vthost
