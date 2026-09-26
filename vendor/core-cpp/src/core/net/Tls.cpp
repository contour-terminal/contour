// SPDX-License-Identifier: Apache-2.0
#include <core/net/Tls.hpp>

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/SocketContract.hpp>
#include <core/net/detail/ScopeGuard.hpp>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace core::net
{

namespace
{
    /// The EARLIEST error on this thread's OpenSSL queue, taken off it, for diagnostics.
    [[nodiscard]] std::string opensslError()
    {
        auto const code = ERR_get_error();
        if (code == 0)
            return "unknown TLS error";
        auto buffer = std::array<char, 256> {};
        ERR_error_string_n(code, buffer.data(), buffer.size());
        return std::string { buffer.data() };
    }

    /// Clamps a size to the positive `int` range OpenSSL's length parameters take.
    [[nodiscard]] int clampToInt(std::size_t value) noexcept
    {
        constexpr auto Max = static_cast<std::size_t>(std::numeric_limits<int>::max());
        return static_cast<int>(std::min(value, Max));
    }

    using SslCtxPtr = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
    using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
    using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using PKeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using BignumPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
    using Asn1OctetStringPtr = std::unique_ptr<ASN1_OCTET_STRING, decltype(&ASN1_OCTET_STRING_free)>;

    [[nodiscard]] BioPtr memBio(std::string_view pem)
    {
        return BioPtr { BIO_new_mem_buf(pem.data(), clampToInt(pem.size())), BIO_free };
    }

    /// Which end of the handshake a context's sockets play.
    enum class TlsRole : std::uint8_t
    {
        Server, ///< `SSL_set_accept_state`: presents the certificate.
        Client, ///< `SSL_set_connect_state`: verifies it, or trusts on first use.
    };

    /// Whether a caller of the outbound flush needs it FINISHED or only needs it to HAPPEN.
    enum class FlushWait : std::uint8_t
    {
        /// Wait for a flush already in progress, then flush whatever is left. A write needs this:
        /// it resolves only once its bytes are on the wire.
        Join,
        /// Return at once if a flush is already in progress. A read needs this: the flush in
        /// progress drains the outgoing BIO to EMPTY, so the read's bytes go with it -- and a read
        /// that parked behind a WRITE could not be retired by `cancelRead`, which retires only the
        /// inner read.
        Skip,
    };

    /// Which direction of the socket an operation belongs to.
    ///
    /// `cancelRead` retires the READ direction and nothing else. Where the handshake runs under a
    /// write, the inner read parked below it is the write's, and a reader's cancel must not reach
    /// it; where a read waits on a gate, that wait is the reader's to lose.
    enum class Direction : std::uint8_t
    {
        Read,  ///< `read` and `waitReadable`.
        Write, ///< `write`, and `shutdownWrite`'s `close_notify`.
        Any,   ///< `handshakeIfNeeded`: a caller of neither direction, which `cancelRead` leaves be.
    };

    /// How a wait on a @c SerialGate ended, where it did not throw.
    enum class GateExit : std::uint8_t
    {
        Open,    ///< The gate was left; the waiter re-decides for itself.
        Retired, ///< `cancelRead` retired this READ waiter: it answers `Cancelled` as a value.
    };

    /// One coroutine at a time through a stretch of the record pump; the others park until it
    /// leaves.
    ///
    /// **Two of these, because OpenSSL is not reentrant and a socket has one write operation.** A
    /// socket here may have a read and a write in flight at once -- `WriteQueue`'s drain beside a
    /// read pump is the ordinary shape -- and both can reach the handshake, and both can reach the
    /// inner transport's `write` through the outbound flush. Two handshake drivers corrupt the
    /// handshake; two flushes put a second write into the inner socket's single write slot
    /// (`contract::claimWriteSlot`), which drops the parked one and interleaves the ciphertext.
    ///
    /// **Shared, not a member, because the socket can die before its waiters do.** A socket
    /// destroyed with an operation parked on it unwinds that operation, and the unwind leaves the
    /// gate -- after the socket, and a member gate with it, is gone. So the state lives behind a
    /// `shared_ptr` that the socket, every waiter and every holder's scope guard each hold, and the
    /// socket's destructor marks it abandoned rather than taking it down.
    ///
    /// **A waiter is never resumed inline** (`.agent/rules/async-and-net.md`, "A queue or a
    /// resource never resumes its consumer inline"). Releasing hands each one to the loop and
    /// returns. Resumed inline, the first waiter ran to wherever its flow went -- which can be
    /// destroying the socket -- and the loop over the rest then resumed the next one onto the
    /// freed socket.
    ///
    /// **A waiter honours its stop token.** A stopped waiter is taken off the gate and handed back
    /// to unwind; left parked, it came back to life when the handshake completed and read the
    /// peer's first bytes into a buffer its caller had already given up on.
    class SerialGate
    {
      public:
        /// @param executor Where released waiters are resumed; must outlive every waiter.
        explicit SerialGate(async::IExecutor& executor) noexcept: _executor(executor) {}

        SerialGate(SerialGate const&) = delete;
        SerialGate& operator=(SerialGate const&) = delete;
        SerialGate(SerialGate&&) = delete;
        SerialGate& operator=(SerialGate&&) = delete;
        ~SerialGate() = default;

        class Awaiter;

        /// @return True while a coroutine is inside.
        [[nodiscard]] bool busy() const
        {
            auto const guard = std::scoped_lock { _mutex };
            return _busy;
        }

        /// Marks the gate held by the calling coroutine.
        void enter()
        {
            auto const guard = std::scoped_lock { _mutex };
            _busy = true;
        }

        /// Opens the gate and hands every parked waiter to the loop.
        ///
        /// Invoked from a scope guard, so it runs on EVERY exit of the holder -- the exceptional
        /// one included. Unwinding through `OperationCancelled` used to reset the flag and leave
        /// the parked coroutines suspended for ever: a `WriteQueue::drain` waiting here never
        /// observed `draining == false`, so `flushThenClose()` hung and a TLS client could not exit.
        void leave() noexcept { release(Release::Leave); }

        /// Retires every READ waiter: each resumes, on a later turn, with @c GateExit::Retired.
        void retireReaders() noexcept { release(Release::RetireReaders); }

        /// The socket is going away: every waiter, now and later, unwinds.
        void abandon() noexcept { release(Release::Abandon); }

        /// @param gate The gate to wait on; the awaiter keeps it alive.
        /// @param direction Which direction the waiting operation belongs to.
        /// @return An awaitable that parks until the gate is left.
        [[nodiscard]] static Awaiter wait(std::shared_ptr<SerialGate> gate, Direction direction) noexcept;

      private:
        /// One parked coroutine. Lives in the awaiter, so in the waiting frame.
        struct Waiter
        {
            async::ParkedWork work {};
            Direction direction = Direction::Any;
            GateExit exit = GateExit::Open;
            bool released = false;  ///< Handed to the executor: the park is over.
            bool cancelled = false; ///< Its stop token took it back.
        };

        /// What a release does to the gate and to whom.
        enum class Release : std::uint8_t
        {
            Leave,         ///< Open the gate; release everyone.
            RetireReaders, ///< Leave the gate as it is; release the READ waiters as retired.
            Abandon,       ///< The socket is gone; release everyone, and every later arrival.
        };

        void release(Release what) noexcept
        {
            auto released = std::vector<async::ParkedWork> {};
            {
                auto const guard = std::scoped_lock { _mutex };
                if (what == Release::Leave)
                    _busy = false;
                if (what == Release::Abandon)
                    _abandoned = true;
                std::erase_if(_waiters, [&](Waiter* waiter) {
                    if (what == Release::RetireReaders && waiter->direction != Direction::Read)
                        return false;
                    if (what == Release::RetireReaders)
                        waiter->exit = GateExit::Retired;
                    waiter->released = true;
                    released.push_back(std::exchange(waiter->work, {}));
                    return true;
                });
            }
            // Outside the lock: the executor's own lock never nests under this one.
            for (auto& work: released)
                _executor.submit(std::move(work));
        }

        async::IExecutor& _executor;
        mutable std::mutex _mutex; ///< A stop callback may run on any thread; all else is the loop's.
        bool _busy = false;
        bool _abandoned = false;
        std::vector<Waiter*> _waiters;
    };

    /// Parks until the gate is left, the waiter is retired or cancelled, or the socket goes away.
    class SerialGate::Awaiter
    {
      public:
        /// @param gate The gate; kept alive by this awaiter.
        /// @param direction Which direction the waiting operation belongs to.
        Awaiter(std::shared_ptr<SerialGate> gate, Direction direction) noexcept: _gate(std::move(gate))
        {
            _waiter.direction = direction;
        }

        Awaiter(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter const&) = delete;
        Awaiter(Awaiter&&) = delete;
        Awaiter& operator=(Awaiter&&) = delete;

        /// A frame destroyed while parked takes its park back, so a release never hands the loop a
        /// handle to a freed frame.
        ~Awaiter()
        {
            _stopRegistration.reset();
            auto const guard = std::scoped_lock { _gate->_mutex };
            std::erase(_gate->_waiters, &_waiter);
        }

        /// @return False. Whether the gate is free is @c await_suspend's first question, asked
        ///         under the gate's mutex: taking a lock is a call, and MSVC 19.44's ARM64 code
        ///         generator drops the enclosing `try` of a `co_await` on a temporary awaiter whose
        ///         `await_ready` makes one (fastcached#1546, `.agent/rules/async-and-net.md`).
        [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

        /// A free or abandoned gate resumes at once, before the token is read or a callback
        /// registered -- what `await_ready` answered when it held that check. Otherwise registers
        /// the stop callback BEFORE publishing the park, as `AsyncQueue` does: a token already
        /// stopped runs the callback here, which finds nothing parked and only records the
        /// cancellation for the re-check below.
        /// @tparam Promise The awaiting coroutine's promise type.
        /// @param awaiting The coroutine to park.
        /// @return True to stay parked.
        template <typename Promise>
        [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
        {
            {
                auto const guard = std::scoped_lock { _gate->_mutex };
                if (!_gate->_busy || _gate->_abandoned)
                    return false;
            }
            if constexpr (async::HasStopToken<Promise>)
                _token = awaiting.promise().stopToken();
            if (_token.stop_possible())
                _stopRegistration.emplace(_token, CancelWait { this });

            auto const guard = std::scoped_lock { _gate->_mutex };
            if (!_gate->_busy || _gate->_abandoned || _waiter.cancelled)
                return false;
            _waiter.work = async::detail::parkedWorkFor(awaiting);
            _gate->_waiters.push_back(&_waiter);
            return true;
        }

        /// @return How the wait ended.
        /// @throws async::OperationCancelled where the socket went away, or the waiter's own stop
        ///         token was requested -- whatever released it, a stopped flow does not go on to
        ///         read into a buffer its caller has abandoned.
        [[nodiscard]] GateExit await_resume()
        {
            _stopRegistration.reset();
            auto const guard = std::scoped_lock { _gate->_mutex };
            std::erase(_gate->_waiters, &_waiter);
            if (_gate->_abandoned || _waiter.cancelled || _token.stop_requested())
                throw async::OperationCancelled {};
            return _waiter.exit;
        }

      private:
        /// The stop callback: takes the park back, if it is still a park, and hands it to the loop
        /// to unwind.
        class CancelWait
        {
          public:
            explicit CancelWait(Awaiter* awaiter) noexcept: _awaiter(awaiter) {}

            void operator()() const noexcept
            {
                auto work = async::ParkedWork {};
                {
                    auto const guard = std::scoped_lock { _awaiter->_gate->_mutex };
                    _awaiter->_waiter.cancelled = true;
                    if (_awaiter->_waiter.released)
                        return;
                    std::erase(_awaiter->_gate->_waiters, &_awaiter->_waiter);
                    _awaiter->_waiter.released = true;
                    work = std::exchange(_awaiter->_waiter.work, {});
                }
                if (work.resume)
                    _awaiter->_gate->_executor.submit(std::move(work));
            }

          private:
            Awaiter* _awaiter;
        };

        std::shared_ptr<SerialGate> _gate;
        Waiter _waiter;
        async::StopToken _token;

        /// Declared LAST, so it is destroyed FIRST: its destructor waits for a callback running on
        /// another thread, and that callback reads the members above.
        std::optional<async::StopCallback<CancelWait>> _stopRegistration;
    };

    SerialGate::Awaiter SerialGate::wait(std::shared_ptr<SerialGate> gate, Direction direction) noexcept
    {
        return Awaiter { std::move(gate), direction };
    }

    /// The answer to a transport that ended before the peer's `close_notify`.
    [[nodiscard]] NetError truncated()
    {
        return makeNetError(NetErrorCode::ConnReset, 0, "peer closed without close_notify");
    }

    /// The answer to a read that `cancelRead` retired while it waited on the handshake.
    [[nodiscard]] NetError retiredRead()
    {
        return makeNetError(
            NetErrorCode::Cancelled, 0, "cancelRead retired a read waiting on the TLS handshake");
    }

    /// A TLS layer over an inner `ISocket`. OpenSSL talks to two memory BIOs; this pumps
    /// ciphertext between those BIOs and the inner transport across coroutine suspensions, so the
    /// handshake and the records ride the loop with no blocking.
    ///
    /// **Every operation is coroutine-backed, and that is the shape a decorator is owed.** A raw
    /// socket's read is a syscall and a retry, so it needs no frame; a TLS read decrypts, may drive
    /// the handshake, and may park on a raw read of its own -- a loop with state that outlives each
    /// step. `ResultAwaitable`'s task constructor owns the frame and hands the awaiting flow's stop
    /// token down into it, so this is exactly as cancellable as a frame-free operation.
    ///
    /// **`ERR_clear_error()` precedes every `SSL_*` call whose result is classified.**
    /// `SSL_get_error` reads the THREAD's error queue, and every connection on a loop shares that
    /// thread: an entry another connection left behind turns this one's routine `WANT_READ` into
    /// `SSL_ERROR_SSL`, and a healthy connection fails for a neighbour's error.
    class TlsSocket final: public ISocket
    {
      public:
        /// @param inner The connected transport (owned).
        /// @param ssl The session, its BIOs attached (owned).
        /// @param executor The loop a waiter parked on one of the gates is resumed on; must outlive
        ///        every operation on this socket.
        TlsSocket(std::unique_ptr<ISocket> inner, SSL* ssl, async::IExecutor& executor):
            _inner(std::move(inner)),
            _ssl(ssl),
            _rbio(SSL_get_rbio(ssl)),
            _wbio(SSL_get_wbio(ssl)),
            _handshaking(std::make_shared<SerialGate>(executor)),
            _flushing(std::make_shared<SerialGate>(executor))
        {
        }

        /// Abandons every operation parked on this socket, and only then frees the session.
        ///
        /// **The order is the fix for a double free.** An operation can be parked in two places:
        /// on one of the gates, or inside the inner transport, driving the handshake or a flush.
        /// The gates are marked abandoned first, so a waiter that is released after this point --
        /// by the loop, or by a driver unwinding -- throws rather than re-entering the pump. Then
        /// the inner transport is destroyed while the session still exists, which ABANDONS its
        /// parked operation: the driver unwinds through its scope guard, which leaves a gate that
        /// is shared rather than a member, so it is still there to leave. The session goes last,
        /// once nothing can reach it.
        ~TlsSocket() override
        {
            _handshaking->abandon();
            _flushing->abandon();
            _inner.reset();
            if (_ssl != nullptr)
                SSL_free(_ssl); // frees the attached BIOs too
        }

        TlsSocket(TlsSocket const&) = delete;
        TlsSocket& operator=(TlsSocket const&) = delete;
        TlsSocket(TlsSocket&&) = delete;
        TlsSocket& operator=(TlsSocket&&) = delete;

        /// **A transport EOF before `close_notify` is @c NetErrorCode::ConnReset, never `0`.**
        /// `0` means the peer has finished sending, and answering it for a transport that simply
        /// ended lets a truncated stream -- cut short by an attacker, or by a crash -- pass as a
        /// complete one. OpenSSL 3 reading from a socket refuses the same thing
        /// (`SSL_R_UNEXPECTED_EOF_WHILE_READING`). Plaintext already decoded is delivered first.
        /// @param buffer The destination; must be non-empty (@c contract::requireReadBuffer).
        /// @return The plaintext byte count, `0` once the peer sent `close_notify`, or a
        ///         @c NetError: @c NetErrorCode::ConnReset for a transport that ended without one.
        [[nodiscard]] IoAwaitable read(std::span<std::byte> buffer) override
        {
            contract::requireReadBuffer(buffer);
            return IoAwaitable { readPlain(buffer) };
        }

        /// @param buffer The source.
        /// @return The byte count written, or a @c NetError.
        [[nodiscard]] IoAwaitable write(std::span<std::byte const> buffer) override
        {
            return IoAwaitable { writePlain(buffer) };
        }

        // writeVectored is NOT overridden: no scattered syscall is reachable through a TLS record
        // layer, so the base's write-each-segment-in-turn default is already the right algorithm.

        /// Drives the handshake to completion now, rather than on the first read or write.
        ///
        /// What an accept loop awaits before it reads a request, so a slow or failing handshake is
        /// settled on the connection's own flow and the first byte framed is plaintext. Idempotent:
        /// a completed handshake answers at once, and a failed one answers its failure again.
        /// @return Nothing once the session is up, or why it could not be established.
        [[nodiscard]] ResultAwaitable<void> handshakeIfNeeded() override
        {
            return ResultAwaitable<void> { handshake(Direction::Any) };
        }

        /// Reports whether the peer has finished sending, decrypting whatever records that takes
        /// and consuming no plaintext.
        ///
        /// **A TLS peer closes by sending a RECORD, so the inner socket cannot answer this.** A
        /// well-behaved peer emits `close_notify` and only then the transport FIN, so at the instant
        /// it goes away there are bytes on the wire and a raw peek reports data pending for a peer
        /// that has in fact finished sending
        /// ([fastcached#712](https://github.com/LASTRADA-Software/fastcached/issues/712)). So the
        /// record is decrypted by `SSL_peek`, which leaves every plaintext byte it decoded in
        /// OpenSSL's buffer for the next `read`. Raw ciphertext IS consumed, into this decorator's
        /// own BIO, and that is not what the interface's "consumes nothing" forbids.
        ///
        /// A transport EOF before `close_notify` is not an end but a truncation, and it is answered
        /// as @c read answers it.
        ///
        /// It parks on an inner read, so it holds the read slot while parked, and @c cancelRead
        /// retires it.
        /// @return The decoded bytes pending (`>0`), `0` once the peer has finished sending --
        ///         `close_notify` -- or a @c NetError, @c NetErrorCode::ConnReset for a transport
        ///         that ended without one, as @c read answers it.
        [[nodiscard]] IoAwaitable waitReadable() override
        {
            if (_peerClosed)
                return IoAwaitable { IoResult { std::size_t { 0 } } };
            if (_handshakeDone)
                if (auto const pending = SSL_pending(_ssl); pending > 0)
                    return IoAwaitable { IoResult { static_cast<std::size_t>(pending) } };
            return IoAwaitable { probeReadable() };
        }

        /// Half-closes the TLS session: writes `close_notify`, flushes it, and only then half-closes
        /// the inner transport.
        ///
        /// **The order is the whole contract.** A FIN with no `close_notify` before it is what a
        /// truncation attack looks like, and a strict peer -- OpenSSL 3's own default, reading from
        /// a socket -- reports it as an error rather than as the end of the stream. So forwarding to
        /// the inner socket, which is all a synchronous signature could do, was never correct.
        ///
        /// **Precondition: no write outstanding** (@c ISocket::shutdownWrite). The alert goes out
        /// through the inner socket's `write`, and that socket has one write operation.
        ///
        /// Reads keep working: OpenSSL delivers the peer's records until its own `close_notify`.
        /// A write after this fails, as on any half-closed socket. A session whose handshake never
        /// began has nothing to close cryptographically, so only the transport is half-closed.
        /// @return Nothing on success, or why the alert or the half-close could not be delivered.
        [[nodiscard]] ResultAwaitable<void> shutdownWrite() override
        {
            return ResultAwaitable<void> { closeNotify() };
        }

        /// Retires a read parked on the INNER transport.
        ///
        /// **A TLS read is not read-only at the transport layer**, which is why this override
        /// exists and why the base's no-op would be wrong here: a read or a `waitReadable` parks on
        /// a raw read whenever OpenSSL wants more bytes, so inheriting the no-op would leave the
        /// inner socket's slot occupied and hand the next read a double-arm. The inner read
        /// completes with `Cancelled`, the pump returns it, and the caller's operation resolves
        /// with it.
        ///
        /// **It retires the READ direction and nothing else.** A read waiting on the handshake gate
        /// is retired there, and answers `Cancelled` as a value on a later turn. The inner read is
        /// retired only where it is a read's: where a write drives the handshake, the inner read
        /// under it is the write's, and cancelling it failed the write -- and, stored as the
        /// handshake's outcome, every later operation. A cancelled handshake is never sticky: the
        /// inner read took no ciphertext, so the next operation drives it again.
        void cancelRead() noexcept override
        {
            _handshaking->retireReaders();
            if (_innerReadOwner == Direction::Read)
                _inner->cancelRead();
        }

        /// @param deadline How long a read may wait; bounds the INNER transport's read, which is
        ///        where a TLS read actually waits.
        void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override
        {
            _inner->setReceiveDeadline(deadline);
        }

        [[nodiscard]] std::string peerAddress() const override { return _inner->peerAddress(); }

        /// Closes the inner transport, WITHOUT a `close_notify`: a synchronous `noexcept` close
        /// cannot await the write an alert needs. A caller that wants its peer to see an orderly
        /// end awaits @c shutdownWrite first.
        void close() noexcept override { _inner->close(); }

        /// True once this end closed, or a read observed the peer's EOF -- which for a TLS layer
        /// arrives as a close_notify, possibly while the inner transport is still open. Asking only
        /// the inner socket would therefore answer "open" for a session the peer has already ended.
        /// @see ISocket::isClosed.
        [[nodiscard]] bool isClosed() const noexcept override { return _peerClosed || _inner->isClosed(); }

      private:
        async::Task<IoResult> readPlain(std::span<std::byte> buffer)
        {
            if (auto const handshaken = co_await handshake(Direction::Read); !handshaken)
                co_return std::unexpected(handshaken.error());

            while (true)
            {
                ERR_clear_error();
                auto const n = SSL_read(_ssl, buffer.data(), clampToInt(buffer.size()));
                if (n > 0)
                    co_return static_cast<std::size_t>(n);
                switch (SSL_get_error(_ssl, n))
                {
                    case SSL_ERROR_ZERO_RETURN:
                        _peerClosed = true; // a close_notify IS this layer's EOF
                        co_return std::size_t { 0 };
                    case SSL_ERROR_WANT_WRITE:
                        if (auto const flushed = co_await flushOut(FlushWait::Skip); !flushed)
                            co_return std::unexpected(flushed.error());
                        break;
                    case SSL_ERROR_WANT_READ: {
                        if (auto const flushed = co_await flushOut(FlushWait::Skip); !flushed)
                            co_return std::unexpected(flushed.error());
                        auto const fed = co_await feedIn(Direction::Read);
                        if (!fed)
                            co_return std::unexpected(fed.error());
                        if (*fed == 0)
                        {
                            _peerClosed = true; // the peer is gone -- but it did not say goodbye
                            co_return std::unexpected(truncated());
                        }
                        break;
                    }
                    default:
                        co_return std::unexpected(
                            makeNetError(NetErrorCode::SystemError, 0, "SSL_read: " + opensslError()));
                }
            }
        }

        async::Task<IoResult> writePlain(std::span<std::byte const> buffer)
        {
            if (auto const handshaken = co_await handshake(Direction::Write); !handshaken)
                co_return std::unexpected(handshaken.error());

            auto total = std::size_t { 0 };
            while (total < buffer.size())
            {
                ERR_clear_error();
                auto const n = SSL_write(_ssl, buffer.data() + total, clampToInt(buffer.size() - total));
                if (n > 0)
                {
                    total += static_cast<std::size_t>(n);
                    if (auto const flushed = co_await flushOut(FlushWait::Join); !flushed)
                        co_return std::unexpected(flushed.error());
                    continue;
                }
                switch (SSL_get_error(_ssl, n))
                {
                    case SSL_ERROR_WANT_WRITE:
                        if (auto const flushed = co_await flushOut(FlushWait::Join); !flushed)
                            co_return std::unexpected(flushed.error());
                        break;
                    // Unreachable once the handshake is done, and deliberately so: every context
                    // sets SSL_OP_NO_RENEGOTIATION, and TLS 1.3 never needs a read to write. Were
                    // it reached, feeding here beside a parked read would put a second read into
                    // the inner socket's single read slot.
                    case SSL_ERROR_WANT_READ: {
                        if (auto const flushed = co_await flushOut(FlushWait::Join); !flushed)
                            co_return std::unexpected(flushed.error());
                        auto const fed = co_await feedIn(Direction::Write);
                        if (!fed)
                            co_return std::unexpected(fed.error());
                        if (*fed == 0)
                            co_return std::unexpected(
                                makeNetError(NetErrorCode::Eof, 0, "SSL_write: peer closed"));
                        break;
                    }
                    default:
                        co_return std::unexpected(
                            makeNetError(NetErrorCode::SystemError, 0, "SSL_write: " + opensslError()));
                }
            }
            co_return total;
        }

        /// Answers @c waitReadable once the answer is a TLS-level one.
        async::Task<IoResult> probeReadable()
        {
            if (auto const handshaken = co_await handshake(Direction::Read); !handshaken)
                co_return std::unexpected(handshaken.error());

            // One byte, never taken: SSL_peek decrypts the whole record into OpenSSL's read buffer
            // and removes nothing, so the caller's next read returns every byte of it.
            auto probe = std::array<std::byte, 1> {};
            while (true)
            {
                ERR_clear_error();
                auto const n = SSL_peek(_ssl, probe.data(), clampToInt(probe.size()));
                if (n > 0)
                    co_return static_cast<std::size_t>(std::max(SSL_pending(_ssl), 1));
                switch (SSL_get_error(_ssl, n))
                {
                    case SSL_ERROR_ZERO_RETURN:
                        _peerClosed = true; // as `read` records it, so `isClosed` agrees with the watch
                        co_return std::size_t { 0 }; // close_notify
                    case SSL_ERROR_WANT_WRITE:
                        if (auto const flushed = co_await flushOut(FlushWait::Skip); !flushed)
                            co_return std::unexpected(flushed.error());
                        break;
                    case SSL_ERROR_WANT_READ: {
                        if (auto const flushed = co_await flushOut(FlushWait::Skip); !flushed)
                            co_return std::unexpected(flushed.error());
                        auto const fed = co_await feedIn(Direction::Read);
                        if (!fed)
                            co_return std::unexpected(fed.error());
                        // A raw EOF with no close_notify is a truncation, and `0` would tell the
                        // caller the stream ended whole. Answered as the next read would be.
                        if (*fed == 0)
                            co_return std::unexpected(truncated());
                        break;
                    }
                    default:
                        co_return std::unexpected(
                            makeNetError(NetErrorCode::SystemError, 0, "SSL_peek: " + opensslError()));
                }
            }
        }

        /// Writes and flushes `close_notify`, then half-closes the inner transport.
        async::Task<std::expected<void, NetError>> closeNotify()
        {
            if (_handshaking->busy() || _handshakeDone)
            {
                if (auto const handshaken = co_await handshake(Direction::Write); handshaken)
                {
                    if ((SSL_get_shutdown(_ssl) & SSL_SENT_SHUTDOWN) == 0)
                    {
                        ERR_clear_error();
                        // 0: our alert is queued and the peer's is not yet seen, which is exactly
                        // a half-close. 1: the peer had already closed. Negative: it failed.
                        if (SSL_shutdown(_ssl) < 0)
                            co_return std::unexpected(makeNetError(
                                NetErrorCode::SystemError, 0, "SSL_shutdown: " + opensslError()));
                    }
                    if (auto const flushed = co_await flushOut(FlushWait::Join); !flushed)
                        co_return std::unexpected(flushed.error());
                }
            }
            co_return co_await _inner->shutdownWrite();
        }

        /// Drives the handshake to completion (idempotent), or -- if another coroutine is already
        /// driving it -- waits for that to finish. OpenSSL is not reentrant, so concurrent read()
        /// and write() must NOT both call SSL_do_handshake: on one loop the two interleave at every
        /// suspension and corrupt the handshake without this gate.
        async::Task<std::expected<void, NetError>> handshake(Direction direction)
        {
            // A loop rather than a one-shot check, because a released waiter re-decides HERE: the
            // driver may have completed, failed, or been cancelled mid-flight, and only the last
            // case leaves the gate open with neither flag set. Falling out of the loop then makes
            // this call the new driver.
            while (true)
            {
                if (_handshakeDone)
                    co_return std::expected<void, NetError> {};
                if (_handshakeError)
                    co_return std::unexpected(*_handshakeError);
                if (!_handshaking->busy())
                    break;
                if (co_await SerialGate::wait(_handshaking, direction) == GateExit::Retired)
                    co_return std::unexpected(retiredRead());
            }

            _handshaking->enter();
            auto outcome = std::expected<void, NetError> {};
            // The gate by value, not through `this`: this guard also runs while the socket is being
            // destroyed under a parked driver, and the socket's members are gone by then.
            auto const openGate = detail::ScopeGuard { [gate = _handshaking]() noexcept { gate->leave(); } };
            while (true)
            {
                ERR_clear_error();
                auto const result = SSL_do_handshake(_ssl);
                auto const err = SSL_get_error(_ssl, result);
                // The reason is read NOW, before the flush below can park: while it is parked,
                // other connections on this thread clear the queue, and it would come back empty
                // or a neighbour's.
                auto const reason = result == 1 || err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE
                                        ? std::string {}
                                        : opensslError();
                // Always flush whatever the last step queued (ClientHello, the server's flight,
                // Finished, ...) before deciding what to await.
                if (auto const flushed = co_await flushOut(FlushWait::Join); !flushed)
                {
                    outcome = std::unexpected(flushed.error());
                    break;
                }
                if (result == 1)
                {
                    _handshakeDone = true;
                    break;
                }
                if (err == SSL_ERROR_WANT_READ)
                {
                    auto const fed = co_await feedIn(direction);
                    if (!fed)
                    {
                        outcome = std::unexpected(fed.error());
                        break;
                    }
                    if (*fed == 0)
                    {
                        outcome =
                            std::unexpected(makeNetError(NetErrorCode::Eof, 0, "TLS handshake: peer closed"));
                        break;
                    }
                }
                else if (err != SSL_ERROR_WANT_WRITE)
                {
                    outcome = std::unexpected(
                        makeNetError(NetErrorCode::SystemError, 0, "TLS handshake: " + reason));
                    break;
                }
            }

            // A failure is sticky -- parked waiters and every later operation observe it -- with one
            // exception: a cancelled inner read took no ciphertext, so the handshake can simply be
            // driven again, and `cancelRead` is documented not to be a close. The reason travels in
            // NetError::context ("TLS handshake: <openssl error>"); net RETURNS failures rather than
            // logging them.
            if (!outcome && outcome.error().code != NetErrorCode::Cancelled)
                _handshakeError = outcome.error();
            co_return outcome;
        }

        /// Drains OpenSSL's outgoing BIO to the inner socket, one flush at a time.
        /// @param wait Whether to wait for a flush already in progress (see @c FlushWait).
        async::Task<std::expected<void, NetError>> flushOut(FlushWait wait)
        {
            while (_flushing->busy())
            {
                if (wait == FlushWait::Skip)
                    co_return std::expected<void, NetError> {};
                // Only writes join a flush, and only READ waiters are ever retired.
                std::ignore = co_await SerialGate::wait(_flushing, Direction::Write);
            }

            _flushing->enter();
            // By value, for the handshake's reason: this guard runs under a destroyed socket too.
            auto const openGate = detail::ScopeGuard { [gate = _flushing]() noexcept { gate->leave(); } };
            auto& chunk = _outChunk;
            while (true)
            {
                auto const pending = BIO_ctrl_pending(_wbio);
                if (pending == 0)
                    co_return std::expected<void, NetError> {};
                auto const take = std::min<std::size_t>(chunk.size(), pending);
                auto const n = BIO_read(_wbio, chunk.data(), clampToInt(take));
                // Reached only after BIO_ctrl_pending said bytes WERE queued, so a read that
                // yields none is a failed BIO, not an empty one. Reporting "nothing to flush"
                // silently dropped that ciphertext: the handshake then waited for a peer response
                // to a flight never written, and both ends hung until an outer timeout.
                if (n <= 0)
                    co_return std::unexpected(makeNetError(
                        NetErrorCode::SystemError, 0, "TLS flushOut: BIO_read failed: " + opensslError()));
                auto const lifetime = std::weak_ptr<void const> { _lifetime };
                auto const written = co_await _inner->write(
                    std::span<std::byte const> { chunk.data(), static_cast<std::size_t>(n) });
                // See `feedIn`: the socket may have gone while the write was on its way back.
                if (lifetime.expired())
                    throw async::OperationCancelled {};
                if (!written)
                    co_return std::unexpected(written.error());
            }
        }

        /// Reads ciphertext from the inner socket into OpenSSL's incoming BIO.
        /// @param direction Whose operation the inner read serves, so `cancelRead` can tell.
        /// @return Bytes fed (0 = a clean inner EOF).
        async::Task<std::expected<std::size_t, NetError>> feedIn(Direction direction)
        {
            auto& chunk = _inChunk;
            // Recorded before the read parks, and never cleared: an inner read is only ever parked
            // by the latest feed, so a stale value names a read that is no longer there, and
            // retiring nothing is what `cancelRead` then does.
            _innerReadOwner = direction;
            auto const lifetime = std::weak_ptr<void const> { _lifetime };
            auto const n = co_await _inner->read(chunk);
            // **The socket may be gone by now, even with DATA in hand.** The inner read settles in
            // the drain that ran its readiness and this frame is resumed later in it (G2), so an
            // entry queued in between -- an owner's `tls->close(); connections.erase(id);` -- may
            // have destroyed this socket and freed the session. Nothing below may touch a member
            // then: the flow unwinds, as it does from a destroyed socket's abandoned operation.
            if (lifetime.expired())
                throw async::OperationCancelled {};
            if (!n)
                co_return std::unexpected(n.error());
            if (*n == 0)
                co_return std::size_t { 0 };
            auto const written = BIO_write(_rbio, chunk.data(), clampToInt(*n));
            if (written <= 0)
                co_return std::unexpected(
                    makeNetError(NetErrorCode::SystemError, 0, "TLS feedIn: BIO_write failed"));
            co_return *n;
        }

        /// A TLS record is at most 16 KiB of payload, so this is the size at which neither
        /// direction ever needs a second trip through OpenSSL for one record.
        static constexpr std::size_t ChunkSize = 16384;

        /// The staging buffers the two BIO bridges copy through.
        ///
        /// MEMBERS, not coroutine locals: an array this size declared inside `feedIn`/`flushOut`
        /// becomes part of their coroutine frame, so every read and every flush -- the hot path of
        /// an encrypted connection -- paid a >16 KiB heap allocation. One per direction, because
        /// the read and write paths run as independent coroutines, and @c _flushing is what keeps
        /// two of them off `_outChunk`.
        std::array<std::byte, ChunkSize> _inChunk {};
        std::array<std::byte, ChunkSize> _outChunk {};

        std::unique_ptr<ISocket> _inner;
        SSL* _ssl;
        BIO* _rbio; ///< Network -> SSL (owned by _ssl).
        BIO* _wbio; ///< SSL -> network (owned by _ssl).
        bool _handshakeDone = false;
        bool _peerClosed = false;                   ///< A read saw the peer's close_notify or inner EOF.
        std::optional<NetError> _handshakeError;    ///< Set once the handshake fails (sticky).
        Direction _innerReadOwner = Direction::Any; ///< Whose operation the latest inner read served.

        /// Held by the coroutine driving the handshake. Shared: see @c SerialGate.
        std::shared_ptr<SerialGate> _handshaking;
        /// Held by the coroutine writing ciphertext out. Shared: see @c SerialGate.
        std::shared_ptr<SerialGate> _flushing;

        /// Expires with this socket: what `feedIn` and `flushOut` ask, never `this`, when an inner
        /// operation hands them back, because the socket may have been destroyed in between.
        std::shared_ptr<void const> _lifetime = std::make_shared<char const>('\0');
    };

    /// The SHA-256 fingerprint of @p cert as lower-case hex, or empty if it cannot be computed.
    [[nodiscard]] std::string fingerprintOf(X509 const* cert)
    {
        auto digest = std::array<unsigned char, EVP_MAX_MD_SIZE> {};
        auto length = 0U;
        if (cert == nullptr || X509_digest(cert, EVP_sha256(), digest.data(), &length) != 1)
            return {};
        constexpr auto Hex = std::string_view { "0123456789abcdef" };
        auto out = std::string {};
        out.reserve(std::size_t { length } * 2);
        for (auto const byte: std::span { digest.data(), length })
        {
            out += Hex[byte >> 4U];
            out += Hex[byte & 0x0FU];
        }
        return out;
    }

    /// The DI context: a configured SSL_CTX plus its handshake role.
    class TlsContext final: public ITlsContext
    {
      public:
        TlsContext(SslCtxPtr ctx, TlsRole role): _ctx(std::move(ctx)), _role(role) {}

        std::unique_ptr<ISocket> wrap(std::unique_ptr<ISocket> inner, async::IExecutor& executor) override
        {
            auto* ssl = SSL_new(_ctx.get());
            if (ssl == nullptr)
                return nullptr;
            // Memory BIOs bridge OpenSSL and the coroutine transport; SSL_set_bio takes ownership
            // of both, so SSL_free later releases them.
            //
            // Checked like SSL_new above: handing a null BIO to SSL_set_bio yields a socket that
            // LOOKS constructed and dereferences null on its first read or write. Ownership has not
            // transferred yet on this path, so both BIOs and the SSL are released here (BIO_free
            // tolerates null, which is what makes the one-sided case work).
            auto* const rbio = BIO_new(BIO_s_mem());
            auto* const wbio = BIO_new(BIO_s_mem());
            if (rbio == nullptr || wbio == nullptr)
            {
                BIO_free(rbio);
                BIO_free(wbio);
                SSL_free(ssl);
                return nullptr;
            }
            SSL_set_bio(ssl, rbio, wbio);
            if (_role == TlsRole::Server)
                SSL_set_accept_state(ssl);
            else
                SSL_set_connect_state(ssl);
            return std::make_unique<TlsSocket>(std::move(inner), ssl, executor);
        }

        [[nodiscard]] std::string certificateFingerprint() const override
        {
            return fingerprintOf(SSL_CTX_get0_certificate(_ctx.get()));
        }

      private:
        SslCtxPtr _ctx;
        TlsRole _role;
    };

    /// A context with the floor every one of them shares: TLS 1.2 and no renegotiation.
    ///
    /// Renegotiation is refused because it is the one way a WRITE can need a READ after the
    /// handshake (`SSL_write` answering `WANT_READ`), and a socket with a read already parked has
    /// no second read slot to give it. TLS 1.3 has no renegotiation at all.
    [[nodiscard]] SslCtxPtr newCtx(SSL_METHOD const* method)
    {
        ERR_clear_error();
        auto ctx = SslCtxPtr { SSL_CTX_new(method), SSL_CTX_free };
        if (ctx)
        {
            SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
            SSL_CTX_set_options(ctx.get(), SSL_OP_NO_RENEGOTIATION);
        }
        return ctx;
    }

    /// Installs a PEM certificate chain and its key into a server context.
    /// @param ctx The context to fill.
    /// @param certPem The leaf, then any intermediates.
    /// @param keyPem The leaf's private key.
    /// @return Nothing, or why the material could not be used.
    [[nodiscard]] std::expected<void, std::string> useServerMaterial(SSL_CTX* ctx,
                                                                     std::string_view certPem,
                                                                     std::string_view keyPem)
    {
        auto certBio = memBio(certPem);
        auto const leaf = X509Ptr { PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), X509_free };
        if (!leaf)
            return std::unexpected("invalid certificate PEM: " + opensslError());
        if (SSL_CTX_use_certificate(ctx, leaf.get()) != 1)
            return std::unexpected("SSL_CTX_use_certificate: " + opensslError());

        // Every certificate after the leaf is served as part of the chain. Only reading the first
        // one silently dropped the intermediates of a named certificate, and a client that could
        // not build the chain to its anchor then failed a handshake the operator had configured
        // correctly.
        while (true)
        {
            auto const intermediate =
                X509Ptr { PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), X509_free };
            if (!intermediate)
                break;
            // add1, not add0: the context takes a reference of its own, so ours is released by
            // the smart pointer on every path, and no ownership changes hands mid-statement.
            if (SSL_CTX_add1_chain_cert(ctx, intermediate.get()) != 1)
                return std::unexpected("SSL_CTX_add1_chain_cert: " + opensslError());
        }
        // Reading past the last certificate leaves "no start line" on the queue; it is the loop's
        // end, not a failure, and must not be reported as the reason for a later one.
        ERR_clear_error();

        auto keyBio = memBio(keyPem);
        auto const key =
            PKeyPtr { PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free };
        if (!key)
            return std::unexpected("invalid private key PEM: " + opensslError());
        if (SSL_CTX_use_PrivateKey(ctx, key.get()) != 1)
            return std::unexpected("SSL_CTX_use_PrivateKey: " + opensslError());
        if (SSL_CTX_check_private_key(ctx) != 1)
            return std::unexpected(std::string { "certificate and private key do not match" });
        return {};
    }

    /// Reads a whole file, for PEM material named by path.
    /// @param path The file.
    /// @param what What the file is, for the error.
    /// @return The contents, or a reason naming the file.
    [[nodiscard]] std::expected<std::string, std::string> readFile(std::filesystem::path const& path,
                                                                   std::string_view what)
    {
        // The path as UTF-8, for the message: `path::string()` converts through the ANSI code page
        // on Windows and THROWS for a name it cannot represent, which would turn a refusal into an
        // exception on exactly the path that is reporting one.
        auto const u8 = path.u8string();
        auto const refusal = "cannot read " + std::string { what } + " '"
                             + std::string { reinterpret_cast<char const*>(u8.data()), u8.size() } + "'";

        // Sized and read in one call rather than through `istreambuf_iterator`, which GCC 14's
        // -Wnull-dereference reports inside libstdc++'s streambuf at -O3.
        auto stream = std::ifstream { path, std::ios::binary | std::ios::ate };
        if (!stream)
            return std::unexpected(refusal);
        auto const size = static_cast<std::streamoff>(stream.tellg());
        if (size < 0)
            return std::unexpected(refusal);
        auto contents = std::string(static_cast<std::size_t>(size), '\0');
        stream.seekg(0);
        if (!stream.read(contents.data(), static_cast<std::streamsize>(size)))
            return std::unexpected(refusal);
        return contents;
    }

    /// Reads a BIO's whole content out as a string (for PEM export).
    [[nodiscard]] std::string bioToString(BIO* bio)
    {
        auto* data = static_cast<char const*>(nullptr);
        auto const length = BIO_get_mem_data(bio, &data);
        return std::string { data, static_cast<std::size_t>(length) };
    }

    /// Generates a P-256 key through the generic `EVP_PKEY_CTX` API.
    [[nodiscard]] PKeyPtr generateKey()
    {
        auto ctx = PKeyCtxPtr { EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free };
        if (!ctx || EVP_PKEY_keygen_init(ctx.get()) != 1)
            return PKeyPtr { nullptr, EVP_PKEY_free };
        // A named curve rather than explicit parameters: some clients refuse the explicit encoding
        // outright, and it is larger for no benefit.
        if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) != 1
            || EVP_PKEY_CTX_set_ec_param_enc(ctx.get(), OPENSSL_EC_NAMED_CURVE) != 1)
            return PKeyPtr { nullptr, EVP_PKEY_free };
        auto* raw = static_cast<EVP_PKEY*>(nullptr);
        if (EVP_PKEY_keygen(ctx.get(), &raw) != 1)
            return PKeyPtr { nullptr, EVP_PKEY_free };
        return PKeyPtr { raw, EVP_PKEY_free };
    }

    /// Renders the subjectAltName extension value for @p names.
    ///
    /// Each name is classified by what it IS rather than by how it is spelled: `a2i_IPADDRESS` is
    /// asked whether the text parses as an address literal, and only what it refuses becomes a DNS
    /// name. Guessing from the spelling gets `2001:db8::1` and a host called `10things` wrong in
    /// opposite directions.
    /// @param names What the certificate should be valid for; empty entries are skipped.
    /// @return An OpenSSL SAN configuration string.
    [[nodiscard]] std::string subjectAltNames(std::vector<std::string> const& names)
    {
        auto value = std::string {};
        for (auto const& name: names)
        {
            if (name.empty())
                continue;
            if (!value.empty())
                value += ',';
            auto const address = Asn1OctetStringPtr { a2i_IPADDRESS(name.c_str()), ASN1_OCTET_STRING_free };
            value += address ? "IP:" : "DNS:";
            value += name;
        }
        ERR_clear_error(); // a refused parse is a DNS name, not an error to report later
        return value;
    }
} // namespace

