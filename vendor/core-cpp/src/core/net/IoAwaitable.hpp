// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ResultAwaitable<R>` — the frame-free, stop-aware result of every socket operation, and
/// `IoAwaitable`, its byte-count spelling.
///
/// **Why an awaitable and not a `Task`.** A `Task` is a coroutine: awaiting one allocates a frame,
/// and a server holding one parked read per connection then pays a frame per connection for work
/// that is doing nothing but waiting. An awaitable allocates nothing — it lives in the AWAITING
/// coroutine's frame, which already exists.
///
/// **What that costs, stated where a caller meets it.** An awaitable is a one-shot temporary bound
/// to its `co_await` expression. It cannot be stored, moved into a container, held across a
/// suspension point or handed to @c async::whenAny, because nothing but the awaiting frame keeps
/// it alive. @c core::async::asTask is the escape hatch for a caller that needs one of those, and
/// it costs exactly the frame this design avoids.
///
/// **Why the retry loop is not in the awaiting coroutine.** `co_await` suspends exactly once, so
/// `await_resume` cannot re-park. A `write` of a buffer larger than the send window needs as many
/// writable edges as it takes, and a level-triggered poller may report a descriptor readable whose
/// `recv` still answers `EAGAIN` — so the loop has to run where the readiness is delivered. That is
/// the owner's @c core::net::ReadyCallback, and this type is the completion slot it writes into.
///
/// Origin: fastcached's `IoAwaitable` (`FastCache/Net/ISocket.hpp` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`), plus the cancellation semantics this library's
/// design spec §2 item 5 requires and which upstream's has none of.

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/NetError.hpp>
#include <core/net/detail/CompletionHook.hpp>
#include <core/net/detail/ParkId.hpp>

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <utility>

namespace core::net
{

/// The loop, named but not included.
///
/// `ISocket.hpp` includes this header, so whatever this header includes lands in every translation
/// unit that touches a socket. `<core/net/EventLoop.hpp>` was that, for ONE call --
/// `requestCancel` on the stop path below -- and `SocketContract.hpp` goes to the trouble of
/// forward-declaring the same class for the same reason. `ParkId` still has to be a complete type
/// here, and it comes from `detail/ParkId.hpp` rather than the park table for the same reason
/// again (core-cpp#43).
class EventLoop;

/// Requests cancellation of @p park on @p loop, out of line so this header need not include the
/// loop's.
///
/// Safe from any thread, which is the property the stop path needs: @c EventLoop::requestCancel is
/// generation-checked, so a request naming an operation that has already finished resolves to
/// nothing rather than to whatever took its place.
/// @param loop The loop the park belongs to.
/// @param park The park to cancel.
void requestCancelOn(EventLoop& loop, ParkId park) noexcept;

/// Takes @p waiter back out of @p loop's ready queue, where @c detail::resumeSoonOn put it, because its
/// frame is being destroyed before the loop reached it. Loop thread only.
/// @param loop The loop it was queued on.
/// @param waiter The coroutine to take back.
void cancelPendingOn(EventLoop& loop, std::coroutine_handle<> waiter) noexcept;

/// The result of one socket operation: a value, or why it could not be produced.
///
/// @tparam R What the operation produces — `std::size_t` for a byte transfer, `void` for a
///         handshake, `ReadWithFd` for a receive that may carry a descriptor.
template <typename R>
class ResultAwaitable
{
  public:
    /// What awaiting this resolves to.
    using Result = std::expected<R, NetError>;

    /// Arms the operation, from inside `await_suspend` and on the loop's thread.
    ///
    /// The owner records @p self as its in-flight operation and starts watching for readiness. It
    /// MAY complete inline — a decorator finding a full record already buffered does — in which
    /// case @c complete stores the answer and suppresses the resume, and `await_suspend` transfers
    /// straight back to the awaiting coroutine rather than resuming it re-entrantly.
    using ArmCallback = void (*)(void* owner, ResultAwaitable& self);

