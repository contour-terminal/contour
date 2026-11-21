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
#include <vtextension/ThreadCpuClock.hpp>
#include <vtextension/World.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

/// The process's handle on a WebAssembly runtime, and the watchdog that enforces per-call deadlines on
/// the instances created from it.
///
/// The runtime is created from the backend the engine is given, with the host functions of the engine's
/// world (so that every module it loads links against them) and with the engine's memory and stack
/// limits. An engine serves one world: a module of the other world does not load on it. The backend brings
/// its engine up with the first runtime alive and down with the last; the watchdog thread lives exactly as
/// long as its Engine, so a process with no extensions pays for neither.
///
/// A deadline is spent in the CPU time of the thread the guarded call runs on, as the engine's
/// IThreadCpuClock reads it, so that a guest whose thread waits for a CPU on a busy machine is not charged
/// for the wait; ExtensionLimits::deadlineRealTimeFactor bounds that wait in real time. The watchdog
/// sleeps in real time towards the earliest moment a deadline could be spent, and reads the thread's clock
/// only then: a call that returns in time costs no reading. Where a thread's clock cannot be read, the
/// deadline is real time.
class Engine: public std::enable_shared_from_this<Engine>, public IDeadlineWatchdog
{
    struct PrivateTag
    {
    };

  public:
    /// Creates a runtime on @p backend and starts the watchdog thread.
    /// @param backend What the runtime is created from; must outlive the engine.
    /// @param world The world the engine serves: its host functions are the runtime's, and only its modules
    ///        load on the engine (CompiledExtension::load()).
    /// @param limits The limits every instance created on this engine is held to.
    /// @param cpuClock What reads a guarded thread's CPU time; must outlive the engine.
    /// @return The engine, or ExtensionError::RuntimeUnavailable if the runtime could not be created or
    ///         the watchdog thread could not be started.
    [[nodiscard]] static std::expected<std::shared_ptr<Engine>, ExtensionError> create(
        wasm::IWasmBackend& backend,
        World world,
        ExtensionLimits limits,
        IThreadCpuClock const& cpuClock = nativeThreadCpuClock());

    /// Use create(); public only so that std::make_shared can reach it.
    Engine(PrivateTag,
           wasm::IWasmBackend& backend,
           World world,
           ExtensionLimits limits,
           IThreadCpuClock const& cpuClock,
           std::shared_ptr<wasm::IWasmRuntime> runtime);

    /// Stops and joins the watchdog, then releases this engine's runtime.
    ~Engine() override;

    Engine(Engine const&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine const&) = delete;
    Engine& operator=(Engine&&) = delete;

    /// @return The world this engine serves.
    [[nodiscard]] World world() const noexcept { return _world; }

    /// @return The limits this engine was created with.
    [[nodiscard]] ExtensionLimits const& limits() const noexcept { return _limits; }

    /// @return The backend the engine's runtime was created from.
    [[nodiscard]] wasm::IWasmBackend& backend() const noexcept { return _backend; }

    /// @return The runtime modules are loaded on.
    [[nodiscard]] wasm::IWasmRuntime& runtime() const noexcept { return *_runtime; }

    /// Arms a deadline: unless disarmed first, the watchdog marks the token's deadline expired once the
    /// calling thread has spent @p timeout of CPU time, or @p timeout times deadlineRealTimeFactor has
    /// passed, then interrupts its instance, which makes the call running in it (and every later one)
    /// fail. Arming a token that is already armed replaces its deadline. The instance and the state must
    /// outlive the deadline: disarm before destroying them.
    /// @param token The instance to guard; neither its instance nor its state may be null.
    /// @param timeout How much the instance may spend from now on.
    void armDeadline(WatchToken token, std::chrono::milliseconds timeout) override;

    /// @return The CPU time the token's deadline has left, bounded by the real time it has left; zero if
    ///         the token is not armed.
    [[nodiscard]] std::chrono::nanoseconds unspent(WatchToken token) override;

    /// Disarms the token's deadline. Once this returns, the watchdog will not touch the instance or
    /// the state on that deadline's account; it may already have, which the state tells.
    /// @param token The instance whose deadline to drop; a token not armed is ignored.
    void disarm(WatchToken token) override;

  private:
    using TimePoint = std::chrono::steady_clock::time_point;

    /// One armed deadline.
    struct Deadline
    {
        TimePoint expiry; ///< When the watchdog next looks: the earliest the budget could be spent.
        TimePoint armed;  ///< When it was armed: what a thread without a readable clock spends from.
        TimePoint latest; ///< When the instance is interrupted however little CPU time it spent.
        std::chrono::nanoseconds budget;      ///< The CPU time it may spend.
        std::optional<ThreadCpuId> thread;    ///< The guarded thread's clock, if it can be read.
        std::chrono::nanoseconds cpuAtArm {}; ///< That clock's reading when the deadline was armed.
        WatchToken token;                     ///< The instance to interrupt.
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

    /// @return What @p deadline has left at @p now: its CPU budget less what its thread spent (real time,
    ///         where the thread's clock cannot be read), bounded by its real-time limit; zero once spent.
    [[nodiscard]] std::chrono::nanoseconds unspentAt(Deadline const& deadline, TimePoint now) const noexcept;

    /// The watchdog thread's body: sleeps until the earliest deadline, interrupts what expired.
    void watchdogLoop();

    wasm::IWasmBackend& _backend;
    World _world;
    ExtensionLimits _limits;
    IThreadCpuClock const& _cpuClock;
    std::shared_ptr<wasm::IWasmRuntime> _runtime; ///< Released after the watchdog has stopped.
    std::mutex _mutex;
    std::condition_variable _wakeup;
    std::vector<Deadline> _deadlines; ///< A min-heap on expiry, guarded by _mutex.
    WatchdogState _state = WatchdogState::Running;
    std::thread _watchdog; ///< Declared last: starts once everything it reads is constructed.
};

} // namespace vtextension
