// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede windows.h (which project headers pull in), so this block
// leads every Win32 net translation unit.
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include <core/net/windows/IocpBackend.hpp>

#include <core/net/Diagnostics.hpp>
#include <core/net/detail/WaitTimeout.hpp>
#include <core/net/windows/InvalidSocket.hpp>
#include <core/net/windows/IocpOperation.hpp>
#include <core/net/windows/NetworkEvents.hpp>
#include <core/net/windows/WaitCompletionPacket.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace core::net
{

namespace
{
    /// The completion keys this backend posts under.
    ///
    /// A socket associated with the port carries its own key — the handle value, as
    /// @c IocpBackend::associate sets it — so these two are chosen to be values no
    /// handle can take: handles are pointer-sized and aligned, and 1 and 2 are neither.
    constexpr auto KeyWake = ULONG_PTR { 1 };
    constexpr auto KeyReadiness = ULONG_PTR { 2 };

    /// How many completions one `GetQueuedCompletionStatusEx` takes at a time.
    ///
    /// The batch is dispatched as one @c detail::ReadyBatch, which merges duplicate
    /// registrations, so a larger number costs stack and buys fewer syscalls. 64 is
    /// fastcached's 32 doubled to match `EventLoopOptions::dispatchBatch`.
    constexpr auto CompletionBatch = std::size_t { 64 };

    /// How long the teardown drain waits for the kernel to give back what it holds
    /// before it gives up and says so.
    ///
    /// Every mechanism has been stood down when it runs, so what is left is packets
    /// already queued plus the abort completions of cancelled receives, both of which
    /// arrive in microseconds. This is the backstop, not the budget: written for a cold
    /// two-core runner under a sanitizer (`.agent/rules/testing.md`).
    constexpr auto DrainBudget = std::chrono::milliseconds { 2000 };

    /// How long one dequeue inside the teardown drain blocks. A short block rather than
    /// a poll: an abort completion takes a moment to arrive and spinning on a zero
    /// timeout burns a core to learn the same thing.
    constexpr auto DrainSliceMs = DWORD { 10 };

    /// What a socket's write bridge selects on. `FD_CLOSE` and `FD_CONNECT` as well as
    /// `FD_WRITE`: a writer parked on a socket the peer resets, or that fails to
    /// connect, must be woken to learn it from its own `send`, and a selection naming
    /// only `FD_WRITE` would leave it parked for ever.
    constexpr auto WriteEventMask = long { FD_WRITE | FD_CONNECT | FD_CLOSE };
} // namespace

// ---------------------------------------------------------------------------------
// Operation — one ARM, and the only thing an lpOverlapped ever points at.
// ---------------------------------------------------------------------------------

/// One armed operation: the `OVERLAPPED` the kernel is handed, and what it means.
///
/// **One node per arm, never reused while the kernel holds it.** A retracted
/// operation's `OVERLAPPED` belongs to the kernel until a later turn delivers its abort
/// completion, so the node is STOOD DOWN and the next arm gets a fresh one. Reusing it
/// is how fastcached turned an abort into a spurious EOF on a healthy socket
/// ([fastcached#710](https://github.com/LASTRADA-Software/fastcached/issues/710),
/// [fastcached#884](https://github.com/LASTRADA-Software/fastcached/issues/884)) — the
/// same invariant as that ticket's other half, that bytes already received beat a later
/// stop: a completion that has happened is a fact, and neither a cancel nor a re-arm
/// may make the record of it ambiguous.
///
/// Standard-layout with @c overlapped first, which is what makes the recovery at
/// dequeue a cast rather than an `offsetof`: the address of a standard-layout object
/// and the address of its first member are the same address, guaranteed.
struct IocpBackend::Operation
{
    OVERLAPPED overlapped {};           ///< MUST be first; `lpOverlapped` points here.
    Slot* slot = nullptr;               ///< The registration this arm belongs to.
    Readiness report = Readiness::None; ///< What to collect when this completes.
    std::uint64_t generation = 0;       ///< Which arm this is; @see detail::ReadinessSlot::generation.
    bool isSocketRead = false;          ///< Whether this is the zero-byte `WSARecv` rather than a signal.

    /// Set when this arm is taken back. A packet naming a retired node is DROPPED: the
    /// readiness it reports belongs to a watch the caller has since withdrawn, and
    /// dispatching it would wake a flow that asked not to be.
    bool retired = false;

    /// How many packets the thread-pool callback has posted for this node. Written by
    /// the helper thread AFTER a successful post, and read by the loop's thread only
    /// once `WaitForThreadpoolWaitCallbacks` has returned. With @c dequeued it is how
    /// the cancel path tells "the callback was cancelled, so the share is mine to give
    /// back" from "a packet is in the port, so the dequeue owns it" — and reading that
    /// the wrong way round is a leak on one side and a double release on the other.
    /// Unused on the wait-completion-packet path, where the cancel says so itself.
    std::atomic<unsigned> posts { 0 };

    unsigned dequeued = 0; ///< How many of @c posts have come back. Loop-thread only.
};

// ---------------------------------------------------------------------------------
// Slot — one REGISTRATION, refcounted, what `ReadinessHandler::slot` refers to.
// ---------------------------------------------------------------------------------

/// The backend-owned object every completion for one registration names, through one of
/// its @c Operation nodes.
///
/// Its lifetime is the union of the registration's and every in-flight operation's,
/// which is the whole point: `detach` gives the handler's share back, and a completion
/// arriving afterwards still finds this alive, finds @c retired(), and drops without
/// reading @c handler — which by then may name storage the caller has freed.
class IocpBackend::Slot final: public detail::ReadinessSlot
{
  public:
    /// @param owner The registration this slot speaks for.
    /// @param completionPort The port its operations complete on (not owned).
    Slot(ReadinessHandler& owner, HANDLE completionPort) noexcept: handler { &owner }, port { completionPort }
    {
    }

    /// Closes the bridge objects. Idempotent, and called on the loop's thread by
    /// @c IocpBackend::retire.
    void closeBridges() noexcept
    {
        if (threadpoolWait != nullptr)
        {
            // Both calls, in this order, and neither is optional: the first stops the
            // wait from firing again, the second waits out a callback that has already
            // started and CANCELS one that has not. Closing without them is documented
            // as undefined, and what it does in practice is run a callback against
            // freed storage.
            SetThreadpoolWait(threadpoolWait, nullptr, nullptr);
            WaitForThreadpoolWaitCallbacks(threadpoolWait, TRUE);
            CloseThreadpoolWait(threadpoolWait);
            threadpoolWait = nullptr;
        }
        waitPacket.reset();
        if (writeEvent != WSA_INVALID_EVENT)
        {
            // Deselect before closing, or Winsock goes on pointing a live socket at a
            // dead event. `WSAEventSelect(s, nullptr, 0)` is how a selection is
            // withdrawn; it leaves the socket non-blocking, which is what
            // `WSAEventSelect` made it and what its owner has to know either way.
            if (socket != detail::InvalidSocket)
                std::ignore = WSAEventSelect(socket, nullptr, 0);
            std::ignore = WSACloseEvent(writeEvent);
            writeEvent = WSA_INVALID_EVENT;
        }
    }

    /// Adds a fresh arm node.
    /// @param report What its completion is collected as.
    /// @param isSocketRead Whether it is the zero-byte receive.
    /// @return The node, owned by this slot until it is reconciled.
    [[nodiscard]] Operation& addNode(Readiness report, bool isSocketRead)
    {
        auto node = std::make_unique<Operation>();
        node->slot = this;
        node->report = report;
        node->isSocketRead = isSocketRead;
        node->generation = nextGeneration();
        auto& reference = *node;
        _nodes.push_back(std::move(node));
        return reference;
    }

    /// Frees a node the kernel has given back.
    /// @param node The node to free. Must not be named by anything still in flight.
    void removeNode(Operation& node) noexcept
    {
        std::erase_if(_nodes, [&node](auto const& held) { return held.get() == &node; });
    }

    /// Retires this registration and every arm still outstanding on it.
    void retireAll() noexcept
    {
        retire();
        signal.store(nullptr, std::memory_order_release);
        read = nullptr;
        for (auto const& node: _nodes)
            node->retired = true;
    }

    /// @return How many arm nodes are still held, reconciled or not.
    [[nodiscard]] std::size_t nodeCount() const noexcept { return _nodes.size(); }

    ReadinessHandler* handler; ///< Valid only while `!retired()`.
    HANDLE port;               ///< Not owned; the backend's port.

    /// The current arm of the waitable-handle bridge, or null.
    ///
    /// Atomic because the thread-pool callback reads it: one wait object serves this
    /// registration for its whole life (creating one per arm would mean closing one per
    /// arm, and a close needs `WaitForThreadpoolWaitCallbacks` first, which is a
    /// blocking call on the dispatch path), so the callback learns WHICH arm it is
    /// firing for from here. Written on the loop's thread before the wait is armed, and
    /// there is never a callback in flight at that moment: the previous arm has either
    /// completed or been cancelled through `WaitForThreadpoolWaitCallbacks`.
    std::atomic<Operation*> signal { nullptr };

    Operation* read = nullptr; ///< The current arm of the zero-byte receive; loop-thread only.

    PTP_WAIT threadpoolWait = nullptr; ///< The thread-pool bridge, if that is the path.
    std::unique_ptr<detail::WaitCompletionPacket> waitPacket; ///< The kernel bridge, if that is.
    WSAEVENT writeEvent = WSA_INVALID_EVENT; ///< `FD_WRITE` selection; socket registrations only.
    SOCKET socket = detail::InvalidSocket;   ///< The socket, for a `HandleKind::Socket` registration.

    /// The owner's operation, for a `HandleKind::Completion` registration, and null for every
    /// other kind. Its `route` names this slot for exactly as long as this field names it; both
    /// are cleared together in @c IocpBackend::retire, which is what lets a completion dequeued
    /// after the park is gone find no route rather than a freed slot.
    detail::IocpOperation* operation = nullptr;

    using detail::ReadinessSlot::nextGeneration;
    using detail::ReadinessSlot::retire;

  protected:
    void dispose() noexcept override { delete this; }

    /// Protected and non-virtual, which on a `final` class is as narrow as private and
    /// is the shape a reader and `cppcoreguidelines-virtual-class-destructor` both
    /// expect: @c dispose() is the only thing that may run it, because a slot is freed by
    /// its refcount reaching zero and by nothing else. A destructor anything could reach
    /// — a `unique_ptr`'s deleter, a `delete` on a stray pointer — is that rule with a
    /// hole in it.
    ~Slot()
    {
        // Defensive, and it has to be: `dispose()` runs on whichever thread released the
        // last share. `retire()` closed these on the loop's thread long before, so this
        // is a no-op on every reachable path — but a `CloseThreadpoolWait` that never ran
        // would be a handle leak nothing reports.
        closeBridges();
    }

  private:
    std::vector<std::unique_ptr<Operation>> _nodes;
};

namespace
{
    /// The thread-pool wait's callback, and the whole of guarantee G3.
    ///
    /// **It posts, and it does nothing else.** No resume, no member of a handler, no
    /// allocation — this runs on a thread the loop does not own, and anything it
    /// touched would be touched concurrently with the loop's own turn. What keeps the
    /// slot alive underneath it is the share the arm took, which is given back only
    /// when the packet posted here is dequeued or the arm is proven cancelled.
    /// @param context The @c IocpBackend::Slot this wait belongs to.
    void CALLBACK onThreadpoolWait(PTP_CALLBACK_INSTANCE /*instance*/,
                                   void* context,
                                   PTP_WAIT /*wait*/,
                                   TP_WAIT_RESULT /*result*/)
    {
        auto* const slot = static_cast<IocpBackend::Slot*>(context);
        auto* const node = slot->signal.load(std::memory_order_acquire);
        if (node == nullptr)
            return; // the arm was taken back between the signal and this callback

        if (PostQueuedCompletionStatus(slot->port, 0, KeyReadiness, &node->overlapped) != 0)
        {
            // AFTER the post, never before. The cancel path reads this to decide who
            // owns the arm's share, and a count raised for a post that then failed
            // would tell it a packet is coming that never does — a share held for ever
            // and a slot that outlives its backend.
            node->posts.fetch_add(1, std::memory_order_release);
            return;
        }
        // Unreachable short of exhaustion inside the kernel's own queue. The share is
        // deliberately NOT released here: `dispose()` would then run on this thread and
        // close the very wait object whose callback this is. Leaving the count alone is
        // what lets the cancel path reclaim it on the loop's thread.
        reportDiagnostic(std::format("IocpBackend: PostQueuedCompletionStatus failed: {}", GetLastError()));
    }
} // namespace

// ---------------------------------------------------------------------------------
// IocpBackend
// ---------------------------------------------------------------------------------

IocpBackend::IocpBackend(WaitBridge bridge):
    // One concurrent thread, which is guarantee G1 said to the kernel as well as
    // asserted in `wait()`. The value does not enforce the rule on its own — it bounds
    // how many threads the kernel RELEASES at once, not how many may call — but a port
    // created for four would be a port whose own parameters contradicted the rule it is
    // run under.
    _port { CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, /*threads*/ 1) },
    _useWaitPackets { bridge == WaitBridge::Auto && detail::waitCompletionPacketsAvailable() }
{
    if (_port == nullptr)
        throw std::runtime_error("core::net: cannot create the IOCP completion port "
                                 "(handle exhaustion?)");
}

