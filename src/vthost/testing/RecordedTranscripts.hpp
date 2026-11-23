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

#include <vthost/proto/Pdu.hpp>

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

/// Protocol version 5: image cells carry their placement's identity and geometry. The same attach as v4's,
/// recorded from the build that introduced it.
namespace v5
{
    constexpr auto ClientHello = recorded("\x4e\x01\x01\x04\x00\x00\x00\x1f\x05\x05\x01\x01\x00\x01\xd0\x0f"
                                          "\x00\x01\x01\x00\x01\x00\x00\x01\x04\x20\x2e\x2f\x5c\x01\xea\x0f"
                                          "\x00\x00\x00\x05\x00\x00\x00\x00");
    constexpr auto ServerHello = recorded("\x60\x01\x02\x04\x00\x00\x00\x29\x05\x05\x05\x01\x01\x10\x35\x66"
                                          "\x31\x64\x32\x63\x33\x62\x34\x61\x35\x39\x36\x38\x37\x37\x0d\x72"
                                          "\x65\x63\x6f\x72\x64\x65\x64\x2d\x68\x6f\x73\x74\x00\x00\x00\x00"
                                          "\x00");
    constexpr auto ResizeRequest = recorded("\x08\x02\x04\x64\x1e");
    constexpr auto Input = recorded("\x0e\x03\x03\x01\x03\x6c\x73\x0d");
} // namespace v5

/// Every recorded transcript, oldest version first.
constexpr std::array RecordedTranscripts {
    RecordedTranscript { .version = 4,
                         .clientHello = v4::ClientHello,
                         .serverHello = v4::ServerHello,
                         .resizeRequest = v4::ResizeRequest,
                         .input = v4::Input },
    RecordedTranscript { .version = 5,
                         .clientHello = v5::ClientHello,
                         .serverHello = v5::ServerHello,
                         .resizeRequest = v5::ResizeRequest,
                         .input = v5::Input },
};

/// The PDUs one version's control client and daemon wrote for a status and an upgrade, each a whole frame:
/// what `contour daemon status` and `contour daemon upgrade` of a later release must still be answered with.
struct RecordedControlTranscript
{
    uint32_t version = 0;                      ///< The protocol version the bytes are of.
    std::span<std::byte const> clientHello;    ///< The client's Control-role ClientHello.
    std::span<std::byte const> serverHello;    ///< The daemon's accepting ServerHello.
    std::span<std::byte const> statusRequest;  ///< The client's DaemonStatusRequest.
    std::span<std::byte const> statusReply;    ///< The daemon's DaemonStatusReply.
    std::span<std::byte const> upgradeRequest; ///< The client's UpgradeDaemon.
    std::span<std::byte const> upgradeReply;   ///< The daemon's UpgradeDaemonReply, an upgrade started.
};

/// Protocol version 4's control conversation: a status and an upgrade, as `contour daemon status|upgrade` of
/// a v4 build and its daemon write them. Recorded from a later build: Delta's was the only encoding 4 -> 5
/// changed, so this build writes every control PDU at version 4 as the v4 build did.
namespace v4::control
{
    constexpr auto ClientHello = recorded("\x2a\x01\x01\x04\x00\x00\x00\x0d\x04\x04\x02\x02\x12\x00\x00\x01"
                                          "\x00\x05\x00\x00\x00\x00");
    constexpr auto ServerHello = recorded("\x6c\x01\x02\x04\x00\x00\x00\x2f\x04\x04\x04\x02\x02\x12\x10\x35"
                                          "\x66\x31\x64\x32\x63\x33\x62\x34\x61\x35\x39\x36\x38\x37\x37\x0d"
                                          "\x72\x65\x63\x6f\x72\x64\x65\x64\x2d\x68\x6f\x73\x74\x05\x30\x2e"
                                          "\x38\x2e\x31\x00\x00\x00\x00");
    constexpr auto DaemonStatusRequest = recorded("\x04\x02\x19");
    constexpr auto DaemonStatusReply =
        recorded("\x4a\x02\x1a\x05\x30\x2e\x38\x2e\x31\xe7\x24\x5a\x14\x2f\x72\x75"
                 "\x6e\x2f\x63\x6f\x6e\x74\x6f\x75\x72\x2f\x63\x6f\x6e\x74\x72\x6f"
                 "\x6c\x00\x00\x00\x00\x00");
    constexpr auto UpgradeDaemon = recorded("\x32\x02\x29\x16\x30\x2e\x38\x2e\x32\x2d\x30\x31\x32\x33\x34\x35"
                                            "\x36\x37\x38\x39\x61\x62\x63\x64\x65\x66");
    constexpr auto UpgradeDaemonReply = recorded("\x08\x02\x2a\x00\x00");
} // namespace v4::control

