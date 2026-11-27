// SPDX-License-Identifier: Apache-2.0
#include <core/net/Tls.hpp>

#include <core/Assert.hpp>
#include <core/Base64.hpp>
#include <core/Utf8Path.hpp>
#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/SocketContract.hpp>
#include <core/net/SocketOption.hpp>
#include <core/net/detail/Alpn.hpp>
#include <core/net/detail/InFlightBudget.hpp>
#include <core/net/detail/ParkedCall.hpp>
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
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
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
    using SslPtr = std::unique_ptr<SSL, decltype(&SSL_free)>;
    using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;

    /// The SHA-256 of @p cert's DER, or nothing if it cannot be computed.
    [[nodiscard]] std::optional<std::array<std::byte, 32>> certificateDigest(X509 const* cert)
    {
        auto digest = std::array<std::byte, 32> {};
        auto length = 0U;
        if (cert == nullptr
            || X509_digest(cert, EVP_sha256(), reinterpret_cast<unsigned char*>(digest.data()), &length) != 1
            || length != digest.size())
            return std::nullopt;
        return digest;
    }

    /// @p cert, DER-encoded.
    [[nodiscard]] std::expected<DerCertificate, std::string> derOf(X509* cert)
    {
        auto* der = static_cast<unsigned char*>(nullptr);
        auto const length = i2d_X509(cert, &der);
        if (length <= 0)
            return std::unexpected("i2d_X509: " + opensslError());
        auto const release = detail::ScopeGuard { [der]() noexcept { OPENSSL_free(der); } };
        auto const* const begin = reinterpret_cast<std::byte const*>(der);
        return DerCertificate(begin, begin + length);
    }

    /// The SHA-256 fingerprint of @p cert as lower-case hex, or empty if it cannot be computed.
    [[nodiscard]] std::string fingerprintOf(X509 const* cert)
    {
        auto const digest = certificateDigest(cert);
        if (!digest)
            return {};
        constexpr auto Hex = std::string_view { "0123456789abcdef" };
        auto out = std::string {};
        out.reserve(digest->size() * 2);
        for (auto const byte: *digest)
        {
            out += Hex[std::to_integer<unsigned>(byte) >> 4U];
            out += Hex[std::to_integer<unsigned>(byte) & 0x0FU];
        }
        return out;
    }

    using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using PKeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using BignumPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
    using Asn1OctetStringPtr = std::unique_ptr<ASN1_OCTET_STRING, decltype(&ASN1_OCTET_STRING_free)>;
    using X509NamePtr = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>;

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

    /// Whether a client context binds each connection to the peer it was dialled for.
    enum class PeerBinding : std::uint8_t
    {
        /// The context's own check, fixed at construction (or none, for trust on first use):
        /// `wrapClient` refuses a per-connection name rather than drop it.
        Fixed = 0,
        /// `wrapClient` sets SNI and the certificate check from its `TlsPeer`, per connection.
        PerConnection,
    };

    using detail::AlpnWire;
    using detail::alpnWireFormat;

    /// What one session's OpenSSL callbacks read and record: reached through the SSL's app data,
    /// owned by the socket that owns the SSL.
    struct SessionNotes
    {
        /// The description byte of the first fatal alert the peer sent.
        std::optional<std::uint8_t> alertReceived;
        /// A server's ALPN names, its order of preference; null for a client or a server with none.
        /// Shared with the context, and held here because the SSL may outlive it.
        std::shared_ptr<AlpnWire const> alpnSelection;
    };

    /// What a peer name is, once @c classifyPeerName has read it.
    enum class PeerNameKind : std::uint8_t
    {
        Dns = 0, ///< A DNS name: checked against the certificate's DNS entries, and sent as SNI.
        Address, ///< An IP literal: checked against its IP entries, and sent as nothing.
    };

    /// A peer name as the certificate check reads it.
    struct PeerName
    {
        PeerNameKind kind = PeerNameKind::Dns;
        /// The DNS name as given, or the address without its brackets and zone.
        std::string checked;
    };

    /// The verifications a context's sockets started and its verifier has not answered, shared by
    /// the context, its sockets and every detached evaluation; @c TlsClientOptions::maxVerificationsInFlight.
    class VerificationBudget
    {
      public:
        /// @param slots How many slots there are.
        explicit VerificationBudget(std::size_t slots) noexcept: _maxInFlight { slots } {}

        /// @return How many slots there are.
        [[nodiscard]] std::size_t maxInFlight() const noexcept { return _maxInFlight; }

        std::atomic<std::size_t> inFlight { 0 }; ///< Slots taken.

      private:
        std::size_t _maxInFlight;
    };

    /// What a client session asks after its handshake, where an @c IChainVerifier checks its peer.
    struct ChainVerification
    {
        /// Null: OpenSSL checked the chain during the handshake, and nothing is asked after it.
        std::shared_ptr<IChainVerifier> verifier;
        /// The name the connection is for, as the certificate check reads it; none to check the
        /// chain alone.
        std::optional<PeerName> peer;
        /// The context's budget; non-null wherever @c verifier is.
        std::shared_ptr<VerificationBudget> budget;
    };

    /// One verification's rendezvous between the detached evaluation and the handshake waiting for it.
    using VerifyCall = detail::ParkedCall<std::expected<void, TlsFailure>>;

    /// One verification's slot and its answer, owned by the detached flow that runs it.
    ///
    /// **Its destructor is the guard of the slot rule**, as @c ExecutorAddressResolver's lease is: an
    /// evaluation that never answers -- its frame freed unrun by an executor shutting down -- gives
    /// the slot back and answers the waiter with a refusal, so neither is stranded.
    class VerificationLease
    {
      public:
        VerificationLease(std::shared_ptr<VerificationBudget> budget,
                          std::shared_ptr<VerifyCall> call) noexcept:
            _budget(std::move(budget)), _call(std::move(call))
        {
        }

        VerificationLease(VerificationLease&&) noexcept = default;
        VerificationLease(VerificationLease const&) = delete;
        VerificationLease& operator=(VerificationLease const&) = delete;
        VerificationLease& operator=(VerificationLease&&) = delete;

        ~VerificationLease() { fail(); }

        /// Gives the slot back, THEN answers the waiter, so a resumed handshake never finds its own
        /// slot still taken. At most once; a later call does nothing.
        /// @param verdict What the verifier answered.
        void finish(std::expected<void, TlsFailure> verdict) noexcept
        {
            auto const call = std::exchange(_call, nullptr);
            auto const budget = std::exchange(_budget, nullptr);
            if (!call)
                return;
            budget->inFlight.fetch_sub(1, std::memory_order_acq_rel);
            call->settle(std::move(verdict));
        }

        /// Finishes with a refusal that allocates nothing, for the paths that cannot take a throw.
        void fail() noexcept
        {
            if (!_call)
                return;
            finish(std::unexpected(
                TlsFailure { .kind = TlsFailureKind::Certificate, .alert = std::nullopt, .message = {} }));
        }

      private:
        std::shared_ptr<VerificationBudget> _budget;
        std::shared_ptr<VerifyCall> _call;
    };

    /// Asks @p verifier about @p chain and answers @p lease's call.
    ///
    /// Detached, because nobody may own it: the handshake waiting for it can be stopped, and its
    /// socket destroyed, long before the platform answers. It holds what it touches by value or by
    /// share, never the socket, and never its loop: the verifier finishes on its own executor, and
    /// the answer reaches the socket only through @p lease's call, which hands a PARKED waiter back to
    /// its loop -- a waiter that a stop took away, with its socket and loop gone since, is handed
    /// nowhere. It runs with no stop token, so a stop never reaches the evaluation.
    /// Whatever is thrown is answered as a refusal: an exception escaping a detached flow ends the
    /// process, and an unanswered waiter would be stranded.
    /// @param verifier Who decides.
    /// @param chain The presented chain, leaf first.
    /// @param serverName The name to check it for; empty for the chain alone.
    /// @param lease The verification's slot and call.
    async::DetachedTask runVerification(std::shared_ptr<IChainVerifier> verifier,
                                        std::vector<DerCertificate> chain,
                                        std::string serverName,
                                        VerificationLease lease)
    {
        try
        {
            lease.finish(co_await verifier->verify(std::move(chain), std::move(serverName)));
        }
        catch (...)
        {
            lease.fail();
        }
    }

    /// The flags every name check here uses: a wildcard stands for one whole leftmost label, and
    /// the subject's common name is never a name (RFC 9525 section 6.3).
    constexpr auto NameCheckFlags = static_cast<unsigned>(X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS)
                                    | static_cast<unsigned>(X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);

    /// Whether @p leaf names @p peer: a DNS subjectAltName for a DNS name, an IP one for an address.
    [[nodiscard]] bool certificateNames(X509* leaf, PeerName const& peer)
    {
        if (peer.kind == PeerNameKind::Address)
            return X509_check_ip_asc(leaf, peer.checked.c_str(), NameCheckFlags) == 1;
        return X509_check_host(leaf, peer.checked.data(), peer.checked.size(), NameCheckFlags, nullptr) == 1;
    }

    /// Where a session stands. One state rather than flags, so "verified" cannot be read off a
    /// finished handshake: with a verifier, the handshake finishing is only `Verifying`.
    enum class SessionState : std::uint8_t
    {
        Handshaking = 0, ///< Not finished; the next operation drives it (again, after a cancel).
        Verifying,       ///< Finished, and the verifier has not answered: nothing moves yet.
        Established,     ///< Finished and trusted: reads and writes move application bytes.
        Failed,          ///< Refused, or broken; sticky, and `_handshakeError` says why.
    };

    [[nodiscard]] SessionNotes* notesOf(SSL const* ssl) noexcept
    {
        return static_cast<SessionNotes*>(SSL_get_app_data(ssl));
    }

    /// Records the first FATAL alert the peer sends, for `TlsFailureKind::AlertReceived`.
    ///
    /// A warning is not what ended the connection: TLS 1.2 lets a server warn (`unrecognized_name`,
    /// 112) and carry on, and the fatal alert that follows, if any, is the one that names the
    /// failure.
    void recordAlert(SSL const* ssl, int where, int value)
    {
        auto* const notes = notesOf(ssl);
        // Both bits: SSL_CB_READ_ALERT and SSL_CB_WRITE_ALERT share SSL_CB_ALERT, so a mask test
        // against the first alone also matches every alert this end sends.
        if ((where & SSL_CB_READ_ALERT) != SSL_CB_READ_ALERT || notes == nullptr || notes->alertReceived)
            return;
        // The value is the level in the high byte and the description in the low one.
        auto const bits = static_cast<unsigned>(value);
        if ((bits >> 8U) != SSL3_AL_FATAL)
            return;
        notes->alertReceived = static_cast<std::uint8_t>(bits & 0xFFU);
    }

    /// A server's ALPN choice: the first of its own names the client offered. None in common is
    /// refused with `no_application_protocol`, as RFC 7301 section 3.2 requires.
    ///
    /// Installed only on a context with names to select from, whose every session the socket gives
    /// them to: a session reaching it without is a defect, not a client to serve without ALPN.
    int selectAlpn(SSL* ssl,
                   unsigned char const** out,
                   unsigned char* outLength,
                   unsigned char const* offered,
                   unsigned int offeredLength,
                   void* /*argument*/)
    {
        auto const* const notes = notesOf(ssl);
        Require(notes != nullptr && notes->alpnSelection != nullptr);
        auto const chosen = detail::selectAlpn(*notes->alpnSelection, std::span { offered, offeredLength });
        if (!chosen)
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        *out = chosen->data();
        *outLength = static_cast<unsigned char>(chosen->size());
        return SSL_TLSEXT_ERR_OK;
    }

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
    class TlsSocket final: public ISocket, public ITlsSession
    {
      public:
        /// @param inner The connected transport (owned).
        /// @param ssl The session, its BIOs attached (owned).
        /// @param executor The loop a waiter parked on one of the gates is resumed on; must outlive
        ///        every operation on this socket.
        /// @param alpnSelection A server's ALPN names, or null.
        /// @param verification For a client checked by an @c IChainVerifier, the verifier and the
        ///        name; empty where OpenSSL checks the chain, or nothing is checked.
        TlsSocket(std::unique_ptr<ISocket> inner,
                  SslPtr ssl,
                  async::IExecutor& executor,
                  std::shared_ptr<AlpnWire const> alpnSelection,
                  ChainVerification verification = {}):
            _inner(std::move(inner)),
            _ssl(ssl.release()),
            _rbio(SSL_get_rbio(_ssl)),
            _wbio(SSL_get_wbio(_ssl)),
            _notes { .alertReceived = std::nullopt, .alpnSelection = std::move(alpnSelection) },
            _verification(std::move(verification)),
            _executor(&executor),
            _handshaking(std::make_shared<SerialGate>(executor)),
            _flushing(std::make_shared<SerialGate>(executor))
        {
            SSL_set_app_data(_ssl, &_notes);
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

        [[nodiscard]] std::expected<std::array<std::byte, 32>, std::string> channelBinding() const override
        {
            if (_state != SessionState::Established)
                return std::unexpected("the TLS session is not established");
            if (SSL_version(_ssl) < TLS1_3_VERSION)
                return std::unexpected("the tls-exporter channel binding needs TLS 1.3 (RFC 9266)");
            ERR_clear_error();
            auto binding = std::array<std::byte, 32> {};
            constexpr auto Label = std::string_view { "EXPORTER-Channel-Binding" };
            if (SSL_export_keying_material(_ssl,
                                           reinterpret_cast<unsigned char*>(binding.data()),
                                           binding.size(),
                                           Label.data(),
                                           Label.size(),
                                           nullptr,
                                           0,
                                           0)
                != 1)
                return std::unexpected("SSL_export_keying_material: " + opensslError());
            return binding;
        }

        [[nodiscard]] std::optional<std::array<std::byte, 32>> peerCertificateDigest() const override
        {
            if (_state != SessionState::Established)
                return std::nullopt;
            auto const peer = X509Ptr { SSL_get1_peer_certificate(_ssl), X509_free };
            return certificateDigest(peer.get());
        }

        [[nodiscard]] std::optional<TlsVersion> negotiatedVersion() const override
        {
            if (_state != SessionState::Established)
                return std::nullopt;
            return SSL_version(_ssl) >= TLS1_3_VERSION ? TlsVersion::Tls13 : TlsVersion::Tls12;
        }

        [[nodiscard]] std::optional<TlsFailure> failure() const override { return _failure; }

        [[nodiscard]] std::string negotiatedProtocol() const override
        {
            if (_state != SessionState::Established)
                return {};
            auto const* selected = static_cast<unsigned char const*>(nullptr);
            auto length = 0U;
            SSL_get0_alpn_selected(_ssl, &selected, &length);
            if (selected == nullptr)
                return {};
            return std::string { reinterpret_cast<char const*>(selected), length };
        }

        /// @see ISocket::tlsSession: this socket is its own session.
        [[nodiscard]] ITlsSession const* tlsSession() const noexcept override { return this; }

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
            if (_state == SessionState::Established)
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

        /// @param settings The probe intervals; applied to the INNER transport, which is where the
        ///        TCP connection is -- TLS records ride on it and notice nothing of the probes.
        [[nodiscard]] std::expected<void, NetError> armKeepAlive(KeepAliveSettings const& settings) override
        {
            return _inner->armKeepAlive(settings);
        }

        [[nodiscard]] std::string peerAddress() const override { return _inner->peerAddress(); }

        /// The INNER transport's: TLS records ride on its connection, which is what has an address.
        [[nodiscard]] std::expected<ResolvedEndpoint, NetError> localEndpoint() const override
        {
            return _inner->localEndpoint();
        }

        /// The INNER transport's, as @c localEndpoint is.
        [[nodiscard]] std::expected<ResolvedEndpoint, NetError> remoteEndpoint() const override
        {
            return _inner->remoteEndpoint();
        }

        /// The INNER transport's: the options are the TCP connection's, which TLS records ride on.
        [[nodiscard]] std::expected<std::uint64_t, NetError> option(SocketOption option) const override
        {
            return _inner->option(option);
        }

        /// Set on the INNER transport, as @c option reads it.
        [[nodiscard]] std::expected<void, NetError> setOption(SocketOption option,
                                                              std::uint64_t value) override
        {
            return _inner->setOption(option, value);
        }

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
        /// Classifies the TLS layer's first failure for @c failure -- of the handshake, or of a record
        /// the peer sent; a write fails only on this end's own state, which is no peer's failure --
        /// as the peer's certificate refused
        /// (only where it was checked: trust on first use completes with a failed verification
        /// result that nothing acted on), a fatal alert received, or anything else.
        /// @param reason OpenSSL's reason, read when the failing call returned.
        void recordFailure(std::string reason)
        {
            if (_failure)
                return;
            if (auto const verdict = SSL_get_verify_result(_ssl);
                verdict != X509_V_OK && (SSL_get_verify_mode(_ssl) & SSL_VERIFY_PEER) != 0)
                _failure = TlsFailure { .kind = TlsFailureKind::Certificate,
                                        .alert = std::nullopt,
                                        .message = X509_verify_cert_error_string(verdict) };
            else if (_notes.alertReceived)
                _failure = TlsFailure { .kind = TlsFailureKind::AlertReceived,
                                        .alert = _notes.alertReceived,
                                        .message = SSL_alert_desc_string_long(*_notes.alertReceived) };
            else
                _failure = TlsFailure { .kind = TlsFailureKind::Protocol,
                                        .alert = std::nullopt,
                                        .message = std::move(reason) };
        }

        /// Defence in depth for a session that verifies its peer: a finished handshake is only
        /// established with a peer certificate and a verification result of OK. OpenSSL refuses
        /// the handshake before either could be otherwise; this holds if it ever did not.
        /// @return Nothing, or the failure, classified as @c TlsFailureKind::Certificate.
        [[nodiscard]] std::expected<void, NetError> confirmVerified()
        {
            if ((SSL_get_verify_mode(_ssl) & SSL_VERIFY_PEER) == 0)
                return {};
            auto const peer = X509Ptr { SSL_get1_peer_certificate(_ssl), X509_free };
            auto const verdict = SSL_get_verify_result(_ssl);
            if (peer && verdict == X509_V_OK)
                return {};
            auto const message = std::string { peer ? X509_verify_cert_error_string(verdict)
                                                    : "the peer presented no certificate" };
            if (!_failure)
                _failure = TlsFailure { .kind = TlsFailureKind::Certificate,
                                        .alert = std::nullopt,
                                        .message = message };
            return std::unexpected(makeNetError(NetErrorCode::SystemError, 0, "TLS handshake: " + message));
        }

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
                    default: {
                        auto reason = opensslError();
                        recordFailure(reason);
                        co_return std::unexpected(
                            makeNetError(NetErrorCode::SystemError, 0, "SSL_read: " + reason));
                    }
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
                    default: {
                        auto reason = opensslError();
                        recordFailure(reason);
                        co_return std::unexpected(
                            makeNetError(NetErrorCode::SystemError, 0, "SSL_peek: " + reason));
                    }
                }
            }
        }

        /// Writes and flushes `close_notify`, then half-closes the inner transport.
        async::Task<std::expected<void, NetError>> closeNotify()
        {
            if (_handshaking->busy() || _state == SessionState::Established)
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
                if (_state == SessionState::Established)
                    co_return std::expected<void, NetError> {};
                if (_state == SessionState::Failed)
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
                // Classified now too, for the same reason and one more: the flush may fail, and the
                // failure that caused it must not be lost behind the transport's.
                auto const failed = result != 1 && err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE;
                if (failed)
                    recordFailure(reason);
                // Always flush whatever the last step queued (ClientHello, the server's flight,
                // Finished, ...) before deciding what to await.
                if (auto const flushed = co_await flushOut(FlushWait::Join); !flushed)
                {
                    outcome = std::unexpected(flushed.error());
                    break;
                }
                if (result == 1)
                {
                    if (auto const verified = confirmVerified(); !verified)
                    {
                        outcome = std::unexpected(verified.error());
                        break;
                    }
                    if (_verification.verifier)
                    {
                        _state = SessionState::Verifying;
                        auto const verified = co_await verifyChain();
                        if (!verified)
                        {
                            outcome = std::unexpected(verified.error());
                            break;
                        }
                    }
                    _state = SessionState::Established;
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
                else if (failed)
                {
                    outcome = std::unexpected(
                        makeNetError(NetErrorCode::SystemError, 0, "TLS handshake: " + reason));
                    break;
                }
            }

            // A failure is sticky -- parked waiters and every later operation observe it -- with two
            // exceptions. A cancelled inner read took no ciphertext, so the handshake can simply be
            // driven again, and `cancelRead` is documented not to be a close. A full verifier budget,
            // the one `ResourceExhausted` a `Verifying` socket answers, refused only the asking and
            // not the peer: the socket stays `Verifying`, and a handshake driven again once a slot
            // is free asks again. The reason travels in NetError::context ("TLS handshake: <openssl
            // error>"); net RETURNS failures rather than logging them.
            auto const retryable = !outcome
                                   && (outcome.error().code == NetErrorCode::Cancelled
                                       || (_state == SessionState::Verifying
                                           && outcome.error().code == NetErrorCode::ResourceExhausted));
            if (!outcome && !retryable)
            {
                _handshakeError = outcome.error();
                _state = SessionState::Failed;
            }
            co_return outcome;
        }

        /// Asks the verifier about the chain the peer presented, for the name the connection is for.
        ///
        /// Runs inside the handshake gate, so a read or a write that arrives meanwhile waits there,
        /// and the socket is `Verifying` until it answers: nothing of the caller's moves first.
        /// @return Nothing once the verifier accepted; otherwise the refusal, classified as
        ///         @c TlsFailureKind::Certificate.
        async::Task<std::expected<void, NetError>> verifyChain()
        {
            // The client's view of the peer chain includes the leaf, first (`SSL_get_peer_cert_chain`;
            // `SSL_get0_peer_certificates` is BoringSSL's).
            auto const* const presented = SSL_get_peer_cert_chain(_ssl);
            // Defence in depth, as `confirmVerified` is: a verifier is never asked to trust nothing.
            if (presented == nullptr || sk_X509_num(presented) <= 0)
                co_return refuseChain("the peer presented no certificate");

            // The name is checked here, before any platform is asked, so it means the same on every
            // one: Windows' SSL policy, for one, falls back to the common name, which RFC 9525 forbids.
            if (_verification.peer && !certificateNames(sk_X509_value(presented, 0), *_verification.peer))
            {
                ERR_clear_error();
                co_return refuseChain("the certificate is not valid for '" + _verification.peer->checked
                                      + "'");
            }

            if (_pendingVerification == nullptr)
            {
                auto chain = std::vector<DerCertificate> {};
                for (auto const index: std::views::iota(0, sk_X509_num(presented)))
                {
                    auto der = derOf(sk_X509_value(presented, index));
                    if (!der)
                        co_return refuseChain(der.error());
                    chain.push_back(std::move(*der));
                }
                // Allocated before the slot is taken, as `ExecutorAddressResolver` does, so a failed
                // allocation has no slot to give back. From the reservation on, the lease owns the
                // slot: a frame that cannot be allocated destroys it, and its destructor gives it back.
                auto call = std::make_shared<VerifyCall>(_executor);
                auto& budget = *_verification.budget;
                if (detail::reserveSlot(budget.inFlight, budget.maxInFlight())
                    == detail::SlotReservation::Refused)
                    co_return std::unexpected(
                        makeNetError(NetErrorCode::ResourceExhausted,
                                     0,
                                     std::format("the chain verifier budget is full ({} in flight)",
                                                 budget.maxInFlight())));
                _pendingVerification = std::move(call);
                runVerification(_verification.verifier,
                                std::move(chain),
                                _verification.peer ? _verification.peer->checked : std::string {},
                                VerificationLease { _verification.budget, _pendingVerification });
            }
            else
                _pendingVerification->rejoin();

            // By value, not only through `this`: the socket may go while the verifier is answering.
            auto const call = _pendingVerification;
            auto const lifetime = std::weak_ptr<void const> { _lifetime };
            // A stop throws out of this wait and leaves the call where it is, for a retry to join.
            auto verdict = co_await VerifyCall::wait(call);
            // As `feedIn`: the socket may have been destroyed while the verifier was answering.
            if (lifetime.expired())
                throw async::OperationCancelled {};
            _pendingVerification.reset();
            if (!verdict)
            {
                // Whatever the verifier called it, a refused chain is a certificate failure.
                auto refused = std::move(verdict.error());
                refused.kind = TlsFailureKind::Certificate;
                _failure = refused;
                co_return std::unexpected(makeNetError(
                    NetErrorCode::SystemError, 0, "TLS certificate refused: " + refused.message));
            }
            co_return std::expected<void, NetError> {};
        }

        /// A refusal this end decided before asking the verifier, classified as a certificate failure.
        [[nodiscard]] std::expected<void, NetError> refuseChain(std::string const& message)
        {
            _failure =
                TlsFailure { .kind = TlsFailureKind::Certificate, .alert = std::nullopt, .message = message };
            return std::unexpected(
                makeNetError(NetErrorCode::SystemError, 0, "TLS certificate refused: " + message));
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
        SessionState _state = SessionState::Handshaking;
        bool _peerClosed = false;                ///< A read saw the peer's close_notify or inner EOF.
        std::optional<NetError> _handshakeError; ///< Why the session is `Failed` (sticky).
        std::optional<TlsFailure> _failure;      ///< The TLS layer's first failure, classified.
        SessionNotes _notes;                     ///< What the session's callbacks record; its app data.
        ChainVerification _verification;         ///< Who is asked after the handshake, if anyone.
        async::IExecutor* _executor;             ///< Where this socket's waiters are resumed.
        /// The verification this socket started and has not yet been answered: kept past a stop, so a
        /// handshake driven again joins it rather than starting another.
        std::shared_ptr<VerifyCall> _pendingVerification;
        Direction _innerReadOwner = Direction::Any; ///< Whose operation the latest inner read served.

        /// Held by the coroutine driving the handshake. Shared: see @c SerialGate.
        std::shared_ptr<SerialGate> _handshaking;
        /// Held by the coroutine writing ciphertext out. Shared: see @c SerialGate.
        std::shared_ptr<SerialGate> _flushing;

        /// Expires with this socket: what `feedIn` and `flushOut` ask, never `this`, when an inner
        /// operation hands them back, because the socket may have been destroyed in between.
        std::shared_ptr<void const> _lifetime = std::make_shared<char const>('\0');
    };

    /// Decides what kind of name @p name is, and what of it is checked.
    ///
    /// **A literal is what `detail::isNumericHost` says it is** -- a dotted quad or IPv6 text, the
    /// strict parse the dial path uses -- not what OpenSSL's looser parser accepts, so the two never
    /// disagree about whether a host was an address. Brackets, as a URL authority writes them, hold
    /// an IPv6 literal and nothing else. A zone (`fe80::1%eth0`) is local routing, not identity: the
    /// address without it is checked, and nothing is sent as SNI. Everything else is a DNS name.
    /// @param name The name the connection is for; non-empty.
    /// @return What kind of name it is, or a reason it cannot be checked.
    [[nodiscard]] std::expected<PeerName, std::string> classifyPeerName(std::string_view name)
    {
        // A NUL cuts a C string short: the check (and SNI) would be for a prefix of the host asked for.
        if (name.contains('\0'))
            return std::unexpected(std::string { "a TLS server name may not contain NUL" });
        auto const bracketed = name.size() >= 2 && name.front() == '[' && name.back() == ']';
        auto const unbracketed = bracketed ? name.substr(1, name.size() - 2) : name;
        auto const address = unbracketed.substr(0, unbracketed.find('%'));
        auto const zoned = address.size() != unbracketed.size();
        auto const isIpv6 = detail::isNumericHost(address) && address.contains(':');
        if (detail::isNumericHost(address) && (isIpv6 || (!bracketed && !zoned)))
            return PeerName { .kind = PeerNameKind::Address, .checked = std::string { address } };
        if (bracketed || zoned)
            return std::unexpected("a bracketed or zoned TLS server name must be an IPv6 literal: '"
                                   + std::string { name } + "'");
        return PeerName { .kind = PeerNameKind::Dns, .checked = std::string { name } };
    }

    /// Points @p param's certificate check at @p name, as @c classifyPeerName reads it: an address
    /// against the certificate's IP entries, a DNS name by whole labels (no wildcard inside a label)
    /// against its DNS entries alone -- never the subject's common name (RFC 9525 section 6.3).
    /// @param param The verification parameters of a context or a session.
    /// @param name The name the connection is for; non-empty.
    /// @return The name as checked, or a reason it cannot be checked.
    [[nodiscard]] std::expected<PeerName, std::string> setVerificationName(X509_VERIFY_PARAM* param,
                                                                           std::string_view name)
    {
        auto peer = classifyPeerName(name);
        if (!peer)
            return peer;
        X509_VERIFY_PARAM_set_hostflags(param, NameCheckFlags);
        ERR_clear_error();
        if (peer->kind == PeerNameKind::Address)
        {
            if (X509_VERIFY_PARAM_set1_ip_asc(param, peer->checked.c_str()) != 1)
                return std::unexpected("X509_VERIFY_PARAM_set1_ip_asc: " + opensslError());
            return peer;
        }
        if (X509_VERIFY_PARAM_set1_host(param, peer->checked.data(), peer->checked.size()) != 1)
            return std::unexpected("TLS server name '" + peer->checked + "': " + opensslError());
        return peer;
    }

    /// Binds @p ssl to @p name: the check @c setVerificationName sets, and SNI for a DNS name.
    ///
    /// The check is set on a verifier context's session too, where `SSL_VERIFY_NONE` means OpenSSL
    /// acts on no result: the name the verifier is asked about is the same one, read the same way.
    /// @param ssl A client session, before its handshake.
    /// @param name A non-empty @c TlsPeer::serverName.
    /// @return The name as checked, or @c NetErrorCode::AddressError for a name the wire cannot carry.
    [[nodiscard]] std::expected<PeerName, NetError> bindPeer(SSL* ssl, std::string_view name)
    {
        auto peer = setVerificationName(SSL_get0_param(ssl), name);
        if (!peer)
            return std::unexpected(makeNetError(NetErrorCode::AddressError, 0, peer.error()));
        if (peer->kind == PeerNameKind::Address)
            return std::move(*peer); // RFC 6066 section 3: no literal address in server_name
        // SSL_set_tlsext_host_name, spelled out: the macro casts the name to `void*` C-style. The
        // name is a DNS name here, so it has no NUL and the check above already accepted it.
        auto host = peer->checked;
        if (SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, host.data()) != 1)
            return std::unexpected(makeNetError(
                NetErrorCode::AddressError, 0, "TLS server name '" + host + "': " + opensslError()));
        return std::move(*peer);
    }

    /// The DI context: a configured SSL_CTX plus its handshake role.
    class TlsContext final: public ITlsContext
    {
      public:
        /// @param ctx The configured context (owned).
        /// @param role The end of the handshake its sockets play.
        /// @param binding For a client, whether `wrapClient` honours a @c TlsPeer.
        /// @param alpnSelection For a server, the ALPN names it selects from; null for none.
        /// @param verifier For a client, the verifier asked after every handshake; null where OpenSSL
        ///        checks the chain, or nothing does.
        /// @param maxVerificationsInFlight With @p verifier, its sockets' shared budget of unanswered
        ///        verifications (@c TlsClientOptions::maxVerificationsInFlight).
        TlsContext(SslCtxPtr ctx,
                   TlsRole role,
                   PeerBinding binding = PeerBinding::Fixed,
                   std::shared_ptr<AlpnWire const> alpnSelection = nullptr,
                   std::shared_ptr<IChainVerifier> verifier = nullptr,
                   std::size_t maxVerificationsInFlight = 0):
            _ctx(std::move(ctx)),
            _role(role),
            _binding(binding),
            _alpnSelection(std::move(alpnSelection)),
            _verifier(std::move(verifier)),
            _budget(std::make_shared<VerificationBudget>(maxVerificationsInFlight))
        {
        }

        /// A verifier context's session asks it too, about the chain alone.
        std::unique_ptr<ISocket> wrap(std::unique_ptr<ISocket> inner, async::IExecutor& executor) override
        {
            auto ssl = newSession();
            if (!ssl)
                return nullptr;
            return std::make_unique<TlsSocket>(
                std::move(inner),
                std::move(ssl),
                executor,
                _alpnSelection,
                ChainVerification { .verifier = _verifier, .peer = std::nullopt, .budget = _budget });
        }

        /// A context built to bind connections sets each one's SNI and check from @p peer; any other
        /// keeps the default, which refuses a name rather than drop it.
        std::expected<std::unique_ptr<ISocket>, NetError> wrapClient(std::unique_ptr<ISocket> inner,
                                                                     async::IExecutor& executor,
                                                                     TlsPeer const& peer) override
        {
            if (_binding != PeerBinding::PerConnection)
                return ITlsContext::wrapClient(std::move(inner), executor, peer);
            // A verifying context checks the leaf for a name; without one, any certificate its trust
            // vouches for -- issued to anybody -- would be accepted for this connection.
            if (peer.serverName.empty())
                return std::unexpected(
                    makeNetError(NetErrorCode::AddressError,
                                 0,
                                 "a verifying TLS client needs the server name it dialled"));
            auto ssl = newSession();
            if (!ssl)
                return std::unexpected(makeNetError(
                    NetErrorCode::ResourceExhausted, 0, "the TLS session could not be allocated"));
            auto bound = bindPeer(ssl.get(), peer.serverName);
            if (!bound)
                return std::unexpected(std::move(bound.error()));
            auto checked = std::optional<PeerName> { std::move(*bound) };
            return std::make_unique<TlsSocket>(
                std::move(inner),
                std::move(ssl),
                executor,
                _alpnSelection,
                ChainVerification { .verifier = _verifier, .peer = std::move(checked), .budget = _budget });
        }

        [[nodiscard]] std::string certificateFingerprint() const override
        {
            return fingerprintOf(SSL_CTX_get0_certificate(_ctx.get()));
        }

      private:
        /// A session of this context's role over two memory BIOs, or null if it could not be made.
        [[nodiscard]] SslPtr newSession() const
        {
            auto ssl = SslPtr { SSL_new(_ctx.get()), SSL_free };
            if (!ssl)
                return ssl;
            // Memory BIOs bridge OpenSSL and the coroutine transport; SSL_set_bio takes ownership
            // of both, so SSL_free later releases them.
            //
            // Checked like SSL_new above: handing a null BIO to SSL_set_bio yields a socket that
            // LOOKS constructed and dereferences null on its first read or write. Ownership has not
            // transferred yet on this path, so both BIOs are released here (BIO_free tolerates
            // null, which is what makes the one-sided case work).
            auto* const rbio = BIO_new(BIO_s_mem());
            auto* const wbio = BIO_new(BIO_s_mem());
            if (rbio == nullptr || wbio == nullptr)
            {
                BIO_free(rbio);
                BIO_free(wbio);
                return SslPtr { nullptr, SSL_free };
            }
            SSL_set_bio(ssl.get(), rbio, wbio);
            if (_role == TlsRole::Server)
                SSL_set_accept_state(ssl.get());
            else
                SSL_set_connect_state(ssl.get());
            return ssl;
        }

        SslCtxPtr _ctx;
        TlsRole _role;
        PeerBinding _binding;
        std::shared_ptr<AlpnWire const> _alpnSelection;
        std::shared_ptr<IChainVerifier> _verifier;
        std::shared_ptr<VerificationBudget> _budget;
    };

    /// A context with the floor every one of them shares: TLS 1.2 (or the version @p options asks for)
    /// and no renegotiation.
    ///
    /// Renegotiation is refused because it is the one way a WRITE can need a READ after the
    /// handshake (`SSL_write` answering `WANT_READ`), and a socket with a read already parked has
    /// no second read slot to give it. TLS 1.3 has no renegotiation at all.
    [[nodiscard]] SslCtxPtr newCtx(SSL_METHOD const* method, TlsOptions const& options)
    {
        ERR_clear_error();
        auto ctx = SslCtxPtr { SSL_CTX_new(method), SSL_CTX_free };
        if (ctx)
        {
            SSL_CTX_set_min_proto_version(
                ctx.get(), options.minimumVersion == TlsVersion::Tls13 ? TLS1_3_VERSION : TLS1_2_VERSION);
            SSL_CTX_set_options(ctx.get(), SSL_OP_NO_RENEGOTIATION);
            // TLS 1.2 suites with no authentication or no encryption, refused whatever the build's
            // default list holds: a handshake with one has no certificate to check. (TLS 1.3 has none.)
            if (SSL_CTX_set_cipher_list(ctx.get(), "DEFAULT:!aNULL:!eNULL") != 1)
                return SslCtxPtr { nullptr, SSL_CTX_free };
            // Every session records the fatal alert its peer sends, for `TlsFailureKind::AlertReceived`.
            SSL_CTX_set_info_callback(ctx.get(), recordAlert);
        }
        return ctx;
    }

    /// Makes a client context offer @p names through ALPN.
    /// @return Nothing, or why the names cannot be offered.
    [[nodiscard]] std::expected<void, std::string> offerAlpn(SSL_CTX* ctx,
                                                             std::vector<std::string> const& names)
    {
        auto wire = alpnWireFormat(names);
        if (!wire)
            return std::unexpected(std::move(wire.error()));
        // Zero is success here, unlike every other SSL_CTX setter.
        if (!wire->empty()
            && SSL_CTX_set_alpn_protos(ctx, wire->data(), static_cast<unsigned int>(wire->size())) != 0)
            return std::unexpected("SSL_CTX_set_alpn_protos: " + opensslError());
        return {};
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
        auto const refusal = "cannot read " + std::string { what } + " '" + core::pathToUtf8(path) + "'";

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
                                                                              std::string_view keyPem,
                                                                              TlsOptions const& options)
{
    auto ctx = newCtx(TLS_server_method(), options);
    if (!ctx)
        return std::unexpected("SSL_CTX_new failed: " + opensslError());
    if (auto used = useServerMaterial(ctx.get(), certPem, keyPem); !used)
        return std::unexpected(std::move(used.error()));
    auto alpn = alpnWireFormat(options.alpn);
    if (!alpn)
        return std::unexpected(std::move(alpn.error()));
    auto selection = std::shared_ptr<AlpnWire const> {};
    if (!alpn->empty())
    {
        selection = std::make_shared<AlpnWire const>(std::move(*alpn));
        SSL_CTX_set_alpn_select_cb(ctx.get(), selectAlpn, nullptr);
    }
    return std::make_shared<TlsContext>(
        std::move(ctx), TlsRole::Server, PeerBinding::Fixed, std::move(selection));
}

