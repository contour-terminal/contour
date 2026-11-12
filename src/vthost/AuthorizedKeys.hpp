// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon's `authorized_keys`: the SSH public keys that may log in to the TCP listener by signing a
/// challenge, each with the control API grants it carries.
///
/// The format is OpenSSH's own, one key per line: `[options] <type> <base64> [comment]`. The only option
/// implemented is `grants="<list>"` (`all`, or a comma list of `read`, `screen`, `input` and `write`; quote
/// it, as a comma ends an unquoted option). A line carrying any other option (`from=`, `command=`,
/// `no-pty`, ...) is skipped and said so, never read with the option ignored: ignoring one would grant
/// wider access than the user wrote. Blank lines and `#` comments are ignored, as are keys of a type the
/// protocol does not take (`ssh-rsa`).

#include <core/net/SshKeys.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtcontrol/Catalog.hpp>

namespace vthost
{

/// How a TCP client may authenticate to the listener.
enum class DaemonAuth : std::uint8_t
{
    Any = 0,  ///< A token, an access-file entry or an authorized key.
    KeysOnly, ///< An authorized key alone; no token is configured.
};

/// @param text The `--auth` value; empty states the default.
/// @return `any` (or empty) or `keys`; otherwise the refusal, naming the choices.
[[nodiscard]] std::expected<DaemonAuth, std::string> parseDaemonAuth(std::string_view text);

/// A key that may log in, and what a connection that proves it holds it is granted.
struct AuthorizedKey
{
    core::net::SshPublicKey key {};                                    ///< The public key.
    vtcontrol::ControlGrants grants = vtcontrol::ControlGrants::all(); ///< What it grants.
};

/// A line of the file that was not used.
struct AuthorizedKeysSkip
{
    std::size_t lineNumber = 0; ///< One-based.
    std::string reason {};      ///< Why, in words for the user.
};

/// The usable keys of an authorized_keys file, and the lines that were not.
struct AuthorizedKeysFile
{
    std::vector<AuthorizedKey> keys {};         ///< In file order.
    std::vector<AuthorizedKeysSkip> skipped {}; ///< In file order.
};

/// @param text An authorized_keys file's contents.
/// @return Its usable keys and the lines skipped with their reasons. A key repeating an earlier line's is
///         skipped too: the first match would win, so a narrower later grant would read as the earlier one.
[[nodiscard]] AuthorizedKeysFile parseAuthorizedKeys(std::string_view text);

/// @param fileSystem What the file is read through.
/// @param path The authorized_keys file.
/// @return Its contents parsed; an absent file is an empty one, not an error. An error only for a file that
///         exists and cannot be read.
[[nodiscard]] std::expected<AuthorizedKeysFile, std::string> readAuthorizedKeys(
    core::platform::FileSystem const& fileSystem, std::filesystem::path const& path);

/// Looks a key up by its wire encoding: the key's identity. Never compare keys with SshPublicKey's own
/// operator==, which compares the comment too.
/// @param keys The authorized keys.
/// @param blob The offered key's blob.
/// @return The entry, or null.
[[nodiscard]] AuthorizedKey const* findAuthorizedKeyByBlob(std::span<AuthorizedKey const> keys,
                                                           std::span<std::byte const> blob);

/// @param keys The authorized keys.
/// @param fingerprint An `SHA256:...` fingerprint, as core::net::sshFingerprintOf() spells it.
/// @return The entry, or null.
[[nodiscard]] AuthorizedKey const* findAuthorizedKeyByFingerprint(std::span<AuthorizedKey const> keys,
                                                                  std::string_view fingerprint);

/// What the user configured for a `--listen-tcp` listener's credentials.
struct ListenerCredentials
{
    DaemonAuth auth = DaemonAuth::Any; ///< `--auth`.
    std::size_t tokenOptionsGiven =
        0;                          ///< How many of `--token`, `--token-file` and `--access-file` were given.
    std::size_t tokens = 0;         ///< Usable tokens: the single one, if non-empty, and the access file's.
    std::size_t authorizedKeys = 0; ///< Usable keys the authorized_keys file holds.
};

/// The part of the startup rule that needs only which options were given, so it can refuse before any file
/// is read: `--auth keys` refuses every kind of token.
/// @param auth `--auth`.
/// @param tokenOptionsGiven How many of `--token`, `--token-file` and `--access-file` were given.
/// @return Nothing, or the refusal, naming the options to remove.
[[nodiscard]] std::expected<void, std::string> checkAuthOptions(DaemonAuth auth,
                                                                std::size_t tokenOptionsGiven);

/// The startup rule: `--listen-tcp` needs a token, an access-file entry or at least one usable authorized
/// key; `--auth keys` needs a key and refuses every kind of token.
/// @param credentials What was configured.
/// @return Nothing, or the refusal, naming what to give.
[[nodiscard]] std::expected<void, std::string> checkListenerCredentials(
    ListenerCredentials const& credentials);

} // namespace vthost