    /// Tells the owner to forget an operation that is still armed, because the awaiting flow is
    /// unwinding and @c self is about to be destroyed.
    ///
    /// **This has no counterpart upstream, and it is what makes the type stop-aware.** fastcached's
    /// `IoAwaitable` is never cancelled out from under its owner, so a socket there can hold a raw
    /// pointer to one for as long as it likes. Here a flow whose token is stopped unwinds through
    /// `await_resume`, and a socket still holding that pointer would write into a destroyed frame
    /// on the next readiness.
    ///
    /// @c awaitable identifies WHICH operation is going away, so an owner that has already
    /// retired this one and armed the next cannot be made to retire the next one instead. It is a
    /// `void*` rather than a typed reference because one owner serves every result type through one
    /// slot, and the only question it answers is identity.
    using RetireCallback = void (*)(void* owner, void* awaitable) noexcept;

    /// An operation that is already finished before it is awaited — the fast path, where the
    /// syscall succeeded without blocking, and the error paths that need no wait at all.
    /// @param result What awaiting resolves to.
    explicit ResultAwaitable(Result result) noexcept: _result(std::move(result)), _settled(true) {}

    /// An operation the owner will arm and later complete.
    /// @param arm Called from `await_suspend`; must not be null.
    /// @param retire Called if the flow unwinds while the operation is still armed; must not be
    ///        null.
    /// @param owner The opaque pointer both are handed. **Must outlive this AWAITABLE, not merely
    ///        the await.** The destructor retires an unsettled operation, so `retire(owner, this)`
    ///        runs even for an operation that was created and never awaited — which is reachable
    ///        through @c core::async::asTask, whose whole purpose is to let a caller hold one. A
    ///        connection struct that stores such a task and destroys its `unique_ptr<ISocket>`
    ///        first hands a dangling owner to the retire hook: the socket's destructor cannot
    ///        neutralise an operation it has no handle on, because nothing was ever written into
    ///        its slot. **So the order is the contract: destroy every operation on a socket before
    ///        the socket**, which for a struct member means declaring the task after the socket.
    ResultAwaitable(ArmCallback arm, RetireCallback retire, void* owner) noexcept:
        _arm(arm), _retire(retire), _owner(owner)
    {
    }

    /// An operation that is genuinely a coroutine, driven by awaiting @p task.
    ///
    /// **The escape hatch in the other direction from @c core::async::asTask, and it is for
    /// transports rather than for callers.** A raw socket's read is a syscall and a retry, which is
    /// why it needs no frame — but a DECORATOR's is not: a TLS read decrypts, may drive a
    /// handshake, and may park on a raw read of its own, which is a loop with state that outlives
    /// each step. Writing that as a hand-rolled state machine to save a frame would trade the one
    /// thing coroutines are for against an allocation the connection already paid for.
    ///
    /// The awaiting flow's stop token reaches @p task through `Task`'s own awaiter, so a
    /// coroutine-shaped transport is exactly as cancellable as a frame-free one — it just unwinds
    /// through its own `co_await`s instead of through a retire hook.
    /// @param task The operation. Consumed; its frame is owned by this awaitable and destroyed with
    ///        it, so it is safe for the caller to drop the awaitable unawaited.
    explicit ResultAwaitable(async::Task<Result> task) noexcept:
        _task(std::move(task).operator co_await()), _taskReady(_task->await_ready())
    {
    }

    ResultAwaitable(ResultAwaitable const&) = delete;
    ResultAwaitable& operator=(ResultAwaitable const&) = delete;

