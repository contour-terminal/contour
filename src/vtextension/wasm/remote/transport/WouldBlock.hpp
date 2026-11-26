// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether a call on a non-blocking descriptor failed only because it would have blocked. POSIX allows
/// EWOULDBLOCK to be a value of its own; where it is EAGAIN (Linux, macOS, the BSDs), comparing with both is
/// one comparison twice, which GCC's -Wlogical-op refuses.

#include <cerrno>

namespace vtextension::wasm::remote::transport
{

/// @param error An errno value.
/// @return Whether it says the call would have blocked: EAGAIN, or EWOULDBLOCK where the system tells the
///         two apart.
[[nodiscard]] constexpr bool wouldBlock(int error) noexcept
{
#if EWOULDBLOCK != EAGAIN
    return error == EAGAIN || error == EWOULDBLOCK;
#else
    return error == EAGAIN;
#endif
}

} // namespace vtextension::wasm::remote::transport
