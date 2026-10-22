// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <expected>
#include <type_traits>

#include <vtextension/ExtensionError.hpp>

// WAMR's opaque module-instance type (wasm_module_inst_t), named here so that this header does not
// pull wasm_export.h into every consumer.
struct WASMModuleInstanceCommon;

namespace vtextension
{

/// Names the module instance a deadline guards: the instance the watchdog terminates on expiry.
struct WatchToken
{
    WASMModuleInstanceCommon* instance = nullptr; ///< The guarded WAMR module instance.

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

    /// Arms a deadline: unless disarmed first, the token's instance is terminated after @p timeout.
    /// Arming a token that is already armed replaces its deadline.
    /// @param token The instance to guard; its instance must not be null.
    /// @param timeout How long from now the instance may keep running.
    virtual void armDeadline(WatchToken token, std::chrono::milliseconds timeout) = 0;

    /// Disarms the token's deadline. Once this returns, the instance is not terminated on that
    /// deadline's account.
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
