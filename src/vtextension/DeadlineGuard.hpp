// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <type_traits>

#include <vtextension/ExtensionError.hpp>

namespace vtextension
{

namespace wasm
{
    class IWasmInstance;
} // namespace wasm

/// Whether the deadline guarding an instance's calls has expired. Written by the watchdog, read by the
/// instance: it is how the instance tells an overrun apart from any other failure, without asking the
/// runtime.
enum class DeadlineState : uint8_t
{
    Running, ///< Not expired: the instance may be running towards the deadline, or be idle.
    Expired, ///< The watchdog found the deadline passed, and interrupted the instance. Final.
};

/// Names what a deadline guards: the instance the watchdog interrupts on expiry, and where it marks the
/// deadline expired before it does.
struct WatchToken
{
    wasm::IWasmInstance* instance = nullptr;     ///< The guarded instance.
    std::atomic<DeadlineState>* state = nullptr; ///< Set to Expired on expiry, before the interrupt.

    [[nodiscard]] constexpr bool operator==(WatchToken const&) const noexcept = default;
};

/// What arms and disarms call deadlines: the Engine's watchdog, or a test's recorder.
class IDeadlineWatchdog
{
  public:
    IDeadlineWatchdog() = default;
    IDeadlineWatchdog(IDeadlineWatchdog const&) = delete;
    IDeadlineWatchdog(IDeadlineWatchdog&&) = delete;
    IDeadlineWatchdog& operator=(IDeadlineWatchdog const&) = delete;
    IDeadlineWatchdog& operator=(IDeadlineWatchdog&&) = delete;
    virtual ~IDeadlineWatchdog() = default;

    /// Arms a deadline: unless disarmed first, the token's deadline is marked expired after
    /// @p timeout, and its instance interrupted. Arming a token that is already armed replaces its
    /// deadline.
    /// @param token The instance to guard; neither its instance nor its state may be null.
    /// @param timeout How long from now the instance may keep running.
    virtual void armDeadline(WatchToken token, std::chrono::milliseconds timeout) = 0;

    /// Disarms the token's deadline. Once this returns, neither the instance nor the state is touched
    /// on that deadline's account.
    /// @param token The instance whose deadline to drop; a token not armed is ignored.
    virtual void disarm(WatchToken token) = 0;
};

namespace detail
{
    /// Disarms a token's deadline when it goes out of scope, however the scope is left.
    class DisarmOnExit
    {
      public:
        DisarmOnExit(IDeadlineWatchdog& watchdog, WatchToken token) noexcept:
            _watchdog { watchdog }, _token { token }
        {
        }

        DisarmOnExit(DisarmOnExit const&) = delete;
        DisarmOnExit(DisarmOnExit&&) = delete;
        DisarmOnExit& operator=(DisarmOnExit const&) = delete;
        DisarmOnExit& operator=(DisarmOnExit&&) = delete;

        /// Takes the watchdog's mutex. That is acceptable in a destructor: it is held only for O(log n)
        /// heap work and while the watchdog terminates an expired instance, never for a guest call, so
        /// this waits briefly at worst and cannot deadlock (the watchdog takes no lock of ours).
        ~DisarmOnExit() { _watchdog.disarm(_token); }

      private:
        IDeadlineWatchdog& _watchdog;
        WatchToken _token;
    };

    /// The result type of a guarded body: an `std::expected` whose error is an ExtensionError.
    template <typename T>
    concept GuardedResult = std::is_same_v<typename T::error_type, ExtensionError>;
} // namespace detail

/// Runs @p body under a deadline, so that no deadline outlives the call and no exception escapes it.
///
/// The deadline is armed before @p body runs and disarmed when it ends, on every path out of it: a
/// return, an error, or an exception. An exception thrown by @p body (or by arming the deadline) is
/// caught and answered as `Trapped`: whatever the call left behind is not to be trusted.
/// @param watchdog What enforces the deadline.
/// @param token The instance the deadline guards.
/// @param timeout How long @p body may run.
/// @param body The call; returns an `std::expected<T, ExtensionError>`.
/// @return What @p body returned; `Trapped` if it threw.
template <typename Body>
    requires detail::GuardedResult<std::invoke_result_t<Body const&>>
[[nodiscard]] auto runGuarded(IDeadlineWatchdog& watchdog,
                              WatchToken token,
                              std::chrono::milliseconds timeout,
                              Body const& body) noexcept -> std::invoke_result_t<Body const&>
{
    try
    {
        watchdog.armDeadline(token, timeout);
        auto const disarm = detail::DisarmOnExit { watchdog, token };
        return body();
    }
    catch (...)
    {
        return std::unexpected(ExtensionError::Trapped);
    }
}

} // namespace vtextension