IocpBackend::~IocpBackend()
{
    // Every mechanism down first, so nothing new can be posted while the drain runs.
    // The contract says a handler is detached before it is destroyed, so anything still
    // registered here is alive and its share is ours to give back.
    for (auto& entry: _registrations)
    {
        retire(*entry.second.slot);
        if (entry.second.handler != nullptr)
            entry.second.handler->slot.reset();
    }
    _registrations.clear();
    _batch.clear();

    drainOutstanding();
    // Not an assertion on what the drain could not reclaim: it has already said what it
    // could not get back, and aborting a destructor over that would turn a diagnosable
    // leak into a crash in whatever was tearing the loop down.

    // Last, and after the drain: a packet still in the port when its handle closes is a
    // share nothing will ever give back.
    if (_port != nullptr)
        CloseHandle(_port);
}

std::expected<void, NetError> IocpBackend::associate(platform::NativeHandle handle)
{
    if (handle == nullptr || handle == platform::InvalidHandle)
        return std::unexpected { makeNetError(NetErrorCode::BadHandle, 0, "ICompletionPort::associate") };

    auto const known = _associated.contains(handle);
    // G4, asserted as well as reported: the kernel's own refusal of a second association
    // is `ERROR_INVALID_PARAMETER`, which is what it also answers for a closed handle and
    // for half a dozen ordinary mistakes.
    assert(!known
           && "a handle was associated with this completion port twice (G4: a SOCKET is "
              "associated with exactly one port). The second association is what fails, and "
              "the loser then awaits completions that are delivered to the winner.");
    if (known)
        return std::unexpected { makeNetError(NetErrorCode::AddressInUse,
                                              0,
                                              "ICompletionPort::associate: already associated "
                                              "with this port") };

    auto* const result = CreateIoCompletionPort(handle, _port, reinterpret_cast<ULONG_PTR>(handle), 0);
    if (result != _port)
        return std::unexpected { makeNetError(NetErrorCode::SystemError,
                                              static_cast<int>(GetLastError()),
                                              "ICompletionPort::associate: CreateIoCompletionPort") };
    _associated.insert(handle);
    return {};
}

