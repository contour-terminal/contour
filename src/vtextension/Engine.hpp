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
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

/// The process's handle on a WebAssembly runtime, and the watchdog that enforces per-call deadlines on
/// the instances created from it.
///
/// The runtime is created from the backend the engine is given, with the statusline world's host
/// functions (so that every module it loads links against them) and with the engine's memory and stack
/// limits. The backend brings its engine up with the first runtime alive and down with the last; the
/// watchdog thread lives exactly as long as its Engine, so a process with no extensions pays for
/// neither.
///
/// The watchdog measures real time on purpose: it bounds a real interpreter running on a real
/// thread, which no injected clock could advance.
class Engine: public std::enable_shared_from_this<Engine>, public IDeadlineWatchdog
{
    struct PrivateTag
    {
    };

  public:
    /// Creates a runtime on @p backend and starts the watchdog thread.
    /// @param backend What the runtime is created from; must outlive the engine.
    /// @param limits The limits every instance created on this engine is held to.
    /// @return The engine, or ExtensionError::RuntimeUnavailable if the runtime could not be created or
    ///         the watchdog thread could not be started.
    [[nodiscard]] static std::expected<std::shared_ptr<Engine>, ExtensionError> create(
        wasm::IWasmBackend& backend, ExtensionLimits limits);

    /// Use create(); public only so that std::make_shared can reach it.
    Engine(PrivateTag,
           wasm::IWasmBackend& backend,
           ExtensionLimits limits,
           std::shared_ptr<wasm::IWasmRuntime> runtime);

    /// Stops and joins the watchdog, then releases this engine's runtime.
    ~Engine() override;

    Engine(Engine const&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine const&) = delete;
    Engine& operator=(Engine&&) = delete;

    /// @return The limits this engine was created with.
    [[nodiscard]] ExtensionLimits const& limits() const noexcept { return _limits; }

    /// @return The backend the engine's runtime was created from.
    [[nodiscard]] wasm::IWasmBackend& backend() const noexcept { return _backend; }

    /// @return The runtime modules are loaded on.
    [[nodiscard]] wasm::IWasmRuntime& runtime() const noexcept { return *_runtime; }

    /// Arms a deadline: unless disarmed first, the watchdog marks the token's deadline expired after
    /// @p timeout, then interrupts its instance, which makes the call running in it (and every later
    /// one) fail. Arming a token that is already armed replaces its deadline. The instance and the
    /// state must outlive the deadline: disarm before destroying them.
    /// @param token The instance to guard; neither its instance nor its state may be null.
    /// @param timeout How long from now the instance may keep running.
    void armDeadline(WatchToken token, std::chrono::milliseconds timeout) override;

    /// Disarms the token's deadline. Once this returns, the watchdog will not touch the instance or
    /// the state on that deadline's account; it may already have, which the state tells.
    /// @param token The instance whose deadline to drop; a token not armed is ignored.
    void disarm(WatchToken token) override;

  private:
    /// One armed deadline.
    struct Deadline
    {
        std::chrono::steady_clock::time_point expiry; ///< When the instance is interrupted.
        WatchToken token;                             ///< The instance to interrupt.
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

    /// The watchdog thread's body: sleeps until the earliest deadline, interrupts what expired.
    void watchdogLoop();

    wasm::IWasmBackend& _backend;
    ExtensionLimits _limits;
    std::shared_ptr<wasm::IWasmRuntime> _runtime; ///< Released after the watchdog has stopped.
    std::mutex _mutex;
    std::condition_variable _wakeup;
    std::vector<Deadline> _deadlines; ///< A min-heap on expiry, guarded by _mutex.
    WatchdogState _state = WatchdogState::Running;
    std::thread _watchdog; ///< Declared last: starts once everything it reads is constructed.
};

} // namespace vtextension
