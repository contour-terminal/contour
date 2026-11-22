// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonConsentKey.hpp>

#include <core/Sha256.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/net/Tls.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <utility>

#include <vthost/SessionSnapshotCodec.hpp>

namespace vthost
{

namespace
{
    /// What a proof is a proof of, so that no HMAC of the key made for anything else can stand for one.
    constexpr auto ProofLabel = std::string_view { "contour daemon-extension-consent v2\n" };

    /// @return @p bytes, each as the `char` it holds.
    [[nodiscard]] std::string asText(std::span<std::byte const> bytes)
    {
        auto text = std::string(bytes.size(), '\0');
        std::ranges::transform(bytes, text.begin(), [](std::byte b) { return static_cast<char>(b); });
        return text;
    }

    /// @return The key-file prefix of the daemon at @p socket: `<hash>-`.
    [[nodiscard]] std::string socketPrefixOf(std::filesystem::path const& socket)
    {
        auto const text = core::pathToUtf8(socket.lexically_normal());
        return std::format("{:016x}-", fnv1a64(std::as_bytes(std::span { text })));
    }
} // namespace

std::filesystem::path daemonConsentKeyDirectory(std::filesystem::path const& stateDirectory)
{
    return stateDirectory / "consent";
}

std::string daemonConsentKeyName(std::filesystem::path const& socket, uint64_t daemonInstance)
{
    return std::format("{}{:016x}.key", socketPrefixOf(socket), daemonInstance);
}

std::expected<HeldConsentKey, std::string> createDaemonConsentKey(IConsentKeyFiles const& files,
                                                                  core::IRandom& random,
                                                                  std::filesystem::path const& directory,
                                                                  std::filesystem::path const& socket,
                                                                  uint64_t daemonInstance)
{
    auto key = ConsentKey {};
    random.fill(key);
    auto const name = daemonConsentKeyName(socket, daemonInstance);
    auto hold = files.keep(directory, name, core::toLowerHexString(key));
    if (!hold)
        return std::unexpected(std::format("cannot keep the daemon's consent key in '{}': {}",
                                           core::pathToUtf8(directory / name),
                                           hold.error()));
    // What runs before this one at the same socket left: a file a live run still holds is its key, and stays.
    auto const prefix = socketPrefixOf(socket);
    if (auto const entries = files.list(directory))
        for (auto const& entry: *entries)
            if (entry.starts_with(prefix) && entry != name)
                std::ignore = files.removeUnlessHeld(directory, entry);
    return HeldConsentKey { .key = key, .hold = std::move(*hold) };
}

std::expected<ConsentKey, std::string> readDaemonConsentKey(IConsentKeyFiles const& files,
                                                            std::filesystem::path const& directory,
                                                            std::filesystem::path const& socket,
                                                            uint64_t daemonInstance)
{
    auto const name = daemonConsentKeyName(socket, daemonInstance);
    auto const where = core::pathToUtf8(directory / name);
    auto const text = files.read(directory, name);
    if (!text)
        return std::unexpected(
            std::format("cannot read the daemon's consent key '{}': {}", where, text.error()));
    auto const raw = core::fromHexString(std::string_view { *text });
    if (!raw || raw->size() != ConsentSecretBytes)
        return std::unexpected(std::format("'{}' holds no consent key", where));
    auto key = ConsentKey {};
    std::ranges::transform(*raw, key.begin(), [](char c) { return static_cast<std::byte>(c); });
    return key;
}

ConsentProofMac consentProofOf(ConsentKey const& key,
                               ConsentContext const& context,
                               ConsentNonce const& nonce)
{
    // label, the run as 16 hex digits, the socket, a NUL, the nonce: no field can run into the next.
    auto message = std::string { ProofLabel };
    message += std::format("{:016x}", context.daemonInstance);
    message += context.socket;
    message += '\0';
    message += asText(nonce);
    return core::hmacSha256(key, std::as_bytes(std::span { message }));
}

ConsentProofCheck checkConsentProof(ConsentKey const& key,
                                    ConsentContext const& context,
                                    ConsentNonce const& nonce,
                                    ConsentProofMac const& proof)
{
    auto const expected = consentProofOf(key, context, nonce);
    return core::net::constantTimeEquals(asText(expected), asText(proof)) ? ConsentProofCheck::Proven
                                                                          : ConsentProofCheck::Refused;
}

} // namespace vthost