IocpBackend::Registration* IocpBackend::find(ReadinessHandler const& handler) noexcept
{
    auto const found = _registrations.find(&handler);
    return found == _registrations.end() ? nullptr : &found->second;
}

std::expected<void, NetError> IocpBackend::attach(ReadinessHandler& handler)
{
    if (handler.handle == platform::InvalidHandle || handler.handle == nullptr)
        return std::unexpected { makeNetError(NetErrorCode::BadHandle, 0, "IocpBackend::attach") };
    if (find(handler) != nullptr)
        return std::unexpected { makeNetError(
            NetErrorCode::BadHandle, 0, "IocpBackend::attach: handler is already attached") };
    if (handler.kind == HandleKind::Fd)
        return std::unexpected { makeNetError(NetErrorCode::Unsupported,
                                              0,
                                              "IocpBackend::attach: HandleKind::Fd names a POSIX "
                                              "descriptor, which this platform has no kernel object "
                                              "for; register the SOCKET or the waitable HANDLE") };

    // The slot lives exactly as long as the union of this registration and every
    // operation the kernel still holds. The `unique_ptr` owns it only until the
    // handler's share is taken; after `release()` the refcount is the sole owner, which
    // is what makes a completion arriving after `detach` safe.
    // Not a `unique_ptr`: the slot's destructor is private, because `dispose()` is the
    // only thing allowed to run it — a deleter that could reach it would be a way round
    // the refcount this whole type exists to keep. The raw pointer owns it for exactly
    // one statement: the reference on the next line is the first share, and from then on
    // the count is the owner.
    auto* const slot = new Slot { handler, _port };
    handler.slot = detail::ReadinessSlotRef { *slot };

    if (handler.kind == HandleKind::Completion)
    {
        // The handle IS the operation. Two parks naming one operation would be two owners each
        // expecting the one completion, and the loser would wait for ever -- G4's shape, one
        // level up -- so it is asserted rather than quietly routed to whichever came last.
        auto* const operation = static_cast<detail::IocpOperation*>(handler.handle);
        assert(operation->route == nullptr
               && "two HandleKind::Completion registrations named one operation; only one of them "
                  "can be told of its completion");
        slot->operation = operation;
        operation->route = slot;
    }
    else if (handler.kind == HandleKind::Socket)
    {
        slot->socket = reinterpret_cast<SOCKET>(handler.handle);
        // G4 lives in the port, and this is where a socket registration reaches it. An
        // owner that associated the handle itself — `ConnectEx` forces the association
        // before the call — has already been recorded there, so this is not a second
        // association; it is the same one.
        if (!isAssociated(handler.handle))
        {
            if (auto const associated = associate(handler.handle); !associated)
            {
                handler.slot.reset(); // the last share: the slot is freed here
                return std::unexpected { associated.error() };
            }
        }
    }

    _registrations.emplace(&handler,
                           Registration { .handler = &handler, .interest = Interest::None, .slot = slot });
    return {};
}

