// SPDX-License-Identifier: Apache-2.0
///
/// The IOCP backend's own cases: the three readiness bridges a completion port has to
/// synthesise, and the ownership rule underneath them.
///
/// What is NOT here is the behaviour every backend shares — that is `BackendParity_test`,
/// which enumerates `testing::BackendMatrix` and so exercises this backend for every
/// property it holds in common with poll, epoll and kqueue. These are the ones
/// only this backend has:
///
/// - **both waitable-handle bridges**, because the kernel's one
///   (`NtAssociateWaitCompletionPacket`) is available on every machine since Windows 8,
///   so without a case that forces the other the thread-pool path would be compiled by
///   CI and run by nobody;
/// - **`HandleKind::Socket`**, which no other backend can serve: a raw SOCKET is not a
///   waitable object, so a wait-for-objects backend could not be handed one and a
///   parity case could not be written over it;
/// - **a completion arriving after its registration is gone**, which is the failure the
///   whole slot mechanism exists to prevent and which only a completion-based backend
///   can have.
///
/// The two refusals — a second thread dequeuing the port (G1), and a handle associated
/// with the port twice (G4) — are assertions, and an assertion cannot be asserted from
/// inside a Catch case: it aborts the process. They are `core-cpp.iocp-canary`, one
/// process per refusal, each judged by the marker it prints before the forbidden call.

// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include <core/net/detail/ReadinessSlot.hpp>
#include <core/net/windows/IocpBackend.hpp>
#include <core/net/windows/WaitCompletionPacket.hpp>
#include <core/net/windows/WindowsLoopback.hpp>
#include <core/platform/WinsockInit.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <ranges>
#include <string_view>
#include <tuple>
#include <utility>

using core::net::Interest;
using core::net::IocpBackend;

namespace
{

/// `INVALID_SOCKET` as a `SOCKET` rather than as the macro, for the reason
/// `IocpBackend.cpp` names its own copy: the macro's inner `~0` is an `int`, so every
/// comparison against it reads as a signed/unsigned one.
constexpr SOCKET InvalidSocketValue = INVALID_SOCKET;

/// How long a case waits for a bridge to report a readiness it has already provoked.
///
/// Every wait here is on a signal that has ALREADY been raised, so the honest budget is
/// one thread-pool dispatch — microseconds. Two seconds is four orders of magnitude of
/// slack for a cold two-core runner under a sanitizer, and a regression exhausts it and
/// fails rather than hanging the binary (`.agent/rules/testing.md`).
constexpr auto ReadinessBudget = std::chrono::seconds { 2 };

/// A MANUAL-reset Win32 event, owned.
///
/// Manual-reset because the thread-pool bridge must see a readiness twice: the
/// thread-pool bridge is re-armed at the top of every wait, and an auto-reset event
/// would be consumed by whichever arm got there first, so a readiness could be drained
/// by a wait that had nothing to do with it.
class OwnedEvent
{
  public:
    OwnedEvent(): _handle { CreateEventW(nullptr, TRUE, FALSE, nullptr) } {}

    OwnedEvent(OwnedEvent const&) = delete;
    OwnedEvent& operator=(OwnedEvent const&) = delete;
    OwnedEvent(OwnedEvent&&) = delete;
    OwnedEvent& operator=(OwnedEvent&&) = delete;

    ~OwnedEvent()
    {
        if (_handle != nullptr)
            CloseHandle(_handle);
    }

    /// @return The raw handle, for registering and for asserting it was made.
    [[nodiscard]] HANDLE get() const noexcept { return _handle; }

    /// Signals the event, which is what makes its registration ready.
    void signal() const noexcept { std::ignore = SetEvent(_handle); }

    /// Clears it again, so a case can tell one readiness from the next.
    void reset() const noexcept { std::ignore = ResetEvent(_handle); }

  private:
    HANDLE _handle;
};

/// One registration and the callbacks it counts.
///
/// Heap-allocated by the cases that free it under a live packet, which is what makes
/// the use-after-free case a use-after-free rather than a read of a stale stack slot a
/// sanitizer has no shadow for.
struct Probe
{
    core::net::ReadinessHandler handler {};
    int readable = 0;
    int writable = 0;
    int failed = 0;

