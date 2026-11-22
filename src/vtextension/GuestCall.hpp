// SPDX-License-Identifier: Apache-2.0
//
// Internal to vtextension: the rules every world's instance class keeps around calls into its guest.
#pragma once

#include <core/Assert.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/DeadlineGuard.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::detail
{

/// One guest instance and the rules around calling into it, whatever world it speaks: the deadline of
/// every call, the fault that ends the instance, the bounded lifting of a host call's arguments, the
/// guest memory the codec reaches, and the dispatch of host calls (which do not nest and let no exception
/// out). Instance and DaemonInstance each hold one and keep only their world's exports and handlers.
///
/// Non-movable: the watchdog's WatchToken and a control verb's ControlCallBudget point at its deadline
/// state.
class GuestCall
{
  public:
    /// @param backend The backend the instance's runtime came from; calls must be made on a thread it
    ///        prepared.
    /// @param watchdog What enforces the deadlines, and measures how much of one is spent.
    /// @param limits The instance's limits. Every reference must outlive the GuestCall.
    GuestCall(wasm::IWasmBackend& backend, IDeadlineWatchdog& watchdog, ExtensionLimits const& limits);
    ~GuestCall();

    GuestCall(GuestCall const&) = delete;
    GuestCall(GuestCall&&) = delete;
    GuestCall& operator=(GuestCall const&) = delete;
    GuestCall& operator=(GuestCall&&) = delete;

    /// Instantiates @p module, checking its memory against the cap, and looks its `cabi_realloc` up. The
    /// one call after construction: the instance needs @p context, which is the GuestCall's owner.
    /// @return Nothing; `OutOfMemory` if a memory's initial size exceeds the cap, `InvalidComponent` if
    ///         the runtime cannot instantiate it or it has no `cabi_realloc`, or the runtime's error.
    [[nodiscard]] std::expected<void, ExtensionError> instantiate(wasm::IWasmModule& module,
                                                                  wasm::IHostContext& context);

    /// @return The instance's function export @p name; nothing if it has none.
    [[nodiscard]] std::optional<wasm::ExportFunction> function(std::string_view name) const;

    /// Runs a WASI reactor's `_initialize`, if the instance exports one, under the load deadline: the
    /// backend does not run it while instantiating, where no deadline could stop it.
    [[nodiscard]] std::expected<void, ExtensionError> initialize();

    /// Runs @p body under a deadline of @p timeout, and records any failure as the instance's fault, an
    /// exception escaping @p body included (as `Trapped`). The deadline is disarmed however the body ends.
    /// @return What @p body returned; the fault if the instance is (or now becomes) unusable.
    template <typename Body>
    [[nodiscard]] auto guarded(std::chrono::milliseconds timeout, Body const& body) -> decltype(body())
    {
        if (auto const ready = beginCall(); !ready)
            return std::unexpected(ready.error());
        auto result = runGuarded(_watchdog, watchToken(), timeout, body);
        if (auto const failed = endCall(result ? std::nullopt : std::optional { result.error() }))
            return std::unexpected(*failed);
        return result;
    }

    /// Calls an export with @p argv as its parameters and, on return, its results.
    [[nodiscard]] std::expected<void, ExtensionError> callExport(wasm::ExportFunction function,
                                                                 std::span<uint32_t> argv);

    /// Runs the host function at @p index of @p table: checks the arity the runtime called it with, refuses
    /// a host call nested in another (from the guest's `cabi_realloc` while the host stores a result),
    /// gives it a fresh lift budget, and turns an exception into `Trapped`. Called from inside the runtime.
    /// @param invoke Runs the handler, given @p index.
    /// @return Nothing; otherwise the error that faults the call.
    template <typename Invoke>
    [[nodiscard]] std::expected<void, ExtensionError> dispatch(std::size_t index,
                                                               std::span<uint32_t const> arguments,
                                                               std::span<wasm::HostFunction const> table,
                                                               Invoke const& invoke) noexcept
    {
        if (index >= table.size() || arguments.size() != table[index].parameterCount
            || _hostCall == HostCallState::InProgress)
            return failHostCall(ExtensionError::Trapped);
        _hostCall = HostCallState::InProgress;
        auto result = abi::Result<void> {};
        try
        {
            resetLiftBudget();
            result = invoke(index);
        }
        catch (...)
        {
            result = std::unexpected(ExtensionError::Trapped);
        }
        _hostCall = HostCallState::Idle;
        if (!result)
            return failHostCall(result.error());
        return {};
    }

    /// Runs @p call with the call's deadline standing still, and resumes it with what was left: for host
    /// work whose time is not the guest's (a command, a durable write). The deadline resumes on every path
    /// out of @p call, an exception included.
    /// @return What @p call answered; `DeadlineExceeded` if the deadline was spent before @p call could
    ///         start, in which case it is not made.
    template <typename Call>
    [[nodiscard]] abi::Result<std::invoke_result_t<Call const&>> pauseDeadline(Call const& call)
    {
        auto const token = watchToken();
        auto const remaining = _watchdog.unspent(token);
        _watchdog.disarm(token);
        // Once disarmed, the watchdog cannot fire any more; it may have already, or the deadline may be
        // spent all the same.
        if (isDeadlineExpired() || remaining <= std::chrono::nanoseconds::zero())
            return std::unexpected(ExtensionError::DeadlineExceeded);

        auto const resume = ResumeDeadlineOnExit { *this, remaining };
        return abi::Result<std::invoke_result_t<Call const&>> { std::in_place, call() };
    }

    /// @return The guest's memory, as the codec reaches it.
    [[nodiscard]] abi::IGuestMemory& memory() noexcept;

    /// @return How much of the current call's deadline is left, as the watchdog measures it.
    [[nodiscard]] std::chrono::nanoseconds remaining() { return _watchdog.unspent(watchToken()); }

    /// @return The deadline's state, which the watchdog sets from its own thread.
    [[nodiscard]] std::atomic<DeadlineState> const& deadlineState() const noexcept { return _deadlineState; }

    /// @return The error that made the instance unusable; `std::nullopt` while it is usable.
    [[nodiscard]] std::optional<ExtensionError> fault() const noexcept { return _fault; }

  private:
    class GuestMemory;

    /// Whether a host call is running in this instance, which no other host call may enter.
    enum class HostCallState : uint8_t
    {
        Idle,
        InProgress,
    };

    /// Arms the call's deadline again with what was left of it when it goes out of scope, however the
    /// scope is left: the end of pauseDeadline, an exception from the paused call included.
    class ResumeDeadlineOnExit
    {
      public:
        ResumeDeadlineOnExit(GuestCall& call, std::chrono::nanoseconds remaining) noexcept:
            _call { call }, _remaining { remaining }
        {
        }

        ResumeDeadlineOnExit(ResumeDeadlineOnExit const&) = delete;
        ResumeDeadlineOnExit(ResumeDeadlineOnExit&&) = delete;
        ResumeDeadlineOnExit& operator=(ResumeDeadlineOnExit const&) = delete;
        ResumeDeadlineOnExit& operator=(ResumeDeadlineOnExit&&) = delete;

        /// Takes the watchdog's mutex, as DisarmOnExit's destructor does, for the same bounded time.
        ~ResumeDeadlineOnExit();

      private:
        GuestCall& _call;
        std::chrono::nanoseconds _remaining;
    };

    /// Prepares a guarded call: the instance must be usable and the thread prepared.
    [[nodiscard]] std::expected<void, ExtensionError> beginCall();

    /// Records how a guarded call ended.
    /// @param error Why it failed; `std::nullopt` if it returned.
    /// @return The instance's fault, if it has one now.
    [[nodiscard]] std::optional<ExtensionError> endCall(std::optional<ExtensionError> error) noexcept;

    /// Gives the next lift the full budget of one host call.
    void resetLiftBudget() noexcept;

    /// @return What names this instance, and its deadline's state, to the watchdog.
    [[nodiscard]] WatchToken watchToken() noexcept;

    /// @return Whether the watchdog found a deadline of this instance passed.
    [[nodiscard]] bool isDeadlineExpired() const noexcept;

    /// @return Why the call that just failed in the guest failed, given what the runtime reported.
    [[nodiscard]] ExtensionError guestFailure(wasm::CallFailure failure) const noexcept;

    /// Records an error a host function hit: the call it faults fails with it.
    /// @return The error, for the runtime.
    [[nodiscard]] std::unexpected<ExtensionError> failHostCall(ExtensionError error) noexcept;

    wasm::IWasmBackend& _backend;
    IDeadlineWatchdog& _watchdog;
    ExtensionLimits const& _limits;
    /// Written by the watchdog, from its own thread, before it interrupts the instance. Once Expired it
    /// stays so: the instance is faulted for good by then.
    std::atomic<DeadlineState> _deadlineState = DeadlineState::Running;
    std::unique_ptr<wasm::IWasmInstance> _instance; ///< Null only until instantiate() succeeded.
    wasm::ExportFunction _realloc;                  ///< The guest's `cabi_realloc`.
    std::unique_ptr<GuestMemory> _memory;
    std::optional<ExtensionError> _fault;
    std::optional<ExtensionError> _hostCallError; ///< What a host function failed with, this call.
    HostCallState _hostCall = HostCallState::Idle;
};

} // namespace vtextension::detail
