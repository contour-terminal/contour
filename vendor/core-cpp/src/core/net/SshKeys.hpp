// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// OpenSSH public keys, `authorized_keys` lines, and SSHSIG signature verification.
///
/// A daemon that authenticates a user by a key in `authorized_keys` uses these to check that the
/// user holds it: the client signs a challenge with `ssh-agent` (or `ssh-keygen -Y sign`), and
/// @c verifySshSignature checks the signature against the listed key. Everything here parses bytes
/// an attacker chose, so every malformed input answers its error enum; nothing throws on bad input.
///
/// The formats are OpenSSH's own: `PROTOCOL.sshsig` for the signature envelope and what it signs,
/// `PROTOCOL.u2f` for the security-key (`sk-`) signatures, RFC 8709 for Ed25519 and RFC 5656 for
/// ECDSA. Four key types are supported; RSA and DSA are not.
///
/// In `core::net_tls`, because verification is OpenSSL's; no OpenSSL type appears here.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::net
{

/// The OpenSSH public key algorithms this module accepts.
enum class SshKeyType : std::uint8_t
{
    Ed25519 = 0, ///< `ssh-ed25519`
    EcdsaP256,   ///< `ecdsa-sha2-nistp256`
    SkEd25519,   ///< `sk-ssh-ed25519@openssh.com`, a FIDO security key
    SkEcdsaP256, ///< `sk-ecdsa-sha2-nistp256@openssh.com`, a FIDO security key
};

/// The name OpenSSH spells @p type with, at the front of a `.pub` line and inside its blob.
/// @param type The key type.
/// @return E.g. `ssh-ed25519` or `sk-ecdsa-sha2-nistp256@openssh.com`.
[[nodiscard]] std::string_view sshKeyTypeName(SshKeyType type) noexcept;

/// An OpenSSH public key.
struct SshPublicKey
{
    /// The algorithm, agreeing with the type name at the front of @c blob.
    SshKeyType type = SshKeyType::Ed25519;
    /// The wire encoding: the base64-decoded middle field of a `.pub` line.
    std::vector<std::byte> blob;
    /// The comment after the key, or empty.
    std::string comment;

    /// Compares every member, @c comment included: two entries for one key that differ only in
    /// their comment are not equal. To ask whether two entries are the same key, compare @c blob
    /// or @c sshFingerprintOf.
    auto operator<=>(SshPublicKey const&) const = default;
};

/// Why a public key or an `authorized_keys` line was refused.
enum class SshKeyError : std::uint8_t
{
    Malformed = 0,   ///< Not a well-formed key: bad base64, a bad field, a short or trailing byte.
    UnsupportedType, ///< A key type outside @c SshKeyType, such as `ssh-rsa`.
};

/// Parses a public key line, `<type> <base64> [comment]`, as `ssh-keygen` writes a `.pub` file.
/// @param line One line; surrounding whitespace and a trailing newline are ignored.
/// @return The key, or why it is refused. The blob is checked field by field, and an ECDSA point
///         must lie on the curve.
[[nodiscard]] std::expected<SshPublicKey, SshKeyError> parseSshPublicKey(std::string_view line);

/// Parses the wire encoding of a public key, as an agent or a certificate carries it.
/// @param blob The key blob: `string type` and the fields that type defines, nothing after.
/// @return The key, with an empty comment; @c SshKeyError::UnsupportedType for a type outside
///         @c SshKeyType, @c SshKeyError::Malformed for a short, oversized or invalid one.
[[nodiscard]] std::expected<SshPublicKey, SshKeyError> parseSshPublicKeyBlob(std::span<std::byte const> blob);

/// One key line of an `authorized_keys` file.
struct AuthorizedKeyLine
{
    /// The options in front of the key, each as written (`grants="read,screen"`, `no-pty`).
    std::vector<std::string> options;
    /// The key.
    SshPublicKey key;
};

/// Parses one line of an `authorized_keys` file: `[options] <type> <base64> [comment]`.
///
/// The options field is a comma-separated list that ends at the first whitespace outside double
/// quotes; a comma inside double quotes does not split it, and `\"` inside quotes is a quote.
/// @param line One line, without or with its newline.
/// @return @c std::nullopt for a blank line or a `#` comment, the key line, or why it is refused.
[[nodiscard]] std::expected<std::optional<AuthorizedKeyLine>, SshKeyError> parseAuthorizedKeysLine(
    std::string_view line);

/// The fingerprint `ssh-keygen -l` prints for @p key.
/// @param key The key.
/// @return `SHA256:` and the unpadded base64 of the SHA-256 of its blob.
[[nodiscard]] std::string sshFingerprintOf(SshPublicKey const& key);

/// Why an SSHSIG signature was refused.
enum class SshSigError : std::uint8_t
{
    Malformed = 0,   ///< Not a well-formed SSHSIG, or its signature does not suit the key's algorithm.
    WrongNamespace,  ///< Signed for another purpose than the one asked for.
    KeyMismatch,     ///< Signed by another key than the expected one.
    BadSignature,    ///< The signature does not verify.
    UserNotPresent,  ///< A security-key signature made without the user-presence flag.
    UnsupportedHash, ///< A message hash other than `sha512`.
};

/// Verifies an SSHSIG signature (`ssh-keygen -Y sign`) of @p message by @p key.
///
/// The signature must embed exactly @p key (compared over the whole blob), be made for
/// @p nameSpace, hash the message with `sha512`, leave the reserved field empty, and verify. A security-key
/// signature must also carry the user-presence flag.
/// @param key The key the signature must be made by.
/// @param nameSpace The purpose the signature must be made for, e.g. `contour-auth`.
/// @param message The signed message.
/// @param sshsig The signature, armored (`-----BEGIN SSH SIGNATURE-----`) or as the raw blob.
/// @return Nothing, or why the signature is refused.
[[nodiscard]] std::expected<void, SshSigError> verifySshSignature(SshPublicKey const& key,
                                                                  std::string_view nameSpace,
                                                                  std::span<std::byte const> message,
                                                                  std::string_view sshsig);

/// The bytes an SSHSIG signature signs, with the `sha512` message hash: what to hand an agent.
/// @param nameSpace The purpose, e.g. `contour-auth`.
/// @param message The message.
/// @return `"SSHSIG" ‖ string namespace ‖ string "" ‖ string "sha512" ‖ string SHA-512(message)`.
[[nodiscard]] std::vector<std::byte> sshsigSignedData(std::string_view nameSpace,
                                                      std::span<std::byte const> message);

/// The raw SSHSIG blob wrapping a signature an agent made over @c sshsigSignedData.
/// @param key The key that signed.
/// @param nameSpace The purpose it was signed for.
/// @param agentSignature The signature from `SSH2_AGENT_SIGN_RESPONSE`, in SSH signature encoding.
/// @return The blob, as @c verifySshSignature accepts it unarmored (hash `sha512`).
[[nodiscard]] std::string assembleSshsig(SshPublicKey const& key,
                                         std::string_view nameSpace,
                                         std::span<std::byte const> agentSignature);

} // namespace core::net