    /// @param handle The handle to watch.
    /// @param kind What @p handle is.
    Probe(core::platform::NativeHandle handle, core::net::HandleKind kind) noexcept
    {
        handler = core::net::ReadinessHandler { .handle = handle,
                                                .kind = kind,
                                                .owner = this,
                                                .onReadable = &Probe::readableCallback,
                                                .onWritable = &Probe::writableCallback,
                                                .onError = &Probe::errorCallback };
    }

    Probe(Probe const&) = delete;
    Probe& operator=(Probe const&) = delete;
    Probe(Probe&&) = delete;
    Probe& operator=(Probe&&) = delete;
    ~Probe() = default;

    /// @return How many callbacks of any kind this probe has had.
    [[nodiscard]] int total() const noexcept { return readable + writable + failed; }

    static void readableCallback(core::net::ReadinessHandler& handler) noexcept
    {
        ++static_cast<Probe*>(handler.owner)->readable;
    }

    static void writableCallback(core::net::ReadinessHandler& handler) noexcept
    {
        ++static_cast<Probe*>(handler.owner)->writable;
    }

    static void errorCallback(core::net::ReadinessHandler& handler) noexcept
    {
        ++static_cast<Probe*>(handler.owner)->failed;
    }
};

/// Waits, bounded, until @p probe has been dispatched to at least once.
/// @param backend The backend to drive.
/// @param probe The probe to watch.
/// @return How many callbacks ran in total across the waits.
[[nodiscard]] std::size_t pumpUntilDispatched(IocpBackend& backend, Probe const& probe)
{
    auto served = std::size_t { 0 };
    auto const deadline = std::chrono::steady_clock::now() + ReadinessBudget;
    while (probe.total() == 0 && std::chrono::steady_clock::now() < deadline)
        served += backend.wait(std::chrono::milliseconds { 20 }).dispatched;
    return served;
}

/// A connected loopback pair, closed on the way out.
class LoopbackPair
{
  public:
    LoopbackPair() noexcept
    {
        core::platform::ensureWinsockInitialized();
        if (!core::net::makeLoopbackPair(_sockets))
            _sockets = { InvalidSocketValue, InvalidSocketValue };
    }

    LoopbackPair(LoopbackPair const&) = delete;
    LoopbackPair& operator=(LoopbackPair const&) = delete;
    LoopbackPair(LoopbackPair&&) = delete;
    LoopbackPair& operator=(LoopbackPair&&) = delete;

    ~LoopbackPair()
    {
        for (auto const socket: _sockets)
            if (socket != InvalidSocketValue)
                closesocket(socket);
    }

    /// @return Whether both sockets were made.
    [[nodiscard]] bool valid() const noexcept
    {
        return _sockets[0] != InvalidSocketValue && _sockets[1] != InvalidSocketValue;
    }

    /// @return The accepted (server) end.
    [[nodiscard]] SOCKET server() const noexcept { return _sockets[0]; }

    /// @return The connecting (client) end.
    [[nodiscard]] SOCKET client() const noexcept { return _sockets[1]; }

    /// @param socket The end to name as a registration handle.
    /// @return It, as the handle type a @c ReadinessHandler carries.
    [[nodiscard]] static core::platform::NativeHandle asHandle(SOCKET socket) noexcept
    {
        return reinterpret_cast<core::platform::NativeHandle>(socket);
    }

  private:
    std::array<SOCKET, 2> _sockets { InvalidSocketValue, InvalidSocketValue };
};

} // namespace

TEST_CASE("IocpBackend reports a waitable handle through either bridge", "[net][backend][windows][iocp]")
{
    // Both bridges, named rather than left to the machine. `NtAssociateWaitCompletionPacket`
    // has existed since Windows 8, so on every machine this suite runs on the Auto path
    // is the kernel one — and the thread-pool fallback, which is what a machine without
    // it would use and what the design calls THE path, would be compiled and never run.
    auto const bridge = GENERATE(IocpBackend::WaitBridge::Auto, IocpBackend::WaitBridge::Threadpool);
    CAPTURE(bridge == IocpBackend::WaitBridge::Auto ? "auto" : "threadpool");

    auto backend = IocpBackend { bridge };
    auto event = OwnedEvent {};
    REQUIRE(event.get() != nullptr);

    auto probe = Probe { event.get(), core::net::HandleKind::Waitable };
    REQUIRE(backend.attach(probe.handler).has_value());
    REQUIRE(backend.setInterest(probe.handler, Interest::Read).has_value());

    // Nothing has signalled, so nothing may be reported. This is the half that
    // distinguishes a working bridge from one that reports every registration on every
    // wait, which would pass every other assertion here.
    CHECK(backend.wait(std::chrono::milliseconds { 20 }).dispatched == 0);
    CHECK(probe.total() == 0);

    event.signal();
    auto const served = pumpUntilDispatched(backend, probe);
    CHECK(served >= 1);
    CHECK(probe.readable >= 1);
    CHECK(probe.writable == 0);
    CHECK(probe.failed == 0);

    backend.detach(probe.handler);
}