std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsServerContextFromFiles(
    std::filesystem::path const& certPath, std::filesystem::path const& keyPath, TlsOptions const& options)
{
    // Read here rather than through OpenSSL's own file functions, so a path takes the platform's
    // spelling -- a wide one on Windows -- and the same material function serves both entry points.
    auto const certPem = readFile(certPath, "certificate file");
    if (!certPem)
        return std::unexpected(certPem.error());
    auto const keyPem = readFile(keyPath, "private key file");
    if (!keyPem)
        return std::unexpected(keyPem.error());
    return makeTlsServerContext(*certPem, *keyPem, options);
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

    // Built apart and copied in by both setters: OpenSSL 4 returns the certificate's own subject as const,
    // so it can no longer be written through X509_get_subject_name().
    auto const subject = X509NamePtr { X509_NAME_new(), &X509_NAME_free };
    if (!subject
        || X509_NAME_add_entry_by_txt(subject.get(),
                                      "CN",
                                      MBSTRING_UTF8,
                                      reinterpret_cast<unsigned char const*>(options.commonName.c_str()),
                                      -1,
                                      -1,
                                      0)
               != 1
        || X509_set_subject_name(cert.get(), subject.get()) != 1
        || X509_set_issuer_name(cert.get(), subject.get()) != 1) // self-signed: issuer == subject
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
    SelfSignedOptions const& options, TlsOptions const& tlsOptions)
{
    // Single-sourced through the PEM generator and the PEM context, so a generated certificate is
    // served by exactly the path a named one is.
    auto material = generateSelfSignedCertificate(options);
    if (!material)
        return std::unexpected(material.error());
    return makeTlsServerContext(material->certPem, material->keyPem, tlsOptions);
}

