// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `PosixSocket` — one reactor-driven POSIX stream socket, replacing fastcached's `EpollSocket`
/// and `KqueueSocket` and contour's `PosixSocket`.
///
/// **One socket rather than three, because there was never a socket's worth of difference between
/// them.** fastcached shipped two that were byte-identical but for the reactor type they named, and
/// the third differed only in being written against a coroutine loop instead of a callback one.
/// What actually varies per platform is the MULTIPLEXER, and that is @c IoBackend's job — so this
/// class asks its @c EventLoop to watch a descriptor and never asks which mechanism did it.
///
/// **It allocates no coroutine frame per operation.** `read` and `write` try the syscall first and,
/// on `EAGAIN`, park a frameless readiness callback on the loop; the retry loop runs in that
/// callback and completes the @c ResultAwaitable the caller is suspended on. See
/// `<core/net/IoAwaitable.hpp>` for why the retry cannot live in the awaiting coroutine.
///
/// **Nor does it register with the backend per operation.** Its parks ask the loop for one
/// registration for the socket's life (@c RegistrationLifetime::UntilClosed), shared by the read
/// side and the write side, so the steady state of a request/response connection -- read parks,
/// wakes, completes, and the next read parks again -- costs the kernel nothing beyond the wait
/// itself. `close()` ends that registration, by announcing the close before it happens.