TEST_CASE("an IocpBackend registration still ready is reported on the next wait too",
          "[net][backend][windows][iocp]")
{
    // **Level-triggering, which on a completion port is not free.** An overlapped
    // operation is ONE-SHOT: it completes once and the kernel forgets it. So "a
    // readiness nobody consumed is reported again on the next wait" — which poll,
    // epoll and kqueue get from their kernels — has to be built here, by
    // re-arming every watched registration at the top of every wait.
    //
    // Nothing else in this suite or in `BackendParity_test` asks for a SECOND report:
    // every other registration is armed by `setInterest` and dispatched once. That was
    // measured rather than assumed — deleting the re-arm left all 166 cases green — so
    // this case exists because its absence was invisible.
    auto const bridge = GENERATE(IocpBackend::WaitBridge::Auto, IocpBackend::WaitBridge::Threadpool);
    auto backend = IocpBackend { bridge };
    auto event = OwnedEvent {};
    REQUIRE(event.get() != nullptr);

    // Manual-reset and never reset: the handle stays ready, so a level-triggered
    // backend owes a report on every wait and an edge-triggered one owes exactly one.
    auto probe = Probe { event.get(), core::net::HandleKind::Waitable };
    REQUIRE(backend.attach(probe.handler).has_value());
    REQUIRE(backend.setInterest(probe.handler, Interest::Read).has_value());
    event.signal();

    std::ignore = pumpUntilDispatched(backend, probe);
    auto const first = probe.readable;
    REQUIRE(first >= 1);

    // No `setInterest` between the two, deliberately: re-arming through the caller is
    // what the mute/unmute case does, and it would make this pass with the re-arm gone.
    auto const deadline = std::chrono::steady_clock::now() + ReadinessBudget;
    while (probe.readable == first && std::chrono::steady_clock::now() < deadline)
        std::ignore = backend.wait(std::chrono::milliseconds { 20 });
    CHECK(probe.readable > first);

    // And it stops when the handle does, which is the other half: a backend that
    // reported every registration on every wait would pass the check above too.
    event.reset();
    std::ignore = backend.wait(std::chrono::milliseconds { 20 }); // consumes whatever was in flight
    auto const settled = probe.readable;
    for ([[maybe_unused]] auto const round: std::views::iota(0, 3))
        std::ignore = backend.wait(std::chrono::milliseconds { 20 });
    CHECK(probe.readable == settled);

    backend.detach(probe.handler);
}

TEST_CASE("a muted IocpBackend registration is silent, and unmuting it reports again",
          "[net][backend][windows][iocp]")
{
    // "Mute means SILENT, on every backend", asked of the waitable-handle bridge.
    //
    // **What it cannot establish, said here rather than left to be assumed:** whether a
    // packet was in the port when the mute landed. The thread-pool callback may not have
    // started, and on the wait-completion-packet path the cancel REMOVES a queued packet,
    // so on that path there is deliberately never one. Neutering `consumeCompletion`'s
    // retirement check leaves this case green — measured, not guessed. The case below it
    // is the deterministic form, over a socket, where `CancelIoEx` guarantees a
    // completion is still coming; this one pins the behaviour, that one pins the
    // mechanism.
    auto const bridge = GENERATE(IocpBackend::WaitBridge::Auto, IocpBackend::WaitBridge::Threadpool);
    auto backend = IocpBackend { bridge };
    auto event = OwnedEvent {};
    REQUIRE(event.get() != nullptr);

    auto probe = Probe { event.get(), core::net::HandleKind::Waitable };
    REQUIRE(backend.attach(probe.handler).has_value());
    REQUIRE(backend.setInterest(probe.handler, Interest::Read).has_value());

    event.signal();
    // Let the bridge fire without dispatching it: the packet is now in the port, or
    // about to be, and the mute below is what must make it harmless.
    REQUIRE(backend.setInterest(probe.handler, Interest::None).has_value());

    for ([[maybe_unused]] auto const round: std::views::iota(0, 5))
        std::ignore = backend.wait(std::chrono::milliseconds { 20 });
    CHECK(probe.total() == 0);

    // And unmuting reports it again: the event is still signalled, so a level-triggered
    // backend owes exactly one more wakeup. A mute implemented by dropping the
    // registration would pass the check above and fail this one.
    REQUIRE(backend.setInterest(probe.handler, Interest::Read).has_value());
    std::ignore = pumpUntilDispatched(backend, probe);
    CHECK(probe.readable >= 1);

    backend.detach(probe.handler);
}

