// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Captures a running daemon into a handover image, the last step before it execs its new binary in place
/// (POSIX), or starts its successor (Windows).

#include <vtpty/CrossingHandle.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include <vthost/handover/HandoverCodec.hpp>
#include <vthost/handover/HandoverImage.hpp>

namespace vthost
{
class SessionHost;
}

namespace vthost::handover
{

/// @param host The host.
/// @param id One of its sessions (Require).
/// @return Session @p id as a handover image names it, without what crosses with it (its PTY, its terminal
///         state, its carried input): its identity, profile, stated settings, color preference and
///         interruption, and for a child its PTY keeper holds, the keeper's name for it. Locks no terminal.
///         Loop thread.
[[nodiscard]] HandoverSession sessionStructureOf(SessionHost const& host, vtworkspace::SessionId id);

/// @param host The host.
/// @return The host's structure: the model with its ids, the object registry, the color preference new
///         sessions are built under, the cell size a client stated, and every session as sessionStructureOf()
///         names it. Nothing names the run (daemonInstance, fromRelease, upgradeGeneration), a listener or a
///         lease: the caller does. Locks no terminal. Loop thread.
[[nodiscard]] HandoverImage hostStructureOf(SessionHost const& host);

/// Asks, without stopping anything, whether a capture of @p host would be refused as things stand: what a
/// live upgrade checks before it announces itself, so that a refusal costs no client its connection. A
/// capture asks again, since a session may change in between. Loop thread.
/// @param host The host to ask about.
/// @return Nothing; or SessionStarting for a session still starting (SessionHost::isAnySessionStarting()),
///         UnsupportedSession for one no new image could adopt, ForeignConsoleHost for one on a pseudoconsole
///         no successor could rebuild, InFlightTailOverflowed for one whose escape sequence in flight outgrew
///         what a capture keeps.
[[nodiscard]] std::expected<void, HandoverError> checkCapturable(SessionHost& host);

/// Captures everything that crosses the exec, or to the successor: every session quiesced
/// (SessionHost::quiesceAll()), with its PTY's descriptors or handles, its child and its encoded terminal;
/// the model with its ids; the object registry; and what the caller names. Ownership of every descriptor and
/// child stays with the host -- releasing them is for when the exec is certain -- so a capture that fails, or
/// is not followed by an exec, is undone by rollbackCapture() alone. Loop thread, as the last step before the
/// image is written: what is handed to a session afterwards is not in it.
///
/// @pre No client is attached (SessionHost::connectedClients()), and the loop has handled what the pumps
///      posted (SessionHost::pumpEventsInFlight() is zero): the caller closes its clients, quiesces the host
///      once, and turns the loop until it has, so that nothing a client or a pump started changes the host
///      after the capture. Checked.
/// @param host The host to capture.
/// @param listeners The listening sockets the new image keeps serving on.
/// @param lease The descriptor or handle of the daemon image lease, which the image names; none for none.
/// @param daemonInstance Which run of the daemon this is, kept across the exec.
/// @param release The release capturing it.
/// @return The image; or why not: UnsupportedSession or ForeignConsoleHost before anything stopped, else
///         SessionStarting (retryable) or InFlightTailOverflowed with every session resumed.
[[nodiscard]] std::expected<HandoverImage, HandoverError> captureHandover(
    SessionHost& host,
    std::span<InheritedListener const> listeners,
    vtpty::CrossingHandle lease,
    uint64_t daemonInstance,
    std::string_view release);

/// Undoes a capture that is not followed by an exec: resumes every quiesced session, each delivering what
/// was handed to it meanwhile. Loop thread.
/// @param host The host captureHandover() captured.
void rollbackCapture(SessionHost& host);

} // namespace vthost::handover