std::expected<void, NetError> IocpBackend::setInterest(ReadinessHandler& handler, Interest interest)
{
    auto* const registration = find(handler);
    if (registration == nullptr)
        return std::unexpected { makeNetError(
            NetErrorCode::BadHandle, 0, "IocpBackend::setInterest: handler is not attached") };

    registration->interest = interest;
    if (interest == Interest::None)
    {
        // Mute means SILENT, on every backend. Both arms are stood down, so a readiness
        // already in the port is dropped when it arrives rather than dispatched to a
        // caller that asked not to hear it. The registration stays, and `detach` still
        // finds it.
        if (auto* const node = registration->slot->signal.load(std::memory_order_acquire); node != nullptr)
            cancelOperation(*node);
        if (auto* const node = registration->slot->read; node != nullptr)
            cancelOperation(*node);
        return {};
    }

    // Armed HERE rather than left to the next wait, because this is the one call that
    // can REPORT the kernel's refusal. A registration the caller believes succeeded and
    // the kernel never made parks a flow with nothing left to resume it: no message, no
    // stack, just a hang
    // ([fastcached#1054](https://github.com/LASTRADA-Software/fastcached/issues/1054)).
    if (!rearm(*registration, ArmFailure::ToCaller))
        return std::unexpected { makeNetError(
            NetErrorCode::SystemError,
            static_cast<int>(GetLastError()),
            "IocpBackend::setInterest: could not arm the readiness bridge") };
    return {};
}