#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoAwaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace core::net
{

/// A reactor-driven, non-blocking POSIX stream socket.
class PosixSocket final: public ISocket
{
  public:
    /// Wraps an already-connected, non-blocking fd.
    /// @param loop The loop whose backend watches readiness (not owned; must outlive this).
    /// @param fd The connected socket fd (ownership transferred; closed by @c close).
    /// @param peerAddress Printable peer address, or "" if unknown.
    PosixSocket(EventLoop& loop, int fd, std::string peerAddress = {}) noexcept;
    ~PosixSocket() override;

    PosixSocket(PosixSocket const&) = delete;
    PosixSocket& operator=(PosixSocket const&) = delete;
    PosixSocket(PosixSocket&&) = delete;
    PosixSocket& operator=(PosixSocket&&) = delete;

    [[nodiscard]] IoAwaitable read(std::span<std::byte> buffer) override;
    [[nodiscard]] ResultAwaitable<ReadWithFd> readWithFd(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable waitReadable() override;
    [[nodiscard]] IoAwaitable write(std::span<std::byte const> buffer) override;
    [[nodiscard]] IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override;

    void cancelRead() noexcept override;
    [[nodiscard]] ResultAwaitable<void> shutdownWrite() override;
    void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override;

    [[nodiscard]] std::string peerAddress() const override { return _peerAddress; }

    void close() noexcept override;

    /// @return True once @c close was called, or a read observed the peer's EOF; @see
    ///         ISocket::isClosed.
    [[nodiscard]] bool isClosed() const noexcept override { return _closed || _peerClosed; }

    /// @return The underlying fd, or -1 once closed (for diagnostics and tests).
    [[nodiscard]] int native() const noexcept { return _fd; }

  private:
    /// Which read verb holds the socket's single read slot.
    ///
    /// The three read verbs resolve to different types — a byte count, a byte count plus a
    /// descriptor, and a readability count — so the slot is type-erased and this says what to cast
    /// it back to. Keeping them in ONE slot is the contract (@c contract::claimReadSlot), not an
    /// implementation shortcut.
    enum class ReadKind : std::uint8_t
    {
        None,   ///< Nothing is parked.
        Bytes,  ///< @c read: consume into the caller's buffer.
        WithFd, ///< @c readWithFd: `recvmsg`, keeping at most one SCM_RIGHTS descriptor.
        Probe,  ///< @c waitReadable: `MSG_PEEK`, consuming nothing.
    };

    /// The socket's single read-side operation.
    struct ReadOperation
    {
        void* awaitable = nullptr; ///< The parked awaitable, cast by @c kind. Borrowed.
        ReadKind kind = ReadKind::None;
        std::span<std::byte> buffer; ///< The caller's destination; must outlive the operation.
        ParkId park {};              ///< The loop park watching for readability.
        TimerId deadline {};         ///< The receive deadline, armed only while this is parked.
    };

    /// The socket's single write-side operation. A write resolves only once EVERY byte is gone, so
    /// this carries a cursor rather than a buffer.
    struct WriteOperation
    {
        IoAwaitable* awaitable = nullptr;                     ///< The parked awaitable. Borrowed.
        std::span<std::byte const> remaining;                 ///< What is left of a flat write.
        std::span<std::span<std::byte const> const> segments; ///< A vectored write's segments.
        std::size_t segmentIndex = 0;                         ///< How many segments are fully sent.
        std::size_t segmentOffset = 0;         ///< How much of @c segmentIndex's segment is sent.
        std::size_t written = 0;               ///< The running total this operation resolves with.
        std::shared_ptr<void const> keepAlive; ///< Pins a vectored write's backing storage.
        ParkId park {};                        ///< The loop park watching for writability.
    };

    /// Closes the fd, telling the loop first so a flow parked on it is resolved rather than left
    /// waiting for readiness the backend can no longer report.
    /// @param policy How a parked operation observes the close. @c close passes @c Resume — this
    ///        object is still alive, so the flow may safely look at it again and gets a
    ///        @c NetErrorCode::Cancelled VALUE. The destructor passes @c Cancel, because by then
    ///        resuming the flow on its normal path would read `this` through a dangling pointer;
    ///        the operation is ABANDONED instead, and unwinding through
    ///        @c async::OperationCancelled never re-enters the body.
    void close(FdWakePolicy policy) noexcept;

    /// Arms the read slot's park and deadline. Called from the awaitable's `await_suspend`.
    /// @param interest Always @c Interest::Read; named for symmetry with the write side.
    /// @return Whether the loop accepted the registration.
    [[nodiscard]] bool armRead(Interest interest);

    /// Arms the write slot's park. Called from the awaitable's `await_suspend`, by `write` and
    /// `writeVectored` alike.
    /// @return Whether the loop accepted the registration.
    [[nodiscard]] bool armWrite();

    /// Retries the read slot's syscall, completing it if it answers.
    void pumpRead();

    /// Retries the write slot's syscalls, completing it once every byte is gone.
    void pumpWrite();

    /// Attempts a consuming read.
    ///
    /// **Shared by the verb and the pump**, so the fast path and the retry cannot drift: every
    /// `EAGAIN`, `EINTR`, `ENOTSOCK` and EOF rule is written once. Latches @c _peerClosed and
    /// flips @c _plainFd where those apply.
    /// @param buffer The destination.
    /// @return What to resolve with, or nullopt if the caller should park (or stay parked).
    [[nodiscard]] std::optional<IoResult> tryRead(std::span<std::byte> buffer);

    /// Attempts a read that may carry one SCM_RIGHTS descriptor.
    /// @param buffer The destination.
    /// @return What to resolve with, or nullopt if the caller should park.
    [[nodiscard]] std::optional<std::expected<ReadWithFd, NetError>> tryReadWithFd(
        std::span<std::byte> buffer);

    /// Attempts the readability probe behind @c waitReadable, consuming nothing.
    ///
    /// **Not `const`, and the earlier justification for making it so had the dependency backwards.**
    /// It read: *"a descriptor that cannot be peeked was already found to be one by the read that
    /// preceded it"* -- but @c waitReadable exists precisely to be called BEFORE the first read, so
    /// there may be no preceding read to have discovered it. On an adopted PTY master or pipe end
    /// (@c net::adoptFd) that left the probe reporting @c SystemError on a healthy descriptor, and
    /// once `_plainFd` was set it answered a flat `1` with no readiness check at all, which turned
    /// a parked watch into a turn-free spin.
    /// @return `0` for EOF, `>0` for pending data, an error, or nullopt to park.
    [[nodiscard]] std::optional<IoResult> tryProbe();

    /// Attempts to send whatever @p operation still owes, advancing its cursor.
    ///
    /// Two shapes, two functions: a flat write walks one span, a gathered one walks a cursor over
    /// many. Writing them as one was measurably harder to follow than it was worth -- clang-tidy
    /// put the combined form at a cognitive complexity of 57 against a threshold of 50, which is
    /// the machine noticing what a reader would.
    /// @param operation The write to advance; its totals are updated in place.
    /// @return The total written once nothing is left, an error, or nullopt to park.
    [[nodiscard]] std::optional<IoResult> trySend(WriteOperation& operation);

    /// @param operation A flat write; its @c remaining span and @c written total are advanced.
    /// @return The total written once nothing is left, an error, or nullopt to park.
    [[nodiscard]] std::optional<IoResult> trySendFlat(WriteOperation& operation);

    /// @param operation A gathered write; its segment cursor and @c written total are advanced.
    /// @return The total written once nothing is left, an error, or nullopt to park.
    [[nodiscard]] std::optional<IoResult> trySendSegments(WriteOperation& operation);

    /// Advances a gathered write's cursor past @p sent bytes, which a partial `sendmsg` may leave
    /// part-way THROUGH a segment rather than between two of them — a cursor that advanced by whole
    /// segments would re-send what the kernel already took.
    /// @param operation The write whose cursor to advance.
    /// @param sent How many bytes the kernel took.
    static void advanceSegmentCursor(WriteOperation& operation, std::size_t sent) noexcept;

    /// Clears the read slot, unregistering its park and disarming its deadline.
    /// @return What was parked, with its kind, for the caller to settle AFTERWARDS — the
    ///         completion resumes a flow that may destroy this socket.
    [[nodiscard]] ReadOperation takeRead() noexcept;

    /// Clears the write slot, unregistering its park.
    /// @return What was parked, for the caller to settle afterwards.
    [[nodiscard]] WriteOperation takeWrite() noexcept;

    /// Completes @p operation with @p error as a VALUE — a cancel by the resource, not by the flow.
    /// @param operation What @c takeRead handed back.
    /// @param error What to report.
    static void settleRead(ReadOperation& operation, NetError error) noexcept;

    /// Completes @p operation so the awaiting flow UNWINDS rather than resuming into a socket that
    /// no longer exists. The destructor's counterpart to @c settleRead.
    /// @param operation What @c takeRead handed back.
    static void abandonRead(ReadOperation& operation) noexcept;

    /// @param state The socket, as a `void*`.
    /// @param wake Why the park woke; see @c ParkWake.
    static void onReadWake(void* state, ParkWake wake);

    /// @param state The socket, as a `void*`.
    /// @param wake Why the park woke; see @c ParkWake.
    static void onWriteWake(void* state, ParkWake wake);

    /// @param state The socket, as a `void*`. Runs when a read's receive deadline elapses.
    static void onReadDeadline(void* state);

    /// @param owner The socket, as a `void*`.
    /// @param awaitable The awaitable that is going away; ignored unless it is the parked one.
    static void retireRead(void* owner, void* awaitable) noexcept;

    /// @param owner The socket, as a `void*`.
    /// @param awaitable The awaitable that is going away; ignored unless it is the parked one.
    static void retireWrite(void* owner, void* awaitable) noexcept;

    EventLoop& _loop;
    int _fd;
    std::string _peerAddress;
    ReadOperation _read;
    WriteOperation _write;

    /// How long a single read may wait, or zero for no bound; @see ISocket::setReceiveDeadline.
    std::chrono::milliseconds _receiveDeadline { 0 };

    bool _plainFd = false; ///< Set on first ENOTSOCK: a PTY/pipe fd, served via read/write.
    bool _closed = false;

    /// Latched by a read that observed the peer's EOF. SEPARATE from @c _closed on purpose:
    /// @c _closed gates read and write, and a peer that shut only its write side leaves this end
    /// perfectly able to keep writing (the tmux half-close). Only @c isClosed consults this, so the
    /// answer changes without the half-close breaking.
    bool _peerClosed = false;
};

} // namespace core::net
