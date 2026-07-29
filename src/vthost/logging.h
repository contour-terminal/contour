// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The multiplexer's log categories — the single table of what `src/vthost` records.
///
/// Names follow the tree's module-first convention (`vt.*` ← vtbackend, `pty.*` ← vtpty,
/// `gui.*` ← contour), so `LOG=vthost.*` maps 1:1 onto this directory. Note that
/// logstore::configure's `*` is a plain PREFIX match, so `vthost.*` sweeps in the trace
/// categories too — exactly as `vt.*` already sweeps in `vt.trace.sequence`. Select the trace
/// tier alone with `vthost.trace.*`.
///
/// Protocol violations deliberately have NO category of their own: they go to the always-on
/// `error`, because a malformed frame or a rejected handshake must be visible without anyone
/// having thought to enable a filter first.
///
/// The GUI's own `gui.attach` / `gui.tmux` categories stay separate on purpose: they are the
/// GUI controller's view of a connection, these are the engine's. In a process that is both,
/// the prefix tells you which side spoke.
///
/// Threading: every emission point in this module runs on the event loop thread. Session pump
/// threads marshal through EventLoop::post before anything logs, and the daemon's signal
/// handling logs from inside its posted continuation rather than the sigwait thread. That
/// invariant is what keeps lines whole and lets the test capture fixture stay lock-free.

#include <crispy/logstore.h>

namespace vthost
{

/// Enabled by default: with no $LOG and no --log, these lines are the foreground user's only
/// feedback that the daemon came up and which sockets it bound. Keep this category
/// BANNER-GRADE — anything chattier belongs in one of the disabled categories below, or it
/// will spam a user who never asked for logging.
auto inline const daemonLog = logstore::category("vthost.daemon",
                                                 "Daemon lifecycle: endpoints bound, listeners "
                                                 "started, shutdown and what asked for it "
                                                 "(a signal, or the last session closing).",
                                                 logstore::category::state::Enabled);

auto inline const connectionLog =
    logstore::category("vthost.conn", "Client connections: accept, handshake, disconnect and why.");

auto inline const sessionLog = logstore::category("vthost.session",
                                                  "Hosted session lifecycle: spawn, PTY-factory "
                                                  "failure, model refusal, resize, exit.");

auto inline const tmuxLog = logstore::category("vthost.tmux",
                                               "tmux control-mode and imsg endpoints: attach, "
                                               "rejection, failure.");

auto inline const clientLog = logstore::category("vthost.client",
                                                 "Native-protocol CLIENT engine: connect, "
                                                 "handshake, disconnect, failures.");

auto inline const protocolTraceLog =
    logstore::category("vthost.trace.proto", "Traces every native PDU sent and received.");

auto inline const tmuxTraceLog =
    logstore::category("vthost.trace.tmux", "Traces every tmux control-mode line, both directions.");

} // namespace vthost
