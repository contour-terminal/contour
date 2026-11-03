// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Captures a running daemon into a handover image, the last step before it execs its new binary in place.

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

/// Captures everything that crosses the exec: every session quiesced (SessionHost::quiesceAll()), with its
/// PTY's descriptors, its child and its encoded terminal; the model with its ids; the object registry; and
/// what the caller names. Ownership of every descriptor and child stays with the host -- releasing them is
/// for when the exec is certain -- so a capture that fails, or is not followed by an exec, is undone by
/// rollbackCapture() alone. Loop thread, as the last step before the image is written: what is handed to a
/// session afterwards is not in it.
///
/// @pre No client is attached (SessionHost::connectedClients()), and the loop has handled what the pumps
///      posted (SessionHost::pumpEventsInFlight() is zero): the caller closes its clients, quiesces the host
///      once, and turns the loop until it has, so that nothing a client or a pump started changes the host
///      after the capture. Checked.
/// @param host The host to capture.
/// @param listeners The listening sockets the new image keeps serving on.
/// @param leaseFd The descriptor of the daemon image lease; -1 for none.
/// @param daemonInstance Which run of the daemon this is, kept across the exec.
/// @param release The release capturing it.
/// @return The image; or why not: UnsupportedSession before anything stopped, else SessionStarting
///         (retryable) or InFlightTailOverflowed with every session resumed; NotSupported on a platform
///         without an in-place exec.
[[nodiscard]] std::expected<HandoverImage, HandoverError> captureHandover(
    SessionHost& host,
    std::span<InheritedListener const> listeners,
    int leaseFd,
    uint64_t daemonInstance,
    std::string_view release);

/// Undoes a capture that is not followed by an exec: resumes every quiesced session, each delivering what
/// was handed to it meanwhile. Loop thread.
/// @param host The host captureHandover() captured.
void rollbackCapture(SessionHost& host);

} // namespace vthost::handover
