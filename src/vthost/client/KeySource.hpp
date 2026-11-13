// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a client's SSH key proofs come from: the user's agent, or a key file signed with `ssh-keygen`
/// (which asks for its passphrase through the askpass broker). A daemon's AuthChallenge is answered with one
/// candidate after another, so that an agent holding several keys finds the one the daemon authorizes.

#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/SshAgent.hpp>
#include <core/net/SshKeys.hpp>
#include <core/platform/FileSystem.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vthost/Askpass.hpp>
#include <vthost/SshTransport.hpp>
#include <vthost/StdioChild.hpp>
#include <vthost/proto/Pdu.hpp>

namespace core::net
{
class EventLoop;
}

namespace vthost::client
{

/// The keys a client may prove itself with, and the signatures that prove it.
class IClientKeySource
{
  public:
    virtual ~IClientKeySource() = default;

    /// @return The keys to offer, in the order to try them, or why there are none to offer.
    [[nodiscard]] virtual core::async::Task<std::expected<std::vector<core::net::SshPublicKey>, std::string>>
    candidates() = 0;

    /// Signs @p message as a key proof: an SSHSIG in the namespace `contour-auth`.
    /// @param key One of the candidates().
    /// @param message What the proof covers (keyAuthMessage()).
    /// @return The armored signature, or why @p key could not sign (the agent refused, the user cancelled a
    ///         passphrase prompt).
    [[nodiscard]] virtual core::async::Task<std::expected<std::string, std::string>> sign(
        core::net::SshPublicKey const& key, std::span<std::byte const> message) = 0;
};

/// A key source over the user's agent and, for a named key file, `ssh-keygen`.
///
/// Without an identity file the candidates are the agent's keys. With one they are that file's `.pub` alone:
/// the user named the key, so the others are not offered. Such a key is signed by the agent when it holds it,
/// else by `ssh-keygen -Y sign -n contour-auth -f FILE`, the message on its stdin, run with the askpass
/// session's environment so that a passphrase is asked for in a dialog.
/// @param loop The loop the signer's child runs on.
/// @param agent The user's agent; null when there is none. Kept for as long as the source lives.
/// @param spawner Starts `ssh-keygen`.
/// @param broker Answers its passphrase prompts; null runs it without prompts.
/// @param fileSystem Where the `.pub` file is read.
/// @param identityFile The private key file; empty to offer the agent's keys.
/// @param askpassTarget What the askpass dialogs name as the one being logged in to.
/// @return The source.
[[nodiscard]] std::shared_ptr<IClientKeySource> makeClientKeySource(
    core::net::EventLoop& loop,
    std::shared_ptr<core::net::ISshAgent> agent,
    std::shared_ptr<IStdioSpawner> spawner,
    std::shared_ptr<IAskpassBroker> broker,
    core::platform::FileSystem const& fileSystem,
    std::filesystem::path identityFile,
    SshTarget askpassTarget);

/// Makes the key source of one attempt on its loop: a key source holds sockets and children, which belong to
/// the loop an attempt runs on, and a headless verb makes its loop only when it runs.
using ClientKeySourceProvider = std::function<std::shared_ptr<IClientKeySource>(core::net::EventLoop& loop)>;

/// A provider of key sources over the agent at @p agentEndpoint.
/// @param agentEndpoint Where the agent listens (core::net::defaultSshAgentEndpoint()); empty for no agent.
/// @param spawner Starts `ssh-keygen`.
/// @param broker Answers its passphrase prompts; null runs it without prompts.
/// @param fileSystem Where the `.pub` file is read; must outlive the provider's sources.
/// @param identityFile The private key file; empty to offer the agent's keys.
/// @param askpassTarget What the askpass dialogs name as the one being logged in to.
/// @return The provider.
[[nodiscard]] ClientKeySourceProvider makeClientKeySourceProvider(
    std::string const& agentEndpoint,
    std::shared_ptr<IStdioSpawner> const& spawner,
    std::shared_ptr<IAskpassBroker> const& broker,
    core::platform::FileSystem const& fileSystem,
    std::filesystem::path const& identityFile,
    SshTarget const& askpassTarget);

/// @param connection A connection.
/// @return Its TLS session's channel binding (RFC 9266); none for a connection that has no TLS session or
///         cannot export one.
[[nodiscard]] std::optional<std::array<std::byte, 32>> channelBindingOf(core::net::ISocket const& connection);

/// Answers a daemon's AuthChallenges from a key source, one candidate per challenge: a refused key is
/// answered by a fresh challenge, so the next key is tried on it. One per connection.
class KeyAuthenticator
{
  public:
    /// @param source Where the keys come from.
    explicit KeyAuthenticator(std::shared_ptr<IClientKeySource> source);

    /// Proves the next candidate key over @p challenge and this TLS session.
    /// @param challenge What the daemon sent.
    /// @param channelBinding The session's channel binding (channelBindingOf()), which the proof covers;
    ///        none when the connection has no TLS session, which no proof can be bound to.
    /// @return The proof, or nullopt when there is none to give: no candidate is left, MaxKeyAuthAttempts
    ///         proofs were given already, nothing could sign, or there was no channel binding. A key that
    ///         could not sign is passed over and counts toward neither the cap nor the keys offered.
    [[nodiscard]] core::async::Task<std::optional<proto::KeyProof>> proofFor(
        proto::AuthChallenge challenge, std::optional<std::array<std::byte, 32>> channelBinding);

    /// @return How many challenges were answered or declined so far.
    [[nodiscard]] std::size_t challenges() const noexcept { return _challenges; }

    /// @param refusal What the daemon refused with.
    /// @return @p refusal, and, when the daemon asked for a key, which fingerprints were offered and that
    ///         `identity_file` chooses another; @p refusal alone when it did not.
    [[nodiscard]] std::string explainRefusal(std::string refusal) const;

  private:
    std::shared_ptr<IClientKeySource> _source;
    std::optional<std::vector<core::net::SshPublicKey>> _candidates; ///< Asked for at the first challenge.
    std::string _whyNone;                                            ///< Why candidates() offered none.
    std::size_t _next = 0; ///< The candidate the next challenge tries.
    std::size_t _challenges = 0;
    /// The fingerprints of the keys proven so far, one per proof given: their count is what the cap counts.
    std::vector<std::string> _offered;
};

} // namespace vthost::client
