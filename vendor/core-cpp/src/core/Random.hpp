// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The operating system's random source, behind an interface so that a test can replace it.

#include <cstddef>
#include <span>

namespace core
{

/// A source of random bytes.
///
/// Whatever draws nonces, salts or keys takes an @c IRandom rather than calling the system, so a test
/// can hand it a deterministic one. The production implementation is @c systemRandom().
class IRandom
{
  public:
    IRandom() = default;
    IRandom(IRandom const&) = delete;
    IRandom& operator=(IRandom const&) = delete;
    IRandom(IRandom&&) = delete;
    IRandom& operator=(IRandom&&) = delete;
    virtual ~IRandom() = default;

    /// Overwrites @p out entirely with random bytes; an empty span is fine and writes nothing.
    /// @param out The bytes to fill.
    virtual void fill(std::span<std::byte> out) = 0;
};

/// The operating system's cryptographically secure random source: `getentropy()` on every POSIX
/// system (Linux, macOS, the BSDs, illumos) and under Emscripten, `BCryptGenRandom()` on Windows. A
/// platform with neither is refused when CMake configures.
///
/// A system that cannot supply random bytes has no safe way to go on, so a failure ends the process
/// (`core::detail::fail`, as a failed @c Require does) rather than hand back bytes that are not random. Safe
/// to call from any thread.
/// @return The process-wide source, valid for the life of the process.
[[nodiscard]] IRandom& systemRandom();

} // namespace core
