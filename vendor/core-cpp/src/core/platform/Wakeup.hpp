// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file Wakeup.hpp
/// @brief Cross-platform wakeup primitive for inter-thread signaling.

#include <core/platform/Types.hpp>

namespace core::platform
{

/// Platform-independent wakeup primitive for cross-thread signaling.
///
/// On Linux: wraps an eventfd (optimal, single fd).
/// On other UNIX: wraps a self-pipe pair.
/// On Windows: wraps a manual-reset Event (HANDLE).
///
/// Usage: one thread calls signal() to wake another thread that is
/// blocked in TerminalInput::poll() or WaitForMultipleObjects().
class Wakeup
{
  public:
    /// @brief Creates the wakeup channel: an eventfd on Linux, a self-pipe on other UNIX systems,
    ///        a manual-reset event on Windows.
    /// @throws std::runtime_error if the operating system refuses it, which it does only when the
    ///         process or the system has run out of descriptors, handles or kernel memory.
    ///
    /// Throwing rather than returning an error: that condition is unrecoverable. No event loop can
    /// run without its wakeup channel, so no caller has anything else to do but give up, and
    /// handing the failure back as a value would only make each caller pass it on.
    Wakeup();
    ~Wakeup();

    Wakeup(Wakeup const&) = delete;
    Wakeup& operator=(Wakeup const&) = delete;
    Wakeup(Wakeup&&) = delete;
    Wakeup& operator=(Wakeup&&) = delete;

    /// @brief Signals the wakeup, unblocking any thread waiting on it.
    void signal() const;

    /// @brief Resets the signaled state (called after draining).
    void reset() const;

    /// @brief Returns the platform-native handle for poll()/WaitForMultipleObjects().
    [[nodiscard]] auto nativeHandle() const noexcept -> NativeHandle;

  private:
    NativeHandle _handle = InvalidHandle;
#if !defined(_WIN32) && !defined(__linux__)
    NativeHandle _writeFd = InvalidHandle; ///< Write end for self-pipe (non-Linux UNIX).
#endif
};

} // namespace core::platform
