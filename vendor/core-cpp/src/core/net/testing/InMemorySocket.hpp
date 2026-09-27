// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `InMemorySocket` — a deterministic, descriptor-free @c ISocket for tests, whose every closed
/// state answers the way a loopback TCP socket does.
///
/// **This is a FAKE, and `<core/net/testing/InMemoryTransport.hpp>` is not.** That header's
/// `makeSocketPair` creates a real AF_UNIX socketpair (POSIX) or a real loopback TCP pair
/// (Windows) and wraps both ends in the platform socket, so everything it does goes through a
/// kernel and a loop. This one has neither: two byte pipes in the process, completed inline on the
/// calling thread, so a case written over it runs without a loop, without a descriptor and without
/// a single wait. **They are kept apart on purpose.** Folding either into the other deletes the
/// difference `SocketClosedStates_test.cpp` exists to measure — a fake pinned against a real socket
/// needs a real socket to pin it against, and a fake that went through the kernel would no longer
/// be the thing a consumer builds a deterministic suite on.
///
/// **Why the parity matters more for a fake than for anything else here.** A consumer builds its
/// protocol, server and consensus suites on this type. A closed state it answers more permissively
/// than a real socket does not fail anywhere: it manufactures a passing test in every suite that
/// reaches it — a write to a peer that has gone "succeeds", so no in-memory case can see a session
/// the other end ended. That is worse than a missing test, because it is invisible and it compounds
/// downstream.
///
/// Origin: fastcached `src/FastCache/Net/InMemoryTransport.{hpp,cpp}`
/// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`), renamed from `InMemoryTransport` to
/// `InMemorySocket` because core-cpp's `InMemoryTransport.hpp` already names the real pair.

