// SPDX-License-Identifier: Apache-2.0
#include <net/Tls.h>

#include <crispy/utils.h>

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace net
{

namespace
{
    /// The most recent OpenSSL error, for diagnostics.
    [[nodiscard]] std::string opensslError()
    {
        auto const code = ERR_get_error();
        if (code == 0)
            return "unknown TLS error";
        auto buffer = std::array<char, 256> {};
        ERR_error_string_n(code, buffer.data(), buffer.size());
        return std::string { buffer.data() };
    }

    using SslCtxPtr = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
    using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
    using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

    [[nodiscard]] BioPtr memBio(std::string_view pem)
    {
        return BioPtr { BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free };
    }

    /// A TLS layer over an inner `ISocket`. OpenSSL talks to two memory BIOs; this
    /// pumps ciphertext between those BIOs and the inner transport across
    /// coroutine suspensions, so the handshake and records ride the reactor with
    /// no blocking. The handshake runs lazily on first read/write.
    class TlsSocket final: public ISocket
    {
      public:
        TlsSocket(std::unique_ptr<ISocket> inner, SSL* ssl):
            _inner(std::move(inner)), _ssl(ssl), _rbio(SSL_get_rbio(ssl)), _wbio(SSL_get_wbio(ssl))
        {
        }

        ~TlsSocket() override
        {
            if (_ssl != nullptr)
                SSL_free(_ssl); // frees the attached BIOs too
        }

        TlsSocket(TlsSocket const&) = delete;
        TlsSocket& operator=(TlsSocket const&) = delete;
        TlsSocket(TlsSocket&&) = delete;
        TlsSocket& operator=(TlsSocket&&) = delete;

        coro::Task<IoResult> read(std::span<std::byte> buffer) override
        {
            if (auto const handshaken = co_await handshake(); !handshaken)
                co_return std::unexpected(handshaken.error());

            while (true)
            {
                auto const n = SSL_read(_ssl, buffer.data(), static_cast<int>(buffer.size()));
                if (n > 0)
                    co_return static_cast<std::size_t>(n);
                switch (SSL_get_error(_ssl, n))
                {
                    case SSL_ERROR_ZERO_RETURN: co_return std::size_t { 0 }; // peer sent close_notify
                    case SSL_ERROR_WANT_WRITE:
                        if (auto const flushed = co_await flushOut(); !flushed)
                            co_return std::unexpected(flushed.error());
                        break;
                    case SSL_ERROR_WANT_READ: {
                        if (auto const flushed = co_await flushOut(); !flushed)
                            co_return std::unexpected(flushed.error());
                        auto const fed = co_await feedIn();
                        if (!fed)
                            co_return std::unexpected(fed.error());
                        if (*fed == 0)
                            co_return std::size_t { 0 }; // inner EOF mid-stream
                        break;
                    }
                    default:
                        co_return std::unexpected(
                            makeNetError(NetErrorCode::Other, 0, "SSL_read: " + opensslError()));
                }
            }
        }

        coro::Task<IoResult> write(std::span<std::byte const> buffer) override
        {
            if (auto const handshaken = co_await handshake(); !handshaken)
                co_return std::unexpected(handshaken.error());

            auto total = std::size_t { 0 };
            while (total < buffer.size())
            {
                auto const n =
                    SSL_write(_ssl, buffer.data() + total, static_cast<int>(buffer.size() - total));
                if (n > 0)
                {
                    total += static_cast<std::size_t>(n);
                    if (auto const flushed = co_await flushOut(); !flushed)
                        co_return std::unexpected(flushed.error());
                    continue;
                }
                switch (SSL_get_error(_ssl, n))
                {
                    case SSL_ERROR_WANT_WRITE:
                        if (auto const flushed = co_await flushOut(); !flushed)
                            co_return std::unexpected(flushed.error());
                        break;
                    case SSL_ERROR_WANT_READ: {
                        if (auto const flushed = co_await flushOut(); !flushed)
                            co_return std::unexpected(flushed.error());
                        auto const fed = co_await feedIn();
                        if (!fed)
                            co_return std::unexpected(fed.error());
                        if (*fed == 0)
                            co_return std::unexpected(
                                makeNetError(NetErrorCode::Eof, 0, "SSL_write: peer closed"));
                        break;
                    }
                    default:
                        co_return std::unexpected(
                            makeNetError(NetErrorCode::Other, 0, "SSL_write: " + opensslError()));
                }
            }
            co_return total;
        }

        [[nodiscard]] std::string peerAddress() const override { return _inner->peerAddress(); }
        void close() noexcept override { _inner->close(); }
        [[nodiscard]] bool isClosed() const noexcept override { return _inner->isClosed(); }

      private:
        /// Suspends a caller until the in-flight handshake, driven by another
        /// coroutine, completes. OpenSSL is not reentrant, so concurrent read() and
        /// write() (as AttachClient does) must NOT both call SSL_do_handshake — the
        /// single-loop path serialized them by timing; a real two-reactor connection
        /// interleaves them and corrupts the handshake without this gate.
        struct HandshakeGate
        {
            TlsSocket* self;
            [[nodiscard]] bool await_ready() const noexcept { return !self->_handshaking; }
            void await_suspend(std::coroutine_handle<> handle) const
            {
                self->_handshakeWaiters.push_back(handle);
            }
            void await_resume() const noexcept {}
        };

        /// Drives the handshake to completion (idempotent), or — if another coroutine
        /// is already driving it — waits for that to finish. Both roles pump BIOs.
        coro::Task<std::expected<void, NetError>> handshake()
        {
            if (_handshakeDone)
                co_return std::expected<void, NetError> {};
            if (_handshakeError)
                co_return std::unexpected(*_handshakeError);
            if (_handshaking)
            {
                co_await HandshakeGate { this };
                if (_handshakeError)
                    co_return std::unexpected(*_handshakeError);
                co_return std::expected<void, NetError> {};
            }

            _handshaking = true;
            auto outcome = std::expected<void, NetError> {};
            // If a co_await below throws OperationCancelled (whenAny sibling won
            // or loop shutdown), reset _handshaking so future I/O on this socket
            // can re-enter the handshake path instead of parking forever.
            auto const resetHandshaking = crispy::finally([this]() noexcept { _handshaking = false; });
            while (true)
            {
                auto const result = SSL_do_handshake(_ssl);
                auto const err = SSL_get_error(_ssl, result);
                // Always flush whatever the last step queued (ClientHello, the
                // server's flight, Finished, …) before deciding what to await.
                if (auto const flushed = co_await flushOut(); !flushed)
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
                    auto const fed = co_await feedIn();
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
                        makeNetError(NetErrorCode::Other, 0, "TLS handshake: " + opensslError()));
                    break;
                }
            }

            if (!outcome)
                _handshakeError = outcome.error(); // parked waiters observe the same failure
            // Resume everyone who parked on us; they re-check the flags and return.
            for (auto const handle: std::exchange(_handshakeWaiters, {}))
                if (handle && !handle.done())
                    handle.resume();
            co_return outcome;
        }

        /// Drains OpenSSL's outgoing BIO to the inner socket.
        coro::Task<std::expected<void, NetError>> flushOut()
        {
            auto chunk = std::array<std::byte, 16384> {};
            while (true)
            {
                auto const pending = BIO_ctrl_pending(_wbio);
                if (pending == 0)
                    co_return std::expected<void, NetError> {};
                auto const take = std::min<std::size_t>(chunk.size(), pending);
                auto const n = BIO_read(_wbio, chunk.data(), static_cast<int>(take));
                if (n <= 0)
                    co_return std::expected<void, NetError> {};
                auto const written = co_await _inner->write(
                    std::span<std::byte const> { chunk.data(), static_cast<std::size_t>(n) });
                if (!written)
                    co_return std::unexpected(written.error());
            }
        }

        /// Reads ciphertext from the inner socket into OpenSSL's incoming BIO.
        /// @return Bytes fed (0 = a clean inner EOF).
        coro::Task<std::expected<std::size_t, NetError>> feedIn()
        {
            auto chunk = std::array<std::byte, 16384> {};
            auto const n = co_await _inner->read(chunk);
            if (!n)
                co_return std::unexpected(n.error());
            if (*n == 0)
                co_return std::size_t { 0 };
            auto const written = BIO_write(_rbio, chunk.data(), static_cast<int>(*n));
            if (written <= 0)
                co_return std::unexpected(
                    makeNetError(NetErrorCode::Other, 0, "TLS feedIn: BIO_write failed"));
            co_return *n;
        }

        std::unique_ptr<ISocket> _inner;
        SSL* _ssl;
        BIO* _rbio; ///< Network → SSL (owned by _ssl).
        BIO* _wbio; ///< SSL → network (owned by _ssl).
        bool _handshakeDone = false;
        bool _handshaking = false;               ///< A coroutine is currently driving the handshake.
        std::optional<NetError> _handshakeError; ///< Set once the handshake fails (sticky).
        std::vector<std::coroutine_handle<>> _handshakeWaiters; ///< Parked on the in-flight handshake.
    };

    /// The DI context: a configured SSL_CTX plus its handshake role.
    class TlsContext final: public ITlsContext
    {
      public:
        TlsContext(SslCtxPtr ctx, bool server): _ctx(std::move(ctx)), _server(server) {}

        std::unique_ptr<ISocket> wrap(std::unique_ptr<ISocket> inner) override
        {
            auto* ssl = SSL_new(_ctx.get());
            if (ssl == nullptr)
                return nullptr;
            // Memory BIOs bridge OpenSSL and the coroutine transport; SSL_set_bio
            // takes ownership of both, so SSL_free later releases them.
            SSL_set_bio(ssl, BIO_new(BIO_s_mem()), BIO_new(BIO_s_mem()));
            if (_server)
                SSL_set_accept_state(ssl);
            else
                SSL_set_connect_state(ssl);
            return std::make_unique<TlsSocket>(std::move(inner), ssl);
        }

      private:
        SslCtxPtr _ctx;
        bool _server;
    };

    [[nodiscard]] SslCtxPtr newCtx(SSL_METHOD const* method)
    {
        auto ctx = SslCtxPtr { SSL_CTX_new(method), SSL_CTX_free };
        if (ctx)
            SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
        return ctx;
    }
} // namespace