    /// Moves an operation that has NOT yet been awaited, and only such an one.
    ///
    /// **Both halves of that sentence are load-bearing.** Once `await_suspend` has run, this object
    /// is pointed at from three places — the owner's operation slot, the stop callback's capture,
    /// and the loop park's cancellation route — so moving it would leave every one of them naming
    /// the old address, and the symptom is a completion written into freed storage. Before it has
    /// been awaited there are no such pointers: the owner's slot is still null (the verb cleared it
    /// through @c contract::claimReadSlot), no callback is registered, and no park exists.
    ///
    /// It exists for exactly one caller, @c core::async::asTask, which has to move the operation
    /// into a coroutine frame before anything awaits it. Everything else takes the prvalue straight
    /// into the awaiting frame and never moves at all. Debug builds refuse the unsafe case rather
    /// than documenting it.
    /// @param other The operation to take over; left inert, so its destructor retires nothing.
    ResultAwaitable(ResultAwaitable&& other) noexcept:
        _result(std::move(other._result)),
        _task(std::move(other._task)),
        _arm(std::exchange(other._arm, nullptr)),
        _retire(std::exchange(other._retire, nullptr)),
        _owner(std::exchange(other._owner, nullptr)),
        _settled(other._settled),
        _taskReady(other._taskReady),
        _abandoned(other._abandoned)
    {
        assert(!other._waiter && !other._cancelReg.has_value() && !other._park
               && "a socket operation was moved after it had been awaited: the owner's slot, the "
                  "stop callback and the loop park all name its old address, so the completion "
                  "would be written into storage that has moved away (see core/net/IoAwaitable.hpp)");
    }

    ResultAwaitable& operator=(ResultAwaitable&&) = delete;

    /// **Retires an operation that was armed and never awaited.** `[[nodiscard]]` makes dropping a
    /// socket operation a warning rather than a mistake nobody sees, but a warning is not a
    /// guarantee: an owner claims its slot when the verb is called, and an awaitable destroyed
    /// before it is ever awaited would leave that slot pointing at freed storage. Reached also on
    /// the ordinary path, where @c await_resume has already retired and cleared the hook.
    ~ResultAwaitable()
    {
        if (!_settled)
            retireNow();
        // Settled and handed to the loop, and the frame is going away before the loop resumed it
        // -- the only way this object can be destroyed with `_queued` set, since `await_resume`
        // clears it. The ready queue must not keep a handle to a frame that no longer exists.
        if (_queued && _loop != nullptr)
            cancelPendingOn(*_loop, std::exchange(_queued, {}));
    }

    /// @return True when the operation already has its answer, so no suspension is needed.
    ///
    /// **A member read and nothing more**, because MSVC 19.44's ARM64 code generator drops the
    /// enclosing `try` of a `co_await` on a temporary awaiter whose `await_ready` makes a call
    /// ([fastcached#1546](https://github.com/LASTRADA-Software/fastcached/issues/1546)), and every
    /// `co_await sock->read(...)` is that shape. It asked the optional and then `Task`'s awaiter;
    /// neither is needed. A coroutine-backed operation is never settled (only @c complete and the
    /// value constructor set the flag, and neither is its path); whether its task has nothing to
    /// suspend for is `Task`'s awaiter's answer, which that awaiter decides in its constructor and
    /// the constructor here copies into @c _taskReady.
    [[nodiscard]] bool await_ready() const noexcept { return _settled || _taskReady; }

    /// Captures the awaiting flow's stop token, arms the operation, and parks unless the owner
    /// answered inline.
    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return @p awaiting to resume at once — already cancelled, nothing to arm, or completed
    ///         inline — `std::noop_coroutine()` to stay parked until @c complete runs, and for a
    ///         coroutine-backed operation whatever `Task`'s own awaiter transfers to.
    ///
    /// A handle rather than a `bool`, so a coroutine-backed operation starts its task by SYMMETRIC
    /// TRANSFER rather than by a nested `resume()`. A nested resume grows the stack by one frame
    /// per decorator per operation, and a TLS pump over a buffered reader is three deep before any
    /// application code appears.
    template <typename Promise>
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        if (_task.has_value())
            return _task->await_suspend(awaiting);

        _waiter = awaiting;
        _unownedRoot = async::detail::unownedRootOf(awaiting);
        if constexpr (async::HasStopToken<Promise>)
            _token = awaiting.promise().stopToken();

        // Checked BEFORE arming, so a flow that is already cancelled never leaves an operation
        // registered at an owner that nothing will come back to retire.
        if (_token.stop_requested() || _arm == nullptr)
            return awaiting;

