// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The PDU catalog's DIAGNOSTIC projection: tag names and one-line summaries.
///
/// Kept out of Pdu.h because that header is deliberately wire-level and std-only; a
/// human-readable rendering is a diagnostic, not a codec. The separation also makes this a
/// pure function of a DecodedPdu — no logging, no transport, no I/O — so it is unit-testable
/// on its own and its cost is entirely the caller's to gate.
///
/// Adding an 18th PDU is adding ONE row to TraceTable in PduTrace.cpp; a static_assert
/// against std::variant_size_v<DecodedPdu> makes forgetting it a build break.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <vthost/proto/Pdu.h>

namespace vthost::proto
{

/// Which way a traced PDU crossed the wire, from the tracing endpoint's point of view.
enum class Direction : std::uint8_t
{
    Recv, ///< Decoded from the peer.
    Send, ///< Encoded to the peer.
};

/// Names a catalog tag for diagnostics.
/// @param type The catalog tag.
/// @return Its catalog name ("Delta", "ClientHello", …); "Unknown" for an off-catalog value.
[[nodiscard]] std::string_view toString(PduType type) noexcept;

/// Renders @p pdu's identifying fields as a compact `k=v k=v` fragment.
///
/// Reports SIZES, never payloads: a trace of a busy session must not become a transcript of
/// the user's keystrokes or screen contents. For the same reason the ClientHello token is
/// reported only as present or absent — it is authentication material.
///
/// @param pdu The PDU to describe.
/// @return The fragment; empty for the payloadless PDUs (CreateTab, NewWindow).
[[nodiscard]] std::string summarize(DecodedPdu const& pdu);

/// Names @p pdu and its identifying fields: `"Delta session=1 gen=3 …"`, or just `"CreateTab"`
/// for a payloadless PDU. The join rule lives here so trace lines and one-off diagnostics
/// cannot spell the same thing two different ways.
/// @param pdu The PDU to describe.
/// @return The name, followed by a space and the fields when there are any.
[[nodiscard]] std::string describe(DecodedPdu const& pdu);

/// Builds one trace line, e.g.
/// `recv #12 ResizeRequest cols=120 lines=40 (11 bytes)` or
/// `send #0 Delta session=1 gen=3 seq=98 lines=7 (412 bytes)`.
/// @param direction Which way the PDU went.
/// @param serial The frame serial (0 = unsolicited push).
/// @param pdu The PDU.
/// @param wireBytes The encoded frame's size, in bytes.
/// @return The assembled line, without a trailing newline.
[[nodiscard]] std::string traceLine(Direction direction,
                                    std::uint64_t serial,
                                    DecodedPdu const& pdu,
                                    std::size_t wireBytes);

} // namespace vthost::proto

template <>
struct std::formatter<vthost::proto::PduType>: formatter<std::string_view>
{
    auto format(vthost::proto::PduType value, auto& ctx) const
    {
        return formatter<std::string_view>::format(vthost::proto::toString(value), ctx);
    }
};