std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsServerContext(std::string_view certPem,
                                                                              std::string_view keyPem)
{
    auto ctx = newCtx(TLS_server_method());
    if (!ctx)
        return std::unexpected("SSL_CTX_new failed: " + opensslError());

    auto certBio = memBio(certPem);
    auto const cert = X509Ptr { PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), X509_free };
    if (!cert)
        return std::unexpected("invalid certificate PEM: " + opensslError());
    if (SSL_CTX_use_certificate(ctx.get(), cert.get()) != 1)
        return std::unexpected("SSL_CTX_use_certificate: " + opensslError());

    auto keyBio = memBio(keyPem);
    auto const key =
        PKeyPtr { PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free };
    if (!key)
        return std::unexpected("invalid private key PEM: " + opensslError());
    if (SSL_CTX_use_PrivateKey(ctx.get(), key.get()) != 1)
        return std::unexpected("SSL_CTX_use_PrivateKey: " + opensslError());
    if (SSL_CTX_check_private_key(ctx.get()) != 1)
        return std::unexpected("certificate and private key do not match");

    return std::make_shared<TlsContext>(std::move(ctx), /*server=*/true);
}

namespace
{
    /// Reads a BIO's whole content out as a string (for PEM export).
    [[nodiscard]] std::string bioToString(BIO* bio)
    {
        auto* data = static_cast<char const*>(nullptr);
        auto const length = BIO_get_mem_data(bio, &data);
        return std::string { data, static_cast<std::size_t>(length) };
    }
} // namespace

