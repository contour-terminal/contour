// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::IocpOperation` — one overlapped operation an OWNER issues on a completion port, and
/// the two things every owner does with one: park until it completes, and ask what it did.
///
/// **Why the owner parks rather than being called back.** fastcached's `IocpCompletion` carried a
/// dispatch pointer the reactor called from inside its dequeue, and that call resumed the awaiting
/// coroutine. Here a backend may only ENQUEUE (Rule 1,
/// [fastcached#475](https://github.com/LASTRADA-Software/fastcached/issues/475)), so a completion
/// reaches its owner exactly as readiness does: the owner registers a loop park whose handle is
/// the operation (@c HandleKind::Completion), the backend reports that park readable when the
/// packet is dequeued, and the loop runs the owner's callback in turn step 2. Cancellation,
/// teardown and the `whenAny` stop route are then the loop's own, and nothing about them had to
/// be written a second time for completions.
///
/// **The one thing the backend still calls is @c IocpOperation::onDequeued, and it only lets
/// go.** It runs last, after which the backend never touches the pointer again, and it is where
/// an owner gives back the share that kept the operation alive while the kernel held it — the
/// operation holds itself, never the socket
/// ([fastcached#465](https://github.com/LASTRADA-Software/fastcached/issues/465)).

// winsock2.h MUST precede windows.h, so this block leads every Win32 net header that needs both.
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/DeadlineTimer.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/NetError.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace core::net::detail
{

/// One overlapped operation, as the port and its owner both see it.
///
/// Standard-layout with @c overlapped first, which is what lets the port recover this object from
/// the `lpOverlapped` it dequeues with a cast rather than an `offsetof`: the address of a
/// standard-layout object and of its first member are the same address. An owner that needs more
/// state DERIVES from this and hands the kernel `&overlapped`; the port only ever sees this part.
struct IocpOperation
{
    OVERLAPPED overlapped {}; ///< MUST be first; `lpOverlapped` points here.

    /// What the port calls when this operation's packet is dequeued, on the loop's thread and
    /// inside the wait, whether or not anything is still parked on it.
    ///
    /// **It may let go and nothing else**: no resume, no callback into an owner. It is the last
    /// thing the port does with this pointer, so it is allowed to free the operation.
    using Dequeued = void (*)(IocpOperation& operation) noexcept;
    Dequeued onDequeued = nullptr;

    /// The port's record of the park waiting for this operation, or null. Written and read by the
    /// port alone — an owner never touches it — and cleared when that park is detached.
    void* route = nullptr;

    /// Set by the port when the packet is dequeued. Read by the owner to tell a real wake from a
    /// stale one, and by the port to report a park registered AFTER the completion arrived: a
    /// cancel detaches the park the stop came through, and the completion may land before its
    /// replacement is registered.
    bool completed = false;

    /// Prepares the operation to be handed to the kernel again.
    void rearm() noexcept
    {
        overlapped = OVERLAPPED {};
        completed = false;
    }

    /// @return How many bytes the completed operation transferred. Meaningful once
    ///         @c completed is set.
    [[nodiscard]] std::size_t bytesTransferred() const noexcept
    {
        return static_cast<std::size_t>(overlapped.InternalHigh);
    }
};

static_assert(std::is_standard_layout_v<IocpOperation>,
              "the port recovers an IocpOperation from its OVERLAPPED by a cast, which is the same "
              "address only on a standard-layout type");

/// `INVALID_SOCKET` as a `SOCKET` rather than as the macro, whose inner `~0` is a signed `int`, so
/// a comparison against it reads as signed/unsigned (`IocpBackend.cpp` names its own copy for the
/// same reason).
inline constexpr SOCKET ClosedSocket = INVALID_SOCKET;

/// Converts a completed operation's status into the Win32/Winsock code the error table is written
/// in; 0 when it succeeded.
///
/// `OVERLAPPED::Internal` is an NTSTATUS, not a Winsock code, so it is converted by
/// `WSAGetOverlappedResult`, which reads it back in the numbering the table matches on. Three
/// facts from fastcached's `Net/IocpStatus.hpp` (at `0708dd54`), each of which is somebody's bug:
///
/// - **a closed socket's operation is `ERROR_OPERATION_ABORTED` by judgement, not by lookup.**
///   The close is what made it complete at all, and it also leaves `WSAGetOverlappedResult` no
///   socket to ask — so reporting its `WSAENOTSOCK` would name the diagnostic's problem rather
///   than the operation's;
/// - **an operation aborted by `closesocket` completes `STATUS_LOCAL_DISCONNECT` (0xC0000241), and
///   one aborted by `CancelIoEx` `STATUS_CANCELLED` (0xC0000120)** — neither has a Winsock row, so
///   both are asked, never tabled;
/// - **not every non-zero NTSTATUS is a failure.** The warning-severity codes are successes
///   carrying a note, and treating one as an abort throws away bytes that were transferred. So a
///   non-zero status that Winsock then calls a success answers 0, and the byte count stands.
/// @param socket The socket the operation was issued on, or `INVALID_SOCKET` once it is closed.
/// @param operation The dequeued operation.
/// @return 0 on success, else the Win32/Winsock error.
[[nodiscard]] inline DWORD completionError(SOCKET socket, IocpOperation& operation) noexcept
{
    if (operation.overlapped.Internal == 0)
        return 0;
    if (socket == ClosedSocket)
        return static_cast<DWORD>(ERROR_OPERATION_ABORTED);
    auto transferred = DWORD { 0 };
    auto flags = DWORD { 0 };
    if (::WSAGetOverlappedResult(
            socket, reinterpret_cast<LPWSAOVERLAPPED>(&operation.overlapped), &transferred, FALSE, &flags)
        == FALSE)
        return static_cast<DWORD>(::WSAGetLastError());
    return 0;
}

/// @param error A value @c completionError returned.
/// @return Whether it says the operation was taken back rather than failing on its own.
///         (`WSA_OPERATION_ABORTED` is the same value, spelled for Winsock.)
[[nodiscard]] constexpr bool isAbort(DWORD error) noexcept
{
    return error == ERROR_OPERATION_ABORTED;
}

/// Registers the loop park a completion of @p operation wakes.
/// @param loop The loop to park on.
/// @param operation The operation, whose ADDRESS is the park's handle.
/// @param onReady What the loop calls in turn step 2, with why.
/// @param state Handed to @p onReady; must outlive the park.
/// @param refusal Where the port's reason goes if it refuses.
/// @return The park, or @c ParkId::invalid() when the loop's backend has no completion port.
[[nodiscard]] inline ParkId parkOnCompletion(EventLoop& loop,
                                             IocpOperation& operation,
                                             ReadyCallback onReady,
                                             void* state,
                                             NetError* refusal = nullptr)
{
    return loop.registerPark(ParkEntry::onReadyCallback(onReady,
                                                        state,
                                                        static_cast<platform::NativeHandle>(&operation),
                                                        HandleKind::Completion,
                                                        Interest::Read),
                             refusal);
}

/// Suspends a coroutine until one operation it issued has completed — the frame-owned wait that
/// `IocpListener::accept` and the `ConnectEx` dial share.
///
/// **The completion is the SINGLE writer of the outcome**, which is the one rule that separates
/// this from the readiness dial. A stop, or a deadline, calls `CancelIoEx` and goes on waiting: the
/// kernel then completes the operation with an abort, or with whatever it had already done, and
/// that is what the coroutine reads. Settling at the stop instead would let the real completion
/// arrive into a wait that is gone, and would throw away a connection the kernel had already
/// made. Only a loop being torn down (@c ParkWake::Abandoned) ends the wait without it, because
/// then nothing will ever dequeue the completion here.
///
/// The operation itself must NOT live in the waiting frame: it lives on the heap and holds its own
/// share until the port dequeues it, so a frame destroyed mid-wait — a flow torn down, a loop
/// abandoning it — leaves the kernel writing into storage that still exists. The destructor
/// retires the park for that case.
class CompletionWait
{
  public:
    /// How the wait ended.
    enum class Outcome : std::uint8_t
    {
        Pending,   ///< Still waiting.
        Completed, ///< The operation's completion arrived; ask @c completionError what it did.
        Abandoned, ///< The loop is going away; nothing more will arrive.
        Refused,   ///< The loop would not park on the operation at all.
        Closed,    ///< The owner closed the resource; the abort still arrives, to nobody.
    };

    /// @param loop The loop to park on; must outlive this.
    /// @param operation The issued operation; must outlive the wait.
    /// @param socket The socket it was issued on, for `CancelIoEx`.
    /// @param deadline When to take it back; `SteadyTimePoint::max()` for never.
    CompletionWait(EventLoop& loop,
                   IocpOperation& operation,
                   SOCKET socket,
                   platform::SteadyTimePoint deadline = platform::SteadyTimePoint::max()) noexcept:
        _loop(loop), _operation(operation), _socket(socket), _deadline(deadline)
    {
    }

    CompletionWait(CompletionWait const&) = delete;
    CompletionWait& operator=(CompletionWait const&) = delete;
    CompletionWait(CompletionWait&&) = delete;
    CompletionWait& operator=(CompletionWait&&) = delete;

    /// Retires the park if the frame is going away while it is still registered — a flow
    /// destroyed mid-wait. The operation outlives this on its own share.
    ~CompletionWait()
    {
        _cancelReg.reset();
        _timer.reset();
        if (_park)
            _loop.unregisterPark(std::exchange(_park, ParkId {}));
        // Finished and handed to the loop, and the frame is going away before the loop resumed
        // it: the ready queue must not keep a handle to it.
        if (_queued)
            std::ignore = _loop.cancelPending(std::exchange(_queued, {}));
    }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    /// @tparam Promise The waiting coroutine's promise type.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return True to stay suspended; false to resume at once with @c outcome already saying why.
    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        auto token = async::StopToken {};
        if constexpr (async::HasStopToken<Promise>)
            token = awaiting.promise().stopToken();

        if (!park())
        {
            _outcome = Outcome::Refused;
            return false;
        }
        _waiter = async::detail::parkedWorkFor(awaiting);

        // A flow that is already cancelled still waits for its completion -- the rule is the
        // same whether the stop came before the wait or during it -- so it is taken back here
        // and the park is left for the abort to arrive through.
        if (token.stop_requested())
            takeBack(Reason::Stop);
        else
            _cancelReg.emplace(token, [this] {
                // Any thread. Names the loop's generation-checked park and nothing else; see
                // `ResultAwaitable::onStop` for the same route.
                if (auto const park = _parkForStop; park)
                    _loop.requestCancel(park);
            });

        if (_deadline != platform::SteadyTimePoint::max())
            _timer.emplace(_loop, _deadline, &CompletionWait::onDeadline, this);
        return true;
    }

    void await_resume() noexcept
    {
        // The loop has resumed this frame, so the queue entry `finish` filed is gone.
        _queued = {};
        // Dropped first: it blocks until a callback running on another thread has finished.
        _cancelReg.reset();
        _timer.reset();
    }

    /// Ends the wait because the owner closed the resource the operation was issued on: settled
    /// NOW, and the waiting coroutine handed to the loop, as `ISocket::close` does for a parked
    /// read -- never resumed inside the owner's `close()` (G2).
    ///
    /// The completion is not the single writer here, and that is the one exception to the rule
    /// above, for the reason `close` is allowed it everywhere: closing IS what aborts the
    /// operation, the operation's storage outlives the wait on its own share, and there is no
    /// value left for the kernel to hand back that the owner would still accept.
    void close() noexcept
    {
        if (_outcome == Outcome::Pending && _waiter.resume)
            finish(Outcome::Closed);
    }

    /// @return How the wait ended.
    [[nodiscard]] Outcome outcome() const noexcept { return _outcome; }

    /// @return Whether the wait's own deadline took the operation back.
    [[nodiscard]] bool timedOut() const noexcept { return _timedOut; }

    /// @return Whether a stop of the waiting flow took the operation back.
    [[nodiscard]] bool stopped() const noexcept { return _stopped; }

  private:
    /// Why the operation is being asked back.
    enum class Reason : std::uint8_t
    {
        Stop,
        Deadline
    };

    /// Registers the park the completion wakes.
    /// @return Whether the loop accepted it.
    [[nodiscard]] bool park()
    {
        _park = parkOnCompletion(_loop, _operation, &CompletionWait::onWake, this);
        // Written once per registration on the loop's thread and before the stop callback can
        // exist or run again, which is what lets that callback read it unatomically; see
        // `ResultAwaitable::cancelThrough`.
        if (!_cancelReg.has_value())
            _parkForStop = _park;
        return static_cast<bool>(_park);
    }

    /// Asks the kernel for the operation back; the completion still arrives.
    /// @param reason Which of the two askers this is.
    void takeBack(Reason reason) noexcept
    {
        (reason == Reason::Stop ? _stopped : _timedOut) = true;
        if (!_operation.completed)
            std::ignore = ::CancelIoEx(reinterpret_cast<HANDLE>(_socket), &_operation.overlapped);
    }

    /// Ends the wait and hands the coroutine to the loop, whose drain step resumes it.
    ///
    /// Never resumed here: `close()` reaches this from inside an owner's `close()`, and a flow
    /// resumed there could destroy the owner under its caller's next statement (G2). The park
    /// callbacks that reach it already run in the drain step, where the queued waiter runs in the
    /// same drain.
    /// @param outcome Why.
    void finish(Outcome outcome) noexcept
    {
        _outcome = outcome;
        if (_park)
            _loop.unregisterPark(std::exchange(_park, ParkId {}));
        auto waiter = std::exchange(_waiter, async::ParkedWork {});
        if (!waiter.resume)
            return;
        _queued = waiter.resume;
        _loop.resumeSoon(std::move(waiter));
    }

    /// The park's callback, in turn step 2.
    /// @param state This wait.
    /// @param wake Why the park woke.
    static void onWake(void* state, ParkWake wake)
    {
        auto& self = *static_cast<CompletionWait*>(state);
        if (self._outcome != Outcome::Pending)
            return;
        switch (wake)
        {
            case ParkWake::Ready:
                if (self._operation.completed)
                    self.finish(Outcome::Completed);
                return;
            case ParkWake::Cancelled:
                // The loop has already detached this park's registration, so the completion
                // would now arrive to nobody: register a fresh one to receive it, and ask the
                // kernel for the operation back. If the completion beat the stop, the fresh park
                // reports it at once -- the port marks it `completed` whether or not anything
                // was registered -- and the value wins.
                if (self._operation.completed)
                {
                    self.finish(Outcome::Completed);
                    return;
                }
                self._loop.unregisterPark(std::exchange(self._park, ParkId {}));
                self.takeBack(Reason::Stop);
                if (!self.park())
                    self.finish(Outcome::Abandoned);
                return;
            case ParkWake::Abandoned: self.finish(Outcome::Abandoned); return;
        }
    }

    /// The deadline's callback: ask the operation back and let its completion report.
    /// @param state This wait.
    static void onDeadline(void* state)
    {
        auto& self = *static_cast<CompletionWait*>(state);
        if (self._outcome == Outcome::Pending)
            self.takeBack(Reason::Deadline);
    }

    EventLoop& _loop;
    IocpOperation& _operation;
    SOCKET _socket;
    platform::SteadyTimePoint _deadline;
    ParkId _park {};
    ParkId _parkForStop {}; ///< The park a stop names; written before the stop callback exists.
    async::ParkedWork _waiter {};
    std::coroutine_handle<> _queued {}; ///< Handed to the loop by @c finish; not yet resumed.
    Outcome _outcome = Outcome::Pending;
    bool _stopped = false;
    bool _timedOut = false;
    std::optional<DeadlineTimer> _timer;
    std::optional<async::StopCallback<std::function<void()>>> _cancelReg;
};

} // namespace core::net::detail
