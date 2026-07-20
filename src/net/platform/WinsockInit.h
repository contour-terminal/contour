// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Process-wide, idempotent WinSock initialization.
///
/// Any code that creates sockets on Windows must call WSAStartup first. Rather
/// than push that onto every entry point (main, each test runner, …), socket-using
/// components call @c ensureWinsockInitialized() at their creation points; the
/// first call performs WSAStartup via a function-local static, and the matching
/// WSACleanup runs at process exit. On POSIX it is a no-op.

namespace net
{

/// Ensures WinSock is initialized for the lifetime of the process. Idempotent and
/// thread-safe (initialization happens once, on first call). No-op on POSIX.
void ensureWinsockInitialized() noexcept;

} // namespace net
