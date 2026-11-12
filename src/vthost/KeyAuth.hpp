// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a client signs to prove it holds an authorized SSH key, shared by both ends of the proof so the
/// two cannot drift apart.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

namespace vthost
{

/// The SSHSIG namespace a key proof is signed in: a signature made for another purpose does not verify.
inline constexpr std::string_view KeyAuthNamespace = "contour-auth";

/// The fixed prefix of the signed message; the version is part of it, so a change of the scheme is a new
/// prefix rather than a reinterpretation.
inline constexpr std::string_view KeyAuthPrefix = "contour-auth-v1";

/// How long a connection on a network listener has, from its accept, to finish the TLS handshake and have its
/// hello accepted; one that has not is closed. Before authentication a peer is nobody, and must not hold a
/// connection's resources for as long as it likes. A client signing its proof meanwhile waits no longer than
/// this for its signature either: the daemon has closed the connection by then.
inline constexpr auto PreAuthTimeout = std::chrono::seconds { 20 };

/// How many failed key proofs a connection may offer before it is refused.
inline constexpr std::size_t MaxKeyAuthAttempts = 5;

/// The message a key proof signs: the challenge, and the TLS session it is answered on.
/// @param nonce The daemon's AuthChallenge nonce.
/// @param channelBinding The session's RFC 9266 channel binding (label `EXPORTER-Channel-Binding`, empty
///        context), which no other connection shares, so a proof cannot be replayed on another one.
/// @return `"contour-auth-v1" ‖ nonce ‖ channelBinding`.
[[nodiscard]] inline std::vector<std::byte> keyAuthMessage(std::span<std::byte const, 32> nonce,
                                                           std::span<std::byte const, 32> channelBinding)
{
    auto message = std::vector<std::byte> {};
    message.reserve(KeyAuthPrefix.size() + nonce.size() + channelBinding.size());
    std::ranges::transform(
        KeyAuthPrefix, std::back_inserter(message), [](char c) { return static_cast<std::byte>(c); });
    message.insert(message.end(), nonce.begin(), nonce.end());
    message.insert(message.end(), channelBinding.begin(), channelBinding.end());
    return message;
}

} // namespace vthost