std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsClientContext(
    std::string_view caPem, std::string_view expectedHostName, TlsOptions const& options)
{
    auto ctx = newCtx(TLS_client_method(), options);
    if (!ctx)
        return std::unexpected("SSL_CTX_new failed: " + opensslError());
    if (auto offered = offerAlpn(ctx.get(), options.alpn); !offered)
        return std::unexpected(std::move(offered.error()));

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
            // The same check a per-connection peer gets: an address against the IP entries, a
            // name against the DNS entries by whole labels.
            if (auto const kind = setVerificationName(SSL_CTX_get0_param(ctx.get()), expectedHostName); !kind)
                return std::unexpected(kind.error());
        }
    }

    return std::make_shared<TlsContext>(std::move(ctx), TlsRole::Client);
}

std::expected<TrustAnchors, std::string> trustAnchorsFromPem(std::string_view pem)
{
    auto bio = memBio(pem);
    auto anchors = TrustAnchors {};
    while (true)
    {
        ERR_clear_error();
        auto const cert = X509Ptr { PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free };
        if (!cert)
        {
            // Running out of certificates is "no start line"; anything else is a block that is there
            // and does not parse, which is not an end to skip quietly.
            auto const code = ERR_peek_last_error();
            if (ERR_GET_LIB(code) == ERR_LIB_PEM && ERR_GET_REASON(code) == PEM_R_NO_START_LINE)
                break;
            return std::unexpected("invalid certificate PEM: " + opensslError());
        }
        auto der = derOf(cert.get());
        if (!der)
            return std::unexpected(std::move(der.error()));
        anchors.certificates.push_back(std::move(*der));
    }
    ERR_clear_error();
    if (anchors.certificates.empty())
        return std::unexpected(std::string { "no certificate in the PEM" });
    return anchors;
}