        // **Two phases, and a throw between them must not leak the operation.** The owner records
        // this object as its in-flight operation, and only then is the stop callback registered --
        // so an exception out of either half leaves an armed operation with no one coming back for
        // it. That is the shape that leaked a park in `InterruptibleSleep`
        // ([core-cpp 8d7b8b2](https://github.com/contour-terminal/core-cpp/commit/8d7b8b2)), and it
        // is closed here by the DESTRUCTOR rather than by a `try`: `await_suspend` exiting through
        // an exception destroys this object as the awaiting frame unwinds, and `~ResultAwaitable`
        // retires whatever is still armed. `_arming` is cleared through a guard so the throwing
        // path cannot leave it set -- a later `complete` would otherwise see an arm still in
        // progress and suppress the resume it is there to perform.
        //
        // While `_arming` is set, `complete` records the answer and does NOT resume: resuming from
        // inside `await_suspend` is undefined behaviour, and transferring back to `awaiting` below
        // reaches `await_resume` by the normal path instead.
        {
            _arming = true;
            auto const clearArming = ArmingGuard { &_arming };
            _arm(_owner, *this);
        }
        if (_settled)
            return awaiting;

        // Registered only once the operation is genuinely parked, and only after `cancelThrough`
        // has been given the park — so the callback, which may run on any thread, reads a park id
        // that was written before it could possibly fire.
        //
        // Not at all where the token can never be stopped -- a detached flow's, which is every
        // connection a server spawns: there is no source to fire it, and the registration would be
        // a `std::function` built and torn down once per parked operation for nothing.
        if (_token.stop_possible())
            _cancelReg.emplace(_token, [this] { onStop(); });
        return std::noop_coroutine();
    }

    /// @return The operation's answer.
    /// @throws async::OperationCancelled if the awaiting flow's OWN stop token was stopped and the
    ///         operation produced no value.
    ///
    /// **The two cancellations are different facts and are reported differently** (design spec §2
    /// item 5). A cancel from the FLOW throws, because the flow is being unwound and its `co_await`
    /// has no sensible value to hand back. A cancel from the RESOURCE — `close()`, `cancelRead()`,
    /// a closed listener — is a `NetErrorCode::Cancelled` VALUE, because the flow is alive and
    /// asked a question about a socket that has gone away. Collapsing them makes a closed socket
    /// indistinguishable from a cancelled flow.
    ///
    /// **A value beats a stop that arrived after it**
    /// ([fastcached#884](https://github.com/LASTRADA-Software/fastcached/issues/884)). A receive
    /// that already took bytes out of the stream cannot un-take them, so a stop landing afterwards
    /// must not discard them: those bytes exist nowhere else. Hence the value is tested first and
    /// the token second, rather than the other way round.
    [[nodiscard]] Result await_resume()
    {
        // A coroutine-backed operation answers through its own frame, which has already observed
        // whatever cancellation reached it: there is no slot at an owner to retire and no token to
        // re-read here.
        if (_task.has_value())
            return _task->await_resume();

        // The loop has resumed this frame, so the queue entry `complete` filed is gone.
        _queued = {};

        // Dropped first: it blocks until a callback running on another thread has finished, so
        // nothing below can race one, and the owner hooks below must not be reached from it.
        _cancelReg.reset();

        // Still armed means the flow is unwinding without the operation ever answering — a stop
        // observed before arming, or a teardown that resumed this park. The owner is still holding
        // a pointer to this object, which is about to stop existing.
        if (!_settled)
        {
            retireNow();
            // Worded only here, where it is read: see `_result`.
            _result = std::unexpected(
                makeNetError(NetErrorCode::Cancelled, 0, "the operation was never completed"));
        }

        // An ABANDONED operation unwinds whatever the token says: the owner is being destroyed, so
        // resuming this flow on its normal path would run its body against storage that has gone.
        // This is `FdWakePolicy::Cancel`'s meaning, in the shape an awaitable can express it.
        if (_abandoned)
            throw async::OperationCancelled {};
        if (_result.has_value())
            return std::move(_result);
        if (_token.stop_requested())
            throw async::OperationCancelled {};
        return std::move(_result);
    }