TEST_CASE("a muted IocpBackend socket registration drops the completion already in flight",
          "[net][backend][windows][iocp][loopback]")
{
    // The case above asks the same question of a waitable handle and CANNOT answer it
    // deterministically: whether a packet is in the port when the mute lands depends on
    // whether the thread-pool callback had started, and on the wait-completion-packet
    // path the cancel REMOVES the queued packet, so no packet ever arrives and the
    // retirement check is never reached. Neutering that check leaves it green.
    //
    // A socket makes it certain. `CancelIoEx` does not take an operation back, it asks
    // for it back: the completion — the real one or its abort — is delivered either
    // way. So after the mute there is always exactly one packet in flight for an arm
    // that has been retired, which is the state "mute means silent" has to survive.
    // Dropping the `node->retired` term in `consumeCompletion` turns this red and
    // nothing else.
    auto backend = IocpBackend {};
    auto pair = LoopbackPair {};
    if (!pair.valid())
        SKIP("could not create a loopback socket pair on this machine");

    auto probe = Probe { LoopbackPair::asHandle(pair.server()), core::net::HandleKind::Socket };
    REQUIRE(backend.attach(probe.handler).has_value());
    REQUIRE(backend.setInterest(probe.handler, Interest::Read).has_value());
    REQUIRE(backend.outstandingOperations() == 1);

    constexpr auto Message = std::string_view { "mute" };
    REQUIRE(std::cmp_equal(send(pair.client(), Message.data(), static_cast<int>(Message.size()), 0),
                           Message.size()));

    REQUIRE(backend.setInterest(probe.handler, Interest::None).has_value());

    // Pumped until the kernel has given the operation back, so the case cannot pass by
    // the packet simply not having arrived yet — which is exactly how the waitable-handle
    // version of this question passes without asking it.
    auto const deadline = std::chrono::steady_clock::now() + ReadinessBudget;
    while (backend.outstandingOperations() != 0 && std::chrono::steady_clock::now() < deadline)
        std::ignore = backend.wait(std::chrono::milliseconds { 20 });

    CHECK(backend.outstandingOperations() == 0);
    CHECK(probe.total() == 0);

    // And unmuting reports it again: the bytes are still in the receive buffer, so a
    // level-triggered backend owes exactly one more wakeup.
    REQUIRE(backend.setInterest(probe.handler, Interest::Read).has_value());
    std::ignore = pumpUntilDispatched(backend, probe);
    CHECK(probe.readable >= 1);

    backend.detach(probe.handler);
}

TEST_CASE("the IocpBackend wait-completion-packet probe answers, and the fallback can be forced",
          "[net][backend][windows][iocp]")
{
    // The probe is not asserted to SUCCEED: its failure must be ordinary, and this
    // suite has to keep passing on a build of Windows that does not export the entry
    // points. What IS asserted is that the two constructors disagree exactly when the
    // probe says they should, which is what makes the case above cover two paths rather
    // than the same one twice.
    auto const probed = core::net::detail::waitCompletionPacketsAvailable();
    INFO("NtAssociateWaitCompletionPacket available: " << probed);

    auto const automatic = IocpBackend { IocpBackend::WaitBridge::Auto };
    auto const forced = IocpBackend { IocpBackend::WaitBridge::Threadpool };

    CHECK(automatic.usesWaitCompletionPackets() == probed);
    CHECK_FALSE(forced.usesWaitCompletionPackets());

    if (!probed)
        WARN("this machine has no NtAssociateWaitCompletionPacket, so both bridge settings "
             "exercised the thread-pool path");
}

