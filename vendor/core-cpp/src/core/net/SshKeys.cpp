// SPDX-License-Identifier: Apache-2.0
#include <core/net/SshKeys.hpp>

#include <core/Base64.hpp>
#include <core/Sha256.hpp>
#include <core/net/Tls.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/detail/SshWire.hpp>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace core::net
{

namespace
{
    using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using PKeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    using EcdsaSigPtr = std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)>;
    using BignumPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;

    using Bytes = std::span<std::byte const>;

    /// The curve and signature scheme under a key type.
    enum class KeyMaterial : std::uint8_t
    {
        Ed25519 = 0, ///< RFC 8709: a 32-byte public key, a 64-byte signature over the data itself.
        P256,        ///< RFC 5656: an uncompressed point, an ECDSA-SHA-256 signature of mpints r, s.
    };

    /// Whether a key lives on a FIDO authenticator, which changes both the blob and what is signed.
    enum class Authenticator : std::uint8_t
    {
        None = 0, ///< An ordinary key.
        Fido,     ///< PROTOCOL.u2f: an application string in the key, flags and counter in the signature.
    };

    /// One supported key type. The signature algorithm name equals the key type name for every one.
    struct KeyAlgorithm
    {
        SshKeyType type;
        std::string_view name;
        KeyMaterial material;
        Authenticator authenticator;
    };

    constexpr auto KeyAlgorithms = std::array {
        KeyAlgorithm { SshKeyType::Ed25519, "ssh-ed25519", KeyMaterial::Ed25519, Authenticator::None },
        KeyAlgorithm { SshKeyType::EcdsaP256, "ecdsa-sha2-nistp256", KeyMaterial::P256, Authenticator::None },
        KeyAlgorithm {
            SshKeyType::SkEd25519, "sk-ssh-ed25519@openssh.com", KeyMaterial::Ed25519, Authenticator::Fido },
        KeyAlgorithm { SshKeyType::SkEcdsaP256,
                       "sk-ecdsa-sha2-nistp256@openssh.com",
                       KeyMaterial::P256,
                       Authenticator::Fido },
    };

    /// The SSH name of the P-256 curve, the middle field of every P-256 key blob.
    constexpr auto P256CurveName = std::string_view { "nistp256" };
    constexpr auto Ed25519KeySize = std::size_t { 32 };
    constexpr auto Ed25519SignatureSize = std::size_t { 64 };
    constexpr auto P256PointSize = std::size_t { 65 }; // 0x04 ‖ X ‖ Y
    constexpr auto P256ScalarSize = std::size_t { 32 };

    /// PROTOCOL.u2f's user-presence bit in the signature flags.
    constexpr auto UserPresenceFlag = std::uint8_t { 0b0000'0001 };

    /// PROTOCOL.sshsig.
    constexpr auto SshsigMagic = std::string_view { "SSHSIG" };
    constexpr auto SshsigVersion = std::uint32_t { 1 };
    constexpr auto ArmorBegin = std::string_view { "-----BEGIN SSH SIGNATURE-----" };
    constexpr auto ArmorEnd = std::string_view { "-----END SSH SIGNATURE-----" };
    constexpr auto Whitespace = std::string_view { " \t\r\n" };

    /// The message hashes an SSHSIG may name, the first being the one this module produces. Only
    /// sha512: it is what `ssh-keygen -Y sign` and this module make, and the callers' protocol fixes it.
    struct SshsigHash
    {
        std::string_view name;
        EVP_MD const* (*algorithm)();
    };
    constexpr auto SshsigHashes = std::array {
        SshsigHash { "sha512", EVP_sha512 },
    };
    constexpr auto ProducedHash = SshsigHashes[0];

    [[nodiscard]] Bytes bytesOf(std::string_view text) noexcept
    {
        return std::as_bytes(std::span { text.data(), text.size() });
    }

    [[nodiscard]] bool equalBytes(Bytes lhs, Bytes rhs) noexcept
    {
        return std::ranges::equal(lhs, rhs);
    }

    [[nodiscard]] std::string_view trim(std::string_view text) noexcept
    {
        auto const first = text.find_first_not_of(Whitespace);
        if (first == std::string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(Whitespace) - first + 1);
    }

    /// Splits off the first whitespace-delimited token of @p text (already trimmed on the left).
    /// @return The token and the rest after it, the rest trimmed.
    [[nodiscard]] std::pair<std::string_view, std::string_view> splitToken(std::string_view text) noexcept
    {
        auto const end = text.find_first_of(Whitespace);
        if (end == std::string_view::npos)
            return { text, {} };
        return { text.substr(0, end), trim(text.substr(end)) };
    }

    [[nodiscard]] KeyAlgorithm const* algorithmNamed(Bytes name) noexcept
    {
        auto const found = std::ranges::find_if(KeyAlgorithms, [name](KeyAlgorithm const& algorithm) {
            return equalBytes(bytesOf(algorithm.name), name);
        });
        return found == KeyAlgorithms.end() ? nullptr : &*found;
    }

    /// Base64 exactly as `ssh-keygen` writes it: the standard alphabet, padded, and canonical --
    /// padding bits zero -- so one key has one spelling.
    [[nodiscard]] std::optional<std::vector<std::byte>> decodeBase64Strictly(std::string_view text)
    {
        if (text.empty() || text.size() % 4 != 0)
            return std::nullopt;
        auto const digits = text.substr(0, text.find_last_not_of('=') + 1);
        if (text.size() - digits.size() > 2)
            return std::nullopt;
        auto const isDigit = [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+'
                   || c == '/';
        };
        if (!std::ranges::all_of(digits, isDigit))
            return std::nullopt;
        auto const decoded = base64::decode(text);
        if (base64::encode(decoded) != text)
            return std::nullopt;
        auto const view = bytesOf(decoded);
        return std::vector<std::byte> { view.begin(), view.end() };
    }

    /// A digest with OpenSSL.
    [[nodiscard]] std::vector<std::byte> digestOf(EVP_MD const* algorithm, Bytes input)
    {
        auto out = std::vector<std::byte>(EVP_MAX_MD_SIZE);
        auto length = 0U;
        if (EVP_Digest(input.data(),
                       input.size(),
                       reinterpret_cast<unsigned char*>(out.data()),
                       &length,
                       algorithm,
                       nullptr)
            != 1)
            return {};
        out.resize(length);
        return out;
    }

    /// An EVP key for a P-256 point, or null if the bytes are not a point on the curve.
    [[nodiscard]] PKeyPtr p256Key(Bytes point)
    {
        auto pointCopy = std::array<unsigned char, P256PointSize> {};
        std::ranges::transform(
            point, pointCopy.begin(), [](std::byte b) { return std::to_integer<unsigned char>(b); });
        auto groupName = std::array<char, 11> { 'p', 'r', 'i', 'm', 'e', '2', '5', '6', 'v', '1', '\0' };
        auto params = std::array {
            OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, groupName.data(), 0),
            OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, pointCopy.data(), pointCopy.size()),
            OSSL_PARAM_construct_end(),
        };
        auto const ctx = PKeyCtxPtr { EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr), EVP_PKEY_CTX_free };
        EVP_PKEY* raw = nullptr;
        if (!ctx || EVP_PKEY_fromdata_init(ctx.get()) != 1
            || EVP_PKEY_fromdata(ctx.get(), &raw, EVP_PKEY_PUBLIC_KEY, params.data()) != 1)
            return { nullptr, EVP_PKEY_free };
        auto key = PKeyPtr { raw, EVP_PKEY_free };
        // fromdata decodes the point; the public check is what proves it lies on the curve.
        auto const check =
            PKeyCtxPtr { EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr), EVP_PKEY_CTX_free };
        if (!check || EVP_PKEY_public_check(check.get()) != 1)
            return { nullptr, EVP_PKEY_free };
        return key;
    }

    /// A key blob taken apart, with the EVP key its material makes.
    struct ParsedKey
    {
        KeyAlgorithm const* algorithm;
        Bytes application; ///< Empty unless the key is on an authenticator.
        PKeyPtr evpKey;
    };

    /// Takes a public key blob apart, field by field, refusing a short, oversized or trailing one.
    [[nodiscard]] std::expected<ParsedKey, SshKeyError> parseKeyBlob(Bytes blob)
    {
        auto reader = detail::SshWireReader { blob };
        auto const name = reader.string();
        if (!name)
            return std::unexpected(SshKeyError::Malformed);
        auto const* const algorithm = algorithmNamed(*name);
        if (algorithm == nullptr)
            return std::unexpected(SshKeyError::UnsupportedType);

        auto evpKey = PKeyPtr { nullptr, EVP_PKEY_free };
        switch (algorithm->material)
        {
            case KeyMaterial::Ed25519: {
                auto const publicKey = reader.string();
                if (!publicKey || publicKey->size() != Ed25519KeySize)
                    return std::unexpected(SshKeyError::Malformed);
                evpKey.reset(
                    EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,
                                                nullptr,
                                                reinterpret_cast<unsigned char const*>(publicKey->data()),
                                                publicKey->size()));
                break;
            }
            case KeyMaterial::P256: {
                auto const curve = reader.string();
                auto const point = reader.string();
                if (!curve || !equalBytes(*curve, bytesOf(P256CurveName)) || !point
                    || point->size() != P256PointSize || (*point)[0] != std::byte { 0x04 })
                    return std::unexpected(SshKeyError::Malformed);
                evpKey = p256Key(*point);
                break;
            }
        }

        auto application = Bytes {};
        if (algorithm->authenticator == Authenticator::Fido)
        {
            auto const read = reader.string();
            if (!read)
                return std::unexpected(SshKeyError::Malformed);
            application = *read;
        }
        if (!reader.atEnd() || !evpKey)
            return std::unexpected(SshKeyError::Malformed);
        return ParsedKey { .algorithm = algorithm, .application = application, .evpKey = std::move(evpKey) };
    }

    /// PROTOCOL.sshsig's signed data.
    [[nodiscard]] std::vector<std::byte> signedDataOf(Bytes nameSpace,
                                                      Bytes reserved,
                                                      Bytes hashName,
                                                      Bytes hash)
    {
        auto out = std::vector<std::byte> {};
        out.insert(out.end(), bytesOf(SshsigMagic).begin(), bytesOf(SshsigMagic).end());
        detail::appendSshString(out, nameSpace);
        detail::appendSshString(out, reserved);
        detail::appendSshString(out, hashName);
        detail::appendSshString(out, hash);
        return out;
    }

    /// The minimal magnitude of a non-negative SSH mpint, or nothing if it is negative, not minimal,
    /// zero, or wider than a P-256 scalar.
    [[nodiscard]] std::optional<Bytes> p256Scalar(Bytes mpint) noexcept
    {
        if (mpint.empty() || (std::to_integer<std::uint8_t>(mpint[0]) & 0x80U) != 0)
            return std::nullopt;
        if (mpint[0] == std::byte { 0 })
        {
            // A leading zero is allowed only as the sign byte of a value whose top bit is set.
            if (mpint.size() < 2 || (std::to_integer<std::uint8_t>(mpint[1]) & 0x80U) == 0)
                return std::nullopt;
            mpint = mpint.subspan(1);
        }
        if (mpint.size() > P256ScalarSize)
            return std::nullopt;
        return mpint;
    }

    /// RFC 5656's `mpint r ‖ mpint s` as the DER `ECDSA-Sig-Value` OpenSSL verifies.
    [[nodiscard]] std::optional<std::vector<unsigned char>> ecdsaDerOf(Bytes sshSignature)
    {
        auto reader = detail::SshWireReader { sshSignature };
        auto const rField = reader.string();
        auto const sField = reader.string();
        if (!rField || !sField || !reader.atEnd())
            return std::nullopt;
        auto const r = p256Scalar(*rField);
        auto const s = p256Scalar(*sField);
        if (!r || !s)
            return std::nullopt;

        auto const toBignum = [](Bytes magnitude) {
            return BignumPtr { BN_bin2bn(reinterpret_cast<unsigned char const*>(magnitude.data()),
                                         static_cast<int>(magnitude.size()),
                                         nullptr),
                               BN_free };
        };
        auto rBignum = toBignum(*r);
        auto sBignum = toBignum(*s);
        auto signature = EcdsaSigPtr { ECDSA_SIG_new(), ECDSA_SIG_free };
        if (!rBignum || !sBignum || !signature
            || ECDSA_SIG_set0(signature.get(), rBignum.get(), sBignum.get()) != 1)
            return std::nullopt;
        std::ignore = rBignum.release(); // owned by the signature from here
        std::ignore = sBignum.release();

        auto const length = i2d_ECDSA_SIG(signature.get(), nullptr);
        if (length <= 0)
            return std::nullopt;
        auto der = std::vector<unsigned char>(static_cast<std::size_t>(length));
        auto* cursor = der.data();
        if (i2d_ECDSA_SIG(signature.get(), &cursor) != length)
            return std::nullopt;
        return der;
    }

    /// Verifies @p signature of @p data by @p key: pure Ed25519, or ECDSA over SHA-256.
    [[nodiscard]] std::expected<void, SshSigError> verifyPrimitive(ParsedKey const& key,
                                                                   Bytes signature,
                                                                   Bytes data)
    {
        auto der = std::vector<unsigned char> {};
        auto const* digest = static_cast<EVP_MD const*>(nullptr);
        switch (key.algorithm->material)
        {
            case KeyMaterial::Ed25519:
                if (signature.size() != Ed25519SignatureSize)
                    return std::unexpected(SshSigError::Malformed);
                der.resize(signature.size());
                std::ranges::transform(
                    signature, der.begin(), [](std::byte b) { return std::to_integer<unsigned char>(b); });
                break;
            case KeyMaterial::P256: {
                auto converted = ecdsaDerOf(signature);
                if (!converted)
                    return std::unexpected(SshSigError::Malformed);
                der = std::move(*converted);
                digest = EVP_sha256();
                break;
            }
        }
        auto const ctx = MdCtxPtr { EVP_MD_CTX_new(), EVP_MD_CTX_free };
        if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, digest, nullptr, key.evpKey.get()) != 1
            || EVP_DigestVerify(ctx.get(),
                                der.data(),
                                der.size(),
                                reinterpret_cast<unsigned char const*>(data.data()),
                                data.size())
                   != 1)
            return std::unexpected(SshSigError::BadSignature);
        return {};
    }

    /// The SSHSIG blob inside @p sshsig: the armored base64, or the bytes themselves.
    [[nodiscard]] std::optional<std::vector<std::byte>> envelopeOf(std::string_view sshsig)
    {
        auto const trimmed = trim(sshsig);
        if (!trimmed.starts_with(ArmorBegin))
        {
            auto const raw = bytesOf(sshsig);
            return std::vector<std::byte> { raw.begin(), raw.end() };
        }
        // The size guard first: BEGIN and END must not overlap, or the body's count underflows.
        if (trimmed.size() < ArmorBegin.size() + ArmorEnd.size() || !trimmed.ends_with(ArmorEnd))
            return std::nullopt;
        auto body = std::string {};
        for (auto const c:
             trimmed.substr(ArmorBegin.size(), trimmed.size() - ArmorBegin.size() - ArmorEnd.size()))
            if (!Whitespace.contains(c))
                body += c;
        return decodeBase64Strictly(body);
    }

    /// The fields of an SSHSIG blob.
    struct Envelope
    {
        Bytes publicKey;
        Bytes nameSpace;
        Bytes reserved;
        Bytes hashName;
        Bytes signature;
    };

    [[nodiscard]] std::optional<Envelope> parseEnvelope(Bytes blob)
    {
        auto reader = detail::SshWireReader { blob };
        auto const magic = reader.raw(SshsigMagic.size());
        auto const version = reader.uint32();
        if (!magic || !equalBytes(*magic, bytesOf(SshsigMagic)) || version != SshsigVersion)
            return std::nullopt;
        auto const publicKey = reader.string();
        auto const nameSpace = reader.string();
        auto const reserved = reader.string();
        auto const hashName = reader.string();
        auto const signature = reader.string();
        if (!publicKey || !nameSpace || !reserved || !hashName || !signature || !reader.atEnd())
            return std::nullopt;
        // OpenSSH writes the reserved field empty and signs it so; anything else is not its format.
        if (!reserved->empty())
            return std::nullopt;
        return Envelope { *publicKey, *nameSpace, *reserved, *hashName, *signature };
    }

    /// The fields of an SSH signature: its algorithm name, the signature proper, and for an
    /// authenticator's, the flags and counter it signed.
    struct SshSignature
    {
        Bytes algorithmName;
        Bytes value;
        std::uint8_t flags = 0;
        std::uint32_t counter = 0;
    };

    [[nodiscard]] std::optional<SshSignature> parseSignature(Bytes blob, Authenticator authenticator)
    {
        auto reader = detail::SshWireReader { blob };
        auto const algorithmName = reader.string();
        auto const value = reader.string();
        if (!algorithmName || !value)
            return std::nullopt;
        auto signature = SshSignature { .algorithmName = *algorithmName, .value = *value };
        if (authenticator == Authenticator::Fido)
        {
            auto const flags = reader.byte();
            auto const counter = reader.uint32();
            if (!flags || !counter)
                return std::nullopt;
            signature.flags = *flags;
            signature.counter = *counter;
        }
        if (!reader.atEnd())
            return std::nullopt;
        return signature;
    }

    /// PROTOCOL.u2f: what an authenticator signs for @p signedData.
    [[nodiscard]] std::vector<std::byte> authenticatorDataOf(Bytes application,
                                                             SshSignature const& signature,
                                                             Bytes signedData)
    {
        auto out = digestOf(EVP_sha256(), application);
        detail::appendSshByte(out, signature.flags);
        detail::appendSshUint32(out, signature.counter);
        auto const dataHash = digestOf(EVP_sha256(), signedData);
        out.insert(out.end(), dataHash.begin(), dataHash.end());
        return out;
    }
} // namespace