std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsServerContext(std::string_view certPem,
                                                                              std::string_view keyPem)
{
    auto ctx = newCtx(TLS_server_method());
    if (!ctx)
        return std::unexpected("SSL_CTX_new failed: " + opensslError());
    if (auto used = useServerMaterial(ctx.get(), certPem, keyPem); !used)
        return std::unexpected(std::move(used.error()));
    return std::make_shared<TlsContext>(std::move(ctx), TlsRole::Server);
}

std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsServerContextFromFiles(
    std::filesystem::path const& certPath, std::filesystem::path const& keyPath)
{
    // Read here rather than through OpenSSL's own file functions, so a path takes the platform's
    // spelling -- a wide one on Windows -- and the same material function serves both entry points.
    auto const certPem = readFile(certPath, "certificate file");
    if (!certPem)
        return std::unexpected(certPem.error());
    auto const keyPem = readFile(keyPath, "private key file");
    if (!keyPem)
        return std::unexpected(keyPem.error());
    return makeTlsServerContext(*certPem, *keyPem);
}

std::expected<CertKeyPem, std::string> generateSelfSignedCertificate(SelfSignedOptions const& options)
{
    ERR_clear_error();
    if (options.commonName.empty())
        return std::unexpected(std::string { "a self-signed certificate needs a common name" });
    // A validity that is not positive yields a certificate already expired, which every client
    // refuses -- a misconfiguration that reads as a TLS failure.
    if (options.validity <= std::chrono::seconds::zero())
        return std::unexpected(std::string { "a self-signed certificate needs a positive validity" });

    auto const& names =
        options.subjectNames.empty() ? std::vector<std::string> { options.commonName } : options.subjectNames;
    // A comma ENDS one entry and starts another in the grammar below, so a name carrying one would
    // silently add a subject name nobody asked for. A colon is NOT refused: OpenSSL splits the type
    // from the value at the FIRST one, which is what makes `::1` a legal IP entry.
    if (auto const injected =
            std::ranges::find_if(names, [](auto const& name) { return name.contains(','); });
        injected != names.end())
        return std::unexpected("a subject name may not contain a comma: '" + *injected + "'");
    auto const altNames = subjectAltNames(names);
    if (altNames.empty())
        return std::unexpected(std::string { "a self-signed certificate needs at least one subject name" });

    auto const key = generateKey();
    if (!key)
        return std::unexpected("generating a P-256 key: " + opensslError());

    auto const cert = X509Ptr { X509_new(), X509_free };
    if (!cert)
        return std::unexpected("X509_new failed: " + opensslError());
    // Version 3, which an extension requires; the field is zero-based, so 2 IS version 3.
    if (X509_set_version(cert.get(), 2) != 1)
        return std::unexpected("setting the certificate version: " + opensslError());

    // A random serial, not a fixed one: a client that has seen two certificates with the same
    // issuer and serial -- which every certificate generated here would be, after a restart --
    // refuses the second outright rather than asking.
    auto const serial = BignumPtr { BN_new(), BN_free };
    if (!serial || BN_rand(serial.get(), 127, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1
        || BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(cert.get())) == nullptr)
        return std::unexpected("setting the certificate serial: " + opensslError());

    // Days and seconds apart, through X509_time_adj_ex: a `long` of seconds is 32 bits on Windows,
    // so a validity beyond about 68 years wrapped into the past.
    auto const validityDays = std::chrono::floor<std::chrono::days>(options.validity);
    auto const validitySeconds = options.validity - validityDays;
    if (X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) == nullptr
        || X509_time_adj_ex(X509_getm_notAfter(cert.get()),
                            static_cast<int>(validityDays.count()),
                            static_cast<long>(validitySeconds.count()),
                            nullptr)
               == nullptr)
        return std::unexpected("setting the certificate validity: " + opensslError());
    if (X509_set_pubkey(cert.get(), key.get()) != 1)
        return std::unexpected("setting the certificate public key: " + opensslError());

    auto* const subject = X509_get_subject_name(cert.get());
    if (X509_NAME_add_entry_by_txt(subject,
                                   "CN",
                                   MBSTRING_UTF8,
                                   reinterpret_cast<unsigned char const*>(options.commonName.c_str()),
                                   -1,
                                   -1,
                                   0)
            != 1
        || X509_set_issuer_name(cert.get(), subject) != 1) // self-signed: issuer == subject
        return std::unexpected("setting the certificate subject: " + opensslError());

    auto extensionContext = X509V3_CTX {};
    X509V3_set_ctx_nodb(&extensionContext);
    X509V3_set_ctx(&extensionContext, cert.get(), cert.get(), nullptr, nullptr, 0);
    auto* const san = X509V3_EXT_conf_nid(nullptr, &extensionContext, NID_subject_alt_name, altNames.c_str());
    if (san == nullptr)
        return std::unexpected("building the subjectAltName extension: " + opensslError());
    auto const added = X509_add_ext(cert.get(), san, -1);
    X509_EXTENSION_free(san);
    if (added != 1)
        return std::unexpected("adding the subjectAltName extension: " + opensslError());

    if (X509_sign(cert.get(), key.get(), EVP_sha256()) == 0)
        return std::unexpected("X509_sign failed: " + opensslError());

    auto certBio = BioPtr { BIO_new(BIO_s_mem()), BIO_free };
    auto keyBio = BioPtr { BIO_new(BIO_s_mem()), BIO_free };
    if (!certBio || !keyBio)
        return std::unexpected("BIO_new failed: " + opensslError());
    if (PEM_write_bio_X509(certBio.get(), cert.get()) != 1)
        return std::unexpected("PEM_write_bio_X509 failed: " + opensslError());
    if (PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
        return std::unexpected("PEM_write_bio_PrivateKey failed: " + opensslError());

    return CertKeyPem { .certPem = bioToString(certBio.get()), .keyPem = bioToString(keyBio.get()) };
}