std::expected<CertKeyPem, std::string> generateSelfSignedCertificate(std::string_view commonName)
{
    auto const key = PKeyPtr { EVP_RSA_gen(2048), EVP_PKEY_free };
    if (!key)
        return std::unexpected("key generation failed: " + opensslError());

    auto const cert = X509Ptr { X509_new(), X509_free };
    if (!cert)
        return std::unexpected("X509_new failed: " + opensslError());
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60 * 24 * 3650); // ~10 years
    X509_set_pubkey(cert.get(), key.get());
    auto* name = X509_get_subject_name(cert.get());
    auto const cn = std::string { commonName };
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC, reinterpret_cast<unsigned char const*>(cn.c_str()), -1, -1, 0);
    X509_set_issuer_name(cert.get(), name); // self-signed: issuer == subject
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

std::expected<std::shared_ptr<ITlsContext>, std::string> makeSelfSignedServerContext()
{
    // Single-source the self-signed material through the PEM generator, then reuse
    // the same file-backed path the daemon takes with --tls-cert/--tls-key.
    auto material = generateSelfSignedCertificate();
    if (!material)
        return std::unexpected(material.error());
    return makeTlsServerContext(material->certPem, material->keyPem);
}

std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsClientContext(std::string_view caPem)
{
    auto ctx = newCtx(TLS_client_method());
    if (!ctx)
        return std::unexpected("SSL_CTX_new failed: " + opensslError());

    if (caPem.empty())
    {
        // TOFU: the preshared token authenticates; TLS only encrypts. The caller
        // may still pin the daemon's certificate fingerprint out of band.
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
    }
    else
    {
        auto caBio = memBio(caPem);
        auto const ca = X509Ptr { PEM_read_bio_X509(caBio.get(), nullptr, nullptr, nullptr), X509_free };
        if (!ca)
            return std::unexpected("invalid CA PEM: " + opensslError());
        if (X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx.get()), ca.get()) != 1)
            return std::unexpected("X509_STORE_add_cert: " + opensslError());
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
    }

    return std::make_shared<TlsContext>(std::move(ctx), /*server=*/false);
}

} // namespace net