std::string_view sshKeyTypeName(SshKeyType type) noexcept
{
    auto const found = std::ranges::find_if(
        KeyAlgorithms, [type](KeyAlgorithm const& algorithm) { return algorithm.type == type; });
    return found == KeyAlgorithms.end() ? std::string_view {} : found->name;
}

std::expected<SshPublicKey, SshKeyError> parseSshPublicKeyBlob(std::span<std::byte const> blob)
{
    ERR_clear_error();
    auto const clearErrors = detail::ScopeGuard { []() noexcept { ERR_clear_error(); } };
    auto const parsed = parseKeyBlob(blob);
    if (!parsed)
        return std::unexpected(parsed.error());
    return SshPublicKey { .type = parsed->algorithm->type,
                          .blob = std::vector<std::byte>(blob.begin(), blob.end()),
                          .comment = {} };
}

std::expected<SshPublicKey, SshKeyError> parseSshPublicKey(std::string_view line)
{
    auto const [typeName, rest] = splitToken(trim(line));
    auto const [encoded, comment] = splitToken(rest);
    if (typeName.empty())
        return std::unexpected(SshKeyError::Malformed);
    auto const* const algorithm = algorithmNamed(bytesOf(typeName));
    if (algorithm == nullptr)
        return std::unexpected(SshKeyError::UnsupportedType);
    auto const blob = decodeBase64Strictly(encoded);
    if (!blob)
        return std::unexpected(SshKeyError::Malformed);

    auto key = parseSshPublicKeyBlob(*blob);
    // The type inside the blob must be the one the line names; an unknown one inside is malformed.
    if (!key || key->type != algorithm->type)
        return std::unexpected(SshKeyError::Malformed);
    key->comment = std::string { comment };
    return key;
}

