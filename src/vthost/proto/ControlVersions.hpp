// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The protocol versions a Control-role connection is negotiated over. A Control connection exchanges a few
/// PDUs only -- the hellos, daemon status, stop and live upgrade, heartbeats and the control API -- and those
/// change far more rarely than what an attached client is sent. So a control connection spans every version
/// since one of them last changed, not just SupportedVersions: `contour daemon upgrade` reaches a daemon
/// built before the newest protocol bump, which is the daemon a live upgrade exists to replace.

#include <algorithm>
#include <array>
#include <cstdint>

#include <vthost/proto/Negotiation.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost::proto
{

/// A PDU a Control-role connection exchanges, and the protocol version its encoding last changed at.
struct ControlPdu
{
    PduType type;         ///< The PDU.
    uint32_t lastChanged; ///< The protocol version whose encoding of it this build still writes and reads.
};

/// Every PDU a Control-role connection may send or receive. A change older peers would misread sets its row
/// to the version that makes it: ControlVersions then no longer reaches back past it, and a control client
/// refuses a daemon that would misread it. A change they read past -- a trailing field they skip -- keeps the
/// row, as raising it would lock `contour daemon upgrade` out of the daemons it exists to replace. The
/// recorded frames of every row (vthost/testing/RecordedTranscripts.hpp) fail a test on either kind.
constexpr std::array ControlPdus {
    ControlPdu { .type = PduType::ClientHello, .lastChanged = 4 },
    ControlPdu { .type = PduType::ServerHello, .lastChanged = 4 },
    ControlPdu { .type = PduType::AuthChallenge, .lastChanged = 4 },
    ControlPdu { .type = PduType::Unsupported, .lastChanged = 4 },
    ControlPdu { .type = PduType::DaemonStatusRequest, .lastChanged = 4 },
    ControlPdu { .type = PduType::DaemonStatusReply, .lastChanged = 4 },
    ControlPdu { .type = PduType::StopDaemon, .lastChanged = 4 },
    ControlPdu { .type = PduType::UpgradeDaemon, .lastChanged = 4 },
    ControlPdu { .type = PduType::UpgradeDaemonReply, .lastChanged = 4 },
    ControlPdu { .type = PduType::DaemonUpgrading, .lastChanged = 4 },
    ControlPdu { .type = PduType::Heartbeat, .lastChanged = 4 },
    ControlPdu { .type = PduType::ControlRequest, .lastChanged = 4 },
    ControlPdu { .type = PduType::ControlReply, .lastChanged = 4 },
    ControlPdu { .type = PduType::ControlEvent, .lastChanged = 4 },
    ControlPdu { .type = PduType::Goodbye, .lastChanged = 4 },
};

/// The protocol versions a Control-role connection is negotiated over: from the newest version at which a
/// control PDU changed up to the newest this build speaks.
constexpr VersionRange ControlVersions {
    .min = std::ranges::max(ControlPdus, {}, &ControlPdu::lastChanged).lastChanged,
    .max = SupportedVersions.max,
};

static_assert(ControlVersions.min >= NegotiatingHelloMarker,
              "a control connection never reaches back to before version negotiation");
static_assert(ControlVersions.min <= SupportedVersions.min,
              "a control connection speaks at least what an attach does");

} // namespace vthost::proto
