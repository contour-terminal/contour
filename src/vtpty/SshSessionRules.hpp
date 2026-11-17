// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The decisions SshSession makes about the end of a session. Extracted so every one of them is testable
/// without libssh2, a socket or a host.

#include <cerrno>
#include <cstdint>

namespace vtpty
{

/// Whether text the session injected for the screen is still waiting to be read.
enum class InjectedRead : uint8_t
{
    Empty = 0, ///< Nothing is waiting.
    Pending,   ///< Text is waiting, e.g. the message of the failure that ended the session.
};

/// The errno SshSession::read() reports once the session is over, here or on another thread. A message the
/// end injected is still to be shown, and read() hands out injected text before it reports the end, so the
/// reader is asked to read again (EINTR) while there is some; EPIPE, the end, once there is none.
/// @param injected Whether injected text is waiting.
/// @return EINTR or EPIPE.
[[nodiscard]] constexpr int sshClosedReadErrno(InjectedRead injected) noexcept
{
    return injected == InjectedRead::Pending ? EINTR : EPIPE;
}

} // namespace vtpty