    /// Publishes the operation's answer at once, and hands the awaiting flow to the loop.
    ///
    /// **Called by the owner, on the loop's thread**, from its readiness callback, from `close()`,
    /// `cancelRead()` or its destructor.
    ///
    /// **It settles now and resumes LATER, in the loop's drain step** -- guarantee G2, and the rule
    /// that a resource never resumes its consumer inline. Until 0.2.1 it resumed the waiter right
    /// here, so a `close()` ran the closed read's flow before `close()` returned; that flow could
    /// run to its end and destroy whatever was still calling `close()`. contour did exactly that:
    /// `NativeClient::detach` is `_writer.close(); _connection->close();`, the first close resumed
    /// the client's read flow, the flow destroyed the client, and the second statement called
    /// through freed storage.
    ///
    /// **Handed over with the chain's root**, which `await_suspend` knew how to find and this
    /// function, with a type-erased handle, does not: a chain nobody owns -- a @c DetachedTask --
    /// is the loop's to FREE if the loop is destroyed before it resumes it, and one handed over as
    /// a bare handle was filed as borrowed and resumed instead, running the rest of its body on a
    /// loop being torn down. The loop claims the root when it queues the waiter, with the
    /// two-atomic @c async::detail::CountedClaim rather than a refcounted work item, because this
    /// is the hot path of every socket operation that parks.
    ///
    /// The loop is the one the owner named through @c cancelThrough. An owner that named none -- a
    /// loop-less test double such as @c testing::InMemorySocket, or a transport written before
    /// 0.2.1 -- has nowhere to defer to, and its waiter is resumed here as before, which is why
    /// such an owner still detaches the operation first and calls this last.
    ///
    /// **A readiness completion resumes its waiter before anything queued after the readiness
    /// callback**: called from a callback the loop's drain step runs, the waiter resumes right after
    /// that callback returns, in the same drain (see @c EventLoop::resumeSoon).
    /// @param result What the operation produced.
    void complete(Result result) noexcept
    {
        _result = std::move(result);
        _settled = true;
        if (_arming)
            return; // inline completion; `await_suspend` transfers back and resumes normally
        auto const waiter = std::exchange(_waiter, {});
        if (!waiter || waiter.done())
            return;
        if (_loop != nullptr)
        {
            _queued = waiter;
            detail::resumeSoonOn(*_loop, waiter, _unownedRoot);
            return;
        }
        waiter.resume();
    }

    /// Publishes that the operation's OWNER is going away, and resumes the awaiting flow so it
    /// unwinds.
    ///
    /// **The destructor's counterpart to @c complete.** `close()` completes with a
    /// @c NetErrorCode::Cancelled value, because the socket is still there for the resumed flow to
    /// look at. A DESTRUCTOR cannot offer that: by the time the flow runs, `this` is gone — so the
    /// flow must not resume on its normal path at all, and `await_resume` throws
    /// @c async::OperationCancelled whatever the flow's own token says. Unwinding never re-enters
    /// the body, which is the only safe thing to do with a frame whose socket has been destroyed.
    ///
    /// Same ordering rule as @c complete: it resumes, so it is the last thing its caller does.
    void abandon() noexcept
    {
        _abandoned = true;
        complete(std::unexpected(
            makeNetError(NetErrorCode::Cancelled, 0, "the socket was destroyed under this operation")));
    }

    /// Says how a stop arriving on ANY thread reaches this operation: through the loop's
    /// generation-checked cancel, naming the park the owner registered for it.
    ///
    /// **Called by the owner from its arm hook**, which is on the loop's thread and strictly before
    /// the stop callback can be registered — which is what makes the park id safe to read from
    /// another thread without it being atomic.
    ///
    /// Routed through the loop rather than straight back into the owner because a stop token may be
    /// stopped from a watchdog, a signal handler or a peer's thread, and a socket's members are the
    /// loop thread's alone. @c EventLoop::requestCancel is safe from any thread, and its
    /// never-reused park ids mean a request naming an operation that has already finished resolves
    /// to nothing rather than to whatever took its place.
    /// @param loop The loop the park belongs to.
    /// @param park The park to cancel; @c ParkId::invalid() leaves the operation uncancellable.
    void cancelThrough(EventLoop& loop, ParkId park) noexcept
    {
        _loop = &loop;
        _park = park;
    }

