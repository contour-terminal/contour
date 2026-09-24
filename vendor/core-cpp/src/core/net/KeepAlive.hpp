// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether a dialled connection probes a silent peer, and how hard.
///
/// Imported from fastcached's `Net/KeepAlive.hpp` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`.

#include <chrono>
#include <cstdint>

namespace core::net
{

/// Whether a dialled connection should carry TCP keepalive probes.
///
/// **What it answers, and the question it does NOT answer.** Keepalive detects a dead connection
/// or a dead HOST — powered off, cable pulled, VPN dropped, laptop suspended — and that is the
/// common hard failure. It does **not** detect a peer that is alive and simply not writing; that
/// needs a liveness signal in the protocol, and this does not make one unnecessary. The two are
/// different questions, and collapsing them is how a slow peer gets killed and a dead one gets
/// waited on.
///
/// **Why it is asked per dial rather than armed for the process.** The obvious home is the
/// function every socket this process owns passes through — and that is exactly why it is the
/// wrong one. Arming it there would also change when an idle connection of some other kind is
/// dropped, fleet-wide, for a change nobody asked for.
///
/// An `enum class` rather than a `bool` because it appears in an API surface:
/// `connect(host, port, { .keepAlive = KeepAlive::Yes })` reads as what it is, where a bare
/// `true` at that position reads as nothing.
enum class KeepAlive : std::uint8_t
{
    No,  ///< Leave the platform default, which is off.
    Yes, ///< Probe, with the intervals in @c KeepAliveSettings.
};

/// How aggressively a keepalive-armed connection probes a silent peer.
///
/// **Bare `SO_KEEPALIVE` is worth nothing.** Without the intervals it inherits the system
/// default, which on Linux is two hours — longer than any deadline this would be protecting. So
/// the flag is never set unless these are, and a platform on which the intervals cannot be
/// applied does not get the flag either.
///
/// | platform | probes stop at |
/// |---|---|
/// | Linux, macOS | `idle + count * interval` = **16 s** |
/// | Windows | `idle + 10 * interval` = **30 s** |
///
/// **The Windows row is not a typo and @c count is not honoured there.** `SIO_KEEPALIVE_VALS`
/// takes the idle time and the interval only; the probe count has been fixed at 10 by the OS
/// since Vista and there is no way to set it. Stated rather than papered over — taking a value
/// that cannot be applied is how a configuration option becomes a lie.
struct KeepAliveSettings
{
    /// Quiet time before the first probe.
    std::chrono::milliseconds idle { 10'000 };

    /// Gap between probes once they start.
    std::chrono::milliseconds interval { 2'000 };

    /// Unanswered probes before the connection is declared dead.
    ///
    /// **Ignored on Windows**, where the OS fixes it at 10; see the class note.
    std::uint32_t count { 3 };
};

} // namespace core::net