std::expected<std::optional<AuthorizedKeyLine>, SshKeyError> parseAuthorizedKeysLine(std::string_view line)
{
    auto const text = trim(line);
    if (text.empty() || text.starts_with('#'))
        return std::optional<AuthorizedKeyLine> {};

    auto entry = AuthorizedKeyLine {};
    auto keyText = text;
    if (algorithmNamed(bytesOf(splitToken(text).first)) == nullptr)
    {
        // An options field: comma-separated, ending at whitespace outside double quotes.
        auto option = std::string {};
        auto quoted = false;
        auto rest = text;
        while (!rest.empty())
        {
            auto const c = rest.front();
            if (!quoted && Whitespace.contains(c))
                break;
            rest.remove_prefix(1);
            if (!quoted && c == ',')
            {
                if (option.empty())
                    return std::unexpected(SshKeyError::Malformed);
                entry.options.push_back(std::exchange(option, {}));
                continue;
            }
            option += c;
            if (c == '"')
                quoted = !quoted;
            else if (quoted && c == '\\' && rest.starts_with('"'))
            {
                option += '"';
                rest.remove_prefix(1);
            }
        }
        if (quoted || option.empty())
            return std::unexpected(SshKeyError::Malformed);
        entry.options.push_back(std::move(option));
        keyText = trim(rest);
        if (keyText.empty())
            return std::unexpected(SshKeyError::Malformed);
    }

    auto key = parseSshPublicKey(keyText);
    if (!key)
        return std::unexpected(key.error());
    entry.key = std::move(*key);
    return std::optional { std::move(entry) };
}

