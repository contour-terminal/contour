// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The machine a daemon runs on, as it states it and as a client attached to it judges it: whether
/// "local" in what the daemon reports means local to the client too.

#include <vtbackend/core/TerminalContext.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace vthost
{

/// Contour's application id: what a machine's id is derived under before it leaves the machine, as
/// sd_id128_get_machine_app_specific(3) derives one for an application.
///
/// A fixed, random 128-bit id, and fixed for good: a daemon and a client deriving under different ids
/// would take every machine for another. On a machine with systemd,
/// `systemd-id128 machine-id --app-specific=71621e7c11854bdbb34c6eddd2c15d97` prints the id a daemon
/// there states (@ref appSpecificMachineId).
constexpr inline std::string_view ContourApplicationId = "71621e7c11854bdbb34c6eddd2c15d97";

/// The machine id this application states for a machine whose machine-id(5) is @p machineId.
///
/// Never the machine id itself, which machine-id(5) asks to keep confidential, but an id derived from it
/// for this application alone, exactly as sd_id128_get_machine_app_specific(3) derives it: HMAC-SHA256
/// keyed by the machine id's 16 bytes over @ref ContourApplicationId's, the digest's first 16 bytes made
/// a version-4 UUID. Two machines with one id state one derived id, and no derived id gives the machine
/// id away.
/// @param machineId The machine's id, as /etc/machine-id holds it.
/// @return 32 lower-case hex digits; empty when @p machineId is no machine id (vtbackend::isMachineId),
///         so an unknown id stays unknown.
[[nodiscard]] std::string appSpecificMachineId(std::string_view machineId);

/// The identity a daemon states for the machine @p self names: its host name, and its machine id
/// derived for this application (@ref appSpecificMachineId).
/// @param self A machine's identity, as read on it.
/// @return The identity as it travels.
[[nodiscard]] vtbackend::HostIdentity appSpecificIdentity(vtbackend::LocalIdentity self);

/// Whether the daemon a client attached to runs on the client's own machine.
///
/// A daemon judges every record's locality against ITS machine, so a record's ContextLocality::Local
/// means "local to the daemon". A client attached over the network from another machine must not take
/// that as its own: there the path names nothing, or something else.
enum class DaemonMachine : uint8_t
{
    Other = 0, ///< Another machine, or one nothing proves is this one.
    Same,      ///< This machine: what is local to the daemon is local here.
};

/// Decides whether @p daemon and @p self name one machine, by the rule a context's locality is decided
/// by (vtbackend::sameMachine), so a record and the context it ran in never disagree about where a path
/// is. Anything short of proof is DaemonMachine::Other — the conservative answer.
///
/// The daemon states its machine id derived for this application, so this machine's is derived the
/// same way before the two are compared: equal machines still compare equal.
/// @param daemon The daemon's identity, as its ServerHello stated it (@ref appSpecificIdentity).
/// @param self This client's identity, as read on this machine.
/// @return Same only when the identities prove one machine.
[[nodiscard]] DaemonMachine daemonMachineOf(vtbackend::HostIdentity const& daemon,
                                            vtbackend::LocalIdentity self);

} // namespace vthost
