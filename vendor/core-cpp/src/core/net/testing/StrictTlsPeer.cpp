// SPDX-License-Identifier: Apache-2.0
#include <core/net/testing/StrictTlsPeer.hpp>

#include <core/net/detail/Alpn.hpp>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace core::net::testing
{

namespace
{
    [[nodiscard]] std::string opensslReason()
    {
        auto const code = ERR_get_error();
        if (code == 0)
            return "no OpenSSL error recorded";
        auto buffer = std::array<char, 256> {};
        ERR_error_string_n(code, buffer.data(), buffer.size());
        return std::string { buffer.data() };
    }

    [[nodiscard]] int clampToInt(std::size_t value) noexcept
    {
        return static_cast<int>(std::min<std::size_t>(value, std::numeric_limits<int>::max()));
    }

    using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
    using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

    [[nodiscard]] X509Ptr parseCertificate(std::string_view pem)
    {
        auto bio = BioPtr { BIO_new_mem_buf(pem.data(), clampToInt(pem.size())), BIO_free };
        return X509Ptr { bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr, X509_free };
    }

    [[nodiscard]] std::string commonNameOf(X509 const* cert)
    {
        if (cert == nullptr)
            return {};
        auto buffer = std::array<char, 256> {};
        auto const length = X509_NAME_get_text_by_NID(
            X509_get_subject_name(cert), NID_commonName, buffer.data(), clampToInt(buffer.size()));
        return length > 0 ? std::string { buffer.data(), static_cast<std::size_t>(length) } : std::string {};
    }

    [[nodiscard]] std::string fingerprintOf(X509 const* cert)
    {
        auto digest = std::array<unsigned char, EVP_MAX_MD_SIZE> {};
        auto length = 0U;
        if (cert == nullptr || X509_digest(cert, EVP_sha256(), digest.data(), &length) != 1)
            return {};
        constexpr auto Hex = std::string_view { "0123456789abcdef" };
        auto out = std::string {};
        for (auto const byte: std::span { digest.data(), length })
        {
            out += Hex[byte >> 4U];
            out += Hex[byte & 0x0FU];
        }
        return out;
    }
} // namespace

struct StrictTlsPeer::State
{
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    BIO* incoming = nullptr; ///< Network -> SSL; owned by `ssl`.
    std::array<std::byte, 16384> staging {};
    bool wireFailed = false;             ///< The wire answered an ERROR, which is not an end of stream.
    detail::AlpnWire alpn;               ///< ALPN names in wire format: a length byte before each.
    std::optional<std::uint8_t> refusal; ///< The alert a server refuses every ClientHello with.
    std::optional<std::uint8_t> warning; ///< The warning a server sends on the ClientHello.

    State() = default;
    State(State const&) = delete;
    State& operator=(State const&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;
    ~State()
    {
        if (ssl != nullptr)
            SSL_free(ssl);
        if (ctx != nullptr)
            SSL_CTX_free(ctx);
    }
};

namespace
{
    /// Finishes a peer's state once its context is configured: an SSL over two memory BIOs.
    /// @param state The peer's state, its context already set.
    /// @param enterRole `SSL_set_accept_state` or `SSL_set_connect_state`.
    [[nodiscard]] std::expected<void, std::string> attachSsl(auto& state, void (*enterRole)(SSL*))
    {
        state.ssl = SSL_new(state.ctx);
        auto* const incoming = BIO_new(BIO_s_mem());
        auto* const outgoing = BIO_new(BIO_s_mem());
        if (state.ssl == nullptr || incoming == nullptr || outgoing == nullptr)
        {
            BIO_free(incoming);
            BIO_free(outgoing);
            return std::unexpected("SSL_new/BIO_new: " + opensslReason());
        }
        SSL_set_bio(state.ssl, incoming, outgoing); // the SSL owns both from here
        state.incoming = incoming;
        enterRole(state.ssl);
        return {};
    }
} // namespace

namespace
{
    /// A server's ALPN choice, through the one the TLS layer makes; @p argument is the peer's own
    /// names in wire format, which its state owns. None in common: `no_application_protocol`.
    int selectAlpn(SSL* /*ssl*/,
                   unsigned char const** out,
                   unsigned char* outLength,
                   unsigned char const* offered,
                   unsigned int offeredLength,
                   void* argument)
    {
        auto const& own = *static_cast<detail::AlpnWire const*>(argument);
        auto const chosen = detail::selectAlpn(own, std::span { offered, offeredLength });
        if (!chosen)
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        *out = chosen->data();
        *outLength = static_cast<unsigned char>(chosen->size());
        return SSL_TLSEXT_ERR_OK;
    }

    /// Warns with the alert the peer was built with, then carries on: what a TLS 1.2 server does
    /// for a name it does not serve (`unrecognized_name`). @p argument points at the alert.
    int warnOnServerName(SSL* /*ssl*/, int* alert, void* argument)
    {
        *alert = *static_cast<std::uint8_t const*>(argument);
        return SSL_TLSEXT_ERR_ALERT_WARNING;
    }

    /// Refuses every ClientHello with the alert the peer was built with; @p argument points at it.
    int refuseClientHello(SSL* /*ssl*/, int* alert, void* argument)
    {
        *alert = *static_cast<std::uint8_t const*>(argument);
        return SSL_CLIENT_HELLO_ERROR;
    }

    /// Caps a context at the newest version @p options allows.
    void capVersion(SSL_CTX* ctx, StrictTlsPeerOptions const& options)
    {
        if (options.maximumVersion == TlsVersion::Tls12)
            SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
    }
} // namespace

StrictTlsPeer::StrictTlsPeer(std::unique_ptr<State> state) noexcept: _state(std::move(state))
{
}

StrictTlsPeer::~StrictTlsPeer() = default;

std::expected<std::unique_ptr<StrictTlsPeer>, std::string> StrictTlsPeer::client(
    StrictTlsPeerOptions const& options)
{
    ERR_clear_error();
    auto state = std::make_unique<State>();
    state->ctx = SSL_CTX_new(TLS_client_method());
    if (state->ctx == nullptr)
        return std::unexpected("SSL_CTX_new: " + opensslReason());
    SSL_CTX_set_verify(state->ctx, SSL_VERIFY_NONE, nullptr);
    capVersion(state->ctx, options);
    auto alpn = detail::alpnWireFormat(options.alpn);
    if (!alpn)
        return std::unexpected(std::string { "an ALPN name must be 1 to 255 bytes" });
    state->alpn = std::move(*alpn);
    // Zero is success here, unlike every other SSL_CTX setter.
    if (!state->alpn.empty()
        && SSL_CTX_set_alpn_protos(
               state->ctx, state->alpn.data(), static_cast<unsigned int>(state->alpn.size()))
               != 0)
        return std::unexpected("SSL_CTX_set_alpn_protos: " + opensslReason());
    if (auto attached = attachSsl(*state, SSL_set_connect_state); !attached)
        return std::unexpected(attached.error());
    return std::unique_ptr<StrictTlsPeer> { new StrictTlsPeer(std::move(state)) };
}

std::expected<std::unique_ptr<StrictTlsPeer>, std::string> StrictTlsPeer::server(
    CertKeyPem const& material, StrictTlsPeerOptions const& options)
{
    ERR_clear_error();
    auto state = std::make_unique<State>();
    state->ctx = SSL_CTX_new(TLS_server_method());
    if (state->ctx == nullptr)
        return std::unexpected("SSL_CTX_new: " + opensslReason());
    auto const cert = parseCertificate(material.certPem);
    auto keyBio =
        BioPtr { BIO_new_mem_buf(material.keyPem.data(), clampToInt(material.keyPem.size())), BIO_free };
    auto const key =
        PKeyPtr { keyBio ? PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr) : nullptr,
                  EVP_PKEY_free };
    if (!cert || !key || SSL_CTX_use_certificate(state->ctx, cert.get()) != 1
        || SSL_CTX_use_PrivateKey(state->ctx, key.get()) != 1)
        return std::unexpected("server material: " + opensslReason());
    capVersion(state->ctx, options);
    auto alpn = detail::alpnWireFormat(options.alpn);
    if (!alpn)
        return std::unexpected(std::string { "an ALPN name must be 1 to 255 bytes" });
    state->alpn = std::move(*alpn);
    if (!state->alpn.empty())
        SSL_CTX_set_alpn_select_cb(state->ctx, selectAlpn, &state->alpn);
    state->refusal = options.refuseWithAlert;
    if (state->refusal)
        SSL_CTX_set_client_hello_cb(state->ctx, refuseClientHello, &*state->refusal);
    state->warning = options.warnWithAlert;
    if (state->warning)
    {
        // The two setter macros, spelled out: the first casts the callback C-style.
        SSL_CTX_callback_ctrl(
            state->ctx, SSL_CTRL_SET_TLSEXT_SERVERNAME_CB, reinterpret_cast<void (*)()>(&warnOnServerName));
        SSL_CTX_ctrl(state->ctx, SSL_CTRL_SET_TLSEXT_SERVERNAME_ARG, 0, &*state->warning);
    }
    if (auto attached = attachSsl(*state, SSL_set_accept_state); !attached)
        return std::unexpected(attached.error());
    return std::unique_ptr<StrictTlsPeer> { new StrictTlsPeer(std::move(state)) };
}

async::Task<bool> StrictTlsPeer::flush(ISocket* wire)
{
    auto* const outgoing = SSL_get_wbio(_state->ssl);
    while (true)
    {
        auto const n = BIO_read(outgoing, _state->staging.data(), clampToInt(_state->staging.size()));
        if (n <= 0)
            co_return true;
        auto const written = co_await wire->write(
            std::span<std::byte const> { _state->staging.data(), static_cast<std::size_t>(n) });
        if (!written)
            co_return false;
    }
}

async::Task<std::size_t> StrictTlsPeer::feed(ISocket* wire)
{
    auto const n = co_await wire->read(_state->staging);
    if (!n)
    {
        // A reset is not an end: kept apart, so `readToEnd` answers `Failed` and not `Truncated`.
        _state->wireFailed = true;
        co_return std::size_t { 0 };
    }
    if (*n == 0)
    {
        // The strict half: a memory BIO answers "retry" when empty unless it is told the stream has
        // ended, and OpenSSL can only call a missing close_notify a truncation once it is.
        BIO_set_mem_eof_return(_state->incoming, 0);
        co_return std::size_t { 0 };
    }
    BIO_write(_state->incoming, _state->staging.data(), clampToInt(*n));
    co_return *n;
}

async::Task<std::expected<void, std::string>> StrictTlsPeer::handshake(ISocket* wire)
{
    while (true)
    {
        ERR_clear_error();
        auto const result = SSL_do_handshake(_state->ssl);
        auto const error = SSL_get_error(_state->ssl, result);
        auto const reason = result == 1 ? std::string {} : opensslReason();
        if (!co_await flush(wire))
            co_return std::unexpected(std::string { "the wire refused a handshake flight" });
        if (result == 1)
            co_return std::expected<void, std::string> {};
        if (error == SSL_ERROR_WANT_READ)
        {
            if (co_await feed(wire) == 0)
                co_return std::unexpected(std::string { "the wire ended during the handshake" });
            continue;
        }
        if (error != SSL_ERROR_WANT_WRITE)
            co_return std::unexpected("SSL_do_handshake: " + reason);
    }
}

async::Task<StreamEnd> StrictTlsPeer::readToEnd(ISocket* wire, std::string* out)
{
    auto buffer = std::array<char, 4096> {};
    auto sawEof = false;
    while (true)
    {
        ERR_clear_error();
        auto const n = SSL_read(_state->ssl, buffer.data(), clampToInt(buffer.size()));
        if (n > 0)
        {
            out->append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        switch (SSL_get_error(_state->ssl, n))
        {
            case SSL_ERROR_ZERO_RETURN: co_return StreamEnd::CloseNotify;
            case SSL_ERROR_WANT_READ:
                if (sawEof || !co_await flush(wire))
                    co_return StreamEnd::Failed;
                sawEof = co_await feed(wire) == 0;
                if (_state->wireFailed)
                    co_return StreamEnd::Failed;
                break;
            case SSL_ERROR_WANT_WRITE:
                if (!co_await flush(wire))
                    co_return StreamEnd::Failed;
                break;
            // OpenSSL 3 reports a transport EOF before close_notify as SSL_ERROR_SSL ("unexpected
            // eof while reading"); 1.1.1 as SSL_ERROR_SYSCALL with an empty queue. Either way, only
            // after this peer told it the stream had ended.
            default: co_return sawEof ? StreamEnd::Truncated : StreamEnd::Failed;
        }
    }
}

async::Task<std::expected<std::string, std::string>> StrictTlsPeer::readAtLeast(ISocket* wire,
                                                                                std::size_t count)
{
    auto out = std::string {};
    auto buffer = std::array<char, 4096> {};
    while (out.size() < count)
    {
        ERR_clear_error();
        auto const n = SSL_read(_state->ssl, buffer.data(), clampToInt(buffer.size()));
        if (n > 0)
        {
            out.append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        auto const error = SSL_get_error(_state->ssl, n);
        if (error != SSL_ERROR_WANT_READ)
            co_return std::unexpected("SSL_read ended the stream after " + std::to_string(out.size())
                                      + " bytes: " + opensslReason());
        if (!co_await flush(wire) || co_await feed(wire) == 0)
            co_return std::unexpected("the wire ended after " + std::to_string(out.size()) + " bytes");
    }
    co_return out;
}

async::Task<bool> StrictTlsPeer::write(ISocket* wire, std::string text)
{
    ERR_clear_error();
    if (SSL_write(_state->ssl, text.data(), clampToInt(text.size())) != clampToInt(text.size()))
        co_return false;
    co_return co_await flush(wire);
}

async::Task<bool> StrictTlsPeer::sendCloseNotify(ISocket* wire)
{
    ERR_clear_error();
    if (SSL_shutdown(_state->ssl) < 0)
        co_return false;
    co_return co_await flush(wire);
}

bool StrictTlsPeer::handshakeFinished() const noexcept
{
    return SSL_is_init_finished(_state->ssl) == 1;
}

std::string StrictTlsPeer::peerCommonName() const
{
    return commonNameOf(SSL_get0_peer_certificate(_state->ssl));
}

std::string StrictTlsPeer::peerFingerprint() const
{
    return fingerprintOf(SSL_get0_peer_certificate(_state->ssl));
}

int StrictTlsPeer::peerChainLength() const
{
    auto const* const chain = SSL_get_peer_cert_chain(_state->ssl);
    return chain != nullptr ? sk_X509_num(chain) : 0;
}

std::string StrictTlsPeer::receivedServerName() const
{
    auto const* const name = SSL_get_servername(_state->ssl, TLSEXT_NAMETYPE_host_name);
    return name != nullptr ? std::string { name } : std::string {};
}

std::string StrictTlsPeer::negotiatedProtocol() const
{
    auto const* selected = static_cast<unsigned char const*>(nullptr);
    auto length = 0U;
    SSL_get0_alpn_selected(_state->ssl, &selected, &length);
    return selected != nullptr ? std::string { reinterpret_cast<char const*>(selected), length }
                               : std::string {};
}

std::expected<CertKeyPem, std::string> expiredCopyOf(CertKeyPem const& material)
{
    ERR_clear_error();
    auto const cert = parseCertificate(material.certPem);
    auto keyBio =
        BioPtr { BIO_new_mem_buf(material.keyPem.data(), clampToInt(material.keyPem.size())), BIO_free };
    auto const key =
        PKeyPtr { keyBio ? PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr) : nullptr,
                  EVP_PKEY_free };
    if (!cert || !key)
        return std::unexpected("material: " + opensslReason());
    constexpr auto Day = long { 24L * 60 * 60 };
    if (X509_gmtime_adj(X509_getm_notBefore(cert.get()), -2 * Day) == nullptr
        || X509_gmtime_adj(X509_getm_notAfter(cert.get()), -Day) == nullptr
        || X509_sign(cert.get(), key.get(), EVP_sha256()) == 0)
        return std::unexpected("re-signing: " + opensslReason());
    auto out = BioPtr { BIO_new(BIO_s_mem()), BIO_free };
    if (!out || PEM_write_bio_X509(out.get(), cert.get()) != 1)
        return std::unexpected("PEM_write_bio_X509: " + opensslReason());
    auto* data = static_cast<char const*>(nullptr);
    auto const length = BIO_get_mem_data(out.get(), &data);
    return CertKeyPem { .certPem = std::string { data, static_cast<std::size_t>(length) },
                        .keyPem = material.keyPem };
}

std::string certificateCommonName(std::string_view certPem)
{
    return commonNameOf(parseCertificate(certPem).get());
}

bool certificateMatchesHost(std::string_view certPem, std::string_view host)
{
    auto const cert = parseCertificate(certPem);
    return cert && X509_check_host(cert.get(), host.data(), host.size(), 0, nullptr) == 1;
}

bool certificateMatchesIp(std::string_view certPem, std::string_view address)
{
    auto const cert = parseCertificate(certPem);
    return cert && X509_check_ip_asc(cert.get(), std::string { address }.c_str(), 0) == 1;
}

void leaveStaleOpensslError()
{
    ERR_raise(ERR_LIB_SSL, SSL_R_BAD_LENGTH);
}

} // namespace core::net::testing
