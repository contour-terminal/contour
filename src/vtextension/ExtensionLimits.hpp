// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>

namespace vtextension
{

/// The tunables that bound what one extension may cost the terminal. Every consumer reads its limit
/// from here; none hard-codes one. The command runtime limit is not repeated here: commands reuse
/// vtbackend::MaxCommandRuntime.
struct ExtensionLimits
{
    /// How long one render or update call may run before the watchdog terminates it.
    std::chrono::milliseconds callDeadline { 50 };

    /// How long instantiation plus the extension's load call may run.
    std::chrono::milliseconds loadDeadline { 500 };

    /// The cap on one instance's linear memory, fixed at instantiation.
    std::size_t memoryBytes = std::size_t { 32 } * 1024 * 1024;

    /// The interpreter stack one instance's calls run on, fixed at instantiation. It lives outside
    /// linear memory (a guest's own shadow stack is inside it), and bounds how deeply guest calls nest.
    std::size_t stackBytes = std::size_t { 128 } * 1024;

    /// The shortest interval between two renders of one extension.
    std::chrono::milliseconds minFrameInterval { 16 };

    /// How many events one extension's queue holds before it drops.
    std::size_t queueCapacity = 64;

    /// How many distinct user variables an extension tracks: how many coalesce by name in its queue, and
    /// how many `state` reports. A name beyond them is dropped from `state` (known names still update)
    /// and queues uncoalesced.
    std::size_t maxUserVars = 32;

    /// How many messages one call (load, update or render) may log; the rest of that call's are dropped.
    /// Bounds how long a guest can keep the host's log callback busy.
    std::size_t maxLogsPerCall = 16;

    /// How many faults within faultWindow disable an extension.
    std::size_t faultsBeforeDisable = 3;

    /// The window faultsBeforeDisable counts within.
    std::chrono::seconds faultWindow { 60 };

    /// The first back-off after a fault; each further fault doubles it.
    std::chrono::milliseconds initialBackoff { 250 };

    /// The most bytes one render may return.
    std::size_t maxRenderBytes = std::size_t { 1 } * 1024 * 1024;

    /// The most bytes the host copies out of a guest for one host call (`log`, `run-command`): the
    /// strings' bytes plus the host-side size of every list element, counted together. A call that
    /// asks for more traps, so that a guest cannot make the host allocate without bound, for
    /// instance with a list whose many elements all point at one large string.
    std::size_t maxHostCallBytes = std::size_t { 64 } * 1024;

    /// The most reply bytes (answers to queries such as DA1 or CSI 6 n) one extension's private terminal
    /// holds until the extension drains them, so a guest that queries without ever reading cannot grow
    /// the buffer without bound. Replies are kept whole: the oldest are evicted to make room for a new
    /// one, a reply larger than this on its own is dropped, and both are counted -- never cut, so what
    /// the extension reads is always complete sequences.
    std::size_t maxReplyBytes = std::size_t { 4 } * 1024;

    /// The largest module file the host reads. Its size is checked before it is read, so that a huge file
    /// (or one that keeps growing) costs no memory; one larger shows the disabled notice.
    std::size_t maxModuleBytes = std::size_t { 16 } * 1024 * 1024;
};

} // namespace vtextension
