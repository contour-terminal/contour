// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <utility>

#include <vthost/proto/Unsupported.hpp>

using namespace vthost::proto;

namespace
{
/// @return A frame of @p pdu at @p serial, as the decoder hands it over.
DecodedFrame frameOf(uint64_t serial, DecodedPdu pdu)
{
    return DecodedFrame { .serial = serial, .pdu = std::move(pdu), .consumed = 0 };
}

/// @return The refusal of @p type's tag.
Unsupported refusalOf(PduType type, uint64_t serial = 3)
{
    return Unsupported { .refusedSerial = serial, .ident = std::to_underlying(type) };
}
} // namespace

TEST_CASE("an unknown PDU is answered with its frame's serial and tag", "[vthost][negotiation]")
{
    auto tracker = UnsupportedTracker {};
    auto const handling = tracker.handle(frameOf(9, Invalid { .ident = 200 }));
    CHECK(handling.verdict == UnsupportedVerdict::Answer);
    CHECK(handling.pdu == Unsupported { .refusedSerial = 9, .ident = 200 });
    // Answering is not refusing: the tracker records only what the PEER refused.
    CHECK_FALSE(tracker.peerRefused(static_cast<PduType>(200)));
}

TEST_CASE("an Unsupported is recorded once, and never answered", "[vthost][negotiation]")
{
    auto tracker = UnsupportedTracker {};
    auto const first = tracker.handle(frameOf(5, refusalOf(PduType::ResizePane)));
    CHECK(first.verdict == UnsupportedVerdict::Refused);
    CHECK(first.pdu == refusalOf(PduType::ResizePane));
    CHECK(tracker.peerRefused(PduType::ResizePane));
    CHECK_FALSE(tracker.peerRefused(PduType::Input));

    // The same tag again, from another refused frame: already known, so nothing new to say.
    CHECK(tracker.handle(frameOf(6, refusalOf(PduType::ResizePane, 4))).verdict
          == UnsupportedVerdict::AlreadyRefused);
    CHECK(tracker.peerRefused(PduType::ResizePane));
}

TEST_CASE("a PDU the tracker has no stake in is left to the caller", "[vthost][negotiation]")
{
    auto tracker = UnsupportedTracker {};
    CHECK(tracker.handle(frameOf(5, Input { .session = 1, .data = {} })).verdict
          == UnsupportedVerdict::NotConcerned);
    CHECK_FALSE(tracker.peerRefused(PduType::Input));
}
