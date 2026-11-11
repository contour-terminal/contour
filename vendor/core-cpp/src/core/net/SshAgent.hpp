// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A client for `ssh-agent`: list the identities it holds, and have it sign.
///
/// The agent protocol is `draft-miller-ssh-agent`: a uint32 length, a message type byte and a body,
/// each way. The private keys never leave the agent, which is the point: a caller proves it holds
/// a key by handing the agent the bytes to sign. Each call dials the agent afresh and closes the
/// connection when done, as OpenSSH does, so an agent that restarts between calls costs nothing.
///
/// **An agent must outlive every Task it returns:** the tasks refer to it, and to the connector it
/// was made with, until they finish.
///
/// No OpenSSL is involved: only the framing, and the public-key parsing of SshKeys.hpp.

#include <core/Environment.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SshAgentEndpoint.hpp>
#include <core/net/SshKeys.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace core::net
{

/// Why an agent request failed.
enum class SshAgentError : std::uint8_t
{
    Unavailable = 0, ///< No agent to reach: no endpoint, a refused connect, or the agent hung up.
    Refused,         ///< The agent answered `SSH_AGENT_FAILURE`: no such key, or the user denied it.
    Protocol,        ///< The agent's answer was malformed or not the one the request calls for.
};

/// What a caller asks of an ssh-agent.
class ISshAgent
{
  public:
    ISshAgent() = default;
    virtual ~ISshAgent() = default;

    ISshAgent(ISshAgent const&) = delete;
    ISshAgent& operator=(ISshAgent const&) = delete;
    ISshAgent(ISshAgent&&) = delete;
    ISshAgent& operator=(ISshAgent&&) = delete;

    /// Lists the agent's identities.
    /// @return The keys of a supported type, in the agent's order (others are dropped, a key the
    ///         agent holds that this module cannot use being no failure), or why the agent failed.
    [[nodiscard]] virtual async::Task<std::expected<std::vector<SshPublicKey>, SshAgentError>>
    identities() = 0;

    /// Has the agent sign @p data with @p key, with no flags (the key's native algorithm).
    /// @param key One of the keys @c identities listed.
    /// @param data The bytes to sign.
    /// @return The signature in SSH signature encoding (`string algorithm, string blob`), as
    ///         @c assembleSshsig takes it; @c SshAgentError::Refused if the agent declined.
    [[nodiscard]] virtual async::Task<std::expected<std::vector<std::byte>, SshAgentError>> sign(
        SshPublicKey const& key, std::span<std::byte const> data) = 0;
};

/// Opens one connection to an agent. The seam between the protocol and the transport: production
/// dials a Unix socket or a Windows named pipe, a test hands out one end of a socketpair.
using SshAgentConnector = std::function<async::Task<std::expected<std::unique_ptr<ISocket>, NetError>>()>;

/// An agent reached through @p connector.
/// @param connector Dials the agent; called once per request.
/// @return The client.
[[nodiscard]] std::unique_ptr<ISshAgent> makeSshAgent(SshAgentConnector connector);

/// An agent at @p endpoint.
/// @param loop The loop that drives the connection; must outlive the agent.
/// @param endpoint A Unix socket path (`SSH_AUTH_SOCK`), or on Windows a named pipe such as
///        `\\.\pipe\openssh-ssh-agent`. An empty endpoint is an agent that is always unavailable.
/// @return The client.
[[nodiscard]] std::unique_ptr<ISshAgent> makeSshAgent(EventLoop& loop, std::string endpoint);

} // namespace core::net