TEST_CASE("an IocpBackend completion that arrives after its handler is gone touches nothing",
          "[net][backend][windows][iocp]")
{
    // The whole reason `lpOverlapped` points at a backend-owned slot. The sequence a
    // production loop reaches every time a park resolves: a bridge is armed, the handle
    // signals, the kernel queues a packet, and the registration is detached and its
    // owner FREED before the loop next dequeues.
    //
    // **What this case pins and what it cannot**, said here rather than assumed: it
    // pins that the sequence is survivable and leaves the backend usable. It CANNOT
    // guarantee a packet was in flight — the thread-pool callback may not have started,
    // and the wait-completion-packet cancel removes a queued packet — so neutering the
    // retirement checks leaves it green, measured under AddressSanitizer. The socket
    // form below it is the deterministic one, because `CancelIoEx` removes nothing.
    auto const bridge = GENERATE(IocpBackend::WaitBridge::Auto, IocpBackend::WaitBridge::Threadpool);
    auto backend = IocpBackend { bridge };
    auto event = OwnedEvent {};
    REQUIRE(event.get() != nullptr);

    auto probe = std::make_unique<Probe>(event.get(), core::net::HandleKind::Waitable);
    REQUIRE(backend.attach(probe->handler).has_value());
    REQUIRE(backend.setInterest(probe->handler, Interest::Read).has_value());
    CHECK(backend.outstandingOperations() == 1);

    event.signal();
    backend.detach(probe->handler);
    probe.reset();

    // Whatever the race produced — a packet already queued, or a callback cancelled
    // before it ran — the port must be usable afterwards and nothing may have been
    // dispatched to the freed probe. A second registration is the control: if the
    // dropped packet had taken the batch or the accounting with it, this would never
    // report.
    auto survivor = Probe { event.get(), core::net::HandleKind::Waitable };
    REQUIRE(backend.attach(survivor.handler).has_value());
    REQUIRE(backend.setInterest(survivor.handler, Interest::Read).has_value());
    std::ignore = pumpUntilDispatched(backend, survivor);
    CHECK(survivor.readable >= 1);

    backend.detach(survivor.handler);
}

TEST_CASE("an IocpBackend socket completion that arrives after its handler is freed touches nothing",
          "[net][backend][windows][iocp][loopback]")
{
    // The deterministic form of the case above, and the one that actually reaches the
    // hazard. A waitable handle cannot be made to have a packet in flight on demand:
    // the thread-pool callback may not have started, and the wait-completion-packet
    // cancel REMOVES a queued packet. `CancelIoEx` removes nothing — it asks for the
    // operation back and the completion is delivered regardless — so after the detach
    // below there is always exactly one packet naming a slot whose handler has been
    // FREED.
    //
    // Measured: with both retirement checks dropped from `consumeCompletion`, the
    // waitable version stays green and this one reports heap-use-after-free under
    // AddressSanitizer, on the `ReadinessHandler` the dispatch reads back.
    //
    // The probe is on the heap and really deleted, so the sanitizer has a shadow for it;
    // a stack probe left standing would make this green against the defect.
    auto backend = IocpBackend {};
    auto pair = LoopbackPair {};
    if (!pair.valid())
        SKIP("could not create a loopback socket pair on this machine");

    auto probe =
        std::make_unique<Probe>(LoopbackPair::asHandle(pair.server()), core::net::HandleKind::Socket);
    REQUIRE(backend.attach(probe->handler).has_value());
    REQUIRE(backend.setInterest(probe->handler, Interest::Read).has_value());
    REQUIRE(backend.outstandingOperations() == 1);

    constexpr auto Message = std::string_view { "gone" };
    REQUIRE(std::cmp_equal(send(pair.client(), Message.data(), static_cast<int>(Message.size()), 0),
                           Message.size()));

    backend.detach(probe->handler);
    probe.reset();

    // Pumped until the kernel has handed the operation back, so the case cannot pass by
    // the completion simply not having arrived.
    auto const deadline = std::chrono::steady_clock::now() + ReadinessBudget;
    while (backend.outstandingOperations() != 0 && std::chrono::steady_clock::now() < deadline)
        std::ignore = backend.wait(std::chrono::milliseconds { 20 });
    CHECK(backend.outstandingOperations() == 0);

    // And the backend is still usable, which a dropped packet that took the accounting
    // with it would break.
    auto survivor = Probe { LoopbackPair::asHandle(pair.client()), core::net::HandleKind::Socket };
    REQUIRE(backend.attach(survivor.handler).has_value());
    REQUIRE(backend.setInterest(survivor.handler, Interest::Write).has_value());
    std::ignore = pumpUntilDispatched(backend, survivor);
    CHECK(survivor.writable >= 1);
    backend.detach(survivor.handler);
}

