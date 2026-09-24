// SPDX-License-Identifier: Apache-2.0
#include <core/net/ReadinessDial.hpp>

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/DeadlineTimer.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/detail/DialPrimitives.hpp>
#include <core/net/detail/ScopeGuard.hpp>

#include <coroutine>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace core::net::detail
{

namespace
{
    /// How an outstanding dial ended.
    ///
    /// **`Ready` does not mean "connected".** It means the loop reported readiness and nothing
    /// else; the connection's fate is read out of `SO_ERROR` afterwards, which is the whole of
    /// Ruling R101. Deliberately NOT a bit per callback: which of readable, writable and failed
    /// the kernel picks is not portable, and a dial that distinguished them would only be a way
    /// to miss one.
    enum class DialOutcome : std::uint8_t
    {
        Pending,   ///< Still outstanding.
        Ready,     ///< The loop reported readiness; ask `SO_ERROR` what it was.
        TimedOut,  ///< This candidate's deadline elapsed first.
        Cancelled, ///< The awaiting flow's own stop token was stopped.
        Abandoned, ///< The loop is going away under this dial.
        Refused,   ///< The loop would not watch the handle at all.
    };

    /// Per-dial state, living in the dialling coroutine's own frame.
    ///
    /// **In the frame rather than in a connector, and that is a property rather than a
    /// convenience** (fastcached's `Detail::ReadinessDialOp`): its address is stable for exactly
    /// as long as the loop can reach it, and it disappears with the attempt. A connector holding
    /// a slot per dial would let a dial that timed out while still in flight tie one up.
    struct DialOp
    {
        EventLoop* loop = nullptr;
        ParkId park {};
        async::ParkedWork waiter {};
        DialOutcome outcome = DialOutcome::Pending;

        /// Armed only while the dial is outstanding, and on the loop's ONE deadline heap rather
        /// than a wake this dial computes for itself.
        std::optional<DeadlineTimer> timer {};

        /// How a stop arriving on ANY thread reaches this park; see @c ResultAwaitable::onStop,
        /// which routes the same way and for the same reason.
        std::optional<async::StopCallback<std::function<void()>>> cancelReg {};
    };

    /// Publishes an outcome, retires the park, and resumes the dialling coroutine.
    ///
    /// **The park is unregistered FIRST, and the reason is the SPIN rather than the lifetime.**
    /// Once the op has settled its callback returns immediately, so a level-triggered backend
    /// still reporting the handle would dispatch, be ignored, and report again on the very next
    /// wait — a busy loop for as long as the registration stands.
    ///
    /// **And it resumes, so it is the last thing its caller does.** The resumed coroutine runs to
    /// its end and destroys the frame this op lives in, so nothing here may touch @p op
    /// afterwards.
    ///
    /// **@c DialOp::park is NOT cleared here, and the omission is deliberate.** The stop callback
    /// reads it and may run on any thread; @c ResultAwaitable is safe to read its own park id
    /// unatomically only because that id is written ONCE, on the loop's thread, strictly before
    /// the callback can be registered. Clearing it here would be a SECOND write to a non-atomic
    /// field that another thread reads, which is a data race by the memory model however narrow
    /// the window.
    ///
    /// **How narrow, measured rather than guessed:** an ordinary cross-thread stop is not it — the
    /// callback's read happens-before the settle it provokes, through the loop's inbound queue —
    /// so the two can only overlap when the dial settles for its OWN reason (a deadline, an
    /// arriving readiness) in the few instructions a concurrent stop is inside the callback. An
    /// earlier version of this function did clear it, and 30 ThreadSanitizer runs of a deadline
    /// deliberately timed against a cross-thread stop did not report it. So this is hardening
    /// against a race that is real on paper and was not provoked, and it costs nothing: the loop's
    /// park ids are never reused, so a later @c requestCancel naming a retired one resolves to
    /// nothing rather than to whatever took its place.
    /// @param op The dial to settle.
    /// @param outcome What happened.
    void settleDial(DialOp& op, DialOutcome outcome) noexcept
    {
        if (op.outcome != DialOutcome::Pending)
            return;
        op.outcome = outcome;

        if (op.park)
            op.loop->unregisterPark(op.park);

        auto waiter = async::detail::Parked { std::exchange(op.waiter, async::ParkedWork {}) };
        if (waiter.handle())
            waiter.resume();
    }

    /// The loop's readiness callback for an outstanding dial.
    /// @param state The @c DialOp, as a `void*`.
    /// @param wake Why the park woke.
    void onDialWake(void* state, ParkWake wake)
    {
        auto& op = *static_cast<DialOp*>(state);
        switch (wake)
        {
            case ParkWake::Ready: settleDial(op, DialOutcome::Ready); return;
            case ParkWake::Cancelled: settleDial(op, DialOutcome::Cancelled); return;
            case ParkWake::Abandoned: settleDial(op, DialOutcome::Abandoned); return;
        }
    }

    /// The deadline's callback: this candidate ran out of time.
    /// @param state The @c DialOp, as a `void*`.
    void onDialDeadline(void* state)
    {
        settleDial(*static_cast<DialOp*>(state), DialOutcome::TimedOut);
    }

    /// Suspends until a dial settles.
    ///
    /// Deliberately NOT a result-carrying awaitable: the awaiting frame OWNS the op it reads the
    /// outcome from, so there is nothing for one to carry.
    struct DialPark
    {
        DialOp* op = nullptr;
        DialHandles const* handles = nullptr;
        platform::SteadyTimePoint deadline = platform::SteadyTimePoint::max();

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        /// @tparam Promise The dialling coroutine's promise type.
        /// @param awaiting The coroutine performing the `co_await`.
        /// @return True to stay parked; false to resume at once, with @c DialOp::outcome already
        ///         saying why.
        template <typename Promise>
        [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
        {
            auto token = async::StopToken {};
            if constexpr (async::HasStopToken<Promise>)
                token = awaiting.promise().stopToken();

            // Checked BEFORE arming, so a flow that is already cancelled never leaves a
            // registration at a loop that nothing will come back to retire.
            if (token.stop_requested())
            {
                op->outcome = DialOutcome::Cancelled;
                return false;
            }

            // **`Interest::Write` and nothing else.** A watched direction always wins on every
            // backend, so a failed connect that the kernel reports as an error wakes this park
            // too; `onError` would only be a second route to the same callback. What the dial
            // then does is ask `SO_ERROR`, never which callback ran.
            op->park = op->loop->registerPark(ParkEntry::onReadyCallback(
                &onDialWake, op, handles->readiness, handles->kind, Interest::Write));
            if (!op->park)
            {
                op->outcome = DialOutcome::Refused;
                return false;
            }

            op->waiter = async::detail::parkedWorkFor(awaiting);

            // Armed AFTER the park, so a deadline that has already passed cannot settle the op
            // before there is anything to retire. `DeadlineTimer` never fires inline, so this
            // cannot re-enter `settleDial` from here either.
            if (deadline != platform::SteadyTimePoint::max())
                op->timer.emplace(*op->loop, deadline, &onDialDeadline, op);

            // Registered only once the park exists, so the callback — which may run on any thread
            // — reads an id that was written before it could possibly fire, and that nothing
            // writes again (see @c settleDial). `EventLoop::requestCancel` is itself safe from any
            // thread, and its never-reused ids mean a request naming a park that has already been
            // retired resolves to nothing.
            op->cancelReg.emplace(token, [op = op] {
                if (op->park)
                    op->loop->requestCancel(op->park);
            });
            return true;
        }

        void await_resume() const noexcept
        {
            // Dropped first: it blocks until a callback running on another thread has finished,
            // so nothing the dial does next can race one.
            op->cancelReg.reset();
            op->timer.reset();
        }
    };

} // namespace

async::Task<SocketResult> dialReadiness(EventLoop* loop,
                                        ResolvedEndpoint endpoint,
                                        platform::SteadyTimePoint deadline,
                                        StreamSocketOptions options)
{
    // Before the socket exists: a loop that could not adopt the result refuses the dial with
    // nothing on the wire (core-cpp#6).
    if (auto const dialable = dialableOn(*loop); !dialable.has_value())
        co_return std::unexpected(dialable.error());

    auto opened = openDialSocket(endpoint);
    if (!opened.has_value())
        co_return std::unexpected(opened.error());

    auto handles = *opened;
    // Closes whatever is LEFT when this frame goes — an `OperationCancelled` thrown out of the
    // park included, which is the path that would otherwise leak a descriptor per cancelled dial.
    // `adoptDialled` empties `handles`, so the success path disarms this by having nothing to
    // close rather than by a flag somebody can forget to set.
    auto const discard = ScopeGuard { [&]() noexcept { closeDialSocket(loop, handles); } };

    // Before the connect, while the window scale can still take the receive buffer into account.
    applySocketBufferSizes(handles.socket, options.buffers);
    auto const started = beginConnect(handles, endpoint);
    if (!started.has_value())
        co_return std::unexpected(started.error());

    if (*started == ConnectProgress::Pending)
    {
        auto op = DialOp { .loop = loop };
        co_await DialPark { .op = &op, .handles = &handles, .deadline = deadline };

        switch (op.outcome)
        {
            case DialOutcome::Ready: break;
            case DialOutcome::TimedOut:
                co_return std::unexpected(
                    makeNetError(NetErrorCode::Timeout, 0, "the connect deadline elapsed"));
            case DialOutcome::Refused:
                co_return std::unexpected(
                    makeNetError(NetErrorCode::SystemError, 0, "the event loop refused to watch this dial"));
            case DialOutcome::Cancelled:
            case DialOutcome::Abandoned:
                // A cancel from the FLOW unwinds rather than reporting a value, which is the same
                // answer `ResultAwaitable::await_resume` gives: the flow is being torn down and
                // its `co_await` has no sensible socket to hand back. The guard above closes the
                // descriptor on the way out.
                throw async::OperationCancelled {};
            case DialOutcome::Pending: break; // unreachable: the park resumes only once settled
        }
    }

    // **Readiness is not success, and this is the only thing that tells them apart.** A refused
    // connect also makes the socket ready; on macOS it arrives as a WRITABLE event, so a dial
    // that trusted the callback would report success and discover otherwise on its first write.
    if (auto const settled = pendingSocketError(handles); !settled.has_value())
        co_return std::unexpected(settled.error());

    applyStreamSocketOptions(handles.socket, options.keepAlive);

    // The peer string is the ADDRESS rather than the requested host: it feeds a connection's log
    // prefix, which records the address, and that is how the accept path already formats it.
    co_return adoptDialled(*loop, handles, formatPeerAddress(endpoint));
}

} // namespace core::net::detail