#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoAwaitable.hpp>
#include <core/net/NetError.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace core::net::testing
{

/// One direction of an in-memory connection: a FIFO of bytes, the facts a TCP stack keeps about
/// that direction, and a wake-up for each end.
///
/// **It records facts and wakes whoever waits on them; it decides nothing.** What a write meets
/// after the reading end has gone is the socket's decision (@c InMemorySocket), made from what the
/// pipe records here, the way a TCP stack decides it from its own state.
class InMemoryPipe
{
  public:
    /// What the pipe calls when something one of its ends waits on has changed.
    ///
    /// A function pointer and a `void*`, the house shape for a callback nothing stores: it is
    /// invoked synchronously, from inside the call that caused it, and it allocates nothing.
    using Wake = void (*)(void* state) noexcept;

    /// @param maxBytesInFlight How many bytes may be buffered before a write waits for the reader
    ///        to drain; zero means no bound.
    explicit InMemoryPipe(std::size_t maxBytesInFlight = 0) noexcept;

    /// Buffers as much of @p bytes as the bound allows. Wakes nobody: the writing socket calls
    /// @c wakeReader once it has recorded what it needs, because the reader it wakes may own that
    /// socket and destroy it.
    /// @param bytes What the writing end sends.
    /// @return How many bytes were taken, from the front of @p bytes.
    std::size_t push(std::span<std::byte const> bytes);

    /// Moves up to @p into.size() buffered bytes out. Wakes nobody, for @c push's reason: the
    /// reading socket calls @c wakeWriter last.
    /// @param into Where the bytes go.
    /// @return How many bytes were copied; `0` with @c isWriteClosed means EOF.
    std::size_t pull(std::span<std::byte> into);

    /// The writing end has finished sending: a FIN. Wakes the reader, which reads what is buffered
    /// and then EOF.
    void closeWrite() noexcept;

    /// The READING end has gone.
    ///
    /// Whether that close was a FIN or a reset is decided from what it left unread, the way a TCP
    /// stack decides it: a socket closed over bytes it never read answers with a reset at once.
    /// @return Whether bytes were still unread, so the close was a RESET.
    bool closeRead() noexcept;

    /// Delivers a reset in this direction: what is buffered is discarded, and both ends are woken
    /// to an error rather than to data or EOF.
    ///
    /// **Buffered bytes are dropped because Winsock drops them.** Measured on loopback
    /// ([fastcached#1553](https://github.com/LASTRADA-Software/fastcached/issues/1553)): Linux and
    /// macOS hand a reset connection's buffered bytes over before the error, Windows answers the
    /// error at once. The fake takes the stricter answer, so a case that passes over it does not
    /// depend on the platform it runs on.
    /// @param code What the reader and the writer of this direction are told from now on.
    void reset(NetErrorCode code) noexcept;

    /// @return Whether the writing end has finished sending.
    [[nodiscard]] bool isWriteClosed() const noexcept { return _writeClosed; }

    /// @return Whether the reading end has gone.
    [[nodiscard]] bool isReadClosed() const noexcept { return _readClosed; }

    /// @return Whether a reset has been delivered in this direction.
    [[nodiscard]] bool isReset() const noexcept { return _reset; }

    /// @return What a reset in this direction reports; meaningful once @c isReset.
    [[nodiscard]] NetErrorCode resetCode() const noexcept { return _resetCode; }

    /// @return How many bytes are buffered.
    [[nodiscard]] std::size_t buffered() const noexcept { return _buffer.size(); }

    /// Installs the reader's wake-up, called after a push, a FIN or a reset.
    /// @param wake What to call, or null to stop being woken.
    /// @param state Handed to @p wake; must outlive the registration.
    void onReadable(Wake wake, void* state) noexcept;

    /// Installs the writer's wake-up, called after a pull that made room, the reader's close, or a
    /// reset.
    /// @param wake What to call, or null to stop being woken.
    /// @param state Handed to @p wake; must outlive the registration.
    void onWritable(Wake wake, void* state) noexcept;

    /// Calls the reader's wake-up, if one is installed. The caller must hold a reference to this
    /// pipe for the duration: the wake-up resumes a coroutine that may destroy either socket.
    void wakeReader() const noexcept;

    /// Calls the writer's wake-up, if one is installed; the same obligation as @c wakeReader.
    void wakeWriter() const noexcept;

  private:
    std::size_t _maxInFlight;
    std::deque<std::byte> _buffer;
    bool _writeClosed { false };
    bool _readClosed { false };
    bool _reset { false };
    NetErrorCode _resetCode { NetErrorCode::ConnReset };
    Wake _readable { nullptr };
    void* _readableState { nullptr };
    Wake _writable { nullptr };
    void* _writableState { nullptr };
};

/// One end of an in-memory connection: reads drain one @c InMemoryPipe, writes fill the other.
///
/// **In every closed state it answers the way a loopback TCP socket does**, which
/// `SocketClosedStates_test.cpp` pins against a real loopback pair on every platform CI runs:
///   - the peer CLOSED with nothing of ours unread (FIN): reads drain, then EOF; the FIRST write
///     after the close is accepted and lost, and the reset it draws fails every later write and
///     every later read;
///   - the peer CLOSED with bytes of ours unread (a reset at once): every write fails, and reads
///     report the reset, with whatever was buffered for us discarded;
///   - the peer HALF-closed (@c shutdownWrite): reads drain, then EOF, and writes still succeed —
///     a half-closed peer still reads;
///   - THIS end closed: every operation answers @c NetErrorCode::BadHandle, @c shutdownWrite does
///     nothing, and a parked operation is completed with @c NetErrorCode::Cancelled;
///   - THIS end half-closed: writes fail with @c NetErrorCode::SystemError (`EPIPE`,
///     `WSAESHUTDOWN`), never with the @c NetErrorCode::WouldBlock a caller would retry, and reads
///     still work.
///
/// Where the platforms differ beyond the spelling, it takes Windows's answer, which is the stricter
/// one — see @c InMemoryPipe::reset.
///
/// **What it does NOT model, stated so nobody relies on it:**
///   - **A flow's stop token does not reach a parked operation.** A real socket routes a stop
///     through its loop's park table (@c ResultAwaitable::cancelThrough); this one has no loop, so
///     a parked read or write stays parked until data, the peer or @c close answers it. The failure
///     that produces is a hang, not a false pass.
///   - **A receive deadline.** @c setReceiveDeadline is the interface's default no-op, which that
///     default documents as safe: a weaker bound, never a wrong one.
///   - **Delivery latency.** Everything a write does to its peer — bytes, a FIN, a reset — has
///     happened by the time the write returns; on a real socket it lands a round trip later.
///   - **The loop's deferred resume.** @c close and @c cancelRead resume a parked flow INSIDE the
///     call here, because there is no loop to hand it to; every real transport settles it there and
///     its loop resumes it on a later drain step (G2, since 0.2.1). A case built on this double
///     therefore cannot see a flow that destroys the socket's owner under the verb's caller --
///     contour's `NativeClient::detach` shape -- and crashes on it where a real socket does not.
///     Such a case runs over @c makeSocketPair and a loop instead.
///
/// **Completion is inline, on the calling thread.** A peer's @c write that fills a parked read
/// resumes the reader before the write returns, so a case can assert on the reader immediately
/// after it. Single-threaded by construction: both ends and every coroutine awaiting them belong to
/// one thread. One consequence of that is worth knowing before it is met: over a BOUNDED pair, a
/// write much larger than the bound, drained by a reader that reads again as soon as it is resumed,
/// hands control back and forth once per bound's worth of bytes on one stack.
///
/// **One read operation and one write operation, enforced as on every real socket**
/// (@c contract::claimReadSlot, @c contract::claimWriteSlot). A double that let two writes
/// interleave would be more permissive than the transport it stands for — the exact divergence
/// this type is pinned against.
class InMemorySocket final: public ISocket
{
  public:
    /// @param inbound The pipe this end reads from — the peer's outbound.
    /// @param outbound The pipe this end writes into — the peer's inbound.
    /// @param peerAddress What @c peerAddress reports; "" (the default) means none, as a real
    ///        in-process transport has.
    InMemorySocket(std::shared_ptr<InMemoryPipe> inbound,
                   std::shared_ptr<InMemoryPipe> outbound,
                   std::string peerAddress = {}) noexcept;

    /// Abandons any parked operation rather than completing it — see @c ISocket::close and the
    /// socket contract's *a destructor abandons, close resolves* — and closes both directions.
    ~InMemorySocket() override;

    InMemorySocket(InMemorySocket const&) = delete;
    InMemorySocket(InMemorySocket&&) = delete;
    InMemorySocket& operator=(InMemorySocket const&) = delete;
    InMemorySocket& operator=(InMemorySocket&&) = delete;

    [[nodiscard]] IoAwaitable read(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable write(std::span<std::byte const> buffer) override;
    [[nodiscard]] IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override;

    /// @copydoc ISocket::waitReadable
    ///
    /// **It PARKS when there is nothing to report, exactly as a reactor socket does**, sharing the
    /// read slot with @c read. Upstream's answered `1` at once, which made a parked watch
    /// unreachable over the fake — the reason a separate `ParkingReadableSocket` had to exist — and
    /// turned a watchdog loop into a spin.
    [[nodiscard]] IoAwaitable waitReadable() override;

    /// @copydoc ISocket::cancelRead
    ///
    /// Overridden because this transport's reads park; the interface's default no-op would leave a
    /// parked read for the caller's next one to drop.
    void cancelRead() noexcept override;

    /// @copydoc ISocket::shutdownWrite
    ///
    /// Sends the FIN on the outbound pipe and leaves the inbound open. Completes inline; after
    /// @c close it does nothing, as every platform socket does.
    [[nodiscard]] ResultAwaitable<void> shutdownWrite() override;

    [[nodiscard]] std::string peerAddress() const override { return _peerAddress; }

    void close() noexcept override;

    /// @return True once @c close was called or a read observed the peer's EOF — @see
    ///         ISocket::isClosed, whose EOF latch this keeps as the platform sockets do.
    [[nodiscard]] bool isClosed() const noexcept override { return _closed || _peerClosed; }

  private:
    /// Which read verb holds the read slot.
    enum class ReadKind : std::uint8_t
    {
        Bytes, ///< @c read: consume into the caller's buffer.
        Probe, ///< @c waitReadable: report, consume nothing.
    };

    /// The socket's single read-side operation.
    struct ReadOperation
    {
        IoAwaitable* awaitable = nullptr; ///< The parked awaitable. Borrowed.
        ReadKind kind = ReadKind::Bytes;
        std::span<std::byte> buffer; ///< The caller's destination; empty for a probe.
    };

    /// The socket's single write-side operation: a cursor over what is left to send.
    struct WriteOperation
    {
        IoAwaitable* awaitable = nullptr;               ///< The parked awaitable. Borrowed.
        std::vector<std::span<std::byte const>> unsent; ///< Every segment; those before @c next are sent.
        std::size_t next = 0;                           ///< The first segment not yet wholly sent.
        std::size_t written = 0;                        ///< The total this write resolves with.
        std::shared_ptr<void const> keepAlive;          ///< Pins a vectored write's storage.
    };

    /// How a close reaches a parked operation.
    enum class CloseMode : std::uint8_t
    {
        Resolve, ///< @c close: complete with @c NetErrorCode::Cancelled, a value.
        Abandon, ///< The destructor: the flow unwinds and never re-enters its body.
    };

    /// Closes both directions and retires what is parked, detaching FIRST and settling LAST.
    /// @param mode Whether the parked flow may look at this socket again.
    void close(CloseMode mode) noexcept;

    /// The read slot's answer, if there is one yet.
    /// @param kind Which verb is asking.
    /// @param buffer Where a @c ReadKind::Bytes read puts its bytes.
    /// @return What to resolve with, or nullopt to park (or stay parked).
    [[nodiscard]] std::optional<IoResult> tryRead(ReadKind kind, std::span<std::byte> buffer);

    /// What a write of @p length bytes meets when this end has half-closed or the peer has gone,
    /// or nullopt when neither has happened. The first write after a graceful close draws the
    /// reset, both ways.
    /// @param length The bytes the write carries.
    /// @return The write's answer, or nullopt to write normally.
    [[nodiscard]] std::optional<IoResult> answerIfCannotWrite(std::size_t length) noexcept;

    /// Pushes what @p operation still owes.
    /// @param operation The write; its cursor and total advance in place.
    /// @return The total once nothing is left, an error, or nullopt to park (or stay parked).
    [[nodiscard]] std::optional<IoResult> trySend(WriteOperation& operation);

    /// Starts a write over @p segments: answers at once if it can, parks otherwise.
    /// @param segments What to send, in order; each must outlive the operation.
    /// @param keepAlive Pins their storage while parked.
    /// @return The awaitable.
    [[nodiscard]] IoAwaitable startWrite(std::span<std::span<std::byte const> const> segments,
                                         std::shared_ptr<void const> keepAlive);

    /// The reader's wake-up: something arrived on the inbound pipe.
    static void onInboundProgress(void* state) noexcept;

    /// The writer's wake-up: the outbound pipe has room, or its reader has gone.
    static void onOutboundProgress(void* state) noexcept;

    /// Tells this socket to forget a read the flow is unwinding from.
    static void retireRead(void* owner, void* awaitable) noexcept;

    /// Tells this socket to forget a write the flow is unwinding from.
    static void retireWrite(void* owner, void* awaitable) noexcept;

    std::shared_ptr<InMemoryPipe> _inbound;
    std::shared_ptr<InMemoryPipe> _outbound;
    std::string _peerAddress;

    /// What a verb recorded for the arm that follows it: the kind and buffer are known at the verb
    /// and the awaitable's final address only at the arm.
    ReadOperation _readPending;
    ReadOperation _read;
    WriteOperation _writePending;
    WriteOperation _write;

    bool _closed { false };
    bool _peerClosed { false };
};

/// Two @c InMemorySocket ends over a pair of pipes.
struct InMemorySocketPair
{
    std::unique_ptr<InMemorySocket> client; ///< The dialling end.
    std::unique_ptr<InMemorySocket> server; ///< The accepted end.

    /// @param maxBytesInFlight Per-direction bound on buffered bytes; zero means none.
    /// @param serverPeerAddress What the SERVER end reports as its peer — the client's apparent
    ///        address. "" (the default) means none.
    /// @return Two connected ends.
    [[nodiscard]] static InMemorySocketPair create(std::size_t maxBytesInFlight = 0,
                                                   std::string serverPeerAddress = {});
};

/// A listener whose connections a test makes with @c connectClient.
///
/// A pending @c accept resolves to the next connection, or with @c NetErrorCode::Cancelled once
/// the listener is closed. Connections queued before the close are still handed out, so a server
/// under test finishes what it already accepted before it observes the shutdown.
class InMemoryListener final: public IListener
{
  public:
    InMemoryListener() = default;

    /// Retires a parked accept by abandoning it, for @c InMemorySocket's destructor's reason.
    ~InMemoryListener() override;

    InMemoryListener(InMemoryListener const&) = delete;
    InMemoryListener(InMemoryListener&&) = delete;
    InMemoryListener& operator=(InMemoryListener const&) = delete;
    InMemoryListener& operator=(InMemoryListener&&) = delete;

    [[nodiscard]] async::Task<AcceptResult> accept() override;

    /// @return 0 always: a pair of in-process pipes has no port, and a test on this dials through
    ///         @c connectClient rather than through an address.
    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return 0; }

    void close() noexcept override;

    /// Makes a connected pair, queues the server end for @c accept, and hands back the client end.
    /// @param maxBytesInFlight Per-direction bound on buffered bytes; zero means none.
    /// @param peerAddress What the accepted end reports as its peer.
    /// @return The client end.
    [[nodiscard]] std::unique_ptr<InMemorySocket> connectClient(std::size_t maxBytesInFlight = 0,
                                                                std::string peerAddress = {});

  private:
    /// The awaitable a parked @c accept suspends on.
    using AcceptAwaitable = ResultAwaitable<std::unique_ptr<ISocket>>;

    /// Hands the next queued connection to a parked accept, if both exist.
    void completePendingAccept() noexcept;

    /// Tells the listener to forget an accept the flow is unwinding from.
    static void retireAccept(void* owner, void* awaitable) noexcept;

    std::deque<std::unique_ptr<ISocket>> _ready;
    AcceptAwaitable* _pending { nullptr };
    bool _closed { false };
};

} // namespace core::net::testing