TEST_CASE("an IocpBackend destroyed with a packet in flight gives every share back",
          "[net][backend][windows][iocp][loopback]")
{
    // The other half of the ownership rule, and the one a detach cannot cover: a port
    // closed with packets still in it never dequeues them, so every share they hold is
    // a slot that leaks. The teardown drain is what takes them back — and LeakSanitizer
    // does not exist on Windows, so nothing else here would notice.
    //
    // **Measured from outside**, through a second share of the slot the case keeps for
    // itself. After the backend is gone, the only share left must be that one: the
    // handler's went back with the detach the teardown performs, and the kernel's went
    // back with the drain. Delete the `drainOutstanding()` call and the count is 2.
    // Nothing else in this suite distinguishes those two numbers.
    auto pair = LoopbackPair {};
    if (!pair.valid())
        SKIP("could not create a loopback socket pair on this machine");

    auto probe = Probe { LoopbackPair::asHandle(pair.server()), core::net::HandleKind::Socket };
    auto held = core::net::detail::ReadinessSlotRef {};

    {
        auto backend = IocpBackend {};
        REQUIRE(backend.attach(probe.handler).has_value());
        REQUIRE(backend.setInterest(probe.handler, Interest::Read).has_value());
        REQUIRE(backend.outstandingOperations() == 1);

        // A completion the kernel is certain to deliver, so the drain has real work.
        constexpr auto Message = std::string_view { "teardown" };
        REQUIRE(std::cmp_equal(send(pair.client(), Message.data(), static_cast<int>(Message.size()), 0),
                               Message.size()));

        held = probe.handler.slot;
        REQUIRE(static_cast<bool>(held));
        // Three shares: the handler's, the armed receive's, and this case's.
        CHECK(held.get()->referenceCount() == 3);
        // Deliberately neither waited on nor detached.
    }

    // The handler holds nothing that outlives the backend, which is the observable half
    // of "the slot is the BACKEND's".
    CHECK_FALSE(static_cast<bool>(probe.handler.slot));
    REQUIRE(held.get() != nullptr);
    CHECK(held.get()->referenceCount() == 1);
    CHECK(held.get()->retired());
}

TEST_CASE("IocpBackend reports socket readability through a zero-byte receive",
          "[net][backend][windows][iocp][loopback]")
{
    // `HandleKind::Socket`, which no other backend can be handed: a SOCKET is not a
    // waitable object, so this is the one bridge that has no parity case to hide in.
    auto backend = IocpBackend {};
    auto pair = LoopbackPair {};
    if (!pair.valid())
        SKIP("could not create a loopback socket pair on this machine");

    auto probe = Probe { LoopbackPair::asHandle(pair.server()), core::net::HandleKind::Socket };
    REQUIRE(backend.attach(probe.handler).has_value());
    // Attaching a socket is what associates it, which is G4's one entry point.
    CHECK(backend.completionPort() != nullptr);
    CHECK(backend.completionPort()->isAssociated(LoopbackPair::asHandle(pair.server())));

    REQUIRE(backend.setInterest(probe.handler, Interest::Read).has_value());

    // An idle socket is not readable, and a zero-byte receive that completed at once
    // would make every assertion below pass for the wrong reason.
    CHECK(backend.wait(std::chrono::milliseconds { 20 }).dispatched == 0);
    CHECK(probe.total() == 0);

    constexpr auto Message = std::string_view { "iocp" };
    REQUIRE(std::cmp_equal(send(pair.client(), Message.data(), static_cast<int>(Message.size()), 0),
                           Message.size()));

    std::ignore = pumpUntilDispatched(backend, probe);
    CHECK(probe.readable >= 1);

    // And the receive consumed nothing: the bytes are still there for the reader, which
    // is the whole point of the zero-byte form. A bridge that used an ordinary receive
    // would wake the reader and have eaten what it came for.
    auto buffer = std::array<char, 16> {};
    auto const got = recv(pair.server(), buffer.data(), static_cast<int>(buffer.size()), 0);
    CHECK(std::cmp_equal(got, Message.size()));

    backend.detach(probe.handler);
}

