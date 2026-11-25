// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only, Linux and FreeBSD: the probe's sandboxed mode, which locks itself down as the extension host
/// does and then tries one operation on a thread that ran before the lockdown.
///
///   extension_host_probe --receive-grant [--sandboxed] [--guest-code=<code>] [--outer-filter=<kind>]
///   [--sandbox-op=<op>] <helper arguments>
///
/// It binds itself to its parent and hardens itself as the host does. An outer filter (Linux), if named, is
/// installed first, as an outer sandbox (Flatpak's bwrap) would have: `no-landlock` (Landlock's calls fail
/// ENOSYS), `flatpak` (clone3 fails ENOSYS; unshare, setns, mount, personality and ptrace EPERM),
/// `passrights` (setting SO_PASSRIGHTS fails EPERM), or `thread` (a thread of its own installs a filter of
/// its own). Then it starts a thread, restricts itself (--sandboxed: makeSystemSandbox()'s
/// restrictBeforeThreads(), with the rows of an engine whose guest code is `--guest-code=` `compiled`, the
/// default, or `interpreted`), starts the op's thread, has both try to open a file, caps itself (--sandboxed)
/// and locks down.
///
/// On its first lane it reports `lockdown=` `locked`, `none` (not --sandboxed) or `failed:<step>:<code>`, the
/// first of restricting, capping and locking down that failed; `no-new-privs=` `on`, `off` or `unknown`,
/// asked after the restriction; `before-restrict=` and `after-restrict=`, what opening a file from each
/// thread came to; and `end`. Then the op's thread runs the op and reports, on its second lane, `op=` `ok`,
/// `refused:<errno>`, `failed:<what>` or (channel-pair) `upward:<refused|discarded>`, and `end` -- unless the
/// op killed the process. It exits 0 once its control channel ended.
///
/// The ops: open-file, open-directory, socket-inet, socket-unix, connect-inet, ioctl-control,
/// send-descriptor, fork, pdfork (FreeBSD), spawn-thread, execve, ptrace-traceme, signal-parent,
/// map-writable-executable, map-shared-unfixed, map-executable, protect-writable-executable,
/// protect-executable, protect-readable, channel-pair, held-limits (how far the system limits each descriptor
/// the sandbox was handed: lanes, then standard input, output and error, `unlimited`, `as-its-role` or
/// `otherwise`, comma-separated), none, and permitted, which takes one Instantiate's
/// grant off control as the host does and then does what the locked-down host does (see SandboxProbe.cpp).

#include <span>
#include <string_view>

namespace vtextension::wasm::remote::process::testing
{

/// @return Whether @p arguments ask for the sandboxed mode: any of its four options.
[[nodiscard]] bool asksForSandboxProbe(std::span<std::string_view const> arguments) noexcept;

/// Runs the sandboxed mode on @p arguments, those after `--receive-grant`.
/// @return The exit code.
[[nodiscard]] int runSandboxProbe(std::span<std::string_view const> arguments);

} // namespace vtextension::wasm::remote::process::testing