namespace
{
    /// A verifying client context over its own SSL_CTX: the floor, and the ALPN names offered.
    [[nodiscard]] std::expected<SslCtxPtr, std::string> newVerifyingCtx(TlsClientOptions const& options)
    {
        auto ctx =
            newCtx(TLS_client_method(), TlsOptions { .minimumVersion = options.minimumVersion, .alpn = {} });
        if (!ctx)
            return std::unexpected("SSL_CTX_new failed: " + opensslError());
        if (auto offered = offerAlpn(ctx.get(), options.alpn); !offered)
            return std::unexpected(std::move(offered.error()));
        return ctx;
    }

    /// The context over anchors: OpenSSL checks every chain against them, during the handshake.
    [[nodiscard]] std::expected<std::shared_ptr<ITlsContext>, std::string> anchoredClientContext(
        TrustAnchors const& anchors, TlsClientOptions const& options)
    {
        // Nothing trusted would refuse every server; refused here, where it is a configuration error.
        if (anchors.certificates.empty())
            return std::unexpected(
                std::string { "a verifying TLS client context needs at least one trust anchor" });
        auto ctx = newVerifyingCtx(options);
        if (!ctx)
            return std::unexpected(std::move(ctx.error()));

        auto* const store = SSL_CTX_get_cert_store(ctx->get());
        for (auto const& der: anchors.certificates)
        {
            auto const* cursor = reinterpret_cast<unsigned char const*>(der.data());
            auto const cert =
                X509Ptr { d2i_X509(nullptr, &cursor, static_cast<long>(der.size())), X509_free };
            // The whole buffer must be the one certificate: trailing bytes are not an anchor's.
            if (!cert || cursor != reinterpret_cast<unsigned char const*>(der.data()) + der.size())
                return std::unexpected("invalid DER trust anchor: " + opensslError());
            if (X509_STORE_add_cert(store, cert.get()) != 1)
                return std::unexpected("X509_STORE_add_cert: " + opensslError());
        }
        // No option reaches past this: every connection's chain is checked, and a refusal fails it.
        SSL_CTX_set_verify(ctx->get(), SSL_VERIFY_PEER, nullptr);
        return std::make_shared<TlsContext>(std::move(*ctx), TlsRole::Client, PeerBinding::PerConnection);
    }

