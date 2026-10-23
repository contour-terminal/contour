// SPDX-License-Identifier: Apache-2.0
//
// WAMR's process-wide state: the reference-counted runtime, which thread brought it up, which bring-up
// is current, and each thread's environment. Every WAMR backend object in a process, and the Engine,
// read and write this one state, so that they never disagree about whether a thread is prepared.
#pragma once

#include <cstdint>
#include <expected>
#include <thread>

#include <vtextension/ExtensionError.hpp>

namespace vtextension::wasm::wamr
{

/// Prepares the calling thread to call into instances, for as long as it lives.
///
/// The runtime needs per-thread state (its thread environment on Windows) on every thread that runs
/// guest code. The runtime is built without hardware bounds checks and without waking blocked calls
/// by signal, so preparing a thread writes no process-wide signal state and threads may prepare
/// concurrently. The thread that brought the runtime up has it already;
/// every other thread holds a ThreadEnv while it calls into instances. Guards nest: an inner one
/// on a thread that is already prepared does nothing. A guard must be destroyed on the thread
/// that entered it.
///
/// The state belongs to one runtime: when the last holder releases it, the runtime is torn down (on
/// Windows that frees the thread-local slot the state lives in), and a later acquireRuntime() brings up a
/// new runtime, a new generation. A guard entered for an earlier generation no longer prepares its
/// thread; a long-lived thread (a pool worker) ends it and enters a new one. @see runtimeGeneration
class ThreadEnv
{
  public:
    /// Prepares the calling thread.
    /// @return The guard; `RuntimeUnavailable` if no runtime is up or the runtime could not set up the
    ///         thread.
    [[nodiscard]] static std::expected<ThreadEnv, ExtensionError> enter();

    ThreadEnv(ThreadEnv&& other) noexcept;
    ThreadEnv(ThreadEnv const&) = delete;
    ThreadEnv& operator=(ThreadEnv const&) = delete;
    ThreadEnv& operator=(ThreadEnv&&) = delete;

    /// Releases the thread's runtime state if this guard set it up. Must run on the thread that
    /// entered the guard.
    ~ThreadEnv();

    /// @return Whether the calling thread may call into instances: it holds a ThreadEnv entered for
    ///         the current runtime generation, or it brought the current runtime up.
    [[nodiscard]] static bool isCurrentThreadPrepared();

    /// @return The runtime generation the calling thread's state was set up for; 0 if none.
    [[nodiscard]] static uint64_t preparedGeneration() noexcept;

    /// @return The runtime generation this guard was entered for.
    [[nodiscard]] uint64_t generation() const noexcept { return _generation; }

  private:
    /// What the guard does on destruction.
    enum class Hold : uint8_t
    {
        Released, ///< Nothing: it was moved from.
        Nested,   ///< Leaves the nesting depth one lower; the state belongs to someone else.
        Owned,    ///< Leaves the nesting depth one lower and tears the thread's state down.
    };

    ThreadEnv(Hold hold, uint64_t generation) noexcept:
        _hold { hold }, _thread { std::this_thread::get_id() }, _generation { generation }
    {
    }

    Hold _hold;
    std::thread::id _thread; ///< The thread that entered the guard, which alone may end it.
    uint64_t _generation;    ///< The runtime generation it was entered for.
};

/// Takes one hold on the runtime, bringing it up (a new generation) if none was held. WAMR counts its
/// own initializations; this mirrors that count, so that a ThreadEnv can tell the bringing-up thread,
/// whose state is the runtime's to tear down.
/// @return Nothing on success; `RuntimeUnavailable` if the runtime could not be initialized.
[[nodiscard]] std::expected<void, ExtensionError> acquireRuntime();

/// Releases one hold acquireRuntime() took; the runtime ends with the last one.
/// Precondition: holds are taken and released on one owner thread; the count drops before the runtime
/// goes, so an acquireRuntime() racing this on another thread would find the runtime about to be destroyed.
void releaseRuntime() noexcept;

/// @return Which runtime is up, or was last: it counts up each time the runtime is brought up (none
///         being held). 0 before the first.
[[nodiscard]] uint64_t runtimeGeneration() noexcept;

} // namespace vtextension::wasm::wamr
