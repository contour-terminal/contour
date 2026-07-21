// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The MSG_IDENTIFY_* handshake a tmux client opens its connection with:
/// a pipelined burst of typed values (flags, TERM, features, tty name, cwd,
/// terminfo entries, the STDIN and STDOUT descriptors via SCM_RIGHTS, the
/// client pid, the environment) terminated by MSG_IDENTIFY_DONE.
///
/// One table drives the whole phase: a row names the payload shape and how
/// the value lands in IdentifyState — adding a message is adding a row.
/// Unknown identify types are ignored (as the real server does); payload-
/// shape violations reject the peer.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <muxserver/imsg/ImsgCodec.h>

namespace muxserver::imsg
{

/// tmux client flags (tmux.h CLIENT_*) that gate acceptance.
inline constexpr uint64_t ClientControl = 0x2000;
inline constexpr uint64_t ClientControlControl = 0x4000;

/// Everything a client stated about itself before MSG_IDENTIFY_DONE.
struct IdentifyState
{
    uint64_t flags = 0;
    int32_t features = 0;
    std::string term;
    std::string ttyName;
    std::string cwd;
    std::vector<std::string> terminfo;
    std::vector<std::string> environment;
    int64_t clientPid = 0;
    UniqueFd stdinFd;
    UniqueFd stdoutFd;
    bool done = false;
};

/// Applies one identify-phase frame to @p state.
/// @return Nothing on success (including ignored unknown types); an error on
///         a payload-shape violation or an identify message after DONE.
[[nodiscard]] std::expected<void, ImsgError> applyIdentify(IdentifyState& state, ImsgFrame frame);

/// Why a completed handshake is not serveable.
enum class RejectReason : uint8_t
{
    NotControlClient, ///< No CLIENT_CONTROL: a full terminal client (-C missing).
    ControlControl,   ///< CLIENT_CONTROLCONTROL (-CC) is not supported.
    MissingStdioFds,  ///< STDIN/STDOUT descriptors did not arrive.
};

/// The acceptance policy over a DONE state: control-mode clients with both
/// stdio descriptors are serveable; everything else names its reason.
[[nodiscard]] std::expected<void, RejectReason> checkAcceptance(IdentifyState const& state);

/// @return A human-readable rejection message for MSG_EXIT.
[[nodiscard]] std::string rejectMessage(RejectReason reason);

} // namespace muxserver::imsg
