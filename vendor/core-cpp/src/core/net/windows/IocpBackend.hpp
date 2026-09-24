// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Windows @c IoBackend that scales: an I/O completion port, plus the bridge that
/// lets one port serve a server's sockets AND a TUI's console input.
///
/// **This is the merge.** contour waits on events with `WaitForMultipleObjects`, which
/// caps at 64 handles and cannot express a completion. fastcached uses a completion
/// port, which scales and expresses one — but a port cannot wait on a console handle,
/// which is why fastcached kept a second coroutine runtime for the one thing its
/// reactor could not park on. What follows is that port with the one thing it lacked
/// bolted on, so a terminal's input and a listener's accepts are the same wait.
///
/// ## What a port cannot do, and the three bridges that cover it
///
/// A port reports *completions*. It has no concept of "this handle is readable", so
/// readiness has to be SYNTHESISED, and each kind of handle needs its own source:
///
/// - **A waitable HANDLE** (console input, an event, `platform::SystemPipe`'s wakeup)
///   gets a thread-pool wait whose callback does one thing:
///   `PostQueuedCompletionStatus`. That is guarantee **G3** — helper threads only post
///   — held by there being nothing else in the callback to get wrong. Where the kernel
///   exposes `NtAssociateWaitCompletionPacket` (a startup probe, never a link against
///   `ntdll`; @see detail::WaitCompletionPacket) the same job is done with no helper
///   thread at all, and the probe failing is an ordinary answer.
/// - **Socket readability** is a zero-byte `WSARecv`, the documented Winsock idiom for
///   "complete when data is pending, consuming nothing" (fastcached
///   `Net/IocpSocket.cpp:555-561`). It completes for data, for EOF and for an error
///   alike, all three of which are @c Readiness::Readable here: this layer's job is to
///   wake the reader, and the reader's own `recv` is what tells it which happened. The
///   `MSG_PEEK` that tells a zero-byte completion from EOF apart belongs to the socket
///   (`IocpSocket.cpp:327-334`), not here.
/// - **Socket writability** has no completion of its own, so it goes through
///   `WSAEventSelect` for `FD_WRITE` and then through the waitable-HANDLE bridge above.
///
/// ## Thread affinity, asserted
///
/// - **G1: exactly one thread dequeues the port.** A port will happily be drained by
///   four threads — that is its selling point elsewhere, and it is forbidden here,
///   because a coroutine resumed on two threads is not a coroutine. @c wait() claims
///   the dequeuer identity and asserts no other thread holds it;
///   `core-cpp.iocp-canary` is a program that violates it and must die.
/// - **G3: helper threads only post.** See above.
/// - **G4: a SOCKET is associated with exactly one port**, and @c ICompletionPort is
///   the only place it happens, so the backend refuses a second association instead of
///   the kernel failing it indistinguishably from six other mistakes.
///
/// ## Ownership: why `lpOverlapped` never points into a handler
///
/// The kernel holds the pointer an operation was issued with until the operation
/// completes, and an operation the caller has since taken back still completes — on a
/// later turn, after the caller may have been destroyed. So every pointer this backend
/// hands the kernel is into a @c detail::ReadinessSlot it owns and refcounts, and the
/// handler holds one share of the same slot. @c ReadinessHandler::slot is where that
/// rule is written for a reader; this file is where it is kept.
///
/// Upstream: fastcached `Async/IocpReactor.{hpp,cpp}` and `Net/IocpSocket.{hpp,cpp}` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`. What is NOT upstream is every bridge
/// above: fastcached's port served sockets only, and its reactor was a loop rather than
/// the one blocking primitive a loop drives.

