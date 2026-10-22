// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <vtextension/DeadlineGuard.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>

namespace vtextension
{

namespace detail
{
    class HostRegistration;
} // namespace detail

/// The process's handle on the WebAssembly runtime (WAMR, fast interpreter) and the watchdog that
/// enforces per-call deadlines on the instances created from it.
///
/// The runtime is initialized when the first Engine is created and destroyed with the last one
/// (WAMR reference-counts wasm_runtime_init / wasm_runtime_destroy itself). The watchdog thread
/// lives exactly as long as its Engine, so a process with no extensions pays for neither.
///
/// The watchdog measures real time on purpose: it bounds a real interpreter running on a real
/// thread, which no injected clock could advance.
///
/// The engine also registers the statusline world's host functions with the runtime, so that every
/// module loaded while it lives links against them, and holds the null device that the guests' WASI
/// standard input, output and error are bound to: a guest's prints reach nobody.
class Engine: public std::enable_shared_from_this<Engine>, public IDeadlineWatchdog
{
    struct PrivateTag
    {
    };

  public:
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
    /// The state belongs to one runtime: when the last engine goes, the runtime is torn down (on
    /// Windows that frees the thread-local slot the state lives in), and a later engine brings up a new
    /// runtime, a new generation. A guard entered for an earlier generation no longer prepares its
    /// thread; a long-lived thread (a pool worker) ends it and enters a new one. @see runtimeGeneration
    class ThreadEnv
    {
      public:
        /// Prepares the calling thread.
        /// @return The guard; `RuntimeUnavailable` if no runtime is up (no engine is alive) or the runtime
        ///         could not set up the thread.
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

    /// @return Which runtime is up, or was last: it counts up each time an engine brings the runtime up
    ///         (none being alive). 0 before the first.
    [[nodiscard]] static uint64_t runtimeGeneration() noexcept;

    /// Initializes (or joins) the WebAssembly runtime and starts the watchdog thread.
    /// @param limits The limits every instance created on this engine is held to.
    /// @return The engine, or ExtensionError::RuntimeUnavailable if WAMR could not initialize or
    ///         the watchdog thread could not be started.
    [[nodiscard]] static std::expected<std::shared_ptr<Engine>, ExtensionError> create(
        ExtensionLimits limits);

    /// Use create(); public only so that std::make_shared can reach it.
    Engine(PrivateTag,
           ExtensionLimits limits,
           int64_t nullDevice,
           std::unique_ptr<detail::HostRegistration> hostRegistration);

    /// Stops and joins the watchdog, unregisters the host functions, then releases this engine's
    /// hold on the runtime.
    ~Engine() override;

    Engine(Engine const&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine const&) = delete;
    Engine& operator=(Engine&&) = delete;

    /// @return The limits this engine was created with.
    [[nodiscard]] ExtensionLimits const& limits() const noexcept { return _limits; }

    /// @return The raw handle of the null device (a file descriptor, or a HANDLE on Windows) that
    ///         guests' WASI standard streams are bound to.
    [[nodiscard]] int64_t nullDeviceHandle() const noexcept { return _nullDevice; }

    /// Arms a deadline: unless disarmed first, the watchdog terminates the token's instance after
    /// @p timeout, which makes the call running in it fail as though it trapped. Arming a token that
    /// is already armed replaces its deadline. The instance must outlive the deadline: disarm
    /// before destroying it.
    /// @param token The instance to guard; its instance must not be null.
    /// @param timeout How long from now the instance may keep running.
    void armDeadline(WatchToken token, std::chrono::milliseconds timeout) override;

    /// Disarms the token's deadline. Once this returns, the watchdog will not terminate the
    /// instance on that deadline's account; it may already have, which the instance's exception
    /// tells.
    /// @param token The instance whose deadline to drop; a token not armed is ignored.
    void disarm(WatchToken token) override;

  private:
    /// Opens the null device.
    /// @return Its raw handle; or nothing if it cannot be opened.
    [[nodiscard]] static std::optional<int64_t> openNullDevice() noexcept;

    /// Closes a handle openNullDevice() returned.
    static void closeNullDevice(int64_t handle) noexcept;

    /// Releases one engine's hold on the runtime, which ends with the last one.
    /// Precondition: engines are created and released on one owner thread; the count drops before the runtime
    /// goes, so a create() racing this on another thread would find the runtime about to be destroyed.
    static void releaseRuntime() noexcept;

    /// One armed deadline.
    struct Deadline
    {
        std::chrono::steady_clock::time_point expiry; ///< When the instance is terminated.
        WatchToken token;                             ///< The instance to terminate.
    };

    /// Whether the watchdog thread keeps running.
    enum class WatchdogState : uint8_t
    {
        Running,
        Stopping,
    };

    /// Orders the deadline heap so that its front is the earliest expiry.
    [[nodiscard]] static bool expiresLater(Deadline const& a, Deadline const& b) noexcept;

    /// Drops the token's deadline, if armed, keeping the heap a heap. Expects _mutex held.
    void removeDeadline(WatchToken token);

    /// The watchdog thread's body: sleeps until the earliest deadline, terminates what expired.
    void watchdogLoop();

    ExtensionLimits _limits;
    int64_t _nullDevice;                                         ///< Owned; closed on destruction.
    std::unique_ptr<detail::HostRegistration> _hostRegistration; ///< Released before the runtime.
    std::mutex _mutex;
    std::condition_variable _wakeup;
    std::vector<Deadline> _deadlines; ///< A min-heap on expiry, guarded by _mutex.
    WatchdogState _state = WatchdogState::Running;
    std::thread _watchdog; ///< Declared last: starts once everything it reads is constructed.
};

} // namespace vtextension