TEST_CASE("IocpBackend reports socket writability through WSAEventSelect",
          "[net][backend][windows][iocp][loopback]")
{
    auto backend = IocpBackend {};
    auto pair = LoopbackPair {};
    if (!pair.valid())
        SKIP("could not create a loopback socket pair on this machine");

    auto probe = Probe { LoopbackPair::asHandle(pair.client()), core::net::HandleKind::Socket };
    REQUIRE(backend.attach(probe.handler).has_value());
    REQUIRE(backend.setInterest(probe.handler, Interest::Write).has_value());

    // A connected socket with an empty send buffer is writable, and `WSAEventSelect`
    // records `FD_WRITE` for one that is already connected. If a future Winsock stops
    // doing that, the bounded pump below fails rather than hanging, and it says so.
    std::ignore = pumpUntilDispatched(backend, probe);
    CHECK(probe.writable >= 1);
    CHECK(probe.readable == 0);

    backend.detach(probe.handler);
}

TEST_CASE("IocpBackend refuses a POSIX descriptor rather than waiting on nothing",
          "[net][backend][windows][iocp]")
{
    // `HandleKind::Fd` names a POSIX descriptor, which this platform has no kernel
    // object for. Accepting it would park a flow on a registration nothing can ever
    // report, which is a hang with no message — the same failure
    // `HostDrivenBackend::attach` refuses for its own reason.
    auto backend = IocpBackend {};
    auto event = OwnedEvent {};
    REQUIRE(event.get() != nullptr);

    auto handler = core::net::ReadinessHandler { .handle = event.get(), .kind = core::net::HandleKind::Fd };
    auto const attached = backend.attach(handler);
    REQUIRE_FALSE(attached.has_value());
    CHECK(attached.error().code == core::net::NetErrorCode::Unsupported);
}

TEST_CASE("IocpBackend's port is the one place a handle is associated", "[net][backend][windows][iocp]")
{
    auto backend = IocpBackend {};
    auto* const port = backend.completionPort();
    REQUIRE(port != nullptr);
    CHECK(port->nativeHandle() != nullptr);

    auto pair = LoopbackPair {};
    if (!pair.valid())
        SKIP("could not create a loopback socket pair on this machine");

    auto const handle = LoopbackPair::asHandle(pair.server());
    CHECK_FALSE(port->isAssociated(handle));
    REQUIRE(port->associate(handle).has_value());
    CHECK(port->isAssociated(handle));

    // The second association is the G4 refusal and it ASSERTS, so it cannot be provoked
    // from inside a Catch case: `core-cpp.iocp-canary.g4` is the program that does it.
    // What is asked here is the other half — that an owner which closes a handle can
    // take the record back, because a record left standing makes the next socket handed
    // that value look already associated and it is then associated with nothing.
    port->forget(handle);
    CHECK_FALSE(port->isAssociated(handle));

    // A handle that was never valid is refused rather than recorded.
    auto const refused = port->associate(core::platform::InvalidHandle);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == core::net::NetErrorCode::BadHandle);
}

TEST_CASE("an IocpBackend wake ends a wait, and one raised before the wait is not lost",
          "[net][backend][windows][iocp]")
{
    // The wake is the port itself here rather than the self-pipe every other blocking
    // backend needs, so the promise `detail::WakeupChannel` makes has to be re-made:
    // a wake with no wait in flight is not lost.
    auto backend = IocpBackend {};

    backend.wake();
    auto const before = std::chrono::steady_clock::now();
    std::ignore = backend.wait(std::nullopt); // would block for ever if the wake were lost
    CHECK(std::chrono::steady_clock::now() - before < ReadinessBudget);

    // ... and a second wake still works, which a coalescing flag left stuck would break
    // on exactly the second call and nowhere else.
    backend.wake();
    auto const again = std::chrono::steady_clock::now();
    std::ignore = backend.wait(std::nullopt);
    CHECK(std::chrono::steady_clock::now() - again < ReadinessBudget);
}
