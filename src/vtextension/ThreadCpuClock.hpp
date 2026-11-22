// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace vtextension
{

/// Names one thread's CPU clock. It stays valid while that thread lives; what it holds is the platform's.
struct ThreadCpuId
{
    std::uint64_t value = 0; ///< The platform's name for the thread's clock.

    [[nodiscard]] constexpr bool operator==(ThreadCpuId const&) const noexcept = default;
};

/// Measures how much CPU time a thread has consumed, read from any thread: what a call's deadline is spent
/// in, so that a guest whose thread waits for a CPU on a loaded machine is not charged for the wait.
class IThreadCpuClock
{
  public:
    IThreadCpuClock() = default;
    IThreadCpuClock(IThreadCpuClock const&) = delete;
    IThreadCpuClock(IThreadCpuClock&&) = delete;
    IThreadCpuClock& operator=(IThreadCpuClock const&) = delete;
    IThreadCpuClock& operator=(IThreadCpuClock&&) = delete;
    virtual ~IThreadCpuClock() = default;

    /// @return The calling thread's clock; nothing if the platform cannot name it.
    [[nodiscard]] virtual std::optional<ThreadCpuId> currentThread() const noexcept = 0;

    /// @param thread A clock currentThread() named, of a thread still alive.
    /// @return The CPU time, user and system, the thread has consumed so far; nothing if it cannot be read.
    [[nodiscard]] virtual std::optional<std::chrono::nanoseconds> consumed(
        ThreadCpuId thread) const noexcept = 0;
};

/// @return The operating system's per-thread CPU clocks: POSIX `pthread_getcpuclockid`, Mach `thread_info`,
///         or Windows `GetThreadTimes`. Stateless, so one instance serves the process.
[[nodiscard]] IThreadCpuClock const& nativeThreadCpuClock() noexcept;

} // namespace vtextension
