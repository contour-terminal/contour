// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Bytes a build of each protocol version wrote, frozen: what every later build must still serve while the
/// version is inside its SupportedVersions.
///
/// NEVER REGENERATED for a shipped version. These bytes are what a released peer sends and reads, and a
/// released peer does not change; re-recording them from a newer encoder would hold the newer build to
/// itself. A new version's transcripts are recorded once, by the hidden `[.dump]` test in
/// Compatibility_test.cpp, when the release that ships it is cut, and appended here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>

namespace vthost::testing
{

/// The PDUs one version's client and daemon wrote for an attach, each a whole frame.
struct RecordedTranscript
{
    uint32_t version = 0;                     ///< The protocol version the bytes are of.
    std::span<std::byte const> clientHello;   ///< The client's ClientHello.
    std::span<std::byte const> serverHello;   ///< The daemon's accepting ServerHello.
    std::span<std::byte const> resizeRequest; ///< The client's ResizeRequest.
    std::span<std::byte const> input;         ///< The client's Input.
};

/// @param text A frame's bytes, each escaped as `\xNN`.
/// @return Them, without the literal's terminating zero.
template <std::size_t N>
[[nodiscard]] consteval std::array<std::byte, N - 1> recorded(char const (&text)[N]) noexcept
{
    auto bytes = std::array<std::byte, N - 1> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, N - 1))
        bytes[index] = static_cast<std::byte>(text[index]);
    return bytes;
}

/// Protocol version 4, the first that negotiates: an attach by a client offering ExtensionPermissions and
/// stating session settings, and the daemon's accepting answer. Recorded from the build that introduced it.
namespace v4
{
    constexpr auto ClientHello = recorded("\x42\x01\x01\x04\x00\x00\x00\x19\x04\x04\x01\x01\x00\x01\xd0\x0f"
                                          "\x00\x01\x01\x00\x01\x00\x00\x01\x04\x20\x2e\x2f\x5c\x01\xea\x0f"
                                          "\x00\x00");
    constexpr auto ServerHello = recorded("\x56\x01\x02\x04\x00\x00\x00\x24\x04\x04\x04\x01\x01\x10\x35\x66"
                                          "\x31\x64\x32\x63\x33\x62\x34\x61\x35\x39\x36\x38\x37\x37\x0d\x72"
                                          "\x65\x63\x6f\x72\x64\x65\x64\x2d\x68\x6f\x73\x74");
    constexpr auto ResizeRequest = recorded("\x08\x02\x04\x64\x1e");
    constexpr auto Input = recorded("\x0e\x03\x03\x01\x03\x6c\x73\x0d");
} // namespace v4

/// Every recorded transcript, oldest version first.
constexpr std::array RecordedTranscripts {
    RecordedTranscript { .version = 4,
                         .clientHello = v4::ClientHello,
                         .serverHello = v4::ServerHello,
                         .resizeRequest = v4::ResizeRequest,
                         .input = v4::Input },
};

} // namespace vthost::testing