#include <core/net/ICompletionPort.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/detail/ReadyBatch.hpp>
#include <core/net/detail/WorkerIdentity.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace core::net
{

namespace detail
{
    struct IocpOperation;
}

/// An @c IoBackend over a Windows I/O completion port.
///
/// It IS its own @c ICompletionPort rather than owning one, which is what makes the
/// association record and the wait live in the same object: a handle is associated with
/// "the port", and the thing that dequeues the port is this. The alternative — a nested
/// port object handed out by pointer — needs a destructor of its own on a class with
/// virtual members, and there is no spelling of that which is both correct here and
/// acceptable to `cppcoreguidelines-virtual-class-destructor`: public-and-virtual is
/// refused on a `final` class, and protected-and-non-virtual is refused by the
/// `unique_ptr` that would have to hold it.
class IocpBackend final: public IoBackend, public ICompletionPort
{
  public:
    /// Which bridge a waitable HANDLE is watched through.
    ///
    /// Injected rather than probed at the point of use, so a test can exercise the
    /// fallback on a machine where the optimisation is available — which is every
    /// machine since Windows 8, so without this the thread-pool path would be compiled
    /// by CI and run by nobody (`.agent/rules/platform.md`: a platform difference is an
    /// injected implementation, never an `#ifdef` in logic).
    enum class WaitBridge : std::uint8_t
    {
        Auto,      ///< `NtAssociateWaitCompletionPacket` if the probe finds it, else the thread pool.
        Threadpool ///< The thread-pool wait, always.
    };

    /// One armed operation, and the object every `lpOverlapped` points into.
    ///
    /// Defined in the implementation; named here only so this file's own thread-pool
    /// callback can take one. Nothing outside can do anything with an incomplete type.
    struct Operation;

    /// One registration's refcounted @c detail::ReadinessSlot. Defined in the
    /// implementation, for the same reason.
    class Slot;

    /// Creates the backend and its completion port.
    /// @param bridge Which waitable-HANDLE bridge to use.
    /// @throws std::runtime_error when the port could not be created, which is handle
    ///         exhaustion and not a condition a caller recovers from — for the reason
    ///         @c detail::WakeupChannel gives.
    explicit IocpBackend(WaitBridge bridge = WaitBridge::Auto);

    ~IocpBackend() override;

    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::Iocp; }

    [[nodiscard]] std::expected<void, NetError> attach(ReadinessHandler& handler) override;

    [[nodiscard]] std::expected<void, NetError> setInterest(ReadinessHandler& handler,
                                                            Interest interest) override;

    void detach(ReadinessHandler& handler) noexcept override;

    [[nodiscard]] WaitResult wait(std::optional<platform::SteadyDuration> timeout) override;

    void wake() noexcept override;

    [[nodiscard]] ICompletionPort* completionPort() noexcept override { return this; }

    [[nodiscard]] std::expected<void, NetError> associate(platform::NativeHandle handle) override;

    [[nodiscard]] bool isAssociated(platform::NativeHandle handle) const noexcept override
    {
        return _associated.contains(handle);
    }

    void forget(platform::NativeHandle handle) noexcept override { _associated.erase(handle); }

    [[nodiscard]] platform::NativeHandle nativeHandle() const noexcept override { return _port; }

    void beginOperation(void* operation) override;

    void withdrawOperation(void* operation) noexcept override;

    /// @return Whether a thread is currently inside @c wait().
    ///
    /// Exists for the G1 canary, which must provoke a SECOND `wait()` while the first
    /// is in flight and would otherwise have to guess at it with a sleep — and a sleep
    /// long enough to be reliable on a cold two-core runner is one nobody wants in a
    /// program that runs on every build (`.agent/rules/testing.md`).
    [[nodiscard]] bool dequeuerRunning() const noexcept { return _dequeuer.running(); }

    /// @return Whether this backend bridges waitable handles with wait completion
    ///         packets rather than with thread-pool waits. For a test that must say
    ///         which path it exercised, and for a diagnostic.
    [[nodiscard]] bool usesWaitCompletionPackets() const noexcept { return _useWaitPackets; }

    /// @return How many operations the kernel still holds a share of. Zero on a backend
    ///         with nothing armed; what the teardown drain waits to reach.
    [[nodiscard]] std::size_t outstandingOperations() const noexcept { return _inFlight.size(); }

    /// @return How many operations OWNERS have issued on this port (@c beginOperation) whose
    ///         completions have not been dequeued yet. The teardown drain waits for these too:
    ///         each one is a share its owner gives back only when its packet comes home.
    [[nodiscard]] std::size_t issuedOperations() const noexcept { return _issued.size(); }

  private:
    /// Where a failure to arm is reported.
    ///
    /// An `enum class` rather than a `bool`, because both call sites would otherwise be
    /// `rearm(registration, true)` and `rearm(registration, false)` and neither says
    /// which is which (`.agent/rules/design-principles.md`).
    enum class ArmFailure : std::uint8_t
    {
        ToCaller, ///< `setInterest` returns it: the caller is there to be told.
        ToBatch   ///< A re-arm inside `wait()` has no caller, so it wakes the handler instead.
    };

    /// One registered handler and what it is currently watched for.
    ///
    /// The slot is a BORROWED pointer here: it is owned by its own refcount, of which
    /// the handler holds one share and each armed operation holds another. This entry
    /// holding an owning share too would keep a slot alive exactly as long as the
    /// registration, which is what the handler's share already says.
    struct Registration
    {
        ReadinessHandler* handler = nullptr; ///< The caller's handler (not owned).
        Interest interest = Interest::None;  ///< What it is watched for; None mutes it.
        Slot* slot = nullptr;                ///< Its slot (not owned; see above).
    };

    /// Arms every watched direction that is not already in flight.
    ///
    /// Run at the top of every @c wait, which is what makes this backend
    /// level-triggered like every other: an operation is one-shot, so the re-arm is
    /// what re-reports a readiness nobody consumed. Doing it there rather than after
    /// the dispatch is deliberate — a callback may detach its own registration, and
    /// re-arming one that is about to be taken back is an operation issued for nobody.
    void rearmAll() noexcept;

    /// Arms one registration's operations.
    /// @param registration The registration to arm.
    /// @param report Where a failure goes.
    /// @return Whether every watched direction is now armed.
    [[nodiscard]] bool rearm(Registration& registration, ArmFailure report) noexcept;

    /// Issues the waitable-HANDLE bridge for @p target.
    /// @param registration The registration to arm.
    /// @param target The waitable handle to watch.
    /// @param report What to collect when it signals.
    /// @return Whether the bridge was armed.
    [[nodiscard]] bool armSignal(Registration& registration, void* target, Readiness report) noexcept;

    /// Issues the zero-byte `WSARecv` that stands in for socket readability.
    /// @param registration The registration to arm.
    /// @return Whether the receive was issued, or completed inline.
    [[nodiscard]] bool armSocketRead(Registration& registration) noexcept;

    /// Takes one armed operation back, releasing its share when nothing is in flight.
    /// @param node The operation to stand down.
    void cancelOperation(Operation& node) noexcept;

    /// Stands every operation of @p slot down, closes its bridges and retires it.
    /// @param slot The slot to retire.
    void retire(Slot& slot) noexcept;

    /// @param handler The handler to look for.
    /// @return Its registration, or nullptr if it is not attached.
    [[nodiscard]] Registration* find(ReadinessHandler const& handler) noexcept;

    /// Whether a dequeued completion is collected into the batch or only accounted for.
    enum class Collect : std::uint8_t
    {
        No, ///< Teardown: give the share back, dispatch nothing.
        Yes ///< A wait: this is what the wait is for.
    };

    /// Reads one dequeued completion into the ready batch, or drops it.
    /// @param key The completion key the packet carried.
    /// @param overlapped The `lpOverlapped` the packet carried.
    /// @param collect Whether to collect readiness.
    void consumeCompletion(std::uintptr_t key, void* overlapped, Collect collect) noexcept;

    /// Routes one dequeued completion of an operation an owner issued: marks it completed,
    /// reports the park waiting for it, and runs its dequeue hook last.
    /// @param operation The owner's operation.
    /// @param collect Whether to collect readiness.
    void consumeOwnerCompletion(detail::IocpOperation& operation, Collect collect) noexcept;

    /// Dequeues and discards until nothing the kernel still holds remains.
    ///
    /// Bounded, and it is the second half of the refcount: a share the kernel holds is
    /// released when its packet is dequeued, so a port closed with packets still in it
    /// leaks every slot they name. What makes it finite is that every mechanism has
    /// been stood down before it runs, so the only packets left are ones already
    /// queued and the abort completions of cancelled receives.
    void drainOutstanding() noexcept;

    /// The port. `void*` rather than `HANDLE`, which is the same type spelled without
    /// `<Windows.h>` — a private header may include it, but nothing here needs to.
    platform::NativeHandle _port = nullptr;

    /// What has been associated with @c _port, which is the whole of guarantee G4's
    /// enforcement: the kernel cannot be asked, and its refusal of a second association
    /// cannot be told from half a dozen other failures.
    std::unordered_set<platform::NativeHandle> _associated;

    std::unordered_map<ReadinessHandler const*, Registration> _registrations;
    detail::ReadyBatch _batch;        ///< What this wait found ready, and what `detach` withdraws from.
    detail::WorkerIdentity _dequeuer; ///< Who is inside `wait()`; G1 is asserted against it.

    bool _useWaitPackets = false; ///< Whether the `NtAssociateWaitCompletionPacket` probe answered.

    /// Whether a wake packet is already queued. Coalesces, so a burst of `wake()` from
    /// another thread costs one packet rather than one per call; cleared by the dequeue
    /// that consumes it, which is on the loop's thread.
    std::atomic<bool> _wakePending { false };

    /// Every `lpOverlapped` this backend has handed the kernel and not yet had back.
    ///
    /// **A set rather than a count, and the difference is not bookkeeping.** A port
    /// serves everything associated with it, so a socket that issues its own overlapped
    /// operations — which is what @c ICompletionPort is lent out FOR — has its
    /// completions dequeued here too, carrying a pointer that is not one of these. A
    /// backend that cast every non-wake packet to its own @c Operation would read an
    /// arbitrary struct as one of its own, which is a use-after-free with extra steps.
    /// So a packet is this backend's exactly when its pointer is in here, and anything
    /// else is left alone.
    ///
    /// Loop-thread only: an arm and a dequeue both happen there, and a helper thread
    /// only posts.
    std::unordered_set<void*> _inFlight;

    /// Every operation an OWNER has announced through @c beginOperation and whose packet has not
    /// come back. Disjoint from @c _inFlight by construction -- one set holds pointers this
    /// backend allocated, the other pointers an owner did -- and asked second, so the backend's
    /// own packets are never mistaken for an owner's. Loop-thread only, like @c _inFlight.
    std::unordered_set<void*> _issued;
};

} // namespace core::net
