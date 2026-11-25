// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only, Linux: the probe's sandboxed mode, which locks itself down as the extension host does and then
/// tries one operation on a thread that ran before the lockdown.
///
///   extension_host_probe --receive-grant [--sandboxed] [--outer-filter=<kind>] [--sandbox-op=<op>] <helper
///   arguments>
///
/// It binds itself to its parent and hardens itself as the host does. An outer filter, if named, is installed
/// first, as an outer sandbox (Flatpak's bwrap) would have: `no-landlock` (Landlock's calls fail ENOSYS),
/// `flatpak` (clone3 fails ENOSYS; unshare, setns, mount, personality and ptrace EPERM), `passrights`
/// (setting SO_PASSRIGHTS fails EPERM), or `thread` (a thread of its own installs a filter of its own). Then
/// it starts a thread, restricts itself (--sandboxed: makeSystemSandbox()'s restrictBeforeThreads()), starts
/// the op's thread, has both try to open a file, and locks down.
///
/// On its first lane it reports `lockdown=` `locked`, `none` (not --sandboxed) or `failed:<step>:<code>`;
/// `before-restrict=` and `after-restrict=`, what opening a file from each thread came to; and `end`. Then
/// the op's thread runs the op and reports, on its second lane, `op=` `ok`, `refused:<errno>`,
/// `failed:<what>` or (channel-pair) `upward:<refused|discarded>`, and `end` -- unless the op killed the
/// process. It exits 0 once its control channel ended.
///
/// The ops: open-file, open-directory, socket-inet, socket-unix, send-descriptor, fork, spawn-thread,
/// execve, ptrace-traceme, signal-parent, map-writable-executable, map-shared-unfixed,
/// protect-writable-executable, channel-pair, none, and permitted, which takes one Instantiate's grant off
/// control as the host does and then does what the locked-down host does (see SandboxProbe.cpp).

#include <span>
#include <string_view>

namespace vtextension::wasm::remote::process::testing
{

/// @return Whether @p arguments ask for the sandboxed mode: any of its three options.
[[nodiscard]] bool asksForSandboxProbe(std::span<std::string_view const> arguments) noexcept;

/// Runs the sandboxed mode on @p arguments, those after `--receive-grant`.
/// @return The exit code.
[[nodiscard]] int runSandboxProbe(std::span<std::string_view const> arguments);

} // namespace vtextension::wasm::remote::process::testing
