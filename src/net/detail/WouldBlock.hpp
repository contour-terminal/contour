// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// One spelling of the non-blocking "would have blocked" errno test, shared by net's call sites.
///
/// POSIX permits EAGAIN and EWOULDBLOCK to name the same value, and on Linux with glibc they do.
/// Testing both in a single `||` is therefore a tautology there, which GCC diagnoses as
/// `-Wlogical-op` ("logical 'or' of equal expressions") and this tree's pedantic builds turn into
/// an error. Every call site had written that `||` out by hand; they now share this one, which
/// selects the comparison with the preprocessor. The preprocessor rather than `if constexpr`
/// deliberately: a discarded `if constexpr` branch in a non-template function is still parsed and
/// still warned about, so only `#if` actually removes the offending expression.

#include <cerrno>

namespace net::detail
{

/// Tests whether @p err is the errno a non-blocking socket or pipe reports when the operation
/// would have blocked and should be retried once the descriptor is ready.
/// @param err An errno value.
/// @return True if @p err means "would block; retry when ready".
[[nodiscard]] constexpr bool isWouldBlock(int err) noexcept
{
#if EAGAIN == EWOULDBLOCK
    return err == EAGAIN;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

} // namespace net::detail