void IocpBackend::detach(ReadinessHandler& handler) noexcept
{
    auto* const registration = find(handler);
    if (registration == nullptr)
        return;

    auto* const slot = registration->slot;
    _registrations.erase(&handler);

    // Retired BEFORE the handler's share is given back: a packet dequeued after this
    // must find `retired()` and must not read `handler`.
    retire(*slot);

    // ... and out of the batch a wait in flight is walking, which erasing above does
    // nothing about: the batch holds its own pointers, taken before any callback ran.
    _batch.withdraw(handler);

    handler.slot.reset();
}

void IocpBackend::wake() noexcept
{
    // Coalesced: a burst of `post()` from another thread costs one packet rather than
    // one per call, and the loop drains them all in one dequeue anyway. A wake raised
    // with no wait in flight is not lost — the packet sits in the port and the next
    // wait returns at once, which is the same promise `detail::WakeupChannel` makes for
    // the backends that need a pipe to make it.
    if (_wakePending.exchange(true, std::memory_order_acq_rel))
        return;
    if (PostQueuedCompletionStatus(_port, 0, KeyWake, nullptr) == 0)
    {
        _wakePending.store(false, std::memory_order_release);
        // The sink can run with the loop's inbound lock held: `EventLoop::handOverFinishedRoot`
        // wakes under it. So a diagnostic sink must not `post()` to this loop -- that takes the same
        // lock, on the same thread -- and should only record.
        reportDiagnostic(
            std::format("IocpBackend::wake: PostQueuedCompletionStatus failed: {}", GetLastError()));
    }
}

bool IocpBackend::armSignal(Registration& registration, void* target, Readiness report) noexcept
{
    auto& slot = *registration.slot;
    auto& node = slot.addNode(report, /*isSocketRead*/ false);
    slot.retain();
    _inFlight.insert(&node.overlapped);
    // Published before the wait is armed, so the callback cannot fire and find the
    // previous arm. There is no callback in flight at this moment: the previous arm
    // either completed or went through `WaitForThreadpoolWaitCallbacks`.
    slot.signal.store(&node, std::memory_order_release);

    auto armed = false;
    if (_useWaitPackets)
    {
        if (!slot.waitPacket)
            slot.waitPacket = std::make_unique<detail::WaitCompletionPacket>();
        armed = slot.waitPacket->valid()
                && slot.waitPacket->associate(_port, target, KeyReadiness, &node.overlapped);
    }
    else
    {
        // One wait object per registration, reused across arms: `SetThreadpoolWait` is
        // one-shot, so the object outlives the arm and only the association does not.
        // Its context is the SLOT rather than the node, because a context is fixed at
        // creation and the node is not.
        if (slot.threadpoolWait == nullptr)
            slot.threadpoolWait = CreateThreadpoolWait(&onThreadpoolWait, &slot, nullptr);
        if (slot.threadpoolWait != nullptr)
        {
            SetThreadpoolWait(slot.threadpoolWait, target, nullptr);
            armed = true;
        }
    }

    if (armed)
        return true;

    slot.signal.store(nullptr, std::memory_order_release);
    node.retired = true;
    _inFlight.erase(&node.overlapped);
    slot.removeNode(node);
    slot.release();
    return false;
}

bool IocpBackend::armSocketRead(Registration& registration) noexcept
{
    auto& slot = *registration.slot;
    auto& node = slot.addNode(Readiness::Readable, /*isSocketRead*/ true);
    slot.retain();
    _inFlight.insert(&node.overlapped);
    slot.read = &node;

    // The zero-byte receive: the documented Winsock idiom for "complete when data is
    // pending, consuming nothing" (fastcached `Net/IocpSocket.cpp:555-561`). It
    // completes for data, for EOF and for an error alike, all of which are
    // `Readiness::Readable` at this layer — the reader is woken and its own `recv`
    // reports which. Telling a zero-byte completion from EOF apart is the SOCKET's job
    // and needs an `MSG_PEEK` (`IocpSocket.cpp:327-334`); doing it here would consume a
    // readiness nobody asked this layer to interpret, and answer a question the
    // `Readiness` vocabulary does not ask.
    auto buffer = WSABUF { .len = 0, .buf = nullptr };
    auto received = DWORD { 0 };
    auto flags = DWORD { 0 };
    auto const rc = WSARecv(slot.socket,
                            &buffer,
                            1,
                            &received,
                            &flags,
                            reinterpret_cast<LPWSAOVERLAPPED>(&node.overlapped),
                            nullptr);
    // `rc == 0` means it completed INLINE, and the packet is still delivered to the
    // port — nothing here sets `SetFileCompletionNotificationModes` — so both answers
    // mean the same thing: a completion is coming, and the dequeue owns the share.
    if (rc == 0 || WSAGetLastError() == WSA_IO_PENDING)
        return true;

    slot.read = nullptr;
    node.retired = true;
    _inFlight.erase(&node.overlapped);
    slot.removeNode(node);
    slot.release();
    return false;
}

