// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `SoftwareSecurityKey` -- a FIDO security key made of software, for the tests of `sk-` key
/// verification.
///
/// `ssh-keygen` makes `sk-` keys and signatures only with a FIDO authenticator attached, which no
/// test machine has. This signs the way `PROTOCOL.u2f` says an authenticator does, with OpenSSL
/// driven by hand, so the verifier is held against an independent signer -- including the
/// signature an honest authenticator makes when the user did not touch it, which is the case that
/// must be refused.

#include <core/net/SshKeys.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::net::testing
{

/// Whether the user touched the authenticator: `PROTOCOL.u2f`'s flag bit 0x01.
enum class UserPresence : std::uint8_t
{
    Absent = 0, ///< The flag is clear.
    Present,    ///< The flag is set.
};

/// One `sk-ssh-ed25519@openssh.com` or `sk-ecdsa-sha2-nistp256@openssh.com` key pair.
class SoftwareSecurityKey
{
  public:
    /// A fresh key pair.
    /// @param type @c SshKeyType::SkEd25519 or @c SshKeyType::SkEcdsaP256.
    /// @param application The application string the key is bound to; OpenSSH uses `ssh:`.
    /// @return The key, or why it could not be made (another type, or an OpenSSL failure).
    [[nodiscard]] static std::expected<std::unique_ptr<SoftwareSecurityKey>, std::string> generate(
        SshKeyType type, std::string application = "ssh:");

    ~SoftwareSecurityKey();
    SoftwareSecurityKey(SoftwareSecurityKey const&) = delete;
    SoftwareSecurityKey& operator=(SoftwareSecurityKey const&) = delete;
    SoftwareSecurityKey(SoftwareSecurityKey&&) = delete;
    SoftwareSecurityKey& operator=(SoftwareSecurityKey&&) = delete;

    /// @param comment The comment to end the line with.
    /// @return The public key as a `.pub` line: `<type> <base64> <comment>`.
    [[nodiscard]] std::string publicKeyLine(std::string_view comment) const;

    /// Signs @p data as an authenticator does: over `SHA-256(application) ‖ flags ‖ counter ‖
    /// SHA-256(data)`.
    /// @param data What to sign, e.g. @c sshsigSignedData.
    /// @param presence Whether to set the user-presence flag.
    /// @param counter The signature counter to report.
    /// @return The signature in SSH encoding (as `SSH2_AGENT_SIGN_RESPONSE` carries it), or why
    ///         OpenSSL could not make it.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> sign(std::span<std::byte const> data,
                                                                          UserPresence presence,
                                                                          std::uint32_t counter) const;

  private:
    struct State;
    explicit SoftwareSecurityKey(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> _state;
};

} // namespace core::net::testing
