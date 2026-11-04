// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a client hands a hosted session for its pump to deliver, as a live-upgrade capture carries it across
/// the exec. Apart from SessionHost.hpp so that the handover image, which SessionHost.hpp names, can carry
/// it without including the host.

#include <cstdint>
#include <string>

namespace vthost
{

/// What the pump does with what a client handed a session.
enum class DeliveryKind : uint8_t
{
    Pty = 0, ///< Writes it to the PTY: a client's input, for the application.
    /// Parses it, as if the application had written it: an engine operation's WriteScreen. On the pump, as a
    /// parse may sleep (DECSCLM) and reply.
    Screen,
};

/// How much of a delivery is still to go.
enum class DeliveryProgress : uint8_t
{
    Whole = 0, ///< None of it went yet.
    /// The rest of a PTY write a capture interrupted: delivered before the terminal's queued replies, so that
    /// none lands in the middle of what the program was already reading (a paste).
    InterruptedRest,
};

/// What a client handed a session and the pump has not delivered yet, as a capture carries it.
struct CarriedDelivery
{
    DeliveryKind kind = DeliveryKind::Pty;               ///< What the pump does with @ref bytes.
    std::string bytes;                                   ///< What it does it with.
    DeliveryProgress progress = DeliveryProgress::Whole; ///< Whether it is the rest of an interrupted write.

    bool operator==(CarriedDelivery const&) const = default;
};

} // namespace vthost
