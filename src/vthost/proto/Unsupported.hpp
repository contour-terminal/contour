// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What either endpoint does about PDU tags one side does not know, once the handshake is done: an unknown
/// PDU is answered with an Unsupported naming it, and a tag the peer said it does not know is not sent to
/// it again. One helper for the daemon's NativeSession and the attach client's NativeClient, so the two
/// ends cannot drift apart on the rule; each keeps only its own sending and logging.

#include <cstdint>
#include <set>

#include <vthost/proto/Pdu.hpp>

namespace vthost::proto
{

/// What an endpoint does with a frame, as far as unknown PDUs are concerned.
enum class UnsupportedVerdict : uint8_t
{
    NotConcerned = 0, ///< Neither an unknown PDU nor an Unsupported: the endpoint dispatches it as usual.
    Answer,           ///< An unknown PDU: the endpoint sends UnsupportedHandling::pdu.
    Refused,          ///< The peer refused a tag for the first time: worth saying, once.
    AlreadyRefused,   ///< The peer refused a tag it had refused before: nothing more to do.
};

/// The tracker's verdict on one frame.
struct UnsupportedHandling
{
    UnsupportedVerdict verdict = UnsupportedVerdict::NotConcerned; ///< What the endpoint does.
    /// The reply to send for UnsupportedVerdict::Answer, or the refusal received for the two Refused
    /// verdicts; empty for UnsupportedVerdict::NotConcerned.
    Unsupported pdu = {};
};

/// Applies the Unsupported rule for one connection, after its handshake. Before it an unknown PDU is a
/// protocol error, which is the handshake's to report, so an endpoint consults this only once handshaken.
class UnsupportedTracker
{
  public:
    /// Judges a received frame: an unknown PDU is answered with its frame's serial and tag, and an
    /// Unsupported is recorded and never answered.
    /// @param frame The frame as decoded.
    /// @return What the endpoint does with it.
    [[nodiscard]] UnsupportedHandling handle(DecodedFrame const& frame);

    /// @param type A PDU type this endpoint is about to send.
    /// @return Whether the peer said it does not know @p type, so that it is not to be sent.
    [[nodiscard]] bool peerRefused(PduType type) const noexcept;

  private:
    std::set<uint64_t> _unsupportedByPeer; ///< The tags the peer answered with Unsupported.
};

} // namespace vthost::proto