std::expected<std::shared_ptr<ITlsContext>, std::string> makeSelfSignedServerContext(
    SelfSignedOptions const& options)
{
    // Single-sourced through the PEM generator and the PEM context, so a generated certificate is
    // served by exactly the path a named one is.
    auto material = generateSelfSignedCertificate(options);
    if (!material)
        return std::unexpected(material.error());
    return makeTlsServerContext(material->certPem, material->keyPem);
}

std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsClientContext(
    std::string_view caPem, std::string_view expectedHostName)
{
    auto ctx = newCtx(TLS_client_method());
    if (!ctx)
        return std::unexpected("SSL_CTX_new failed: " + opensslError());

    if (caPem.empty())
    {
        // Trust on first use: something else authenticates; TLS only encrypts. The caller may
        // still pin the server's certificate fingerprint out of band.
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
    }
    else
    {
        // Every certificate in the PEM is a trust anchor, not only the first: a bundle of two CAs
        // silently losing the second is the chain defect `useServerMaterial` had, on this side.
        auto caBio = memBio(caPem);
        auto anchors = 0;
        while (true)
        {
            auto const ca = X509Ptr { PEM_read_bio_X509(caBio.get(), nullptr, nullptr, nullptr), X509_free };
            if (!ca)
                break;
            if (X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx.get()), ca.get()) != 1)
                return std::unexpected("X509_STORE_add_cert: " + opensslError());
            ++anchors;
        }
        if (anchors == 0)
            return std::unexpected("invalid CA PEM: " + opensslError());
        ERR_clear_error(); // reading past the last certificate is the loop's end, not a failure
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);

        // Bind the certificate to the host that was asked for. Without this, verification proves
        // only that the pinned CA signed the certificate -- so ANY certificate it ever signed, for
        // any name, would be accepted here. Set on the CTX, so it governs every SSL object wrap()
        // creates from it.
        if (!expectedHostName.empty())
        {
            auto* const param = SSL_CTX_get0_param(ctx.get());
            // An IP-literal host must be checked as an IP: X509_VERIFY_PARAM_set1_host would look
            // for it in a dNSName, which a correct certificate does not carry.
            auto const isIpLiteral =
                X509_VERIFY_PARAM_set1_ip_asc(param, std::string { expectedHostName }.c_str()) == 1;
            ERR_clear_error(); // a refused IP parse means "a DNS name", not a failure
            if (!isIpLiteral
                && X509_VERIFY_PARAM_set1_host(param, expectedHostName.data(), expectedHostName.size()) != 1)
                return std::unexpected("X509_VERIFY_PARAM_set1_host: " + opensslError());
            // Never accept a wildcard as the LEFTMOST label of a partial match ("*.example.com"
            // matching "a.b.example.com"); OpenSSL's default already refuses it, this pins it.
            X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        }
    }

    return std::make_shared<TlsContext>(std::move(ctx), TlsRole::Client);
}

std::expected<std::string, std::string> certificateFingerprint(std::string_view certPem)
{
    ERR_clear_error();
    auto bio = memBio(certPem);
    auto const cert = X509Ptr { PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free };
    if (!cert)
        return std::unexpected("invalid certificate PEM: " + opensslError());
    auto fingerprint = fingerprintOf(cert.get());
    if (fingerprint.empty())
        return std::unexpected("X509_digest failed: " + opensslError());
    return fingerprint;
}

bool constantTimeEquals(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size())
        return false;
    if (lhs.empty())
        return true;
    // CRYPTO_memcmp rather than a hand-rolled XOR-accumulate loop: the compiler is free to turn
    // the latter back into an early-exit compare, which is exactly what must not happen.
    return CRYPTO_memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

} // namespace core::net