std::string sshFingerprintOf(SshPublicKey const& key)
{
    return sshStyleFingerprint(Sha256::hash(std::span<std::byte const> { key.blob }));
}

std::expected<void, SshSigError> verifySshSignature(SshPublicKey const& key,
                                                    std::string_view nameSpace,
                                                    std::span<std::byte const> message,
                                                    std::string_view sshsig)
{
    auto const blob = envelopeOf(sshsig);
    if (!blob)
        return std::unexpected(SshSigError::Malformed);
    auto const envelope = parseEnvelope(*blob);
    if (!envelope)
        return std::unexpected(SshSigError::Malformed);
    // Not a secret, so no constant-time compare -- but the whole blob, not a prefix or a type.
    if (!equalBytes(envelope->publicKey, key.blob))
        return std::unexpected(SshSigError::KeyMismatch);
    if (!equalBytes(envelope->nameSpace, bytesOf(nameSpace)))
        return std::unexpected(SshSigError::WrongNamespace);
    auto const hash = std::ranges::find_if(SshsigHashes, [&](SshsigHash const& candidate) {
        return equalBytes(bytesOf(candidate.name), envelope->hashName);
    });
    if (hash == SshsigHashes.end())
        return std::unexpected(SshSigError::UnsupportedHash);

    ERR_clear_error();
    auto const clearErrors = detail::ScopeGuard { []() noexcept { ERR_clear_error(); } };
    auto const parsedKey = parseKeyBlob(key.blob);
    if (!parsedKey || parsedKey->algorithm->type != key.type)
        return std::unexpected(SshSigError::Malformed);
    auto const& algorithm = *parsedKey->algorithm;

    auto const signature = parseSignature(envelope->signature, algorithm.authenticator);
    if (!signature || !equalBytes(signature->algorithmName, bytesOf(algorithm.name)))
        return std::unexpected(SshSigError::Malformed);

    auto const messageHash = digestOf(hash->algorithm(), message);
    auto const signedData =
        signedDataOf(envelope->nameSpace, envelope->reserved, envelope->hashName, messageHash);
    auto const verified =
        algorithm.authenticator == Authenticator::Fido
            ? verifyPrimitive(*parsedKey,
                              signature->value,
                              authenticatorDataOf(parsedKey->application, *signature, signedData))
            : verifyPrimitive(*parsedKey, signature->value, signedData);
    if (!verified)
        return verified;
    // Checked after the signature, so the flag that is read is one the authenticator signed.
    if (algorithm.authenticator == Authenticator::Fido && (signature->flags & UserPresenceFlag) == 0)
        return std::unexpected(SshSigError::UserNotPresent);
    return {};
}

std::vector<std::byte> sshsigSignedData(std::string_view nameSpace, std::span<std::byte const> message)
{
    auto const messageHash = digestOf(ProducedHash.algorithm(), message);
    return signedDataOf(bytesOf(nameSpace), {}, bytesOf(ProducedHash.name), messageHash);
}

std::string assembleSshsig(SshPublicKey const& key,
                           std::string_view nameSpace,
                           std::span<std::byte const> agentSignature)
{
    auto out = std::vector<std::byte> {};
    out.insert(out.end(), bytesOf(SshsigMagic).begin(), bytesOf(SshsigMagic).end());
    detail::appendSshUint32(out, SshsigVersion);
    detail::appendSshString(out, key.blob);
    detail::appendSshString(out, nameSpace);
    detail::appendSshString(out, std::string_view {});
    detail::appendSshString(out, ProducedHash.name);
    detail::appendSshString(out, agentSignature);
    return { reinterpret_cast<char const*>(out.data()), out.size() };
}

} // namespace core::net
