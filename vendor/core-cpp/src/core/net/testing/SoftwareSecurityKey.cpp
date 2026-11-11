// SPDX-License-Identifier: Apache-2.0
#include <core/net/testing/SoftwareSecurityKey.hpp>

#include <core/Base64.hpp>
#include <core/Sha256.hpp>
#include <core/net/detail/SshWire.hpp>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace core::net::testing
{

namespace
{
    using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using PKeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    using EcdsaSigPtr = std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)>;

    /// PROTOCOL.u2f's user-presence bit.
    constexpr auto UserPresenceFlag = std::uint8_t { 0b0000'0001 };

    [[nodiscard]] std::string opensslReason()
    {
        auto buffer = std::array<char, 256> {};
        ERR_error_string_n(ERR_get_error(), buffer.data(), buffer.size());
        return std::string { buffer.data() };
    }

    /// A fresh key pair of the OpenSSL algorithm @p name, on @p group if it has one.
    [[nodiscard]] PKeyPtr generateKey(char const* name, char const* group)
    {
        auto const ctx = PKeyCtxPtr { EVP_PKEY_CTX_new_from_name(nullptr, name, nullptr), EVP_PKEY_CTX_free };
        EVP_PKEY* raw = nullptr;
        if (!ctx || EVP_PKEY_keygen_init(ctx.get()) != 1
            || (group != nullptr && EVP_PKEY_CTX_set_group_name(ctx.get(), group) != 1)
            || EVP_PKEY_generate(ctx.get(), &raw) != 1)
            return { nullptr, EVP_PKEY_free };
        return { raw, EVP_PKEY_free };
    }

    /// The big-endian magnitude of @p value as an SSH mpint: a zero sign byte in front of a set top bit.
    [[nodiscard]] std::vector<std::byte> mpintOf(BIGNUM const* value)
    {
        auto magnitude = std::vector<std::byte>(static_cast<std::size_t>(BN_num_bytes(value)));
        BN_bn2bin(value, reinterpret_cast<unsigned char*>(magnitude.data()));
        if (!magnitude.empty() && (std::to_integer<std::uint8_t>(magnitude[0]) & 0b1000'0000U) != 0)
            magnitude.insert(magnitude.begin(), std::byte { 0 });
        return magnitude;
    }
} // namespace

struct SoftwareSecurityKey::State
{
    SshKeyType type = SshKeyType::SkEd25519;
    std::string application;
    PKeyPtr key { nullptr, EVP_PKEY_free };
    std::vector<std::byte> publicBlob;
};

SoftwareSecurityKey::SoftwareSecurityKey(std::unique_ptr<State> state) noexcept: _state(std::move(state))
{
}

SoftwareSecurityKey::~SoftwareSecurityKey() = default;

std::expected<std::unique_ptr<SoftwareSecurityKey>, std::string> SoftwareSecurityKey::generate(
    SshKeyType type, std::string application)
{
    ERR_clear_error();
    auto state = std::make_unique<State>();
    state->type = type;
    state->application = std::move(application);
    auto& blob = state->publicBlob;
    detail::appendSshString(blob, sshKeyTypeName(type));
    switch (type)
    {
        case SshKeyType::SkEd25519: {
            state->key = generateKey("ED25519", nullptr);
            auto publicKey = std::array<unsigned char, 32> {};
            auto length = publicKey.size();
            if (!state->key || EVP_PKEY_get_raw_public_key(state->key.get(), publicKey.data(), &length) != 1)
                return std::unexpected("Ed25519 key: " + opensslReason());
            detail::appendSshString(blob, std::as_bytes(std::span { publicKey }.first(length)));
            break;
        }
        case SshKeyType::SkEcdsaP256: {
            state->key = generateKey("EC", "P-256");
            auto point = std::array<unsigned char, 65> {};
            auto length = std::size_t { 0 };
            if (!state->key
                || EVP_PKEY_get_octet_string_param(
                       state->key.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size(), &length)
                       != 1)
                return std::unexpected("P-256 key: " + opensslReason());
            detail::appendSshString(blob, std::string_view { "nistp256" });
            detail::appendSshString(blob, std::as_bytes(std::span { point }.first(length)));
            break;
        }
        case SshKeyType::Ed25519:
        case SshKeyType::EcdsaP256: return std::unexpected("not a security-key type");
    }
    detail::appendSshString(blob, state->application);
    return std::unique_ptr<SoftwareSecurityKey> { new SoftwareSecurityKey(std::move(state)) };
}

std::string SoftwareSecurityKey::publicKeyLine(std::string_view comment) const
{
    auto const blob = std::string_view { reinterpret_cast<char const*>(_state->publicBlob.data()),
                                         _state->publicBlob.size() };
    return std::string { sshKeyTypeName(_state->type) } + ' ' + base64::encode(blob) + ' '
           + std::string { comment };
}

std::expected<std::vector<std::byte>, std::string> SoftwareSecurityKey::sign(std::span<std::byte const> data,
                                                                             UserPresence presence,
                                                                             std::uint32_t counter) const
{
    ERR_clear_error();
    auto const flags = presence == UserPresence::Present ? UserPresenceFlag : std::uint8_t { 0 };

    // PROTOCOL.u2f: SHA-256(application) ‖ flags ‖ counter ‖ SHA-256(data).
    auto signedBytes = std::vector<std::byte> {};
    auto const applicationHash = Sha256::hash(std::string_view { _state->application });
    signedBytes.insert(signedBytes.end(), applicationHash.begin(), applicationHash.end());
    detail::appendSshByte(signedBytes, flags);
    detail::appendSshUint32(signedBytes, counter);
    auto const dataHash = Sha256::hash(data);
    signedBytes.insert(signedBytes.end(), dataHash.begin(), dataHash.end());

    auto const isEcdsa = _state->type == SshKeyType::SkEcdsaP256;
    auto const ctx = MdCtxPtr { EVP_MD_CTX_new(), EVP_MD_CTX_free };
    auto const* const tbs = reinterpret_cast<unsigned char const*>(signedBytes.data());
    auto length = std::size_t { 0 };
    if (!ctx
        || EVP_DigestSignInit(
               ctx.get(), nullptr, isEcdsa ? EVP_sha256() : nullptr, nullptr, _state->key.get())
               != 1
        || EVP_DigestSign(ctx.get(), nullptr, &length, tbs, signedBytes.size()) != 1)
        return std::unexpected("sign: " + opensslReason());
    auto produced = std::vector<unsigned char>(length);
    if (EVP_DigestSign(ctx.get(), produced.data(), &length, tbs, signedBytes.size()) != 1)
        return std::unexpected("sign: " + opensslReason());
    produced.resize(length);

    auto value = std::vector<std::byte> {};
    if (isEcdsa)
    {
        // The DER ECDSA-Sig-Value OpenSSL makes, as RFC 5656's mpint r ‖ mpint s.
        auto const* cursor = produced.data();
        auto const parsed = EcdsaSigPtr { d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(produced.size())),
                                          ECDSA_SIG_free };
        if (!parsed)
            return std::unexpected("DER signature: " + opensslReason());
        detail::appendSshString(value, mpintOf(ECDSA_SIG_get0_r(parsed.get())));
        detail::appendSshString(value, mpintOf(ECDSA_SIG_get0_s(parsed.get())));
    }
    else
    {
        auto const raw = std::as_bytes(std::span { produced });
        value.assign(raw.begin(), raw.end());
    }

    auto signature = std::vector<std::byte> {};
    detail::appendSshString(signature, sshKeyTypeName(_state->type));
    detail::appendSshString(signature, value);
    detail::appendSshByte(signature, flags);
    detail::appendSshUint32(signature, counter);
    return signature;
}

} // namespace core::net::testing
