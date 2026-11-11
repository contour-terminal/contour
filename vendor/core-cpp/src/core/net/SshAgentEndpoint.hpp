// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where an ssh-agent listens. Transport, so it lives in core::net: the agent protocol on top of it
/// is in `core::net_tls` (SshAgent.hpp), which includes this.

#include <core/Environment.hpp>

#include <string>

namespace core::net
{

/// Where this process's agent listens.
/// @param environment The environment to read `SSH_AUTH_SOCK` from (unused on Windows).
/// @return `SSH_AUTH_SOCK` on POSIX, empty when unset; `\\.\pipe\openssh-ssh-agent` on Windows.
[[nodiscard]] std::string defaultSshAgentEndpoint(core::Environment const& environment);

} // namespace core::net
