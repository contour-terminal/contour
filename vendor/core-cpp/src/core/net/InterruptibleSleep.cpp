// SPDX-License-Identifier: Apache-2.0
#include <core/net/InterruptibleSleep.hpp>

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/net/detail/ScopeGuard.hpp>

#include <coroutine>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>

namespace core::net
{

namespace
{

    /// A deadline park woken by a token the AWAITING COROUTINE did not supply.
    ///
    /// @c DelayAwaiter reads its token out of the parking coroutine's promise, which is right for
    /// `co_await loop.delay(...)` and not enough here: @c interruptibleSleepUntil is told which
    /// token to watch, and that token is commonly not the flow's. So this watches both — the
    /// supplied one and the flow's own — and reports which of them ended the wait.
    ///
    /// Both registrations point at the same @c EventLoop::requestCancel, which is idempotent per
    /// park: the second resolves a park whose waiter the first already took, and does nothing.
    class TokenDelayAwaiter
    {
      public:
        /// @param loop The loop to park on.
        /// @param deadline When to resume if nothing stops a token first.
        /// @param token The token the caller asked to be interrupted by.
        TokenDelayAwaiter(EventLoop& loop,
                          platform::SteadyTimePoint deadline,
                          async::StopToken token) noexcept:
            _loop(loop), _deadline(deadline), _token(std::move(token))
        {
        }

        TokenDelayAwaiter(TokenDelayAwaiter const&) = delete;
        TokenDelayAwaiter(TokenDelayAwaiter&&) = delete;
        TokenDelayAwaiter& operator=(TokenDelayAwaiter const&) = delete;
        TokenDelayAwaiter& operator=(TokenDelayAwaiter&&) = delete;
        ~TokenDelayAwaiter() = default;

        /// @return False: an elapsed deadline is answered by @c await_suspend, never here, as for
        ///         @c DelayAwaiter (fastcached#1546, `.agent/rules/async-and-net.md`).
        [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

        /// Parks the awaiting coroutine on the deadline and arms both stop callbacks.
        ///
        /// An elapsed deadline declines to park BEFORE the flow's token is read, so the answer is
        /// the one `await_ready` gave when it held this check: @c await_resume then consults only
        /// the supplied token, reporting `Cancelled` if a stop reached it and `Deadline` otherwise.
        /// @tparam Promise The awaiting coroutine's promise type.
        /// @param awaiting The coroutine performing the `co_await`.
        /// @return False (resume now) if the deadline has passed or either token is already
        ///         stopped; true to park.
        template <typename Promise>
        [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
        {
            if (_deadline <= _loop.clock().now())
                return false;
            if constexpr (async::HasStopToken<Promise>)
                _flowToken = awaiting.promise().stopToken();
            if (_token.stop_requested() || _flowToken.stop_requested())
                return false;
            // The park is filed BEFORE either stop callback exists, and nothing unregisters it if
            // an emplace below throws: `await_suspend` exiting by exception unwinds the awaiting
            // frame through its `co_await` without ever running `await_resume`, which is the only
            // other caller of `unregisterPark`. The loop would then hold a park naming storage
            // that is being destroyed. `ScopeGuard` has no dismiss, so the flag is what makes this
            // fire on the exceptional exit alone, and it is set after the LAST emplace because
            // this awaiter has two; `unregisterPark` on an invalid id is a no-op, which covers a
            // throw from `registerPark` itself.
            //
            // `noexcept` on the lambda is required, not decorative: `ScopeGuard`'s constraint is
            // `is_nothrow_invocable_v<Callable&>`, and an unmarked lambda fails it as a deduction
            // failure with no viable constructor rather than as a readable message.
            // `unregisterPark` is itself `noexcept`, so the marking is honest.
            //
            // **Unreachable on both mainline toolchains**, and this closes it rather than fixes a
            // live defect: each closure is an `EventLoop*` plus a `ParkId`, sixteen bytes, inside
            // the small-buffer optimisation of libstdc++'s and libc++'s `std::function`, so no
            // allocation happens and the throw cannot occur. Matched to `DelayAwaiter`'s guard in
            // `EventLoop.hpp` so the two cannot drift.
            auto registered = false;
            auto const undo = detail::ScopeGuard { [&]() noexcept {
                if (!registered)
                    _loop.unregisterPark(_park);
            } };
            _park =
                _loop.registerPark(ParkEntry::onDeadline(async::detail::parkedWorkFor(awaiting), _deadline));
            _tokenReg.emplace(_token, [&loop = _loop, park = _park] { loop.requestCancel(park); });
            _flowReg.emplace(_flowToken, [&loop = _loop, park = _park] { loop.requestCancel(park); });
            registered = true;
            return true;
        }

        /// @return Why the wait ended.
        /// @throws async::OperationCancelled if the FLOW's own token was stopped and the supplied
        ///         one was not. The order is what makes a caller that handed in its own token get
        ///         an answer rather than an exception.
        [[nodiscard]] WakeReason await_resume()
        {
            _tokenReg.reset();
            _flowReg.reset();
            _loop.unregisterPark(_park);
            if (_token.stop_requested())
                return WakeReason::Cancelled;
            if (_flowToken.stop_requested())
                throw async::OperationCancelled {};
            return WakeReason::Deadline;
        }

      private:
        std::optional<async::StopCallback<std::function<void()>>> _tokenReg;
        std::optional<async::StopCallback<std::function<void()>>> _flowReg;
        EventLoop& _loop;
        platform::SteadyTimePoint _deadline;
        async::StopToken _token;
        async::StopToken _flowToken;
        ParkId _park {};
    };

} // namespace

async::Task<WakeReason> interruptibleSleepUntil(EventLoop* loop,
                                                async::StopToken token,
                                                platform::SteadyTimePoint deadline)
{
    // Asked before anything is parked: a caller that is already stopped must not touch the loop at
    // all, or a stop issued during teardown files a park on a table that is about to be emptied.
    if (token.stop_requested())
        co_return WakeReason::Cancelled;

    // No loop means no deadline mechanism (the in-memory transport), and `sleepUntil` resolves
    // inline there. Reporting Deadline keeps the two consistent: the wait is over, and nothing
    // cancelled it.
    if (loop == nullptr)
        co_return WakeReason::Deadline;

    co_return co_await TokenDelayAwaiter { *loop, deadline, std::move(token) };
}

async::Task<WakeReason> interruptibleSleepUntil(EventLoop* loop,
                                                async::StopToken token,
                                                platform::SteadyTimePoint deadline,
                                                platform::SteadyDuration wakeBound)
{
    // The bound is read and dropped, which is what the header's @deprecated says: there is no poll
    // left for it to bound. Named rather than `/*wakeBound*/` so the reason is attached to a
    // parameter a caller can still see in the signature.
    std::ignore = wakeBound;
    co_return co_await interruptibleSleepUntil(loop, std::move(token), deadline);
}

} // namespace core::net
