// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A connected pair of REAL sockets on a loop, for tests that need the production read and write
/// path without a listener: an AF_UNIX socketpair on POSIX, a loopback TCP pair on Windows, each end
/// wrapped in the platform socket. It parks, cancels and fails exactly like production I/O because
/// it IS production I/O -- a kernel and a loop are underneath every call.
///
/// **It is not a fake, and `<core/net/testing/InMemorySocket.hpp>` is.** That one is two byte pipes
/// in the process with no descriptor and no loop, answering inline; this one goes through the
/// kernel. They are kept apart deliberately: `SocketClosedStates_test.cpp` pins the fake against a
/// real pair, and merging the two would delete the difference that test measures. The name is
/// contour's, kept because consumers include it by that name.

#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoResult.hpp>

#include <expected>
#include <memory>

namespace core::net::testing
{

/// A connected pair of endpoints: bytes written to @c first are readable on
/// @c second and vice versa.
struct SocketPair
{
    std::unique_ptr<ISocket> first;  ///< One endpoint.
    std::unique_ptr<ISocket> second; ///< The peer endpoint.
};

/// Creates a connected in-process @c ISocket pair driven by @p loop.
/// @param loop The loop whose reactor drives readiness (not owned).
/// @return The connected pair, or a @c NetError if the underlying pair could not
///         be created.
[[nodiscard]] std::expected<SocketPair, NetError> makeSocketPair(EventLoop& loop);

} // namespace core::net::testing
