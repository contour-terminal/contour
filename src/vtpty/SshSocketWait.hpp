// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A bounded wait on a POSIX descriptor, for the I/O SSH sessions do on sockets libssh2 would otherwise
/// read without a timeout (the agent's). Not on Windows, where libssh2 reaches the agent through a pipe.

#ifndef _WIN32

    #include <chrono>
    #include <expected>

namespace vtpty
{

/// Waits until @p fd is ready for @p events, or @p deadline has passed. A signal does not end the wait.
/// @param fd       The descriptor.
/// @param events   poll() events, e.g. POLLIN or POLLOUT.
/// @param deadline When to give up.
/// @return Nothing once ready (or once poll() reports an error or hang-up, which the I/O that follows will
///         report itself); ETIMEDOUT once @p deadline has passed; poll()'s errno if it fails.
[[nodiscard]] std::expected<void, int> waitForDescriptor(int fd,
                                                         short events,
                                                         std::chrono::steady_clock::time_point deadline);

} // namespace vtpty

#endif