    /// The context over a verifier: OpenSSL completes the handshake without a chain check of its
    /// own, and every session asks the verifier before it is established.
    [[nodiscard]] std::expected<std::shared_ptr<ITlsContext>, std::string> verifiedClientContext(
        std::shared_ptr<IChainVerifier> verifier, TlsClientOptions const& options)
    {
        if (!verifier)
            return std::unexpected(std::string { "a verifying TLS client context needs a chain verifier" });
        auto ctx = newVerifyingCtx(options);
        if (!ctx)
            return std::unexpected(std::move(ctx.error()));
        // VERIFY_NONE is OpenSSL's check only: the handshake still proves the server holds the leaf's
        // key, and the socket refuses to be established until the verifier accepted the chain.
        SSL_CTX_set_verify(ctx->get(), SSL_VERIFY_NONE, nullptr);
        return std::make_shared<TlsContext>(std::move(*ctx),
                                            TlsRole::Client,
                                            PeerBinding::PerConnection,
                                            nullptr,
                                            std::move(verifier),
                                            options.maxVerificationsInFlight);
    }
} // namespace

std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsClientContext(TrustSource trust,
                                                                              TlsClientOptions const& options)
{
    if (auto const* const anchors = std::get_if<TrustAnchors>(&trust))
        return anchoredClientContext(*anchors, options);
    return verifiedClientContext(std::get<std::shared_ptr<IChainVerifier>>(std::move(trust)), options);
}

ITlsSession const* tlsSessionOf(ISocket const& socket) noexcept
{
    return socket.tlsSession();
}

std::string sshStyleFingerprint(std::array<std::byte, 32> const& sha256)
{
    auto const raw = std::string_view { reinterpret_cast<char const*>(sha256.data()), sha256.size() };
    auto text = base64::encode(raw);
    while (!text.empty() && text.back() == '=')
        text.pop_back();
    return "SHA256:" + text;
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
