// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <vtextension/DeadlineGuard.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/wasm/wamr/WamrThreadEnv.hpp>

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
    /// @see wasm::wamr::ThreadEnv
    using ThreadEnv = wasm::wamr::ThreadEnv;

    /// @return Which runtime is up, or was last: it counts up each time an engine brings the runtime up
    ///         (none being alive). 0 before the first.
    [[nodiscard]] static uint64_t runtimeGeneration() noexcept { return wasm::wamr::runtimeGeneration(); }

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