bool IocpBackend::rearm(Registration& registration, ArmFailure report) noexcept
{
    auto& slot = *registration.slot;
    auto* const handler = registration.handler;
    if (registration.interest == Interest::None || handler == nullptr)
        return true;

    auto armed = true;
    switch (handler->kind)
    {
        case HandleKind::Waitable: {
            // One mechanism, reporting whichever directions the registration asked for:
            // a signalled waitable object says "something happened on this handle", not
            // which direction, so it reports every direction the registration asked for.
            if (slot.signal.load(std::memory_order_acquire) != nullptr)
                break; // already in flight; its completion is what re-arms it
            auto observed = Readiness::None;
            if (hasInterest(registration.interest, Interest::Read))
                observed = observed | Readiness::Readable;
            if (hasInterest(registration.interest, Interest::Write))
                observed = observed | Readiness::Writable;
            armed = armSignal(registration, handler->handle, observed);
            break;
        }
        case HandleKind::Socket: {
            if (hasInterest(registration.interest, Interest::Read) && slot.read == nullptr)
                armed = armSocketRead(registration);
            if (hasInterest(registration.interest, Interest::Write)
                && slot.signal.load(std::memory_order_acquire) == nullptr)
            {
                // Writability has no completion of its own, so it is selected onto an
                // event and the waitable-handle bridge takes it from there. The event
                // is the backend's: `WSAEventSelect` allows exactly one event per
                // socket and this one is it, which is also why a socket registered here
                // must not be selected by anybody else.
                auto selected = slot.writeEvent != WSA_INVALID_EVENT;
                if (!selected)
                {
                    slot.writeEvent = WSACreateEvent();
                    selected = slot.writeEvent != WSA_INVALID_EVENT
                               && WSAEventSelect(slot.socket, slot.writeEvent, WriteEventMask) == 0;
                }
                // Only when the selection took. An event nothing selects onto never
                // signals, so arming a bridge over it would spend a share on an
                // operation that can never complete and report success while doing it.
                armed = selected && armSignal(registration, slot.writeEvent, Readiness::Writable) && armed;
            }
            break;
        }
        case HandleKind::Completion: {
            // Nothing to arm: the OWNER issued the operation, and its completion is what makes
            // this readable. What is done here is the level-triggering the other kinds get from
            // re-arming: a completion that has been dequeued is reported on every wait until the
            // owner takes the park back. That is also what reports a park registered AFTER its
            // completion arrived -- a cancel detaches the park a stop came through, and the
            // owner registers a fresh one to hear the abort on -- because the dequeue marked the
            // operation whether or not anything was registered to hear it.
            if (hasInterest(registration.interest, Interest::Read) && slot.operation != nullptr
                && slot.operation->completed)
                _batch.add(*handler, Readiness::Readable);
            break;
        }
        case HandleKind::Fd: armed = false; break; // refused at attach; unreachable
    }

    if (armed)
        return true;

    reportDiagnostic(std::format("IocpBackend: could not arm a readiness bridge: {}", GetLastError()));
    if (report == ArmFailure::ToCaller)
        return false;

    // A re-arm inside `wait()` has no caller to report to, and a failure that reached
    // nobody would park a flow with nothing left to resume it. So it wakes the handler
    // instead, on the direction it watches — Ruling R101's order, which holds here for
    // its own reason: the caller learns what went wrong from its own `recv` or `send`,
    // so taking the wakeup away from it loses the wakeup and explains nothing.
    auto observed = Readiness::Failed;
    if (hasInterest(registration.interest, Interest::Read))
        observed = observed | Readiness::Readable;
    if (hasInterest(registration.interest, Interest::Write))
        observed = observed | Readiness::Writable;
    _batch.add(*handler, observed);
    return false;
}

void IocpBackend::rearmAll() noexcept
{
    // Over a snapshot of the keys rather than over the map: `rearm` adds to `_batch`
    // and never to `_registrations`, so the map is stable here — the snapshot is what
    // keeps that true of a future edit rather than of today's code only.
    auto handlers = std::vector<ReadinessHandler const*> {};
    handlers.reserve(_registrations.size());
    for (auto const& entry: _registrations)
        handlers.push_back(entry.first);
    for (auto const* const key: handlers)
        if (auto const found = _registrations.find(key); found != _registrations.end())
            std::ignore = rearm(found->second, ArmFailure::ToBatch);
}

