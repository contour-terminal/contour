// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::ParkedCall<T>` — one blocking call's rendezvous between whatever runs it and the
/// coroutine on a loop that waits for its answer, with a stop that ends the wait and not the call.
///
/// Shared by every resolver that hands `getaddrinfo` to somebody else's thread
/// (@c ThreadedAddressResolver, @c ExecutorAddressResolver), by the TLS socket waiting for its chain
/// verifier, and shaped for any other call that cannot be interrupted once it has started.

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>

#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace core::net::detail
{

/// Where a parked call stands. One state rather than an `answered` and a `cancelled` flag, because
/// the waiter is handed back exactly once and so exactly one of the two outcomes reached it.
enum class SlotState : std::uint8_t
{
    Pending,   ///< Nobody has decided yet; a waiter, if there is one, is parked.
    Answered,  ///< The answer took the waiter (or arrived before there was one) and will be delivered.
    Cancelled, ///< A stop took the waiter first; the flow throws, and a later answer is only kept.
};

/// One call's rendezvous between the thread that runs it and the coroutine that waits for it.
///
/// Held by `std::shared_ptr`, because either side may reach it last: the runner must be able to
/// publish into it after the waiting flow was abandoned, and the flow must be able to read it after
/// the runner is gone.
///
/// **Whoever takes the waiter first decides the outcome**, and the other finds it empty. @c settle
/// takes it to deliver an answer, a stop takes it to deliver a cancellation, and both do so under
/// the one mutex — so exactly one of them hands the coroutine back, and the loser's news goes
/// nowhere. That is what makes an answer arriving after a stop harmless: the frame it would have
/// been delivered into may already be gone. Such an answer is still stored, and an owner who keeps
/// the call (a stream whose bytes must not be lost) takes it with @c takeLateAnswer.
///
/// **Nothing here resumes inline.** The waiter is SUBMITTED to the executor it parked on, so the
/// flow continues on the loop's thread and never on the runner's, or the stopper's.
///
/// **A stopped wait can be joined again** (@c rejoin): an owner that keeps the call -- a TLS socket
/// whose handshake is driven again after a cancel -- waits for the same call rather than starting a
/// second one.
///
/// **Settling never throws.** It runs where nothing can take an exception -- a detached flow, a
/// destructor, a stop callback -- so the one call that can fail, the loop's `submit` running out of
/// memory, ends the process (`noexcept`): a waiter that cannot be handed back can be neither resumed
/// nor freed safely from here.
/// @tparam T What the call answers.
template <typename T>
class ParkedCall
{
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "settling moves the answer where nothing can take a throw");

  public:
    /// @param executor Where the waiter is handed back: the loop the waiting flow runs on. Null only
    ///        where nothing ever parks on the call; a waiter with nowhere to go is freed rather than
    ///        leaked (@c async::detail::Parked).
    explicit ParkedCall(async::IExecutor* executor) noexcept: _executor(executor) {}

    /// Publishes the answer and hands the waiter back to the loop.
    ///
    /// The first answer wins; a second is ignored. After a stop the answer is kept but delivered
    /// to nobody.
    /// @param answer What the call produced.
    void settle(T answer) noexcept
    {
        auto waiter = async::detail::Parked {};
        {
            auto const guard = std::scoped_lock { _mutex };
            if (_answer.has_value())
                return;
            _answer.emplace(std::move(answer));
            if (_state == SlotState::Pending)
                _state = SlotState::Answered;
            waiter = async::detail::Parked { std::exchange(_waiter, async::ParkedWork {}) };
        }
        handBack(std::move(waiter));
    }

    /// The stop callback of a parked call: takes the waiter back and hands it to the loop,
    /// cancelled.
    ///
    /// **It may run on any thread** — the loop's, a whenAny sibling's, a watchdog's — so it does
    /// what @c settle does and nothing more. A named functor holding one plain pointer rather than
    /// a `std::function`, so registering it allocates nothing. The call outlives it: the awaiter
    /// that owns this callback also holds the call's `shared_ptr`, and drops the callback first.
    struct Cancel
    {
        ParkedCall* call = nullptr;

        void operator()() const noexcept
        {
            auto waiter = async::detail::Parked {};
            {
                auto const guard = std::scoped_lock { call->_mutex };
                // An answer that got here first is delivered, and a stop after it changes nothing:
                // the flow is already on its way back with a result it can use.
                if (call->_state != SlotState::Pending)
                    return;
                call->_state = SlotState::Cancelled;
                waiter = async::detail::Parked { std::exchange(call->_waiter, async::ParkedWork {}) };
            }
            call->handBack(std::move(waiter));
        }
    };

    /// Suspends until the call is settled, or until the awaiting flow is stopped.
    ///
    /// Race-free the way @c ResultAwaitable is: the answer check and the waiter registration
    /// happen under one lock, so an answer landing between them resumes through the normal path
    /// rather than parking on an answer that has already arrived.
    struct Awaiter
    {
        std::shared_ptr<ParkedCall> call;
        std::optional<async::StopCallback<Cancel>> cancelReg {};

        /// @return False. Whether the answer is already there is @c await_suspend's first question,
        ///         asked under the call's mutex: taking a lock is a call, and MSVC 19.44's ARM64
        ///         code generator drops the enclosing `try` of a `co_await` on a temporary awaiter
        ///         whose `await_ready` makes one (fastcached#1546, `.agent/rules/async-and-net.md`).
        [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

        /// Templated on the promise so `parkedWorkFor` can ask the PARKING coroutine's own promise
        /// whether anything else owns its chain: @c settle hands this chain to a loop that may be
        /// destroyed before it runs it, so this is the last place the question can be asked. The
        /// same promise is where the flow's stop token is read.
        ///
        /// **An answer already there resumes at once, before the stop callback exists**, so a stop
        /// that has already fired cannot turn a finished call into a cancellation.
        ///
        /// **The stop callback is registered BEFORE the waiter is published.** A stop landing in
        /// between then finds no waiter, records @c SlotState::Cancelled, and the check below sees
        /// it and does not park; registered after, a stop that had already fired would run the
        /// callback inline, find the waiter, and hand it to the loop while this function is still
        /// deciding whether to suspend — the same frame queued and returned-into at once.
        /// @tparam Promise The suspending coroutine's promise type.
        /// @param handle The suspended flow.
        /// @return True to stay suspended; false when the answer or the stop arrived first.
        template <typename Promise>
        [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            {
                auto const guard = std::scoped_lock { call->_mutex };
                if (call->_state == SlotState::Answered)
                    return false;
            }
            if constexpr (async::HasStopToken<Promise>)
                cancelReg.emplace(handle.promise().stopToken(), Cancel { .call = call.get() });

            auto const guard = std::scoped_lock { call->_mutex };
            if (call->_state != SlotState::Pending)
                return false;
            call->_waiter = async::detail::parkedWorkFor(handle);
            return true;
        }

        /// @return The call's answer.
        /// @throws async::OperationCancelled if the flow was stopped before the answer arrived.
        [[nodiscard]] T await_resume()
        {
            // Dropped first: it waits for a callback running on another thread to finish, so the
            // call is not read while that callback is still writing it.
            cancelReg.reset();
            auto const guard = std::scoped_lock { call->_mutex };
            if (call->_state == SlotState::Cancelled)
                throw async::OperationCancelled {};
            return std::move(*call->_answer);
        }
    };

    /// @param call The call to wait for; the awaiter keeps it alive.
    /// @return What a flow `co_await`s to wait for @p call's answer.
    [[nodiscard]] static Awaiter wait(std::shared_ptr<ParkedCall> call) noexcept
    {
        return Awaiter { .call = std::move(call) };
    }

    /// Takes the answer that arrived after a stop had already handed the waiter back.
    ///
    /// For an owner that must not lose it -- bytes a blocking read returned after its reader gave
    /// up are the next read's, not nobody's. An answer that was delivered is never here.
    /// @return The late answer, once; nothing if the call was not stopped or has not answered yet.
    [[nodiscard]] std::optional<T> takeLateAnswer() noexcept
    {
        auto const guard = std::scoped_lock { _mutex };
        if (_state != SlotState::Cancelled)
            return std::nullopt;
        return std::exchange(_answer, std::nullopt);
    }

    /// Makes a call whose wait a stop ended waitable again, for an owner that joins it once more
    /// rather than starting the work a second time.
    ///
    /// An answer that arrived after the stop is then delivered to the next @c wait at once; without
    /// one, the next waiter parks as the first did. A call that was not stopped is left as it is.
    void rejoin() noexcept
    {
        auto const guard = std::scoped_lock { _mutex };
        if (_state == SlotState::Cancelled)
            _state = _answer.has_value() ? SlotState::Answered : SlotState::Pending;
    }

  private:
    /// Hands @p waiter back to the executor, or frees it where there is nowhere to hand it.
    ///
    /// An `async::detail::Parked` rather than a bare `ParkedWork`, so the branch that hands nothing
    /// on FREES an unowned chain instead of dropping it — `ParkedWork`'s contract, resumed or freed,
    /// never neither. A BORROWED handle's claim is empty, so the guard destroys nothing and the
    /// caller's owner still frees it.
    /// @param waiter What was taken out of the call; may be empty.
    void handBack(async::detail::Parked waiter) noexcept
    {
        if (waiter.handle() && _executor != nullptr)
            _executor->submit(waiter.take());
    }

    async::IExecutor* _executor;
    std::mutex _mutex;
    std::optional<T> _answer;
    async::ParkedWork _waiter {};
    SlotState _state = SlotState::Pending;
};

} // namespace core::net::detail