/// Protocol version 5's control conversation, the same as v4's but for the versions its hellos state.
namespace v5::control
{
    constexpr auto ClientHello = recorded("\x2a\x01\x01\x04\x00\x00\x00\x0d\x05\x05\x02\x02\x12\x00\x00\x01"
                                          "\x00\x05\x00\x00\x00\x00");
    constexpr auto ServerHello = recorded("\x6c\x01\x02\x04\x00\x00\x00\x2f\x05\x05\x05\x02\x02\x12\x10\x35"
                                          "\x66\x31\x64\x32\x63\x33\x62\x34\x61\x35\x39\x36\x38\x37\x37\x0d"
                                          "\x72\x65\x63\x6f\x72\x64\x65\x64\x2d\x68\x6f\x73\x74\x05\x30\x2e"
                                          "\x38\x2e\x31\x00\x00\x00\x00");
    constexpr auto DaemonStatusRequest = recorded("\x04\x02\x19");
    constexpr auto DaemonStatusReply =
        recorded("\x4a\x02\x1a\x05\x30\x2e\x38\x2e\x31\xe7\x24\x5a\x14\x2f\x72\x75"
                 "\x6e\x2f\x63\x6f\x6e\x74\x6f\x75\x72\x2f\x63\x6f\x6e\x74\x72\x6f"
                 "\x6c\x00\x00\x00\x00\x00");
    constexpr auto UpgradeDaemon = recorded("\x32\x02\x29\x16\x30\x2e\x38\x2e\x32\x2d\x30\x31\x32\x33\x34\x35"
                                            "\x36\x37\x38\x39\x61\x62\x63\x64\x65\x66");
    constexpr auto UpgradeDaemonReply = recorded("\x08\x02\x2a\x00\x00");
} // namespace v5::control

/// One PDU a Control-role connection exchanges, as a version's peer wrote it: a whole frame, every optional
/// field of it stated.
struct RecordedControlPdu
{
    proto::PduType type;              ///< The PDU.
    std::span<std::byte const> frame; ///< Its frame.
};

/// Every PDU in proto::ControlPdus, as one protocol version's peers wrote them.
struct RecordedControlPdus
{
    uint32_t version = 0;                     ///< The protocol version the bytes are of.
    std::span<RecordedControlPdu const> pdus; ///< One frame per row of proto::ControlPdus.
};

/// Every PDU a Control-role connection exchanged at protocol version 4, one example each with every optional
/// field stated (testing/PduExamples.hpp). Recorded from a later build: Delta's was the only encoding 4 -> 5
/// changed, so this build writes each of these at version 4 as the v4 build did.
namespace v4::control::pdus
{
    constexpr auto ClientHello = recorded("\xd2\x01\x01\x01\x04\x00\x00\x00\x61\x04\x0c\x02\x01\xe7\x24\x06"
                                          "\x73\x33\x63\x72\x33\x74\x01\x90\x4e\x00\x01\x01\x00\x01\x00\x00"
                                          "\x01\x11\x73\x65\x63\x72\x65\x74\x2d\x64\x65\x6c\x69\x6d\x69\x74"
                                          "\x65\x72\x73\x00\x00\x01\x84\x01\x2b\x08\x07\x04\x77\x6f\x72\x6b"
                                          "\x00\x09\x12\x11\x2f\x68\x6f\x6d\x65\x2f\x75\x73\x65\x72\x2f\x73"
                                          "\x65\x63\x72\x65\x74\x05\x02\x0a\x0b\x10\x73\x65\x63\x72\x65\x74"
                                          "\x2d\x73\x69\x67\x6e\x61\x74\x75\x72\x65\x00");
    constexpr auto ServerHello = recorded("\x5e\x01\x02\x04\x00\x00\x00\x28\x04\x05\x05\x01\x01\x11\x73\x65"
                                          "\x63\x72\x65\x74\x2d\x6d\x61\x63\x68\x69\x6e\x65\x2d\x69\x64\x0b"
                                          "\x73\x65\x63\x72\x65\x74\x2d\x68\x6f\x73\x74\x00\x03\x00\x00\x00");
    constexpr auto Unsupported = recorded("\x0a\x01\x18\x09\xc8\x01");
    constexpr auto DaemonStatusRequest = recorded("\x04\x01\x19");
    constexpr auto DaemonStatusReply =
        recorded("\xf2\x01\x01\x1a\x05\x30\x2e\x38\x2e\x31\x92\x21\xd9\x39\x1e\x2f"
                 "\x72\x75\x6e\x2f\x75\x73\x65\x72\x2f\x31\x30\x30\x30\x2f\x63\x6f"
                 "\x6e\x74\x6f\x75\x72\x2f\x64\x65\x66\x61\x75\x6c\x74\x00\x00\x03"
                 "\x01\x00\x02\x03\x0a\x0e\x64\x69\x64\x20\x6e\x6f\x74\x20\x73\x65"
                 "\x74\x74\x6c\x65\x01\x02\x32\x53\x48\x41\x32\x35\x36\x3a\x34\x37"
                 "\x44\x45\x51\x70\x6a\x38\x48\x42\x53\x61\x2b\x2f\x54\x49\x6d\x57"
                 "\x2b\x35\x4a\x43\x65\x75\x51\x65\x52\x6b\x6d\x35\x4e\x4d\x70\x4a"
                 "\x57\x5a\x47\x33\x68\x53\x75\x46\x55\xe8\x20");
    constexpr auto StopDaemon = recorded("\x06\x01\x1b\x01");
    constexpr auto Heartbeat = recorded("\x06\x01\x1d\x07");
    constexpr auto ControlRequest =
        recorded("\x44\x01\x1e\x07\x0a\x70\x61\x6e\x65\x2e\x73\x70\x6c\x69\x74\x0c"
                 "\x7b\x22\x70\x61\x6e\x65\x22\x3a\x22\x78\x22\x7d\x06\x70\x61\x6e"
                 "\x65\x3a\x70");
    constexpr auto ControlReply = recorded("\x0e\x01\x1f\x07\x01\x02\x7b\x7d");
    constexpr auto ControlEvent = recorded("\x30\x01\x20\x03\x14\x7b\x22\x74\x79\x70\x65\x22\x3a\x22\x70\x61"
                                           "\x6e\x65\x2e\x62\x65\x6c\x6c\x22\x7d");
    constexpr auto UpgradeDaemon = recorded("\x22\x01\x29\x0e\x30\x2e\x39\x2e\x30\x2d\x30\x31\x32\x33\x61\x62"
                                            "\x63\x64");
    constexpr auto UpgradeDaemonReply =
        recorded("\x32\x01\x2a\x06\x15\x61\x20\x73\x65\x73\x73\x69\x6f\x6e\x20\x69"
                 "\x73\x20\x73\x74\x61\x72\x74\x69\x6e\x67");
    constexpr auto DaemonUpgrading = recorded("\x10\x01\x2b\x05\x30\x2e\x39\x2e\x30");
    constexpr auto AuthChallenge = recorded("\x46\x01\x37\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                                            "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                                            "\x00\x00\x00\x02");
    constexpr auto Goodbye = recorded("\x04\x01\x3c");