void IocpBackend::cancelOperation(Operation& node) noexcept
{
    if (node.retired)
        return;
    node.retired = true;

    auto& slot = *node.slot;
    if (slot.signal.load(std::memory_order_acquire) == &node)
        slot.signal.store(nullptr, std::memory_order_release);
    if (slot.read == &node)
        slot.read = nullptr;

    auto outstanding = true;
    if (node.isSocketRead)
    {
        // `CancelIoEx` does not take the operation back, it ASKS for it back: the abort
        // completion is still delivered, so a packet is still coming and the dequeue
        // still owns the share.
        std::ignore = CancelIoEx(reinterpret_cast<HANDLE>(slot.socket), &node.overlapped);
    }
    else if (_useWaitPackets)
    {
        outstanding = !(slot.waitPacket && slot.waitPacket->cancel());
    }
    else if (slot.threadpoolWait != nullptr)
    {
        SetThreadpoolWait(slot.threadpoolWait, nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(slot.threadpoolWait, TRUE);
        // Authoritative only now: the callback has either run to completion or been
        // cancelled, so the count cannot change under this comparison.
        outstanding = node.posts.load(std::memory_order_acquire) != node.dequeued;
    }
    else
    {
        outstanding = false; // never armed through a mechanism that can still deliver
    }

    if (outstanding)
        return; // the dequeue owns the share, and the node with it

    _inFlight.erase(&node.overlapped);
    slot.removeNode(node);
    slot.release();
}

void IocpBackend::retire(Slot& slot) noexcept
{
    // Each current arm stood down first, while the mechanisms are still there to cancel
    // through; then the bridges closed; then every node marked, including ones already
    // retired whose packets have not come back.
    if (auto* const node = slot.signal.load(std::memory_order_acquire); node != nullptr)
        cancelOperation(*node);
    if (auto* const node = slot.read; node != nullptr)
        cancelOperation(*node);
    slot.closeBridges();
    slot.retireAll();
    // The route goes with the registration: a completion dequeued after this finds none and is
    // only accounted for. The operation itself belongs to its owner and is not touched.
    if (slot.operation != nullptr)
    {
        slot.operation->route = nullptr;
        slot.operation = nullptr;
    }
    // The port's association record is deliberately NOT cleared here. An association
    // outlives a registration — it ends when the HANDLE is closed, which is the
    // owner's event and not this one — and forgetting it here would make the next
    // `attach` of the same live socket ask for a second association and be refused.
    // `ICompletionPort::forget` is what a closing owner calls, and its comment says why.
}

void IocpBackend::consumeCompletion(std::uintptr_t key, void* overlapped, Collect collect) noexcept
{
    if (key == KeyWake)
    {
        _wakePending.store(false, std::memory_order_release);
        return;
    }
    if (overlapped == nullptr)
        return; // a bare nudge, carrying nothing

    // **Is this packet even ours?** A port serves everything associated with it, and
    // `ICompletionPort` is lent out precisely so a socket can issue overlapped
    // operations of its own on this one. Those complete here too, carrying a pointer
    // into the SOCKET's structures — and casting one of those to an `Operation` would
    // read an arbitrary struct as this backend's own. So a packet belongs to this
    // backend exactly when its pointer is one this backend handed over.
    //
    // An OWNER's operation -- a socket's receive, a listener's accept, a dial's connect -- is
    // recognised by the record `beginOperation` made before it was issued, and routed rather
    // than interpreted: this backend never reads what it did.
    if (_issued.erase(overlapped) != 0)
    {
        consumeOwnerCompletion(*static_cast<detail::IocpOperation*>(overlapped), collect);
        return;
    }
    // Anything else is a pointer nobody told this port about, and reading it as either kind
    // would be reading an arbitrary struct. It is dropped, and said.
    if (!_inFlight.contains(overlapped))
    {
        reportDiagnostic("IocpBackend: a completion arrived for an operation nobody announced to "
                         "this port (ICompletionPort::beginOperation); it was dropped");
        return;
    }

    // The recovery, and the reason `Operation` is standard-layout with `overlapped`
    // first: the address of the object and the address of its first member are the same
    // address, so this is a cast rather than an `offsetof` on a type with no guarantee
    // to offer one.
    static_assert(std::is_standard_layout_v<Operation>,
                  "an OVERLAPPED handed to the kernel is recovered by casting it back to its "
                  "enclosing Operation, which is the same address only on a standard-layout type");
    auto* const node = static_cast<Operation*>(overlapped);
    auto& slot = *node->slot;

    ++node->dequeued;
    assert(node->generation <= slot.generation()
           && "a completion named an arm later than any this registration issued");

    if (!node->retired && !slot.retired() && collect == Collect::Yes)
    {
        // Only here is `slot.handler` read, and only because neither retirement was
        // set: a retired slot names storage the caller may already have freed.
        _batch.add(*slot.handler, node->report);
        if (slot.socket != detail::InvalidSocket && !node->isSocketRead
            && slot.writeEvent != WSA_INVALID_EVENT)
        {
            // The one way an indication is taken off a Winsock event, and the reason
            // nothing here reaches for `WSAResetEvent`: a reset clears the EVENT and
            // leaves the RECORD standing, so an indication raised between the failing
            // syscall and the reset is lost for good (`windows/NetworkEvents.hpp`).
            std::ignore = consumeNetworkEvents(slot.socket, slot.writeEvent);
        }
    }

    if (slot.signal.load(std::memory_order_acquire) == node)
        slot.signal.store(nullptr, std::memory_order_release);
    if (slot.read == node)
        slot.read = nullptr;

    _inFlight.erase(overlapped);
    slot.removeNode(*node);
    slot.release();
}

void IocpBackend::consumeOwnerCompletion(detail::IocpOperation& operation, Collect collect) noexcept
{
    operation.completed = true;

    // Reported to the park waiting for it, if there is one and it still listens. `route` is
    // cleared by `retire` whenever the park goes, so a non-null route names a live slot; the
    // registration is looked up rather than trusted for its interest, because a mute is recorded
    // there and not on the slot.
    if (auto* const slot = static_cast<Slot*>(operation.route);
        slot != nullptr && !slot->retired() && collect == Collect::Yes)
    {
        if (auto const* const registration = find(*slot->handler);
            registration != nullptr && hasInterest(registration->interest, Interest::Read))
            _batch.add(*slot->handler, Readiness::Readable);
    }

    // LAST, and nothing touches `operation` afterwards: this is where its owner gives back the
    // share that kept it alive while the kernel held it, so it may be freed right here. A packet
    // that names a socket already destroyed is the ordinary case this exists for
    // ([fastcached#465](https://github.com/LASTRADA-Software/fastcached/issues/465)).
    if (auto* const onDequeued = operation.onDequeued; onDequeued != nullptr)
        onDequeued(operation);
}

void IocpBackend::beginOperation(void* operation)
{
    _issued.insert(operation);
}

void IocpBackend::withdrawOperation(void* operation) noexcept
{
    _issued.erase(operation);
}

WaitResult IocpBackend::wait(std::optional<platform::SteadyDuration> timeout)
{
    // G1: exactly one thread dequeues the port. A completion port will happily be
    // drained by four threads and that is its selling point elsewhere; here it would
    // resume one coroutine on two threads. `core-cpp.iocp-canary` is a program that
    // violates this and must die.
    assert((!_dequeuer.running() || _dequeuer.isOnWorkerThread())
           && "a second thread entered IocpBackend::wait() while another is dequeuing this port "
              "(G1: exactly one thread dequeues a loop or a completion port)");
    auto const onWorker = detail::WorkerIdentity::Scope { _dequeuer };

    // Before the wait: an arm is what makes a readiness reportable at all, and a
    // registration the last dispatch detached must not be re-armed.
    rearmAll();

    auto const timeoutMs = detail::toTimeoutMillis(timeout);
    // A failure `rearmAll` collected has to be dispatched, not slept through: an
    // indefinite wait with a `Failed` entry already in the batch would hold it until
    // something unrelated woke the port. So the batch decides first, and only then does
    // the caller's timeout — two questions, and nesting them as one expression asked the
    // reader to unpick which.
    auto waitFor = DWORD { 0 };
    if (_batch.size() == 0)
        waitFor = timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs);

    auto entries = std::array<OVERLAPPED_ENTRY, CompletionBatch> {};
    auto removed = ULONG { 0 };
    auto const ok = GetQueuedCompletionStatusEx(
        _port, entries.data(), static_cast<ULONG>(CompletionBatch), &removed, waitFor, FALSE);

    if (ok == 0)
    {
        if (auto const error = GetLastError(); error != WAIT_TIMEOUT)
            reportDiagnostic(std::format("IocpBackend: GetQueuedCompletionStatusEx failed: {}", error));
        // A timeout is not a failure, and a failure here has no handle to pin it on;
        // either way whatever `rearmAll` collected still has to be dispatched.
        return _batch.size() != 0 ? WaitResult { .dispatched = _batch.dispatch() } : WaitResult {};
    }

    for (auto const index: std::views::iota(std::size_t { 0 }, static_cast<std::size_t>(removed)))
        consumeCompletion(entries[index].lpCompletionKey, entries[index].lpOverlapped, Collect::Yes);

    return WaitResult { .dispatched = _batch.dispatch() };
}