    /// @return Whether the operation has already produced its answer. An owner asks before
    ///         completing a second time, and a stop path asks before cancelling something that
    ///         has already won.
    [[nodiscard]] bool settled() const noexcept { return _settled; }

  private:
    /// Clears the arming flag however the arm hook leaves -- normally or through an exception.
    ///
    /// A four-line type rather than `core::net::detail::ScopeGuard` because that one is constrained
    /// `requires std::is_nothrow_invocable_v<Callable&>` and lives in a header this one does not
    /// otherwise need; the whole job here is one store.
    class ArmingGuard
    {
      public:
        /// @param flag The flag to clear on destruction; must outlive this.
        explicit ArmingGuard(bool* flag) noexcept: _flag(flag) {}
        ArmingGuard(ArmingGuard const&) = delete;
        ArmingGuard(ArmingGuard&&) = delete;
        ArmingGuard& operator=(ArmingGuard const&) = delete;
        ArmingGuard& operator=(ArmingGuard&&) = delete;
        ~ArmingGuard() { *_flag = false; }

      private:
        bool* _flag;
    };

    /// Tells the owner to forget this operation, exactly once.
    void retireNow() noexcept
    {
        if (auto* const retire = std::exchange(_retire, nullptr); retire != nullptr)
            retire(_owner, this);
    }

    /// Runs on whichever thread stopped the token; touches nothing but the two values
    /// @c cancelThrough wrote on the loop's thread before this could be armed.
    void onStop() const noexcept
    {
        if (_loop != nullptr && _park)
            requestCancelOn(*_loop, _park);
    }

    /// Defaults to a cancellation rather than to a value: an awaitable resumed without its owner
    /// ever answering — a loop torn down under a parked operation — must report something true,
    /// and "the operation did not happen" is it.
    ///
    /// **With no words until they are read.** `NetError::context` is a `std::string`, and this
    /// initialiser runs for every operation that parks: its sentence, longer than any standard
    /// library's inline capacity, was a heap allocation and a free per parked operation -- per
    /// request, on a server -- for a value read only by `await_resume` on the never-completed path,
    /// which words it there.
    Result _result { std::unexpected(makeNetError(NetErrorCode::Cancelled)) };

    /// Set only for a coroutine-backed operation, and then it is the whole implementation: every
    /// other member below belongs to the frame-free path and stays at its default.
    std::optional<typename async::Task<Result>::Awaiter> _task;

    std::optional<async::StopCallback<std::function<void()>>> _cancelReg;
    std::coroutine_handle<> _waiter {};

    /// The root of @c _waiter's chain where nobody owns it, or empty; set with it, because only
    /// `await_suspend` knows the promise type that answers it, and handed to the loop by
    /// @c complete, which claims it there.
    std::coroutine_handle<> _unownedRoot {};

    /// The waiter @c complete handed to the loop, until `await_resume` runs; the destructor takes it
    /// back if the frame is destroyed first.
    std::coroutine_handle<> _queued {};
    async::StopToken _token;
    ArmCallback _arm = nullptr;
    RetireCallback _retire = nullptr;
    void* _owner = nullptr;
    EventLoop* _loop = nullptr;
    ParkId _park {};
    bool _settled = false;

    /// A coroutine-backed operation whose task has nothing to suspend for -- it owns no frame -- as
    /// `Task`'s awaiter found it when this was made. `Task`'s @c await_suspend is reached only where
    /// its @c await_ready answered false, so this must be asked first.
    bool _taskReady = false;

    /// Set by @c abandon: the owner is being destroyed, so `await_resume` unwinds rather than
    /// reporting a value the flow would act on.
    bool _abandoned = false;

    /// True only while `await_suspend` is running the arm hook, so a synchronous @c complete
    /// records its answer and leaves the resume to `await_suspend`, which transfers back to the
    /// awaiting coroutine instead of parking.
    bool _arming = false;
};

/// The awaitable every byte transfer resolves through: a count, or a @c NetError.
using IoAwaitable = ResultAwaitable<std::size_t>;

} // namespace core::net