    constexpr std::array Pdus {
        RecordedControlPdu { .type = proto::PduType::ClientHello, .frame = ClientHello },
        RecordedControlPdu { .type = proto::PduType::ServerHello, .frame = ServerHello },
        RecordedControlPdu { .type = proto::PduType::Unsupported, .frame = Unsupported },
        RecordedControlPdu { .type = proto::PduType::DaemonStatusRequest, .frame = DaemonStatusRequest },
        RecordedControlPdu { .type = proto::PduType::DaemonStatusReply, .frame = DaemonStatusReply },
        RecordedControlPdu { .type = proto::PduType::StopDaemon, .frame = StopDaemon },
        RecordedControlPdu { .type = proto::PduType::Heartbeat, .frame = Heartbeat },
        RecordedControlPdu { .type = proto::PduType::ControlRequest, .frame = ControlRequest },
        RecordedControlPdu { .type = proto::PduType::ControlReply, .frame = ControlReply },
        RecordedControlPdu { .type = proto::PduType::ControlEvent, .frame = ControlEvent },
        RecordedControlPdu { .type = proto::PduType::UpgradeDaemon, .frame = UpgradeDaemon },
        RecordedControlPdu { .type = proto::PduType::UpgradeDaemonReply, .frame = UpgradeDaemonReply },
        RecordedControlPdu { .type = proto::PduType::DaemonUpgrading, .frame = DaemonUpgrading },
        RecordedControlPdu { .type = proto::PduType::AuthChallenge, .frame = AuthChallenge },
        RecordedControlPdu { .type = proto::PduType::Goodbye, .frame = Goodbye },
    };
} // namespace v4::control::pdus

/// Every recorded control PDU set, oldest version first. A version whose control PDUs are written as the
/// version before's names that version's frames.
constexpr std::array RecordedControlPduSets {
    RecordedControlPdus { .version = 4, .pdus = v4::control::pdus::Pdus },
    RecordedControlPdus { .version = 5, .pdus = v4::control::pdus::Pdus }, // 4 -> 5 changed none of them
};

/// Every recorded control conversation, oldest version first.
constexpr std::array RecordedControlTranscripts {
    RecordedControlTranscript { .version = 4,
                                .clientHello = v4::control::ClientHello,
                                .serverHello = v4::control::ServerHello,
                                .statusRequest = v4::control::DaemonStatusRequest,
                                .statusReply = v4::control::DaemonStatusReply,
                                .upgradeRequest = v4::control::UpgradeDaemon,
                                .upgradeReply = v4::control::UpgradeDaemonReply },
    RecordedControlTranscript { .version = 5,
                                .clientHello = v5::control::ClientHello,
                                .serverHello = v5::control::ServerHello,
                                .statusRequest = v5::control::DaemonStatusRequest,
                                .statusReply = v5::control::DaemonStatusReply,
                                .upgradeRequest = v5::control::UpgradeDaemon,
                                .upgradeReply = v5::control::UpgradeDaemonReply },
};

} // namespace vthost::testing