void IocpBackend::drainOutstanding() noexcept
{
    if (_inFlight.empty() && _issued.empty())
        return;

    auto entries = std::array<OVERLAPPED_ENTRY, CompletionBatch> {};
    auto const deadline = std::chrono::steady_clock::now() + DrainBudget;
    while (!_inFlight.empty() || !_issued.empty())
    {
        auto removed = ULONG { 0 };
        auto const ok = GetQueuedCompletionStatusEx(
            _port, entries.data(), static_cast<ULONG>(CompletionBatch), &removed, DrainSliceMs, FALSE);
        if (ok != 0)
            for (auto const index: std::views::iota(std::size_t { 0 }, static_cast<std::size_t>(removed)))
                consumeCompletion(entries[index].lpCompletionKey, entries[index].lpOverlapped, Collect::No);

        if (std::chrono::steady_clock::now() >= deadline)
        {
            // Said rather than asserted, and said with the number: what is left is a
            // share the kernel never gave back, which leaks the slots it names. A
            // sanitizer run reports the leak; this reports which side of the handover
            // it came from, which the leak alone does not say.
            reportDiagnostic(std::format("IocpBackend: {} readiness and {} owner operation(s) still "
                                         "held by the kernel after {}ms; what they name leaks",
                                         _inFlight.size(),
                                         _issued.size(),
                                         DrainBudget.count()));
            return;
        }
    }
}

} // namespace core::net
